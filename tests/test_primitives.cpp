#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>

#include "pricetime/hash.hpp"
#include "pricetime/level.hpp"
#include "pricetime/order_pool.hpp"
#include "pricetime/types.hpp"

using namespace pricetime;

namespace {

Order make_order(OrderId id, Qty qty, Price price = 100, Side side = Side::Bid) {
  Order o;
  o.id = id;
  o.price = price;
  o.qty = qty;
  o.side = side;
  return o;
}

}  // namespace

TEST_CASE("level FIFO: push_back preserves arrival order") {
  Level lvl;
  lvl.price = 100;
  Order a = make_order(1, 10);
  Order b = make_order(2, 20);
  Order c = make_order(3, 30);
  lvl.push_back(&a);
  lvl.push_back(&b);
  lvl.push_back(&c);

  CHECK(lvl.order_count == 3);
  CHECK(lvl.total_qty == 60);
  CHECK(lvl.head == &a);
  CHECK(lvl.tail == &c);
  CHECK(a.prev == nullptr);
  CHECK(a.next == &b);
  CHECK(b.prev == &a);
  CHECK(b.next == &c);
  CHECK(c.prev == &b);
  CHECK(c.next == nullptr);
  CHECK(a.level == &lvl);
  CHECK(c.level == &lvl);
}

TEST_CASE("level FIFO: remove head, middle, tail") {
  Level lvl;
  Order a = make_order(1, 10);
  Order b = make_order(2, 20);
  Order c = make_order(3, 30);
  lvl.push_back(&a);
  lvl.push_back(&b);
  lvl.push_back(&c);

  SUBCASE("middle") {
    lvl.remove(&b);
    CHECK(lvl.order_count == 2);
    CHECK(lvl.total_qty == 40);
    CHECK(lvl.head == &a);
    CHECK(lvl.tail == &c);
    CHECK(a.next == &c);
    CHECK(c.prev == &a);
    CHECK(b.level == nullptr);
  }
  SUBCASE("head") {
    lvl.remove(&a);
    CHECK(lvl.head == &b);
    CHECK(b.prev == nullptr);
    CHECK(lvl.total_qty == 50);
  }
  SUBCASE("tail") {
    lvl.remove(&c);
    CHECK(lvl.tail == &b);
    CHECK(b.next == nullptr);
    CHECK(lvl.total_qty == 30);
  }
  SUBCASE("all, in mixed order") {
    lvl.remove(&b);
    lvl.remove(&c);
    lvl.remove(&a);
    CHECK(lvl.empty());
    CHECK(lvl.total_qty == 0);
    CHECK(lvl.head == nullptr);
    CHECK(lvl.tail == nullptr);
  }
}

TEST_CASE("level reduce: partial fill keeps queue position") {
  Level lvl;
  Order a = make_order(1, 10);
  Order b = make_order(2, 20);
  lvl.push_back(&a);
  lvl.push_back(&b);

  lvl.reduce(&a, 4);
  CHECK(a.qty == 6);
  CHECK(lvl.total_qty == 26);
  CHECK(lvl.head == &a);  // still first in line
  CHECK(lvl.order_count == 2);

  lvl.reduce(&a, 6);
  CHECK(a.qty == 0);
  lvl.remove(&a);
  CHECK(lvl.total_qty == 20);
  CHECK(lvl.head == &b);
}

TEST_CASE("order pool reuses released nodes and resets them") {
  OrderPool pool;
  Order* a = pool.alloc();
  a->id = 42;
  a->qty = 7;
  a->next = a;  // deliberately dirty the links
  Order* b = pool.alloc();
  CHECK(pool.allocated() == 2);

  pool.release(a);
  Order* c = pool.alloc();
  CHECK(c == a);  // LIFO recycling
  CHECK(pool.allocated() == 2);  // recycled, not grown
  CHECK(c->id == 0);
  CHECK(c->qty == 0);
  CHECK(c->next == nullptr);
  CHECK(c->level == nullptr);

  pool.release(b);
  pool.release(c);
}

TEST_CASE("fnv1a64 matches published test vectors") {
  Fnv1a64 empty;
  CHECK(empty.value == 14695981039346656037ull);  // offset basis

  Fnv1a64 a;
  a.mix_byte(static_cast<std::uint8_t>('a'));
  CHECK(a.value == 0xaf63dc4c8601ec8cull);

  Fnv1a64 foobar;
  for (char ch : {'f', 'o', 'o', 'b', 'a', 'r'}) {
    foobar.mix_byte(static_cast<std::uint8_t>(ch));
  }
  CHECK(foobar.value == 0x85944171f73967e8ull);
}

TEST_CASE("fnv1a64 mix(u64) is defined as little-endian byte order") {
  Fnv1a64 whole;
  whole.mix(0x0102030405060708ull);

  Fnv1a64 bytes;
  const std::uint8_t le[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
  for (std::uint8_t b : le) {
    bytes.mix_byte(b);
  }
  CHECK(whole.value == bytes.value);
}
