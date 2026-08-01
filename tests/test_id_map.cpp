#include <doctest/doctest.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "pricetime/id_map.hpp"
#include "pricetime/types.hpp"

using namespace pricetime;

namespace {

// The maps store Order* but never dereference them, so distinct fake
// addresses are enough to check that values round-trip.
Order* fake(std::uintptr_t n) { return reinterpret_cast<Order*>(n * 16 + 8); }

std::uint64_t rng(std::uint64_t& s) {
  s += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = s;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

}  // namespace

TEST_CASE_TEMPLATE("id map basics", M, StdIdMap, OpenAddressIdMap) {
  M m;
  CHECK(m.size() == 0);
  CHECK(m.find(1) == nullptr);

  CHECK(m.insert(1, fake(1)));
  CHECK(m.insert(2, fake(2)));
  CHECK(m.size() == 2);
  CHECK(m.find(1) == fake(1));
  CHECK(m.find(2) == fake(2));
  CHECK(m.find(3) == nullptr);

  CHECK_FALSE(m.insert(1, fake(9)));  // duplicate rejected
  CHECK(m.find(1) == fake(1));        // and the original value survives
  CHECK(m.size() == 2);

  CHECK(m.erase(1));
  CHECK(m.find(1) == nullptr);
  CHECK(m.size() == 1);
  CHECK_FALSE(m.erase(1));  // double erase rejected
  CHECK(m.erase(2));
  CHECK(m.size() == 0);
}

TEST_CASE_TEMPLATE("id map: grows past its initial capacity", M, StdIdMap, OpenAddressIdMap) {
  M m;
  constexpr OrderId kN = 20000;  // well past OpenAddressIdMap's 1024 slots
  for (OrderId i = 1; i <= kN; ++i) REQUIRE(m.insert(i, fake(i)));
  CHECK(m.size() == kN);
  for (OrderId i = 1; i <= kN; ++i) REQUIRE(m.find(i) == fake(i));
  CHECK(m.find(kN + 1) == nullptr);
}

TEST_CASE_TEMPLATE("id map: reserve avoids growth but keeps behaviour", M, StdIdMap,
                   OpenAddressIdMap) {
  M m;
  m.reserve(50000);
  for (OrderId i = 1; i <= 30000; ++i) REQUIRE(m.insert(i, fake(i)));
  for (OrderId i = 1; i <= 30000; ++i) REQUIRE(m.find(i) == fake(i));
  CHECK(m.size() == 30000);
}

TEST_CASE("open addressing: reserve actually sizes the table") {
  OpenAddressIdMap m;
  CHECK(m.capacity() == OpenAddressIdMap::kInitialCapacity);
  m.reserve(100000);
  CHECK(m.capacity() >= 100000 * 10 / 7);
  const std::size_t before = m.capacity();
  for (OrderId i = 1; i <= 50000; ++i) REQUIRE(m.insert(i, fake(i)));
  CHECK(m.capacity() == before);  // no rehash happened on the hot path
}

// Backward-shift deletion is the subtle part: an erase in the middle of a
// probe chain must not orphan the elements behind it. Colliding keys are
// forced by construction here, since a good hash makes natural collisions rare.
TEST_CASE("open addressing: erase preserves colliding probe chains") {
  OpenAddressIdMap m;
  // With a 1024-slot table, ids spaced 1024 apart are unrelated under the
  // mixing hash, so instead build a long chain by brute force: insert many
  // keys, then delete an interior subset and verify every survivor is found.
  constexpr OrderId kN = 5000;
  for (OrderId i = 1; i <= kN; ++i) REQUIRE(m.insert(i, fake(i)));
  for (OrderId i = 2; i <= kN; i += 2) REQUIRE(m.erase(i));
  CHECK(m.size() == kN / 2);
  for (OrderId i = 1; i <= kN; i += 2) {
    INFO("odd key ", i, " should have survived");
    REQUIRE(m.find(i) == fake(i));
  }
  for (OrderId i = 2; i <= kN; i += 2) {
    INFO("even key ", i, " should be gone");
    REQUIRE(m.find(i) == nullptr);
  }
}

// The real proof: a long randomized sequence of inserts and erases, checked
// against std::unordered_map after every operation.
TEST_CASE("open addressing: matches std::unordered_map under random churn") {
  OpenAddressIdMap fast;
  std::unordered_map<OrderId, Order*> ref;
  std::vector<OrderId> live;
  std::uint64_t seed = 0x1234567;
  OrderId next = 1;

  for (int step = 0; step < 200000; ++step) {
    const std::uint64_t roll = rng(seed) % 100;
    if (roll < 55 || live.empty()) {
      const OrderId id = next++;
      Order* v = fake(id);
      const bool a = fast.insert(id, v);
      const bool b = ref.emplace(id, v).second;
      REQUIRE(a == b);
      live.push_back(id);
    } else if (roll < 90) {
      const std::size_t i = static_cast<std::size_t>(rng(seed) % live.size());
      const OrderId id = live[i];
      live[i] = live.back();
      live.pop_back();
      const bool a = fast.erase(id);
      const bool b = ref.erase(id) != 0;
      REQUIRE(a == b);
    } else {
      // Look up a mix of live and long-dead ids.
      const OrderId id = 1 + rng(seed) % (next > 1 ? next - 1 : 1);
      const auto it = ref.find(id);
      REQUIRE(fast.find(id) == (it == ref.end() ? nullptr : it->second));
    }
    REQUIRE(fast.size() == ref.size());
  }

  for (const auto& kv : ref) REQUIRE(fast.find(kv.first) == kv.second);
}
