// Replaces the global allocation functions for the test binary so tests can
// assert that a code path performs no heap allocation at all.
//
// Replacing operator new/delete is standard-sanctioned ([basic.stc.dynamic]);
// every form that could route around the counter is defined here, including
// the sized and array variants, so nothing slips past.
//
// The counters are atomic with relaxed ordering. An earlier version used
// plain increments, justified by the test binary being single-threaded --
// which stopped being true the moment the SPSC queue tests started spawning
// threads, and ThreadSanitizer caught the resulting race in counted_free.
// Relaxed is sufficient: nothing here synchronizes anything, the counters are
// only ever compared against each other, and the windows being measured are
// single-threaded so there is no contention to perturb the measurement.
#include "alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {

std::atomic<std::size_t> g_allocations{0};
std::atomic<std::size_t> g_deallocations{0};
std::atomic<std::size_t> g_bytes{0};

void* counted_alloc(std::size_t size) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(size, std::memory_order_relaxed);
  // malloc(0) may legally return nullptr, which operator new must not do.
  void* p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void counted_free(void* p) noexcept {
  if (p == nullptr) return;
  g_deallocations.fetch_add(1, std::memory_order_relaxed);
  std::free(p);
}

}  // namespace

namespace pricetime::test {

AllocStats alloc_stats() noexcept {
  AllocStats s;
  s.allocations = g_allocations.load(std::memory_order_relaxed);
  s.deallocations = g_deallocations.load(std::memory_order_relaxed);
  s.bytes = g_bytes.load(std::memory_order_relaxed);
  return s;
}

}  // namespace pricetime::test

void* operator new(std::size_t size) { return counted_alloc(size); }
void* operator new[](std::size_t size) { return counted_alloc(size); }

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return counted_alloc(size);
  } catch (...) {
    return nullptr;
  }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return counted_alloc(size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* p) noexcept { counted_free(p); }
void operator delete[](void* p) noexcept { counted_free(p); }
void operator delete(void* p, std::size_t) noexcept { counted_free(p); }
void operator delete[](void* p, std::size_t) noexcept { counted_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { counted_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { counted_free(p); }
