#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pricetime/book.hpp"
#include "pricetime/types.hpp"

namespace pricetime::wire {

// pricetime's order-entry protocol: what a client speaks to the matching
// engine over TCP.
//
// Notes on the format:
//
//  - Fixed-size messages, 40 bytes in both directions. A stream protocol
//    needs framing; a fixed size makes framing free (read exactly 40 bytes)
//    and the decode branchless. The handful of bytes a variable-length
//    encoding would save is noise next to the syscall that carries it.
//  - Little-endian, unlike ITCH. This protocol only ever runs
//    between our own processes on little-endian hosts, so byte order should
//    cost nothing; ITCH is big-endian because it says so, and it is decoded,
//    not imitated, at this boundary. Encoding is still explicit byte
//    assembly rather than a struct memcpy, so the format is defined by this
//    file and not by whatever a compiler chose to pad.
//  - The last two bytes of every message are a magic tag. TCP guarantees
//    order, not alignment with our reads after a bug: if a desync ever
//    happens, the magic turns "plausible garbage forever" into an error at
//    the first misread message.
//  - Every request carries a client timestamp that is echoed verbatim in
//    every response it generates. Wire-to-wire latency then needs no clock
//    agreement beyond "client reads the same invariant TSC twice".
//
// Layout, identical for both directions:
//   [0]      kind
//   [1]      aux      (request: side; response: Result code)
//   [2..5]   qty
//   [6..13]  price
//   [14..21] id
//   [22..29] peer id  (request: replace's new id; response: counterparty)
//   [30..37] client timestamp
//   [38..39] magic "pt"

inline constexpr std::size_t kMessageSize = 40;
inline constexpr std::uint8_t kMagic0 = 'p';
inline constexpr std::uint8_t kMagic1 = 't';

// --- little-endian field helpers -----------------------------------------

inline void put_le32(std::uint8_t* p, std::uint32_t v) noexcept {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

inline void put_le64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

[[nodiscard]] inline std::uint32_t get_le32(const std::uint8_t* p) noexcept {
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

[[nodiscard]] inline std::uint64_t get_le64(const std::uint8_t* p) noexcept {
  std::uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
  return v;
}

// --- requests (client -> engine) ------------------------------------------

enum class ReqKind : std::uint8_t {
  Enter = 'E',    // new limit order
  Cancel = 'X',   // cancel remaining quantity
  Replace = 'U',  // cancel + re-enter with new id (loses time priority)
  Reduce = 'Q',   // OUCH-style partial reduce: KEEPS time priority
};

struct Request {
  ReqKind kind = ReqKind::Enter;
  Side side = Side::Bid;
  Qty qty = 0;
  Price price = 0;
  OrderId id = 0;
  OrderId new_id = 0;  // Replace only
  std::uint64_t client_ts = 0;
};

inline void encode(const Request& r, std::uint8_t* out) noexcept {
  out[0] = static_cast<std::uint8_t>(r.kind);
  out[1] = static_cast<std::uint8_t>(r.side);
  put_le32(out + 2, r.qty);
  put_le64(out + 6, static_cast<std::uint64_t>(r.price));
  put_le64(out + 14, r.id);
  put_le64(out + 22, r.new_id);
  put_le64(out + 30, r.client_ts);
  out[38] = kMagic0;
  out[39] = kMagic1;
}

// Returns false when the magic bytes are wrong (stream desynchronized).
[[nodiscard]] inline bool decode(const std::uint8_t* in, Request& r) noexcept {
  if (in[38] != kMagic0 || in[39] != kMagic1) return false;
  r.kind = static_cast<ReqKind>(in[0]);
  r.side = static_cast<Side>(in[1]);
  r.qty = get_le32(in + 2);
  r.price = static_cast<Price>(get_le64(in + 6));
  r.id = get_le64(in + 14);
  r.new_id = get_le64(in + 22);
  r.client_ts = get_le64(in + 30);
  return true;
}

// --- responses (engine -> client) ------------------------------------------

enum class RespKind : std::uint8_t {
  Accepted = 'A',
  Rejected = 'J',
  Executed = 'F',  // one fill; id = your order, peer = counterparty order
  Canceled = 'C',
  Reduced = 'R',   // qty = shares actually removed
};

struct Response {
  RespKind kind = RespKind::Accepted;
  Result code = Result::Ok;
  Qty qty = 0;
  Price price = 0;
  OrderId id = 0;
  OrderId peer = 0;
  std::uint64_t client_ts = 0;
};

inline void encode(const Response& r, std::uint8_t* out) noexcept {
  out[0] = static_cast<std::uint8_t>(r.kind);
  out[1] = static_cast<std::uint8_t>(r.code);
  put_le32(out + 2, r.qty);
  put_le64(out + 6, static_cast<std::uint64_t>(r.price));
  put_le64(out + 14, r.id);
  put_le64(out + 22, r.peer);
  put_le64(out + 30, r.client_ts);
  out[38] = kMagic0;
  out[39] = kMagic1;
}

[[nodiscard]] inline bool decode(const std::uint8_t* in, Response& r) noexcept {
  if (in[38] != kMagic0 || in[39] != kMagic1) return false;
  r.kind = static_cast<RespKind>(in[0]);
  r.code = static_cast<Result>(in[1]);
  r.qty = get_le32(in + 2);
  r.price = static_cast<Price>(get_le64(in + 6));
  r.id = get_le64(in + 14);
  r.peer = get_le64(in + 22);
  r.client_ts = get_le64(in + 30);
  return true;
}

}  // namespace pricetime::wire
