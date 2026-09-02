#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "pricetime/itch.hpp"
#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_writer.hpp"
#include "pricetime/symbol.hpp"

using namespace pricetime;
using namespace pricetime::itch;
using pricetime::itch::Writer;

TEST_CASE("big-endian readers assemble fields correctly") {
  const std::uint8_t bytes[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  CHECK(be16(bytes) == 0x0102u);
  CHECK(be32(bytes) == 0x01020304u);
  CHECK(be48(bytes) == 0x010203040506ull);
  CHECK(be64(bytes) == 0x0102030405060708ull);
}

TEST_CASE("big-endian readers are not confused by host byte order") {
  // 0x0000000000000001 big-endian is the value 1; read little-endian it would
  // be 2^56. This is the bug that silently ruins an ITCH parser.
  const std::uint8_t one[8] = {0, 0, 0, 0, 0, 0, 0, 1};
  CHECK(be64(one) == 1u);
  const std::uint8_t big[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  CHECK(be32(big) == 4294967295u);
}

TEST_CASE("symbol round-trips through the 8-byte wire field") {
  const Symbol s("AAPL");
  CHECK(s.str() == "AAPL");
  CHECK(s.data[4] == ' ');  // right-padded
  const Symbol wire = Symbol::from_wire(s.data.data());
  CHECK(wire == s);
  CHECK(wire.bits() == s.bits());

  const Symbol eight("ABCDEFGH");
  CHECK(eight.str() == "ABCDEFGH");
  const Symbol truncated("ABCDEFGHIJ");  // longer than the field
  CHECK(truncated.str() == "ABCDEFGH");
  CHECK(Symbol("A") != Symbol("B"));
}

TEST_CASE("message lengths match the ITCH 5.0 specification") {
  CHECK(message_length('S') == 12);
  CHECK(message_length('R') == 39);
  CHECK(message_length('A') == 36);
  CHECK(message_length('F') == 40);
  CHECK(message_length('E') == 31);
  CHECK(message_length('C') == 36);
  CHECK(message_length('X') == 23);
  CHECK(message_length('D') == 19);
  CHECK(message_length('U') == 35);
  CHECK(message_length('P') == 44);
  CHECK(message_length('z') == 0);  // unknown type reports 0, never a guess
}

// The writer encodes big-endian by hand and the decoder decodes it
// independently; agreement between them is evidence both match the spec.
TEST_CASE("add order round-trips through encode and decode") {
  Writer w;
  w.add_order(1234567890123ull, 42, Side::Ask, 500, Symbol("MSFT"), 4212500);
  const auto& b = w.bytes();
  REQUIRE(b.size() == 2 + 36);
  CHECK(be16(b.data()) == 36);

  const AddOrder m = decode_add_order(b.data() + 2);
  CHECK(m.timestamp == 1234567890123ull);
  CHECK(m.reference == 42);
  CHECK(m.side == Side::Ask);
  CHECK(m.shares == 500);
  CHECK(m.symbol.str() == "MSFT");
  CHECK(m.price == 4212500);  // $421.25 with four implied decimals
}

TEST_CASE("every message type round-trips") {
  Writer w;
  const Symbol sym("TSLA");
  w.system_event(1, 'Q');
  w.stock_directory(2, sym);
  w.add_order(3, 100, Side::Bid, 200, sym, 1000000);
  w.add_order_mpid(4, 101, Side::Ask, 300, sym, 1010000);
  w.order_executed(5, 100, 50, 900);
  w.order_executed_with_price(6, 100, 25, 901, false, 999900);
  w.order_cancel(7, 101, 100);
  w.order_replace(8, 101, 102, 400, 1020000);
  w.order_delete(9, 102);
  w.trade_non_cross(10, 0, Side::Bid, 75, sym, 1005000, 902);

  std::vector<std::pair<char, std::size_t>> seen;
  const ReadResult r = for_each_framed_message(
      w.bytes().data(), w.bytes().size(),
      [&](const std::uint8_t* m, std::size_t len) {
        seen.emplace_back(static_cast<char>(m[0]), len);
      });
  REQUIRE(r.ok());
  CHECK(r.messages == 10);
  CHECK(r.offset == w.bytes().size());
  REQUIRE(seen.size() == 10);
  CHECK(seen[0].first == 'S');
  CHECK(seen[2].first == 'A');
  CHECK(seen[3].first == 'F');
  CHECK(seen[3].second == 40);
  CHECK(seen[5].first == 'C');
  CHECK(seen[7].first == 'U');
  CHECK(seen[9].first == 'P');
}

TEST_CASE("executed-with-price carries its own price, plain executed does not") {
  Writer w;
  w.order_executed(5, 100, 50, 900);
  const OrderExecuted plain = decode_order_executed(w.bytes().data() + 2);
  CHECK_FALSE(plain.has_price);
  CHECK(plain.shares == 50);
  CHECK(plain.match_number == 900);

  Writer w2;
  w2.order_executed_with_price(6, 101, 25, 901, false, 123400);
  const OrderExecuted priced = decode_order_executed(w2.bytes().data() + 2);
  CHECK(priced.has_price);
  CHECK(priced.price == 123400);
  CHECK_FALSE(priced.printable);
  CHECK(priced.shares == 25);
}

TEST_CASE("replace decodes both references without repeating the side") {
  Writer w;
  w.order_replace(8, 500, 501, 400, 1020000);
  const OrderReplace m = decode_order_replace(w.bytes().data() + 2);
  CHECK(m.original_reference == 500);
  CHECK(m.new_reference == 501);
  CHECK(m.shares == 400);
  CHECK(m.price == 1020000);
}

TEST_CASE("reader rejects a truncated file instead of reading past the end") {
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, Symbol("AAPL"), 1000);
  w.add_order(2, 2, Side::Bid, 100, Symbol("AAPL"), 1000);
  auto bytes = w.bytes();

  SUBCASE("message cut short") {
    bytes.resize(bytes.size() - 5);
    const ReadResult r = for_each_framed_message(bytes.data(), bytes.size(),
                                                 [](const std::uint8_t*, std::size_t) {});
    CHECK(r.status == ReadStatus::TruncatedMessage);
    CHECK(r.messages == 1);  // the first message was still delivered
  }

  SUBCASE("length prefix cut short") {
    bytes.resize(bytes.size() - 37);  // leaves a single dangling prefix byte
    const ReadResult r = for_each_framed_message(bytes.data(), bytes.size(),
                                                 [](const std::uint8_t*, std::size_t) {});
    CHECK(r.status == ReadStatus::TruncatedPrefix);
    CHECK(r.messages == 1);
  }
}

// A framing length that disagrees with the spec means the stream is
// misaligned; continuing would produce plausible-looking garbage.
TEST_CASE("reader catches a framing length that contradicts the spec") {
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, Symbol("AAPL"), 1000);
  auto bytes = w.bytes();
  bytes[1] = 35;  // claim 35 bytes for a type that must be 36

  const ReadResult r =
      for_each_framed_message(bytes.data(), bytes.size(), [](const std::uint8_t*, std::size_t) {});
  CHECK(r.status == ReadStatus::LengthMismatch);
  CHECK(r.bad_type == 'A');
  CHECK(r.declared_length == 35);
  CHECK(r.expected_length == 36);
  CHECK(r.messages == 0);
}

// Forward compatibility: a type this build does not know about still parses,
// because the framing length, not the table, advances the cursor.
TEST_CASE("framed reader skips unknown message types without desynchronizing") {
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, Symbol("AAPL"), 1000);
  auto bytes = w.bytes();
  // Append a hypothetical future message: 10 bytes, type 'z'.
  bytes.push_back(0);
  bytes.push_back(10);
  bytes.push_back(static_cast<std::uint8_t>('z'));
  for (int i = 0; i < 9; ++i) bytes.push_back(0);
  Writer w2;
  w2.order_delete(2, 1);
  for (std::uint8_t b : w2.bytes()) bytes.push_back(b);

  std::vector<char> types;
  const ReadResult r = for_each_framed_message(
      bytes.data(), bytes.size(),
      [&](const std::uint8_t* m, std::size_t) { types.push_back(static_cast<char>(m[0])); });
  REQUIRE(r.ok());
  CHECK(r.messages == 3);
  REQUIRE(types.size() == 3);
  CHECK(types[0] == 'A');
  CHECK(types[1] == 'z');
  CHECK(types[2] == 'D');  // still aligned after the unknown message
}

// The unframed path has no such luxury.
TEST_CASE("raw reader treats an unknown type as fatal") {
  std::vector<std::uint8_t> raw;
  raw.push_back(static_cast<std::uint8_t>('z'));
  for (int i = 0; i < 20; ++i) raw.push_back(0);
  const ReadResult r =
      for_each_raw_message(raw.data(), raw.size(), [](const std::uint8_t*, std::size_t) {});
  CHECK(r.status == ReadStatus::UnknownType);
  CHECK(r.bad_type == 'z');
}

TEST_CASE("raw reader walks an unframed stream using spec lengths") {
  Writer w;
  w.add_order(1, 1, Side::Bid, 100, Symbol("AAPL"), 1000);
  w.order_delete(2, 1);
  // Strip the 2-byte prefixes to build an unframed stream.
  std::vector<std::uint8_t> raw;
  const auto& framed = w.bytes();
  std::size_t off = 0;
  while (off < framed.size()) {
    const std::size_t len = be16(framed.data() + off);
    for (std::size_t i = 0; i < len; ++i) raw.push_back(framed[off + 2 + i]);
    off += 2 + len;
  }
  std::vector<char> types;
  const ReadResult r = for_each_raw_message(
      raw.data(), raw.size(),
      [&](const std::uint8_t* m, std::size_t) { types.push_back(static_cast<char>(m[0])); });
  REQUIRE(r.ok());
  CHECK(r.messages == 2);
  CHECK(types[0] == 'A');
  CHECK(types[1] == 'D');
}

TEST_CASE("empty input is a clean no-op") {
  const ReadResult r = for_each_framed_message(nullptr, 0, [](const std::uint8_t*, std::size_t) {});
  CHECK(r.ok());
  CHECK(r.messages == 0);
}
