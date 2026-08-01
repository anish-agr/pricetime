#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace pricetime::bench {
// Pin the calling thread to one logical CPU so samples come from a single
// core's TSC and the scheduler stops migrating us mid-measurement.
inline bool pin_current_thread(int cpu) {
  if (cpu < 0 || cpu >= 64) return false;
  return SetThreadAffinityMask(GetCurrentThread(), 1ull << cpu) != 0;
}
}  // namespace pricetime::bench

#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>

namespace pricetime::bench {
inline bool pin_current_thread(int cpu) {
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}
}  // namespace pricetime::bench

#else
namespace pricetime::bench {
inline bool pin_current_thread(int) { return false; }
}  // namespace pricetime::bench
#endif
