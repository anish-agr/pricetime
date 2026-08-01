#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "book_test_util.hpp"
#include "pricetime/book.hpp"
#include "pricetime/ladder_dense.hpp"
#include "pricetime/ladder_map.hpp"

using namespace pricetime;
using pricetime::test::check_invariants;
using pricetime::test::make_book;
using pricetime::test::StreamRunner;

namespace {

constexpr std::uint64_t kSeed = 0xC0FFEE;
constexpr int kOps = 100000;
constexpr int kCheckEvery = 2000;

// Pinned fingerprint of the book after the seeded stream below. Must be
// identical on every platform, compiler, and ladder policy; a change here
// means matching semantics changed and is either a bug or a versioned,
// deliberate decision. Regenerate by running this test and copying the value
// doctest reports.
constexpr std::uint64_t kGoldenHash = 14253304357271957781ull;

}  // namespace

TEST_CASE_TEMPLATE("property: invariants hold throughout a random stream", L, MapLadder,
                   DenseLadder) {
  auto book = make_book<L>();
  StreamRunner<L> gen(kSeed);
  for (int i = 1; i <= kOps; ++i) {
    gen.step(book);
    if (i % kCheckEvery == 0) check_invariants(book);
  }
  check_invariants(book);
  CHECK(book.open_orders() > 0);  // stream really left a populated book behind
}

TEST_CASE_TEMPLATE("property: same stream twice gives identical state hashes", L, MapLadder,
                   DenseLadder) {
  auto book_a = make_book<L>();
  auto book_b = make_book<L>();
  StreamRunner<L> gen_a(kSeed);
  StreamRunner<L> gen_b(kSeed);
  for (int i = 1; i <= kOps; ++i) {
    gen_a.step(book_a);
    gen_b.step(book_b);
    if (i % kCheckEvery == 0) {
      REQUIRE(book_a.state_hash() == book_b.state_hash());
    }
  }
  CHECK(book_a.state_hash() == book_b.state_hash());
}

TEST_CASE("differential: dense and map ladders agree at every checkpoint") {
  auto book_dense = make_book<DenseLadder>();
  auto book_map = make_book<MapLadder>();
  StreamRunner<DenseLadder> gen_dense(kSeed);
  StreamRunner<MapLadder> gen_map(kSeed);
  for (int i = 1; i <= kOps; ++i) {
    gen_dense.step(book_dense);
    gen_map.step(book_map);
    if (i % kCheckEvery == 0) {
      REQUIRE(book_dense.state_hash() == book_map.state_hash());
    }
  }
  REQUIRE(book_dense.state_hash() == book_map.state_hash());
  REQUIRE(book_dense.open_orders() == book_map.open_orders());
  REQUIRE(book_dense.counters().traded_qty == book_map.counters().traded_qty);
}

TEST_CASE("golden replay: seeded stream hashes to the pinned constant") {
  auto book = make_book<MapLadder>();
  StreamRunner<MapLadder> gen(kSeed);
  for (int i = 0; i < kOps; ++i) gen.step(book);
  const std::uint64_t h = book.state_hash();
  CHECK(h == kGoldenHash);
}
