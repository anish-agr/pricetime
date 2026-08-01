#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace pricetime::bench {

// Pin the calling thread to one logical CPU so samples come from a single
// core's TSC and the scheduler stops migrating us mid-measurement.
inline bool pin_current_thread(int cpu) {
  if (cpu < 0 || cpu >= 64) return false;
  return SetThreadAffinityMask(GetCurrentThread(), 1ull << cpu) != 0;
}

// On hybrid CPUs (P + E cores), benching on an E-core silently reports the
// wrong machine. Pick a logical CPU from the highest EfficiencyClass core
// (higher class = higher performance); among equals prefer the highest index,
// which skips core 0 and its interrupt load. Returns -1 if undeterminable.
inline int pick_performance_core() {
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return -1;
  std::vector<unsigned char> buf(len);
  auto* base = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore, base, &len)) return -1;
  int best_cpu = -1;
  int best_class = -1;
  for (DWORD off = 0; off < len;) {
    const auto* info =
        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
    if (info->Relationship == RelationProcessorCore) {
      const int ec = static_cast<int>(info->Processor.EfficiencyClass);
      const KAFFINITY mask = info->Processor.GroupMask[0].Mask;
      for (int b = 0; b < 64; ++b) {
        if ((mask >> b) & 1u) {
          if (ec >= best_class) {
            best_class = ec;
            best_cpu = b;
          }
          break;  // one representative logical CPU per physical core
        }
      }
    }
    off += info->Size;
  }
  return best_cpu;
}

// Fewer preemptions during sampling. HIGH (not REALTIME: that needs admin and
// can starve the machine).
inline bool raise_priority() {
  const bool a = SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0;
  const bool b = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
  return a && b;
}

}  // namespace pricetime::bench

#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

namespace pricetime::bench {

inline bool pin_current_thread(int cpu) {
  if (cpu < 0) return false;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<unsigned>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

inline int pick_performance_core() { return -1; }  // pass --core explicitly

inline bool raise_priority() {
  return setpriority(PRIO_PROCESS, 0, -20) == 0;  // usually needs privileges
}

}  // namespace pricetime::bench

#else
namespace pricetime::bench {
inline bool pin_current_thread(int) { return false; }
inline int pick_performance_core() { return -1; }
inline bool raise_priority() { return false; }
}  // namespace pricetime::bench
#endif
