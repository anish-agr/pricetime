#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pricetime/itch.hpp"

// MoldUDP64: the framing NASDAQ actually multicasts ITCH inside.
//
// A UDP datagram carries:
//   session   10 bytes, alphanumeric, space-padded
//   sequence   8 bytes, big-endian — sequence number of the FIRST message
//   count      2 bytes, big-endian — messages in this packet
// followed by `count` blocks of [2-byte big-endian length][message].
//
// The sequence number is the entire point. UDP loses packets silently; with
// every message numbered, a receiver knows exactly what it missed and how
// much, which is what makes a re-request channel possible at all. A feed
// without sequencing does not tell you it is lying to you.
//
// Two special counts, per spec: 0x0000 is a heartbeat (sequence = next
// expected, lets receivers detect gaps during silence), 0xFFFF announces
// end of session.

namespace pricetime::mold {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint16_t kHeartbeat = 0x0000;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;

struct Header {
  char session[11] = {};  // NUL-terminated copy of the 10-byte field
  std::uint64_t sequence = 0;
  std::uint16_t count = 0;
};

// Builds one outgoing packet. Reused across packets: begin(), add() until
// full, then the bytes are sent and begin() starts the next one.
class Packer {
 public:
  explicit Packer(const char* session10 = "PRICETIME ") {
    std::memset(session_, ' ', sizeof(session_));
    const std::size_t n = std::strlen(session10);
    std::memcpy(session_, session10, n < 10 ? n : 10);
  }

  // Starts a packet whose first message will carry `first_sequence`.
  void begin(std::uint64_t first_sequence) {
    len_ = kHeaderSize;
    count_ = 0;
    first_seq_ = first_sequence;
  }

  // Appends one message; false if it would not fit (send, then begin anew).
  [[nodiscard]] bool add(const std::uint8_t* msg, std::size_t msg_len) {
    if (len_ + 2 + msg_len > sizeof(buf_)) return false;
    buf_[len_] = static_cast<std::uint8_t>((msg_len >> 8) & 0xFF);
    buf_[len_ + 1] = static_cast<std::uint8_t>(msg_len & 0xFF);
    std::memcpy(buf_ + len_ + 2, msg, msg_len);
    len_ += 2 + msg_len;
    ++count_;
    return true;
  }

  // Finalizes the header; the packet is then bytes()[0 .. size()).
  void seal() { write_header(count_); }

  void seal_heartbeat(std::uint64_t next_sequence) {
    len_ = kHeaderSize;
    first_seq_ = next_sequence;
    write_header(kHeartbeat);
  }

  void seal_end_of_session(std::uint64_t next_sequence) {
    len_ = kHeaderSize;
    first_seq_ = next_sequence;
    write_header(kEndOfSession);
  }

  [[nodiscard]] const std::uint8_t* bytes() const noexcept { return buf_; }
  [[nodiscard]] std::size_t size() const noexcept { return len_; }
  [[nodiscard]] std::uint16_t count() const noexcept { return count_; }

 private:
  void write_header(std::uint16_t count) {
    std::memcpy(buf_, session_, 10);
    for (int i = 0; i < 8; ++i) {
      buf_[10 + i] = static_cast<std::uint8_t>(first_seq_ >> (8 * (7 - i)));
    }
    buf_[18] = static_cast<std::uint8_t>((count >> 8) & 0xFF);
    buf_[19] = static_cast<std::uint8_t>(count & 0xFF);
  }

  char session_[10];
  std::uint8_t buf_[1400];  // stays under a common 1500-byte MTU
  std::size_t len_ = kHeaderSize;
  std::uint16_t count_ = 0;
  std::uint64_t first_seq_ = 0;
};

// Parses one received datagram. The handler is called per message as
// h(sequence, msg, len). Returns false on a malformed packet.
template <class Handler>
[[nodiscard]] bool unpack(const std::uint8_t* pkt, std::size_t pkt_len, Header& hdr,
                          Handler&& h) {
  if (pkt_len < kHeaderSize) return false;
  std::memcpy(hdr.session, pkt, 10);
  hdr.session[10] = '\0';
  hdr.sequence = 0;
  for (int i = 0; i < 8; ++i) hdr.sequence = (hdr.sequence << 8) | pkt[10 + i];
  hdr.count = static_cast<std::uint16_t>((pkt[18] << 8) | pkt[19]);
  if (hdr.count == kHeartbeat || hdr.count == kEndOfSession) return pkt_len == kHeaderSize;

  std::size_t off = kHeaderSize;
  for (std::uint16_t i = 0; i < hdr.count; ++i) {
    if (pkt_len - off < 2) return false;
    const std::size_t len = itch::be16(pkt + off);
    off += 2;
    if (len == 0 || pkt_len - off < len) return false;
    h(hdr.sequence + i, pkt + off, len);
    off += len;
  }
  return off == pkt_len;
}

// Tracks the receive sequence and counts what UDP silently dropped. This is
// bookkeeping a real receiver pairs with a re-request channel; here it turns
// silent loss into a number that tests and stats can assert on.
class GapTracker {
 public:
  // Returns the number of messages missed immediately before this packet.
  std::uint64_t on_packet(const Header& hdr) {
    std::uint64_t missed = 0;
    if (started_ && hdr.sequence > next_) missed = hdr.sequence - next_;
    // A packet from the past (duplicate or reorder) does not move us back.
    if (!started_ || hdr.sequence >= next_) {
      started_ = true;
      if (hdr.count != kHeartbeat && hdr.count != kEndOfSession) {
        next_ = hdr.sequence + hdr.count;
      } else {
        next_ = hdr.sequence > next_ ? hdr.sequence : next_;
      }
    }
    total_missed_ += missed;
    return missed;
  }

  [[nodiscard]] std::uint64_t next_expected() const noexcept { return next_; }
  [[nodiscard]] std::uint64_t total_missed() const noexcept { return total_missed_; }

 private:
  bool started_ = false;
  std::uint64_t next_ = 1;
  std::uint64_t total_missed_ = 0;
};

}  // namespace pricetime::mold
