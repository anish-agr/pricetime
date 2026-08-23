#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/id_map.hpp"
#include "pricetime/itch_writer.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/moldudp64.hpp"
#include "pricetime/net.hpp"
#include "pricetime/spsc_queue.hpp"
#include "pricetime/symbol.hpp"
#include "pricetime/thread_util.hpp"
#include "pricetime/wire.hpp"

namespace pricetime {

// The matching engine as a server. Single symbol, single client session —
// the threading structure is the point of this milestone, not multi-tenancy.
//
//   TCP in ──► recv thread ──SPSC──► match thread ──SPSC──► send thread ──► TCP out
//                                        │
//                                        └─────SPSC──► md thread ──► UDP (ITCH)
//
// Why this shape:
//
//  - The matching thread owns the book EXCLUSIVELY. No lock ever guards the
//    book, because no other thread touches it; the SPSC queues are the only
//    synchronization in the process. This is the standard exchange
//    architecture in miniature, and the reason M1 could stay single-threaded
//    without that being a dead end.
//  - Receive and send are separate threads so a slow reader (the client not
//    draining responses) exerts backpressure through the response queue
//    without ever stalling message intake or matching.
//  - Market data goes out as ITCH 5.0 messages, each in its own framed UDP
//    datagram. Not ITCH-"style": the actual encoding, produced by the same
//    Writer the tests use. Anything that can read a NASDAQ file can read
//    this feed — including our own replayer, which is how the feed-integrity
//    test works: a book rebuilt purely from the UDP stream must hash
//    identically to the engine's own.
//
// Shutdown is a cascade rather than a flag check in a hot loop: closing the
// sockets unblocks the recv thread, whose exit drains into the match thread,
// and so on down. Each stage finishes processing everything already queued
// before exiting, so no accepted order is ever silently dropped.
class Engine {
 public:
  struct Config {
    std::uint16_t tcp_port = 0;  // 0 = ephemeral, read back via tcp_port()
    std::string md_host = "127.0.0.1";
    std::uint16_t md_port = 0;   // 0 = market data disabled
    std::size_t queue_capacity = 8192;
    Symbol symbol{"TEST"};
    // Explicit thread placement, -1 = let the scheduler decide. On a hybrid
    // CPU an unpinned matching thread drifts onto E-cores and the wire-to-
    // wire p50 shows it immediately; pinning match to a P-core is the single
    // most effective latency knob this config has.
    int match_core = -1;
    int recv_core = -1;
    int send_core = -1;
    int md_core = -1;
    // Busy-poll the client socket instead of blocking in the kernel. Costs a
    // core; removes the scheduler-wakeup latency from every message.
    bool busy_poll = false;
  };

  struct Stats {
    std::uint64_t requests = 0;
    std::uint64_t responses = 0;
    std::uint64_t executions = 0;
    std::uint64_t md_datagrams = 0;
    std::uint64_t desyncs = 0;
  };

  explicit Engine(Config cfg)
      : cfg_(cfg), q_in_(cfg.queue_capacity), q_out_(cfg.queue_capacity),
        q_md_(cfg.queue_capacity) {}

  ~Engine() { stop(); }

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  [[nodiscard]] bool start() {
    if (!listener_.listen(cfg_.tcp_port)) return false;
    if (cfg_.md_port != 0 && !md_out_.open(cfg_.md_host, cfg_.md_port)) return false;
    epoch_ = std::chrono::steady_clock::now();
    recv_thread_ = std::thread([this] { recv_loop(); });
    match_thread_ = std::thread([this] { match_loop(); });
    send_thread_ = std::thread([this] { send_loop(); });
    md_thread_ = std::thread([this] { md_loop(); });
    return true;
  }

  void stop() {
    stopping_.store(true, std::memory_order_release);
    listener_.interrupt();
    {
      // The recv thread assigns conn_ once, when accept() returns; this lock
      // is only ever contended in that instant, but without it stop() racing
      // that assignment is a genuine data race on the socket handle.
      std::lock_guard<std::mutex> lock(conn_mu_);
      conn_.interrupt();
    }
    join(recv_thread_);
    join(match_thread_);
    join(send_thread_);
    join(md_thread_);
  }

  // Blocks until the client disconnects and every queued stage has drained.
  void wait_for_session_end() {
    join(recv_thread_);
    join(match_thread_);
    join(send_thread_);
    join(md_thread_);
  }

  [[nodiscard]] std::uint16_t tcp_port() const noexcept { return listener_.port(); }
  [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

  // The book is only safe to inspect after the match thread has exited
  // (wait_for_session_end / stop); the type system cannot express that, so
  // the name does.
  [[nodiscard]] const OrderBook<MapLadder, OpenAddressIdMap>& book_after_shutdown() const {
    return book_;
  }

 private:
  void join(std::thread& t) {
    if (t.joinable()) t.join();
  }

  [[nodiscard]] std::uint64_t now_ns() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - epoch_)
                                          .count());
  }

  template <class T>
  void push_spin(SpscQueue<T>& q, const T& v) {
    while (!q.try_push(v)) std::this_thread::yield();
  }

  // --- stage 1: TCP in -> request queue ---------------------------------

  void recv_loop() {
    if (cfg_.recv_core >= 0) {
      pin_current_thread(cfg_.recv_core);
      raise_priority();
    }
    {
      net::TcpStream accepted = listener_.accept();
      std::lock_guard<std::mutex> lock(conn_mu_);
      conn_ = std::move(accepted);
    }
    if (!conn_.valid()) {
      recv_done_.store(true, std::memory_order_release);
      return;
    }
    if (cfg_.busy_poll) conn_.set_busy_poll();
    // Buffered intake: one recv() may deliver many pipelined messages, and
    // paying one syscall per 40-byte message caps the whole engine at the
    // syscall rate — the first throughput run measured exactly that. A burst
    // costs one syscall for up to 256 messages; a lone message still arrives
    // with single-message latency because recv returns whatever is there.
    std::uint8_t buf[wire::kMessageSize * 256];
    std::size_t have = 0;
    bool alive = true;
    while (alive && !stopping_.load(std::memory_order_acquire)) {
      const int n = conn_.recv_some(buf + have, sizeof(buf) - have);
      if (n <= 0) break;  // disconnect, shutdown, or error
      have += static_cast<std::size_t>(n);
      std::size_t off = 0;
      while (have - off >= wire::kMessageSize) {
        wire::Request req;
        if (!wire::decode(buf + off, req)) {
          // Desynchronized stream: nothing after this point can be trusted,
          // and resynchronizing a fixed-size protocol means guessing. Drop
          // the session instead.
          ++stats_.desyncs;
          alive = false;
          break;
        }
        ++stats_.requests;
        push_spin(q_in_, req);
        off += wire::kMessageSize;
      }
      if (off > 0 && off < have) {
        std::memmove(buf, buf + off, have - off);
      }
      have -= off;
    }
    recv_done_.store(true, std::memory_order_release);
  }

  // --- stage 2: match ----------------------------------------------------

  void match_loop() {
    if (cfg_.match_core >= 0) {
      pin_current_thread(cfg_.match_core);
      raise_priority();
    }
    book_.reserve_orders(1u << 20);
    wire::Request req;
    for (;;) {
      if (!q_in_.try_pop(req)) {
        if (recv_done_.load(std::memory_order_acquire) && !q_in_.try_pop(req)) break;
        std::this_thread::yield();
        continue;
      }
      handle(req);
    }
    match_done_.store(true, std::memory_order_release);
  }

  void handle(const wire::Request& req) {
    const std::uint64_t ts = now_ns();
    execs_.clear();
    Result r = Result::Ok;
    switch (req.kind) {
      case wire::ReqKind::Enter:
        r = book_.add_limit(req.id, req.side, req.price, req.qty,
                            [this](const Execution& e) { execs_.push_back(e); });
        respond(r == Result::Ok ? wire::RespKind::Accepted : wire::RespKind::Rejected, r, req,
                req.id, req.qty, req.price);
        break;
      case wire::ReqKind::Cancel:
        r = book_.cancel(req.id);
        respond(r == Result::Ok ? wire::RespKind::Canceled : wire::RespKind::Rejected, r, req,
                req.id, 0, 0);
        if (r == Result::Ok) md_delete(ts, req.id);
        break;
      case wire::ReqKind::Reduce: {
        Qty removed = 0;
        r = book_.reduce_resting(req.id, req.qty, &removed);
        respond(r == Result::Ok ? wire::RespKind::Reduced : wire::RespKind::Rejected, r, req,
                req.id, removed, 0);
        if (r == Result::Ok && removed > 0) md_cancel(ts, req.id, removed);
        break;
      }
      case wire::ReqKind::Replace:
        r = book_.replace(req.id, req.new_id, req.price, req.qty,
                          [this](const Execution& e) { execs_.push_back(e); });
        respond(r == Result::Ok ? wire::RespKind::Accepted : wire::RespKind::Rejected, r, req,
                req.new_id, req.qty, req.price);
        break;
    }

    // Fills after the ack, one response per side of each execution: the
    // aggressor's owner and the resting order's owner each learn about their
    // own order. With a single session both land on the same socket, but the
    // protocol is written for the day they do not.
    Qty filled = 0;
    for (const Execution& e : execs_) {
      ++stats_.executions;
      filled += e.qty;
      wire::Response fill;
      fill.kind = wire::RespKind::Executed;
      fill.code = Result::Ok;
      fill.qty = e.qty;
      fill.price = e.price;
      fill.client_ts = req.client_ts;
      fill.id = e.aggressor_id;
      fill.peer = e.resting_id;
      push_response(fill);
      fill.id = e.resting_id;
      fill.peer = e.aggressor_id;
      push_response(fill);
      md_execute(ts, e);
    }

    // Market data shows only DISPLAYED quantity, exactly as NASDAQ's feed
    // does: the executed portion of an aggressive order never appears on the
    // add/replace message — it already appeared as executions against the
    // resting side. Emitting the full entered quantity here is the bug that
    // makes a feed-reconstructed book silently diverge from the engine's,
    // which is precisely what the feed-integrity test checks.
    if (r == Result::Ok && req.kind == wire::ReqKind::Enter) {
      const Qty remainder = req.qty - filled;
      if (remainder > 0) {
        md_.clear();
        md_.add_order(ts, req.id, req.side, remainder, cfg_.symbol, req.price);
        md_push(md_);
      }
    } else if (r == Result::Ok && req.kind == wire::ReqKind::Replace) {
      const Qty remainder = req.qty - filled;
      if (remainder > 0) {
        md_.clear();
        md_.order_replace(ts, req.id, req.new_id, remainder, req.price);
        md_push(md_);
      } else {
        // Fully executed on arrival: the new order never displays, so the
        // feed shows the old one simply leaving the book.
        md_delete(ts, req.id);
      }
    }
    execs_.clear();
  }

  void respond(wire::RespKind kind, Result code, const wire::Request& req, OrderId id, Qty qty,
               Price price) {
    wire::Response resp;
    resp.kind = kind;
    resp.code = code;
    resp.qty = qty;
    resp.price = price;
    resp.id = id;
    resp.client_ts = req.client_ts;
    push_response(resp);
  }

  void push_response(const wire::Response& r) {
    ++stats_.responses;
    push_spin(q_out_, r);
  }

  // --- market-data emission ---------------------------------------------
  // The md queue carries pre-encoded framed ITCH messages (small, fixed-cap
  // byte arrays) so the md thread does no book-related work at all.

  struct MdMsg {
    std::uint8_t bytes[64] = {};
    std::uint8_t len = 0;
  };

  void md_push(const itch::Writer& w) {
    if (cfg_.md_port == 0) return;
    MdMsg m;
    const auto& b = w.bytes();
    if (b.size() > sizeof(m.bytes)) return;  // cannot happen with these types
    for (std::size_t i = 0; i < b.size(); ++i) m.bytes[i] = b[i];
    m.len = static_cast<std::uint8_t>(b.size());
    push_spin(q_md_, m);
  }

  void md_execute(std::uint64_t ts, const Execution& e) {
    md_.clear();
    md_.order_executed(ts, e.resting_id, e.qty, ++match_number_);
    md_push(md_);
  }

  void md_delete(std::uint64_t ts, OrderId id) {
    md_.clear();
    md_.order_delete(ts, id);
    md_push(md_);
  }

  void md_cancel(std::uint64_t ts, OrderId id, Qty shares) {
    md_.clear();
    md_.order_cancel(ts, id, shares);
    md_push(md_);
  }

  // --- stage 3: response queue -> TCP out --------------------------------

  void send_loop() {
    if (cfg_.send_core >= 0) {
      pin_current_thread(cfg_.send_core);
      raise_priority();
    }
    // Coalesce everything already queued into one send. Under a pipelined
    // load this turns hundreds of syscalls into one; under ping-pong load
    // the queue never holds more than one response, so a lone ack still
    // goes out immediately and pays no batching delay. This is the
    // latency/throughput lever every gateway has, resolved in the only way
    // that costs the latency path nothing.
    wire::Response resp;
    std::uint8_t buf[wire::kMessageSize * 256];
    for (;;) {
      if (!q_out_.try_pop(resp)) {
        if (match_done_.load(std::memory_order_acquire) && !q_out_.try_pop(resp)) break;
        std::this_thread::yield();
        continue;
      }
      std::size_t k = 0;
      for (;;) {
        wire::encode(resp, buf + k);
        k += wire::kMessageSize;
        if (k >= sizeof(buf) || !q_out_.try_pop(resp)) break;
      }
      if (!conn_.send_all(buf, k)) {
        // Client gone: keep draining the queue so the match thread is never
        // blocked on a dead session.
        while (q_out_.try_pop(resp)) {
        }
      }
    }
    send_done_.store(true, std::memory_order_release);
  }

  // --- stage 4: md queue -> MoldUDP64 over UDP ---------------------------

  void md_loop() {
    if (cfg_.md_core >= 0) {
      pin_current_thread(cfg_.md_core);
      raise_priority();
    }
    mold::Packer packer;
    std::uint64_t seq = 1;
    MdMsg m;
    for (;;) {
      if (!q_md_.try_pop(m)) {
        if (match_done_.load(std::memory_order_acquire) && !q_md_.try_pop(m)) break;
        std::this_thread::yield();
        continue;
      }
      // One message per packet: latency over packing density. A throughput-
      // oriented feed would batch messages already queued, the same trade
      // the TCP send thread makes; the sequence numbers make either policy
      // safe for receivers.
      packer.begin(seq);
      // The md queue carries framed messages (2-byte length + payload) as
      // the ITCH writer produces them; Mold blocks are framed the same way,
      // so the payload goes in without the writer's own prefix.
      const std::size_t framed_len = static_cast<std::size_t>(m.len);
      if (framed_len >= 2) {
        const bool ok = packer.add(m.bytes + 2, framed_len - 2);
        if (ok) {
          packer.seal();
          seq += packer.count();
          if (md_out_.send(packer.bytes(), packer.size())) ++stats_.md_datagrams;
        }
      }
    }
    packer.seal_end_of_session(seq);
    if (cfg_.md_port != 0 && md_out_.send(packer.bytes(), packer.size())) {
      ++stats_.md_datagrams;
    }
  }

  Config cfg_;
  net::NetInit net_init_;
  net::TcpListener listener_;
  net::TcpStream conn_;
  std::mutex conn_mu_;
  net::UdpSender md_out_;

  SpscQueue<wire::Request> q_in_;
  SpscQueue<wire::Response> q_out_;
  SpscQueue<MdMsg> q_md_;

  OrderBook<MapLadder, OpenAddressIdMap> book_;
  std::vector<Execution> execs_;
  itch::Writer md_;
  std::uint64_t match_number_ = 0;
  std::chrono::steady_clock::time_point epoch_{};

  std::thread recv_thread_;
  std::thread match_thread_;
  std::thread send_thread_;
  std::thread md_thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> recv_done_{false};
  std::atomic<bool> match_done_{false};
  std::atomic<bool> send_done_{false};

  Stats stats_;
};

}  // namespace pricetime
