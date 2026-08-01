#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define PRICETIME_X86 1
#elif defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <x86intrin.h>
#define PRICETIME_X86 1
#else
#define PRICETIME_X86 0
#endif

namespace pricetime::bench {

// Timestamp in TSC ticks. On anything modern the TSC is invariant: it counts
// at a fixed reference rate regardless of turbo or idle states, so tick deltas
// convert to wall time with one calibration. Falls back to steady_clock
// nanoseconds on non-x86.
inline std::uint64_t now_ticks() noexcept {
#if PRICETIME_X86
  return __rdtsc();
#else
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
#endif
}

// Ticks per nanosecond, measured against steady_clock over `millis` ms.
inline double calibrate_ticks_per_ns(unsigned millis = 200) {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  const std::uint64_t c0 = now_ticks();
  const auto deadline = t0 + std::chrono::milliseconds(millis);
  while (clock::now() < deadline) {
  }
  const std::uint64_t c1 = now_ticks();
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - t0).count();
  return ns > 0 ? static_cast<double>(c1 - c0) / static_cast<double>(ns) : 1.0;
}

inline std::string cpu_brand() {
#if PRICETIME_X86
  char brand[49] = {};
#if defined(_MSC_VER)
  int regs[4] = {0, 0, 0, 0};
  __cpuid(regs, static_cast<int>(0x80000000u));
  if (static_cast<std::uint32_t>(regs[0]) < 0x80000004u) return "unknown x86";
  for (int i = 0; i < 3; ++i) {
    __cpuid(regs, static_cast<int>(0x80000002u + static_cast<unsigned>(i)));
    std::memcpy(brand + 16 * i, regs, 16);
  }
#else
  unsigned int regs[4] = {0, 0, 0, 0};
  if (__get_cpuid_max(0x80000000u, nullptr) < 0x80000004u) return "unknown x86";
  for (int i = 0; i < 3; ++i) {
    __get_cpuid(0x80000002u + static_cast<unsigned>(i), &regs[0], &regs[1], &regs[2], &regs[3]);
    std::memcpy(brand + 16 * i, regs, 16);
  }
#endif
  return brand;
#else
  return "unknown";
#endif
}

}  // namespace pricetime::bench
