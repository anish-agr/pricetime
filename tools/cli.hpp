#pragma once

// Small helpers shared by the command-line tools. They live here rather than
// in include/pricetime because they are about presenting results, not about
// the book, and nothing in the library should depend on them.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pricetime/symbol.hpp"

namespace pricetime::cli {

// 1234567 -> "1,234,567"
inline std::string commas(std::uint64_t v) {
  std::string s = std::to_string(v);
  for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(s.size()) - 3; i > 0; i -= 3) {
    s.insert(static_cast<std::size_t>(i), ",");
  }
  return s;
}

// "AAPL,MSFT" -> {AAPL, MSFT}. Empty entries are skipped.
inline std::vector<Symbol> parse_symbols(const char* csv) {
  std::vector<Symbol> out;
  std::string cur;
  for (const char* p = csv;; ++p) {
    if (*p == ',' || *p == '\0') {
      if (!cur.empty()) out.push_back(Symbol(cur));
      cur.clear();
      if (*p == '\0') break;
    } else {
      cur.push_back(*p);
    }
  }
  return out;
}

}  // namespace pricetime::cli
