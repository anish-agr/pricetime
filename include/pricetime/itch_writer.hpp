#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pricetime/symbol.hpp"
#include "pricetime/types.hpp"

namespace pricetime::itch {

// Builds well-formed ITCH 5.0 BinaryFILE bytes in memory.
//
// Written as an encoder independent of the decoder — big-endian assembled by
// hand, from the spec rather than from itch.hpp — so agreement between the
// two is evidence about the specification rather than two copies of one
// misunderstanding.
//
// It earns its place in the library rather than the tests for three reasons:
// it makes the whole pipeline testable without a multi-gigabyte download, it
// can construct situations a captured day may not contain (replace chains,
// executions that empty a level, over-consumption), and the engine's market-data
// feed needs to emit ITCH-shaped events anyway.
class Writer {
 public:
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
  [[nodiscard]] std::size_t message_count() const noexcept { return count_; }

  // Drops the buffered bytes but keeps the allocation, so a long-lived writer
  // that encodes one message at a time (the market-data thread) never
  // reallocates after warmup.
  void clear() noexcept {
    buf_.clear();
    count_ = 0;
  }

  void system_event(std::uint64_t ts, char code) {
    begin('S', 12);
    put48(ts);
    put8(static_cast<std::uint8_t>(code));
    end();
  }

  void stock_directory(std::uint64_t ts, const Symbol& sym, char category = 'Q') {
    begin('R', 39);
    put48(ts);
    put_symbol(sym);
    put8(static_cast<std::uint8_t>(category));
    pad(39);
    end();
  }

  void add_order(std::uint64_t ts, OrderId ref, Side side, Qty shares, const Symbol& sym,
                 Price price) {
    begin('A', 36);
    put48(ts);
    put64(ref);
    put8(static_cast<std::uint8_t>(side == Side::Bid ? 'B' : 'S'));
    put32(shares);
    put_symbol(sym);
    put32(static_cast<std::uint32_t>(price));
    end();
  }

  // 'F' is an add carrying a market-participant id; the first 36 bytes are
  // byte-identical to 'A', which is why the decoder handles both.
  void add_order_mpid(std::uint64_t ts, OrderId ref, Side side, Qty shares, const Symbol& sym,
                      Price price, const std::string& mpid = "NSDQ") {
    begin('F', 40);
    put48(ts);
    put64(ref);
    put8(static_cast<std::uint8_t>(side == Side::Bid ? 'B' : 'S'));
    put32(shares);
    put_symbol(sym);
    put32(static_cast<std::uint32_t>(price));
    for (int i = 0; i < 4; ++i) {
      put8(static_cast<std::uint8_t>(i < static_cast<int>(mpid.size()) ? mpid[static_cast<std::size_t>(i)] : ' '));
    }
    end();
  }

  void order_executed(std::uint64_t ts, OrderId ref, Qty shares, std::uint64_t match) {
    begin('E', 31);
    put48(ts);
    put64(ref);
    put32(shares);
    put64(match);
    end();
  }

  void order_executed_with_price(std::uint64_t ts, OrderId ref, Qty shares, std::uint64_t match,
                                 bool printable, Price price) {
    begin('C', 36);
    put48(ts);
    put64(ref);
    put32(shares);
    put64(match);
    put8(static_cast<std::uint8_t>(printable ? 'Y' : 'N'));
    put32(static_cast<std::uint32_t>(price));
    end();
  }

  void order_cancel(std::uint64_t ts, OrderId ref, Qty shares) {
    begin('X', 23);
    put48(ts);
    put64(ref);
    put32(shares);
    end();
  }

  void order_delete(std::uint64_t ts, OrderId ref) {
    begin('D', 19);
    put48(ts);
    put64(ref);
    end();
  }

  void order_replace(std::uint64_t ts, OrderId orig, OrderId fresh, Qty shares, Price price) {
    begin('U', 35);
    put48(ts);
    put64(orig);
    put64(fresh);
    put32(shares);
    put32(static_cast<std::uint32_t>(price));
    end();
  }

  // A trade of non-displayed liquidity: appears on the tape, must never touch
  // the book.
  void trade_non_cross(std::uint64_t ts, OrderId ref, Side side, Qty shares, const Symbol& sym,
                       Price price, std::uint64_t match) {
    begin('P', 44);
    put48(ts);
    put64(ref);
    put8(static_cast<std::uint8_t>(side == Side::Bid ? 'B' : 'S'));
    put32(shares);
    put_symbol(sym);
    put32(static_cast<std::uint32_t>(price));
    put64(match);
    end();
  }

  // An administrative message the book logic ignores; included so tests cover
  // the "skip what you do not model" path.
  void stock_trading_action(std::uint64_t ts, const Symbol& sym, char state) {
    begin('H', 25);
    put48(ts);
    put_symbol(sym);
    put8(static_cast<std::uint8_t>(state));
    pad(25);
    end();
  }

 private:
  void begin(char type, std::size_t len) {
    msg_start_ = buf_.size();
    msg_len_ = len;
    buf_.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>(len & 0xFF));
    body_start_ = buf_.size();
    put8(static_cast<std::uint8_t>(type));
    put16(0);  // stock locate
    put16(0);  // tracking number
  }

  void end() {
    pad(msg_len_);
    ++count_;
    (void)msg_start_;
  }

  // Zero-fills whatever trailing fields this writer does not populate, so the
  // message is exactly its spec length.
  void pad(std::size_t to_len) {
    while (buf_.size() - body_start_ < to_len) buf_.push_back(0);
  }

  void put8(std::uint8_t v) { buf_.push_back(v); }

  void put16(std::uint16_t v) {
    buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
  }

  void put32(std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      buf_.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
  }

  void put48(std::uint64_t v) {
    for (int shift = 40; shift >= 0; shift -= 8) {
      buf_.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
  }

  void put64(std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      buf_.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFF));
    }
  }

  void put_symbol(const Symbol& s) {
    for (char c : s.data) buf_.push_back(static_cast<std::uint8_t>(c));
  }

  std::vector<std::uint8_t> buf_;
  std::size_t count_ = 0;
  std::size_t msg_start_ = 0;
  std::size_t body_start_ = 0;
  std::size_t msg_len_ = 0;
};

}  // namespace pricetime::itch
