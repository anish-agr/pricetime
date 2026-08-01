#pragma once

#include <cstdint>

namespace pricetime {

// FNV-1a 64-bit. Values are decomposed into bytes little-endian-first before
// mixing, so a given input sequence hashes identically on every platform and
// compiler. Used for book-state fingerprints (replay determinism), not
// anything adversarial.
struct Fnv1a64 {
  static constexpr std::uint64_t kOffset = 14695981039346656037ull;
  static constexpr std::uint64_t kPrime = 1099511628211ull;

  std::uint64_t value = kOffset;

  constexpr void mix_byte(std::uint8_t b) noexcept {
    value ^= b;
    value *= kPrime;
  }

  constexpr void mix(std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
      mix_byte(static_cast<std::uint8_t>(v >> (8 * i)));
    }
  }
};

}  // namespace pricetime
