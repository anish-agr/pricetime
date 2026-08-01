// Latency-bench harness self-test: pins a core, calibrates the TSC against
// steady_clock, and measures the timer's own overhead. Order-book scenarios
// land with the M1 book.
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "affinity.hpp"
#include "histogram.hpp"
#include "timing.hpp"

int main(int argc, char** argv) {
  namespace pb = pricetime::bench;

  int core = 1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--core") == 0 && i + 1 < argc) {
      core = std::atoi(argv[++i]);
    }
  }

  const bool pinned = pb::pin_current_thread(core);
  const double ticks_per_ns = pb::calibrate_ticks_per_ns();

  constexpr int kOverheadSamples = 100001;
  pb::SampleSet overhead(kOverheadSamples);
  for (int i = 0; i < kOverheadSamples; ++i) {
    const std::uint64_t t0 = pb::now_ticks();
    const std::uint64_t t1 = pb::now_ticks();
    overhead.add(t1 - t0);
  }
  const pb::LatencyStats st = overhead.stats();

  std::printf("cpu: %s\n", pb::cpu_brand().c_str());
  std::printf("pinned to core %d: %s\n", core, pinned ? "yes" : "NO (unpinned)");
  std::printf("tsc calibration: %.3f ticks/ns\n", ticks_per_ns);
  std::printf("timer overhead (back-to-back rdtsc): p50 %.0f ticks (%.1f ns), p99 %.0f ticks\n",
              st.p50, st.p50 / ticks_per_ns, st.p99);
  return 0;
}
