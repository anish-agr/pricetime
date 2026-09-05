#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "fuzz_itch.hpp"
#include "pricetime/itch_writer.hpp"
#include "pricetime/symbol.hpp"

using namespace pricetime;
using pricetime::fuzz::fuzz_one;

namespace {

std::uint64_t volatile g_escape_sink = 0;

// Keeps a result observable so no stage of the target can be optimized away.
// The value is read back as well as written: a write-only volatile still trips
// -Wunused-but-set-variable on GCC, the same trap test_alloc.cpp documents.
bool escape(std::uint64_t v) {
  g_escape_sink = v;
  return g_escape_sink == v;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
}

void run(const std::uint8_t* data, std::size_t size, const char* what) {
  bool sound = true;
  const std::uint64_t sink = fuzz_one(data, size, &sound);
  INFO("input: ", what, " (", size, " bytes)");
  CHECK(sound);
  CHECK(escape(sink));
}

std::vector<std::uint8_t> valid_stream() {
  itch::Writer w;
  const Symbol a("AAPL");
  const Symbol b("MSFT");
  w.system_event(1, 'Q');
  for (int i = 0; i < 24; ++i) {
    const OrderId ref = static_cast<OrderId>(i + 1);
    w.add_order(static_cast<std::uint64_t>(100 + i), ref, i % 2 == 0 ? Side::Bid : Side::Ask, 100,
                i % 3 == 0 ? a : b, 999900 + (i % 5));
    if (i % 4 == 0) w.order_executed(static_cast<std::uint64_t>(101 + i), ref, 40, 7000 + i);
    if (i % 5 == 0) w.order_cancel(static_cast<std::uint64_t>(102 + i), ref, 10);
    if (i % 7 == 0) w.order_delete(static_cast<std::uint64_t>(103 + i), ref);
  }
  return w.bytes();
}

}  // namespace

// Every input the fuzzer has found stays a regression test, replayed here on
// every compiler and platform CI covers rather than only where libFuzzer runs.
TEST_CASE("fuzz corpus: every committed input parses without breaking an invariant") {
#ifdef PRICETIME_FUZZ_CORPUS_DIR
  const std::filesystem::path dir(PRICETIME_FUZZ_CORPUS_DIR);
  REQUIRE(std::filesystem::exists(dir));
  std::size_t seen = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (!entry.is_regular_file()) continue;
    const std::vector<std::uint8_t> bytes = read_file(entry.path());
    run(bytes.data(), bytes.size(), entry.path().filename().string().c_str());
    ++seen;
  }
  CHECK(seen > 0);
#else
  MESSAGE("corpus directory not configured; skipping");
#endif
}

TEST_CASE("fuzz corpus: degenerate inputs are handled without a corpus file") {
  run(nullptr, 0, "null, zero length");
  const std::uint8_t one[1] = {0x00};
  run(one, 1, "single zero byte");
  const std::uint8_t two[2] = {0x00, 0x00};
  run(two, 2, "zero length prefix");
  const std::uint8_t huge[4] = {0xFF, 0xFF, 'A', 'A'};
  run(huge, 4, "length far beyond the buffer");
  // A correctly framed message whose declared length is one byte short of the
  // buffer, so the last byte of the message is missing.
  std::vector<std::uint8_t> cut = valid_stream();
  cut.pop_back();
  run(cut.data(), cut.size(), "final message cut short");
}

// A fuzzer needs clang; this needs nothing, so the parser gets a fresh
// randomized beating on every CI run everywhere. Deterministic seed, so a
// failure is reproducible from the reported iteration.
TEST_CASE("fuzz corpus: randomized mutations of a valid stream") {
  const std::vector<std::uint8_t> base = valid_stream();
  std::mt19937_64 rng(0xF0FEEDull);

  for (int iter = 0; iter < 3000; ++iter) {
    std::vector<std::uint8_t> buf = base;
    const int mutations = 1 + static_cast<int>(rng() % 8);
    for (int m = 0; m < mutations && !buf.empty(); ++m) {
      switch (rng() % 4) {
        case 0:  // flip a byte
          buf[rng() % buf.size()] = static_cast<std::uint8_t>(rng() & 0xFF);
          break;
        case 1:  // truncate
          buf.resize(1 + (rng() % buf.size()));
          break;
        case 2: {  // splice a run of random bytes over the top
          const std::size_t at = rng() % buf.size();
          const std::size_t n = std::min<std::size_t>(buf.size() - at, 1 + (rng() % 16));
          for (std::size_t i = 0; i < n; ++i) {
            buf[at + i] = static_cast<std::uint8_t>(rng() & 0xFF);
          }
          break;
        }
        default:  // zero a length prefix somewhere
          if (buf.size() >= 2) buf[(rng() % (buf.size() / 2)) * 2] = 0;
          break;
      }
    }
    if (buf.empty()) continue;
    bool sound = true;
    const std::uint64_t sink = fuzz_one(buf.data(), buf.size(), &sound);
    if (!sound) {
      INFO("mutation iteration ", iter, " broke a structural invariant");
      REQUIRE(sound);
    }
    escape(sink);
  }
}

TEST_CASE("fuzz corpus: pure random bytes") {
  std::mt19937_64 rng(0xBADF00Dull);
  std::vector<std::uint8_t> buf;
  for (int iter = 0; iter < 2000; ++iter) {
    buf.resize(rng() % 512);
    for (std::uint8_t& b : buf) b = static_cast<std::uint8_t>(rng() & 0xFF);
    bool sound = true;
    const std::uint64_t sink = fuzz_one(buf.data(), buf.size(), &sound);
    if (!sound) {
      INFO("random iteration ", iter);
      REQUIRE(sound);
    }
    escape(sink);
  }
}
