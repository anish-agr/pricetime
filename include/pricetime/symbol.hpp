#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace pricetime {

// A NASDAQ stock symbol: exactly 8 bytes on the wire, space-padded on the
// right (e.g. "AAPL    "). Kept as a fixed 8-byte value rather than a string
// so it fits in a register, compares in one 64-bit operation, and never
// allocates — symbol comparison happens once per message on a feed that
// carries tens of millions of them per day.
struct Symbol {
  std::array<char, 8> data{};

  Symbol() { data.fill(' '); }

  // Builds from a human-readable ticker, padding or truncating to 8 bytes.
  explicit Symbol(std::string_view s) {
    data.fill(' ');
    const std::size_t n = s.size() < 8 ? s.size() : 8;
    std::memcpy(data.data(), s.data(), n);
  }

  // Builds from an 8-byte on-wire field.
  static Symbol from_wire(const char* p) {
    Symbol s;
    std::memcpy(s.data.data(), p, 8);
    return s;
  }

  [[nodiscard]] std::uint64_t bits() const noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, data.data(), 8);
    return v;
  }

  // Ticker without the trailing padding.
  [[nodiscard]] std::string str() const {
    std::size_t n = data.size();
    while (n > 0 && data[n - 1] == ' ') --n;
    return std::string(data.data(), n);
  }

  friend bool operator==(const Symbol& a, const Symbol& b) noexcept {
    return a.data == b.data;
  }
  friend bool operator!=(const Symbol& a, const Symbol& b) noexcept { return !(a == b); }
  friend bool operator<(const Symbol& a, const Symbol& b) noexcept { return a.data < b.data; }
};

struct SymbolHash {
  [[nodiscard]] std::size_t operator()(const Symbol& s) const noexcept {
    // splitmix64 finalizer over the 8 raw bytes.
    std::uint64_t z = s.bits() + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return static_cast<std::size_t>(z ^ (z >> 31));
  }
};

}  // namespace pricetime
