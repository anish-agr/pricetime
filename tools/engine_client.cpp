// Order-entry client and wire-to-wire latency measurement.
//
//   engine_client [--host H] [--port N] [--ops N] [--core N] [--mode latency|throughput]
//
// Latency mode is a strict ping-pong: stamp the TSC into the request, wait
// for the ack (which echoes the stamp), read the TSC again. The difference is
// the full round trip: client encode -> kernel -> TCP -> engine recv thread ->
// SPSC -> match -> SPSC -> send thread -> TCP -> kernel -> client decode.
// Client and server read the same invariant TSC, so no clock sync is needed —
// which is also why this measurement is only valid on one machine.
//
// Loopback numbers measure the ENGINE STACK, not a network: two kernel
// crossings per direction and zero wire time. They are directly comparable
// run to run, and are an honest floor for what the engine adds on top of
// whatever network carries it. They are not a claim about any real link.
//
// Throughput mode pipelines: a reader thread drains responses while the main
// thread sends flat out, measuring sustained messages/second through the same
// path with the queues actually filling.
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <string>
#include <thread>

#include "../bench/affinity.hpp"
#include "../bench/histogram.hpp"
#include "../bench/timing.hpp"
#include "pricetime/net.hpp"
#include "pricetime/wire.hpp"

using namespace pricetime;
namespace pb = pricetime::bench;

namespace {

struct Config {
  std::string host = "127.0.0.1";
  std::uint16_t port = 9130;
  std::uint64_t ops = 100000;
  int core = -1;
  bool throughput = false;
  bool spin = false;
};

std::uint64_t splitmix(std::uint64_t& s) {
  s += 0x9E3779B97F4A7C15ull;
  std::uint64_t z = s;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

void print_stats(const char* label, pb::LatencyStats st, double tpn) {
  std::printf("%-18s p50 %7.1f  p90 %7.1f  p99 %7.1f  p99.9 %8.1f  max %9.0f ns\n", label,
              st.p50 / tpn, st.p90 / tpn, st.p99 / tpn, st.p999 / tpn,
              static_cast<double>(st.max) / tpn);
}

int run_latency(net::TcpStream& conn, const Config& cfg, double tpn) {
  std::uint8_t out[wire::kMessageSize];
  std::uint8_t in[wire::kMessageSize];
  pb::SampleSet enter_rtt(cfg.ops);
  pb::SampleSet cancel_rtt(cfg.ops);
  std::uint64_t seed = 0xC11E27;
  OrderId next_id = 1;

  // Warmup outside the sample sets: first packets pay for cold caches, page
  // faults, and the TCP window opening.
  const std::uint64_t warmup = cfg.ops / 10 + 100;

  for (std::uint64_t i = 0; i < cfg.ops + warmup; ++i) {
    // Enter a resting order (never crosses: bids far below asks)...
    wire::Request req;
    req.kind = wire::ReqKind::Enter;
    req.id = next_id++;
    req.side = i % 2 == 0 ? Side::Bid : Side::Ask;
    req.price = req.side == Side::Bid ? static_cast<Price>(900 + splitmix(seed) % 50)
                                      : static_cast<Price>(1100 + splitmix(seed) % 50);
    req.qty = static_cast<Qty>(1 + splitmix(seed) % 100);

    req.client_ts = pb::now_ticks();
    wire::encode(req, out);
    if (!conn.send_all(out, sizeof(out))) return 2;
    if (!conn.recv_all(in, sizeof(in))) return 2;
    const std::uint64_t t_ack = pb::now_ticks();
    wire::Response resp;
    if (!wire::decode(in, resp) || resp.kind != wire::RespKind::Accepted) {
      std::fprintf(stderr, "unexpected response at op %" PRIu64 "\n", i);
      return 2;
    }
    if (i >= warmup) enter_rtt.add(t_ack - resp.client_ts);

    // ...then cancel it, so the book stays flat and every op is comparable.
    wire::Request cxl;
    cxl.kind = wire::ReqKind::Cancel;
    cxl.id = req.id;
    cxl.client_ts = pb::now_ticks();
    wire::encode(cxl, out);
    if (!conn.send_all(out, sizeof(out))) return 2;
    if (!conn.recv_all(in, sizeof(in))) return 2;
    const std::uint64_t t_cxl = pb::now_ticks();
    if (!wire::decode(in, resp) || resp.kind != wire::RespKind::Canceled) return 2;
    if (i >= warmup) cancel_rtt.add(t_cxl - resp.client_ts);
  }

  std::printf("\nwire-to-wire round trip (loopback, %" PRIu64 " samples/op, warmup %" PRIu64
              " discarded)\n",
              cfg.ops, warmup);
  print_stats("enter -> ack", enter_rtt.stats(), tpn);
  print_stats("cancel -> ack", cancel_rtt.stats(), tpn);
  return 0;
}

int run_throughput(net::TcpStream& conn, const Config& cfg) {
  std::atomic<std::uint64_t> received{0};
  std::atomic<bool> reader_stop{false};
  std::thread reader([&] {
    // Buffered read: count whole messages out of arbitrarily sized chunks.
    std::uint8_t in[wire::kMessageSize * 256];
    std::size_t have = 0;
    while (!reader_stop.load(std::memory_order_acquire)) {
      const int n = conn.recv_some(in + have, sizeof(in) - have);
      if (n <= 0) break;
      have += static_cast<std::size_t>(n);
      const std::size_t whole = have / wire::kMessageSize;
      received.fetch_add(whole, std::memory_order_relaxed);
      const std::size_t used = whole * wire::kMessageSize;
      if (used > 0 && used < have) std::memmove(in, in + used, have - used);
      have -= used;
    }
  });

  // Batched sends: 256 messages per syscall. One syscall per message capped
  // the first version of this benchmark at the syscall rate (~30k/s), which
  // is a statement about loopback plumbing, not about the engine.
  constexpr std::size_t kBatch = 256;
  std::uint8_t out[wire::kMessageSize * kBatch];
  std::uint64_t seed = 0x7B0;
  OrderId next_id = 1;
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t sent = 0;
  while (sent < cfg.ops) {
    std::size_t k = 0;
    while (k < sizeof(out) && sent < cfg.ops) {
      wire::Request req;
      req.kind = wire::ReqKind::Enter;
      req.id = next_id++;
      req.side = splitmix(seed) % 2 == 0 ? Side::Bid : Side::Ask;
      // Overlapping bands: a realistic share of orders cross and execute.
      req.price = req.side == Side::Bid ? static_cast<Price>(995 + splitmix(seed) % 11)
                                        : static_cast<Price>(1000 + splitmix(seed) % 11);
      req.qty = static_cast<Qty>(1 + splitmix(seed) % 100);
      req.client_ts = sent;
      wire::encode(req, out + k);
      k += wire::kMessageSize;
      ++sent;
    }
    if (!conn.send_all(out, k)) return 2;
  }
  const double send_secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  // Let the response stream go quiet before declaring the count final.
  std::uint64_t last = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const std::uint64_t now = received.load(std::memory_order_relaxed);
    if (now == last) break;
    last = now;
  }
  const double total_secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  reader_stop.store(true, std::memory_order_release);
  conn.interrupt();
  reader.join();

  std::printf("\nthroughput (pipelined, loopback)\n");
  std::printf("sent      %" PRIu64 " requests in %.2f s  (%.2f M req/s)\n", cfg.ops, send_secs,
              static_cast<double>(cfg.ops) / send_secs / 1e6);
  std::printf("received  %" PRIu64 " responses in %.2f s  (%.2f M resp/s)\n", last, total_secs,
              static_cast<double>(last) / total_secs / 1e6);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      cfg.host = argv[++i];
    } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      cfg.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--ops") == 0 && i + 1 < argc) {
      cfg.ops = std::strtoull(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
      cfg.core = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      cfg.throughput = std::strcmp(argv[++i], "throughput") == 0;
    } else if (std::strcmp(argv[i], "--spin") == 0) {
      cfg.spin = true;
    }
  }

  net::NetInit net_init;
  if (!net_init.ok()) return 2;

  int core = cfg.core;
  if (core == -1) {
    core = pb::pick_performance_core();
  }
  const bool pinned = core >= 0 && pb::pin_current_thread(core);
  pb::raise_priority();
  const double tpn = pb::calibrate_ticks_per_ns();

  net::TcpStream conn;
  if (!conn.connect(cfg.host, cfg.port)) {
    std::fprintf(stderr, "error: cannot connect to %s:%u\n", cfg.host.c_str(), cfg.port);
    return 2;
  }

  std::printf("cpu: %s\n", pb::cpu_brand().c_str());
  std::printf("pinned to core %d: %s\n", core, pinned ? "yes" : "no");
  std::printf("tsc: %.3f ticks/ns\n", tpn);

  return cfg.throughput ? run_throughput(conn, cfg) : run_latency(conn, cfg, tpn);
}
