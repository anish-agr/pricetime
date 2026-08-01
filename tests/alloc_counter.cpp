// Replaces the global allocation functions for the test binary so tests can
// assert that a code path performs no heap allocation at all.
//
// Replacing operator new/delete is standard-sanctioned ([basic.stc.dynamic]);
// every form that could route around the counter is defined here, including
// the sized and array variants, so nothing slips past. Counting is a plain
// non-atomic increment: the test binary is single-threaded, and making these
// atomic would perturb the very measurement they exist to take.
#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace {

pricetime::test::AllocStats g_stats;

void* counted_alloc(std::size_t size) {
  ++g_stats.allocations;
  g_stats.bytes += size;
  // malloc(0) may legally return nullptr, which operator new must not do.
  void* p = std::malloc(size == 0 ? 1 : size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}

void counted_free(void* p) noexcept {
  if (p == nullptr) return;
  ++g_stats.deallocations;
  std::free(p);
}

}  // namespace

namespace pricetime::test {

AllocStats alloc_stats() noexcept { return g_stats; }

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
