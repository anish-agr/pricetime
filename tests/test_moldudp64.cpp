#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "pricetime/itch_writer.hpp"
#include "pricetime/moldudp64.hpp"
#include "pricetime/symbol.hpp"

using namespace pricetime;

namespace {

// A framed ITCH message from the writer, with the 2-byte prefix stripped —
// the payload shape Mold blocks carry.
std::vector<std::uint8_t> one_message() {
  itch::Writer w;
  w.add_order(123, 7, Side::Bid, 100, Symbol("TEST"), 999900);
  return std::vector<std::uint8_t>(w.bytes().begin() + 2, w.bytes().end());
}

}  // namespace

TEST_CASE("mold: a packed packet unpacks to the same messages and sequences") {
  const auto msg = one_message();
  mold::Packer p("SESSIONAA ");
  p.begin(41);
  REQUIRE(p.add(msg.data(), msg.size()));
  REQUIRE(p.add(msg.data(), msg.size()));
  REQUIRE(p.add(msg.data(), msg.size()));
  p.seal();
  CHECK(p.count() == 3);
  CHECK(p.size() == mold::kHeaderSize + 3 * (2 + msg.size()));

  mold::Header hdr;
  std::vector<std::uint64_t> seqs;
  std::size_t bytes_seen = 0;
  REQUIRE(mold::unpack(p.bytes(), p.size(), hdr,
                       [&](std::uint64_t seq, const std::uint8_t* m, std::size_t len) {
                         seqs.push_back(seq);
                         bytes_seen += len;
                         CHECK(m[0] == 'A');
                       }));
  CHECK(std::string(hdr.session) == "SESSIONAA ");
  CHECK(hdr.sequence == 41);
  CHECK(hdr.count == 3);
  REQUIRE(seqs.size() == 3);
  CHECK(seqs[0] == 41);  // per-message sequence numbers are consecutive
  CHECK(seqs[1] == 42);
  CHECK(seqs[2] == 43);
  CHECK(bytes_seen == 3 * msg.size());
}

TEST_CASE("mold: packer refuses a message that will not fit") {
  const auto msg = one_message();
  mold::Packer p;
  p.begin(1);
  std::size_t added = 0;
  while (p.add(msg.data(), msg.size())) ++added;
  CHECK(added > 0);
  CHECK(p.size() <= 1400);  // never exceeds the MTU budget
}

TEST_CASE("mold: heartbeat and end-of-session are header-only") {
  mold::Packer p;
  p.seal_heartbeat(100);
  CHECK(p.size() == mold::kHeaderSize);

  mold::Header hdr;
  REQUIRE(mold::unpack(p.bytes(), p.size(), hdr,
                       [](std::uint64_t, const std::uint8_t*, std::size_t) {
                         FAIL("heartbeat must deliver no messages");
                       }));
  CHECK(hdr.count == mold::kHeartbeat);
  CHECK(hdr.sequence == 100);

  p.seal_end_of_session(200);
  REQUIRE(mold::unpack(p.bytes(), p.size(), hdr,
                       [](std::uint64_t, const std::uint8_t*, std::size_t) {}));
  CHECK(hdr.count == mold::kEndOfSession);
}

TEST_CASE("mold: truncated and malformed packets are rejected") {
  const auto msg = one_message();
  mold::Packer p;
  p.begin(1);
  REQUIRE(p.add(msg.data(), msg.size()));
  p.seal();

  mold::Header hdr;
  auto sink = [](std::uint64_t, const std::uint8_t*, std::size_t) {};
  CHECK_FALSE(mold::unpack(p.bytes(), mold::kHeaderSize - 1, hdr, sink));  // short header
  CHECK_FALSE(mold::unpack(p.bytes(), p.size() - 3, hdr, sink));           // truncated body
  // Trailing garbage after the declared blocks is also a malformed packet.
  std::vector<std::uint8_t> padded(p.bytes(), p.bytes() + p.size());
  padded.push_back(0);
  CHECK_FALSE(mold::unpack(padded.data(), padded.size(), hdr, sink));
}

TEST_CASE("mold: gap tracker counts exactly what was lost") {
  mold::GapTracker g;

  mold::Header h;
  h.sequence = 1;
  h.count = 3;  // messages 1..3
  CHECK(g.on_packet(h) == 0);
  CHECK(g.next_expected() == 4);

  h.sequence = 4;
  h.count = 2;  // 4..5, contiguous
  CHECK(g.on_packet(h) == 0);

  h.sequence = 9;
  h.count = 1;  // 6,7,8 vanished
  CHECK(g.on_packet(h) == 3);
  CHECK(g.total_missed() == 3);
  CHECK(g.next_expected() == 10);

  // A duplicate or reordered packet from the past neither counts as a gap
  // nor rewinds the tracker.
  h.sequence = 4;
  h.count = 2;
  CHECK(g.on_packet(h) == 0);
  CHECK(g.next_expected() == 10);

  // A heartbeat carrying the next expected sequence signals "no gap" during
  // silence; one from the future reveals the loss.
  h.sequence = 10;
  h.count = mold::kHeartbeat;
  CHECK(g.on_packet(h) == 0);
  h.sequence = 15;
  h.count = mold::kHeartbeat;
  CHECK(g.on_packet(h) == 5);
  CHECK(g.total_missed() == 8);
}
