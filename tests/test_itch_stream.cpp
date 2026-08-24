#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_stream.hpp"
#include "pricetime/itch_writer.hpp"
#include "pricetime/symbol.hpp"

using namespace pricetime;
using namespace pricetime::itch;

namespace {

// RAII temp file holding the writer's bytes.
struct TempItch {
  std::string path;

  explicit TempItch(const std::vector<std::uint8_t>& bytes, const char* name) : path(name) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  }

  ~TempItch() { std::remove(path.c_str()); }
};

Writer make_stream(int messages) {
  Writer w;
  const Symbol sym("TEST");
  w.system_event(1, 'Q');
  for (int i = 0; i < messages; ++i) {
    const OrderId ref = static_cast<OrderId>(i + 1);
    w.add_order(static_cast<std::uint64_t>(10 + i), ref, i % 2 == 0 ? Side::Bid : Side::Ask,
                100, sym, 999900 + (i % 7));
    if (i % 3 == 0) w.order_executed(static_cast<std::uint64_t>(11 + i), ref, 40, 5000 + i);
    if (i % 5 == 0) w.order_delete(static_cast<std::uint64_t>(12 + i), ref);
  }
  return w;
}

}  // namespace

// The stitch is the whole point: with a chunk far smaller than the file,
// nearly every message straddles a boundary, and the stream must deliver
// byte-for-byte what the one-shot reader delivers.
TEST_CASE("stream reader matches the in-memory reader across tiny chunks") {
  const Writer w = make_stream(500);
  const TempItch f(w.bytes(), "stream_test_a.itch");

  std::vector<std::uint8_t> whole;
  const ReadResult ref = for_each_framed_message(
      w.bytes().data(), w.bytes().size(), [&](const std::uint8_t* m, std::size_t len) {
        whole.insert(whole.end(), m, m + len);
      });
  REQUIRE(ref.ok());

  // Chunk sizes chosen to hit every ugly alignment: smaller than one message,
  // exactly a message-ish, prime, and large.
  for (const std::size_t chunk : {7u, 36u, 61u, 1024u, 1u << 20}) {
    std::vector<std::uint8_t> streamed;
    const ReadResult r = for_each_framed_stream(
        f.path,
        [&](const std::uint8_t* m, std::size_t len) {
          streamed.insert(streamed.end(), m, m + len);
        },
        chunk);
    INFO("chunk size ", chunk);
    REQUIRE(r.ok());
    CHECK(r.messages == ref.messages);
    CHECK(streamed == whole);  // byte-identical delivery, boundaries invisible
  }
}

TEST_CASE("stream reader reports truncation at the true end of file") {
  const Writer w = make_stream(50);
  auto bytes = w.bytes();
  bytes.resize(bytes.size() - 5);  // cut the last message short
  const TempItch f(bytes, "stream_test_b.itch");

  const ReadResult r = for_each_framed_stream(
      f.path, [](const std::uint8_t*, std::size_t) {}, 64);
  CHECK(r.status == ReadStatus::TruncatedMessage);
  CHECK(r.messages > 0);  // everything before the cut was still delivered
}

TEST_CASE("stream reader surfaces a mid-file desync with its absolute offset") {
  const Writer w = make_stream(50);
  auto bytes = w.bytes();
  // Corrupt a length prefix deep in the file: claim 35 bytes for an 'A'.
  std::size_t off = 0;
  int seen = 0;
  while (off < bytes.size()) {
    const std::size_t len = be16(bytes.data() + off);
    if (bytes[off + 2] == 'A' && ++seen == 20) {
      bytes[off + 1] = 35;
      break;
    }
    off += 2 + len;
  }
  const TempItch f(bytes, "stream_test_c.itch");

  const ReadResult r = for_each_framed_stream(
      f.path, [](const std::uint8_t*, std::size_t) {}, 128);
  CHECK(r.status == ReadStatus::LengthMismatch);
  CHECK(r.offset == off);  // absolute, not chunk-relative
  CHECK(r.bad_type == 'A');
}

TEST_CASE("stream reader handles a missing file and an empty file") {
  const ReadResult missing = for_each_framed_stream(
      "does_not_exist.itch", [](const std::uint8_t*, std::size_t) {});
  CHECK_FALSE(missing.ok());

  const TempItch f(std::vector<std::uint8_t>{}, "stream_test_d.itch");
  const ReadResult empty = for_each_framed_stream(
      f.path, [](const std::uint8_t*, std::size_t) {});
  CHECK(empty.ok());
  CHECK(empty.messages == 0);
}
