#pragma once

#include <cstddef>
#include <cstdint>

#include "pricetime/itch.hpp"

namespace pricetime::itch {

enum class ReadStatus : std::uint8_t {
  Ok = 0,
  TruncatedPrefix,   // fewer than 2 bytes left where a length prefix was due
  TruncatedMessage,  // prefix promised more bytes than the file contains
  UnknownType,       // type byte not in the ITCH 5.0 table
  LengthMismatch,    // framing length disagrees with the spec length
  ZeroLength,
};

struct ReadResult {
  ReadStatus status = ReadStatus::Ok;
  std::size_t messages = 0;
  std::size_t offset = 0;  // byte offset where reading stopped
  char bad_type = ' ';
  std::size_t declared_length = 0;
  std::size_t expected_length = 0;

  [[nodiscard]] bool ok() const noexcept { return status == ReadStatus::Ok; }
};

// Walks a downloaded ITCH BinaryFILE: a bare concatenation of
// [2-byte big-endian length][message], with no file header and no trailer.
//
// The length prefix, not the type table, is what advances the cursor. That
// ordering is deliberate: it means a file containing message types added to
// the spec after this code was written still parses cleanly instead of
// desynchronizing. The type table is used as a cross-check on known types,
// where a disagreement is a genuine signal that the stream is misaligned or
// that the file is a different ITCH version — far better caught loudly here
// than as nonsense prices a million messages later.
//
// The handler is called as h(const std::uint8_t* msg, std::size_t len).
template <class Handler>
ReadResult for_each_framed_message(const std::uint8_t* data, std::size_t size, Handler&& h) {
  ReadResult r;
  std::size_t off = 0;
  while (off < size) {
    if (size - off < 2) {
      r.status = ReadStatus::TruncatedPrefix;
      r.offset = off;
      return r;
    }
    const std::size_t len = be16(data + off);
    if (len == 0) {
      r.status = ReadStatus::ZeroLength;
      r.offset = off;
      return r;
    }
    if (size - off - 2 < len) {
      r.status = ReadStatus::TruncatedMessage;
      r.offset = off;
      r.declared_length = len;
      return r;
    }
    const std::uint8_t* msg = data + off + 2;
    const char type = static_cast<char>(msg[0]);
    const std::size_t expected = message_length(type);
    if (expected != 0 && expected != len) {
      r.status = ReadStatus::LengthMismatch;
      r.offset = off;
      r.bad_type = type;
      r.declared_length = len;
      r.expected_length = expected;
      return r;
    }
    h(msg, len);
    ++r.messages;
    off += 2 + len;
  }
  r.offset = off;
  return r;
}

// Walks an unframed stream, where lengths come only from the type table.
// This is the shape ITCH arrives in over MoldUDP64, whose packet header
// already delimits messages. Here an unknown type IS fatal: without a length
// prefix there is no way to skip it.
template <class Handler>
ReadResult for_each_raw_message(const std::uint8_t* data, std::size_t size, Handler&& h) {
  ReadResult r;
  std::size_t off = 0;
  while (off < size) {
    const char type = static_cast<char>(data[off]);
    const std::size_t len = message_length(type);
    if (len == 0) {
      r.status = ReadStatus::UnknownType;
      r.offset = off;
      r.bad_type = type;
      return r;
    }
    if (size - off < len) {
      r.status = ReadStatus::TruncatedMessage;
      r.offset = off;
      r.declared_length = len;
      return r;
    }
    h(data + off, len);
    ++r.messages;
    off += len;
  }
  r.offset = off;
  return r;
}

}  // namespace pricetime::itch
