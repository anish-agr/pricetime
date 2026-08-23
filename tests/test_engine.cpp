#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "pricetime/engine.hpp"
#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_replay.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/moldudp64.hpp"
#include "pricetime/multi_book.hpp"
#include "pricetime/net.hpp"
#include "pricetime/wire.hpp"

using namespace pricetime;

TEST_CASE("wire protocol: request round-trips through encode and decode") {
  wire::Request in;
  in.kind = wire::ReqKind::Replace;
  in.side = Side::Ask;
  in.qty = 12345;
  in.price = -987654321;  // negative survives: the field is a signed tick count
  in.id = 0xDEADBEEFCAFEull;
  in.new_id = 0xFEEDFACE0001ull;
  in.client_ts = 0x1122334455667788ull;

  std::uint8_t buf[wire::kMessageSize];
  wire::encode(in, buf);
  CHECK(buf[38] == wire::kMagic0);
  CHECK(buf[39] == wire::kMagic1);

  wire::Request out;
  REQUIRE(wire::decode(buf, out));
  CHECK(out.kind == in.kind);
  CHECK(out.side == in.side);
  CHECK(out.qty == in.qty);
  CHECK(out.price == in.price);
  CHECK(out.id == in.id);
  CHECK(out.new_id == in.new_id);
  CHECK(out.client_ts == in.client_ts);
}

TEST_CASE("wire protocol: response round-trips and bad magic is rejected") {
  wire::Response in;
  in.kind = wire::RespKind::Executed;
  in.code = Result::Ok;
  in.qty = 77;
  in.price = 1000100;
  in.id = 42;
  in.peer = 43;
  in.client_ts = 999;

  std::uint8_t buf[wire::kMessageSize];
  wire::encode(in, buf);
  wire::Response out;
  REQUIRE(wire::decode(buf, out));
  CHECK(out.kind == in.kind);
  CHECK(out.qty == 77);
  CHECK(out.peer == 43);

  buf[39] ^= 0xFF;  // corrupt the magic: a desynchronized stream must not parse
  CHECK_FALSE(wire::decode(buf, out));
}

namespace {

// A tiny synchronous client: send one request, then read responses.
class TestClient {
 public:
  [[nodiscard]] bool connect(std::uint16_t port) { return conn_.connect("127.0.0.1", port); }

  [[nodiscard]] bool send(const wire::Request& r) {
    std::uint8_t buf[wire::kMessageSize];
    wire::encode(r, buf);
    return conn_.send_all(buf, sizeof(buf));
  }

  [[nodiscard]] bool recv(wire::Response& r) {
    std::uint8_t buf[wire::kMessageSize];
    if (!conn_.recv_all(buf, sizeof(buf))) return false;
    return wire::decode(buf, r);
  }

  void disconnect() { conn_.close(); }

 private:
  net::NetInit init_;
  net::TcpStream conn_;
};

wire::Request enter(OrderId id, Side side, Price price, Qty qty, std::uint64_t ts = 7) {
  wire::Request r;
  r.kind = wire::ReqKind::Enter;
  r.id = id;
  r.side = side;
  r.price = price;
  r.qty = qty;
  r.client_ts = ts;
  return r;
}

}  // namespace

TEST_CASE("engine loopback: full session over real sockets") {
  net::NetInit net_init;
  REQUIRE(net_init.ok());

  // Market data lands on a real UDP socket bound first so its port is known.
  net::UdpReceiver md_rx;
  REQUIRE(md_rx.bind(0));

  Engine::Config cfg;
  cfg.md_port = md_rx.port();
  Engine engine(cfg);
  REQUIRE(engine.start());
  REQUIRE(engine.tcp_port() != 0);

  TestClient client;
  REQUIRE(client.connect(engine.tcp_port()));

  wire::Response resp;

  // Passive ask rests.
  REQUIRE(client.send(enter(1, Side::Ask, 1000, 100, 111)));
  REQUIRE(client.recv(resp));
  CHECK(resp.kind == wire::RespKind::Accepted);
  CHECK(resp.id == 1);
  CHECK(resp.client_ts == 111);  // echoed verbatim: the latency measurement channel

  // Crossing bid: ack, then both sides of the fill, then nothing rests.
  REQUIRE(client.send(enter(2, Side::Bid, 1000, 40, 222)));
  REQUIRE(client.recv(resp));
  CHECK(resp.kind == wire::RespKind::Accepted);
  CHECK(resp.client_ts == 222);
  REQUIRE(client.recv(resp));
  CHECK(resp.kind == wire::RespKind::Executed);
  CHECK(resp.id == 2);    // aggressor's copy
  CHECK(resp.peer == 1);
  CHECK(resp.qty == 40);
  CHECK(resp.price == 1000);  // filled at the resting price
  REQUIRE(client.recv(resp));
  CHECK(resp.kind == wire::RespKind::Executed);
  CHECK(resp.id == 1);    // resting order's copy
  CHECK(resp.peer == 2);

  // Reduce keeps priority: cut the ask's remainder from 60 to 35.
  {
    wire::Request r;
    r.kind = wire::ReqKind::Reduce;
    r.id = 1;
    r.qty = 25;
    r.client_ts = 333;
    REQUIRE(client.send(r));
    REQUIRE(client.recv(resp));
    CHECK(resp.kind == wire::RespKind::Reduced);
    CHECK(resp.qty == 25);  // shares actually removed
  }

  // Replace onto the other side of the spread: fully fills against nothing —
  // rests, then gets canceled.
  {
    wire::Request r;
    r.kind = wire::ReqKind::Replace;
    r.id = 1;
    r.new_id = 10;
    r.price = 1001;
    r.qty = 35;
    r.client_ts = 444;
    REQUIRE(client.send(r));
    REQUIRE(client.recv(resp));
    CHECK(resp.kind == wire::RespKind::Accepted);
    CHECK(resp.id == 10);
  }
  {
    wire::Request r;
    r.kind = wire::ReqKind::Cancel;
    r.id = 10;
    r.client_ts = 555;
    REQUIRE(client.send(r));
    REQUIRE(client.recv(resp));
    CHECK(resp.kind == wire::RespKind::Canceled);
  }

  // Unknown cancel is rejected, with the Result code on the wire.
  {
    wire::Request r;
    r.kind = wire::ReqKind::Cancel;
    r.id = 999;
    r.client_ts = 666;
    REQUIRE(client.send(r));
    REQUIRE(client.recv(resp));
    CHECK(resp.kind == wire::RespKind::Rejected);
    CHECK(resp.code == Result::RejectedUnknownId);
  }

  client.disconnect();
  engine.wait_for_session_end();

  CHECK(engine.stats().requests == 6);
  CHECK(engine.stats().executions == 1);
  CHECK(engine.book_after_shutdown().open_orders() == 0);
  CHECK(engine.book_after_shutdown().counters().traded_qty == 40);

  // The feed said everything the book did, so a book rebuilt purely from the
  // MoldUDP64 stream must fingerprint identically to the engine's own — and
  // the sequence numbers must account for every message with no gaps.
  MultiBook<MapLadder> md_books;
  itch::Replayer<MapLadder> md_rep(md_books);
  mold::GapTracker gaps;
  bool saw_end = false;
  std::uint8_t dgram[1500];
  for (;;) {
    const int n = md_rx.recv(dgram, sizeof(dgram), 300);
    if (n <= 0) break;
    mold::Header hdr;
    REQUIRE(mold::unpack(dgram, static_cast<std::size_t>(n), hdr,
                         [&](std::uint64_t, const std::uint8_t* m, std::size_t len) {
                           md_rep.apply(m, len);
                         }));
    gaps.on_packet(hdr);
    if (hdr.count == mold::kEndOfSession) {
      saw_end = true;
      break;
    }
  }
  CHECK(engine.stats().md_datagrams > 0);
  CHECK(gaps.total_missed() == 0);
  CHECK(saw_end);
  const auto* md_book = md_books.find(Symbol("TEST"));
  REQUIRE(md_book != nullptr);
  CHECK(md_book->state_hash() == engine.book_after_shutdown().state_hash());
  CHECK(md_book->open_orders() == 0);
}

// Volume through the whole pipeline: many orders, alternating resting and
// crossing, with the feed-integrity check at the end. This is the test that
// runs under ThreadSanitizer: four engine threads, two queues of real
// traffic, and real sockets.
TEST_CASE("engine loopback: sustained two-sided flow keeps engine and feed in agreement") {
  net::NetInit net_init;
  REQUIRE(net_init.ok());
  net::UdpReceiver md_rx;
  REQUIRE(md_rx.bind(0));

  Engine::Config cfg;
  cfg.md_port = md_rx.port();
  Engine engine(cfg);
  REQUIRE(engine.start());

  TestClient client;
  REQUIRE(client.connect(engine.tcp_port()));

  constexpr int kOrders = 600;
  std::uint64_t seed = 0xE45E;
  auto rng = [&seed]() {
    seed += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = seed;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };

  // Send everything pipelined, reading responses on the fly to avoid
  // deadlocking on full TCP buffers (the client is both producer and sink).
  std::uint64_t responses = 0;
  OrderId next = 1;
  for (int i = 0; i < kOrders; ++i) {
    const Side side = rng() % 2 == 0 ? Side::Bid : Side::Ask;
    // Overlapping bands, so a healthy fraction crosses and executes.
    const Price price = side == Side::Bid ? static_cast<Price>(995 + rng() % 11)
                                          : static_cast<Price>(1000 + rng() % 11);
    REQUIRE(client.send(enter(next++, side, price, static_cast<Qty>(1 + rng() % 50))));
    wire::Response resp;
    REQUIRE(client.recv(resp));  // keeps flow balanced; fills accumulate beyond this
    ++responses;
  }

  // Drain before disconnecting. Crossing orders produce three responses (ack
  // plus both sides of the fill) while the loop above read only one per
  // request, so requests can still be in flight here — and closing a socket
  // with unread inbound data sends RST, which on Windows discards the
  // undelivered stream and cost the engine the tail of the session (observed
  // as 582/600 requests, roughly one run in ten). Responses are strictly
  // ordered through one queue, so a deliberately rejected sentinel read back
  // proves everything before it arrived.
  {
    wire::Request flush;
    flush.kind = wire::ReqKind::Cancel;
    flush.id = 0xF1005;  // never entered, must reject
    REQUIRE(client.send(flush));
    wire::Response resp;
    for (;;) {
      REQUIRE(client.recv(resp));
      ++responses;
      if (resp.kind == wire::RespKind::Rejected && resp.id == 0xF1005) break;
    }
  }
  client.disconnect();
  engine.wait_for_session_end();

  CHECK(engine.stats().requests == kOrders + 1);  // + the flush sentinel
  CHECK(engine.stats().responses == responses);

  // Drain the Mold stream and compare.
  MultiBook<MapLadder> md_books;
  itch::Replayer<MapLadder> md_rep(md_books);
  mold::GapTracker gaps;
  std::uint8_t dgram[1500];
  for (;;) {
    const int n = md_rx.recv(dgram, sizeof(dgram), 300);
    if (n <= 0) break;
    mold::Header hdr;
    REQUIRE(mold::unpack(dgram, static_cast<std::size_t>(n), hdr,
                         [&](std::uint64_t, const std::uint8_t* m, std::size_t len) {
                           md_rep.apply(m, len);
                         }));
    gaps.on_packet(hdr);
    if (hdr.count == mold::kEndOfSession) break;
  }
  CHECK(gaps.total_missed() == 0);
  const auto* md_book = md_books.find(Symbol("TEST"));
  REQUIRE(md_book != nullptr);
  CHECK(md_book->state_hash() == engine.book_after_shutdown().state_hash());
  CHECK(md_book->open_orders() == engine.book_after_shutdown().open_orders());
  CHECK(engine.book_after_shutdown().counters().traded_qty > 0);  // flow really crossed
}
