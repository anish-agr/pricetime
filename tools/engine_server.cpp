// Runs the matching engine as a server for one client session.
//
//   engine_server [--port N] [--md-host H --md-port N] [--symbol SYM]
//
// Serves exactly one TCP session, then reports totals and the final book and
// exits. Market data goes out over UDP as real ITCH 5.0 messages when an md
// port is given; point replay tooling at a capture of it and it parses.
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "pricetime/engine.hpp"
#include "pricetime/thread_util.hpp"

using namespace pricetime;

int main(int argc, char** argv) {
  Engine::Config cfg;
  cfg.tcp_port = 9130;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      cfg.tcp_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--md-host") == 0 && i + 1 < argc) {
      cfg.md_host = argv[++i];
    } else if (std::strcmp(argv[i], "--md-port") == 0 && i + 1 < argc) {
      cfg.md_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
      cfg.symbol = Symbol(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--match-core") == 0 && i + 1 < argc) {
      cfg.match_core = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--recv-core") == 0 && i + 1 < argc) {
      cfg.recv_core = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--send-core") == 0 && i + 1 < argc) {
      cfg.send_core = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--spin") == 0) {
      cfg.busy_poll = true;
    }
  }

  raise_priority();
  Engine engine(cfg);
  if (!engine.start()) {
    std::fprintf(stderr, "error: cannot listen on port %u\n", cfg.tcp_port);
    return 2;
  }
  std::printf("engine listening on 127.0.0.1:%u", engine.tcp_port());
  if (cfg.md_port != 0) {
    std::printf(", market data -> %s:%u (ITCH 5.0 over UDP)", cfg.md_host.c_str(), cfg.md_port);
  }
  std::printf("\nserving one session...\n");
  engine.wait_for_session_end();

  const Engine::Stats& s = engine.stats();
  const auto& book = engine.book_after_shutdown();
  std::printf("session over: %" PRIu64 " requests, %" PRIu64 " responses, %" PRIu64
              " executions, %" PRIu64 " md datagrams\n",
              s.requests, s.responses, s.executions, s.md_datagrams);
  std::printf("final book: %zu open orders, traded %" PRIu64 ", state %016" PRIx64 "\n",
              book.open_orders(), book.counters().traded_qty, book.state_hash());
  return 0;
}
