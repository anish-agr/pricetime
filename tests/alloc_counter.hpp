#pragma once

#include <cstddef>

namespace pricetime::test {

// Process-wide allocation counters, fed by the replaced global operator new /
// delete in alloc_counter.cpp.
//
// Why bother: latency numbers from a shared CI runner are noise, so most
// projects simply cannot regression-test performance. Allocation counts are
// different — they are deterministic, machine-independent, and a heap call on
// the matching hot path is exactly the kind of regression that silently
// destroys tail latency. This turns "the hot path does not allocate" from a
// comment into an assertion CI can enforce.
struct AllocStats {
  std::size_t allocations = 0;
  std::size_t deallocations = 0;
  std::size_t bytes = 0;
};

// Snapshot of the counters. Reading is relaxed: the guards below are only
// ever used from a single thread, and the counters exist to be compared
// against each other rather than to synchronize anything.

AllocStats alloc_stats() noexcept;

// RAII window: measures allocations that happen during its lifetime.
class AllocGuard {
 public:
  AllocGuard() : start_(alloc_stats()) {}

  [[nodiscard]] std::size_t allocations() const noexcept {
    return alloc_stats().allocations - start_.allocations;
  }

  [[nodiscard]] std::size_t bytes() const noexcept {
    return alloc_stats().bytes - start_.bytes;
  }

 private:
  AllocStats start_;
};

}  // namespace pricetime::test
