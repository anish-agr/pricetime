// libFuzzer driver for the ITCH parser. Build with clang:
//
//   cmake -B build-fuzz -DPRICETIME_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
//   cmake --build build-fuzz
//   ./build-fuzz/fuzz/fuzz_itch fuzz/corpus -max_total_time=60
//
// CI runs exactly that on a time budget. The corpus directory is committed,
// so every input the fuzzer has ever found stays a regression test, replayed
// on every compiler by tests/test_fuzz_corpus.cpp even where libFuzzer is not
// available.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "fuzz_itch.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  bool sound = true;
  const std::uint64_t sink = pricetime::fuzz::fuzz_one(data, size, &sound);
  if (!sound) {
    // A structural invariant broke. Abort so libFuzzer saves the input.
    std::abort();
  }
  // Keep the work observable without affecting coverage. Read back as well as
  // written, since a write-only volatile trips -Wunused-but-set-variable.
  static std::uint64_t volatile escape_sink;
  escape_sink = sink;
  return escape_sink == sink ? 0 : 0;
}
