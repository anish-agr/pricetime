#pragma once

// The body of the ITCH fuzz target, in a header so two callers share it: the
// libFuzzer driver in fuzz_itch.cpp, and a test that replays the committed
// corpus on every platform and compiler in CI.
//
// That split matters. A fuzzer only runs where clang and libFuzzer are
// available, but the inputs it has already found are regression tests
// everywhere, and those are the ones that would otherwise rot.
//
// What is being fuzzed, in priority order:
//
//  1. Framing and decoding. This is the actual attack surface: a decoder
//     reads fixed byte offsets out of a length-prefixed buffer, and every
//     offset is a chance to read past the end. The framed reader is supposed
//     to guarantee that a message handed to a decoder is both fully present
//     and of the spec's length for its type; this is what checks that claim
//     against arbitrary bytes rather than against well-formed files.
//  2. The unframed reader, which has a harder job. With no length prefix the
//     type table is the only source of length, so an unknown type is fatal
//     rather than skippable. That is the path MoldUDP64 payloads take.
//  3. The book's structural invariants, for small inputs only. Books are
//     expensive to create (each reserves a large id map) and a fuzzer feeding
//     random symbol bytes would otherwise allocate one per message until it
//     runs out of memory, which is a finding about the harness rather than
//     about the code.
//
// Note what stage 3 does NOT assert. Feed reconstruction rests orders
// passively at whatever price the message carries, so arbitrary bytes can
// absolutely produce a crossed book, and that is correct behaviour rather
// than a bug. Only the structural properties below hold for every possible
// input, so only those are checked.

#include <cstddef>
#include <cstdint>

#include "pricetime/itch.hpp"
#include "pricetime/itch_reader.hpp"
#include "pricetime/itch_replay.hpp"
#include "pricetime/ladder_map.hpp"
#include "pricetime/multi_book.hpp"

namespace pricetime::fuzz {

// Above this many bytes the reconstruction stage is skipped; see note 3.
inline constexpr std::size_t kReplayByteLimit = 1024;

// Decodes one already-framed, already-length-checked message every way its
// type allows. Touching every decoded field is what makes a bad offset show
// up as a sanitizer report rather than as a value nobody reads.
inline std::uint64_t decode_message(const std::uint8_t* msg, std::size_t len) {
  using namespace pricetime::itch;
  std::uint64_t sink = 0;
  switch (static_cast<MsgType>(msg[0])) {
    case MsgType::AddOrder:
    case MsgType::AddOrderMpid: {
      const AddOrder m = decode_add_order(msg);
      sink ^= m.reference ^ m.timestamp ^ m.shares ^ static_cast<std::uint64_t>(m.price) ^
              m.symbol.bits() ^ static_cast<std::uint64_t>(m.side);
      break;
    }
    case MsgType::OrderExecuted:
    case MsgType::OrderExecutedWithPrice: {
      const OrderExecuted m = decode_order_executed(msg);
      sink ^= m.reference ^ m.timestamp ^ m.shares ^ m.match_number;
      break;
    }
    case MsgType::OrderCancel: {
      const OrderCancel m = decode_order_cancel(msg);
      sink ^= m.reference ^ m.timestamp ^ m.shares;
      break;
    }
    case MsgType::OrderDelete: {
      const OrderDelete m = decode_order_delete(msg);
      sink ^= m.reference ^ m.timestamp;
      break;
    }
    case MsgType::OrderReplace: {
      const OrderReplace m = decode_order_replace(msg);
      sink ^= m.original_reference ^ m.new_reference ^ m.timestamp ^ m.shares ^
              static_cast<std::uint64_t>(m.price);
      break;
    }
    case MsgType::SystemEvent: {
      const SystemEvent m = decode_system_event(msg);
      sink ^= m.timestamp ^ static_cast<std::uint64_t>(m.code);
      break;
    }
    default:
      // Unknown to this build but length-consistent: the reader is allowed to
      // hand it over, and nothing may read into it.
      sink ^= len;
      break;
  }
  return sink;
}

// Structural properties that hold for any input at all, well formed or not.
// A book rebuilt from garbage may be crossed, empty, or absurd; it may not be
// internally inconsistent.
template <class BookType>
[[nodiscard]] bool book_structurally_sound(const BookType& book) {
  std::size_t counted_orders = 0;
  bool ok = true;
  for (const Side s : {Side::Bid, Side::Ask}) {
    bool first = true;
    Price previous = 0;
    book.for_each_level(s, [&](const Level& lvl) {
      // Levels arrive best to worst, and an emptied level must have been
      // removed from the ladder rather than left behind.
      if (!first) {
        const bool ordered = s == Side::Bid ? lvl.price < previous : lvl.price > previous;
        if (!ordered) ok = false;
      }
      first = false;
      previous = lvl.price;
      if (lvl.order_count == 0 || lvl.head == nullptr || lvl.tail == nullptr) {
        ok = false;
        return;
      }
      if (lvl.head->prev != nullptr || lvl.tail->next != nullptr) ok = false;

      std::uint64_t sum = 0;
      std::uint32_t n = 0;
      const Order* prev = nullptr;
      for (const Order* o = lvl.head; o != nullptr; o = o->next) {
        if (o->prev != prev) ok = false;          // links agree in both directions
        if (o->level != &lvl) ok = false;         // back-pointer names this level
        if (o->qty == 0) ok = false;              // a spent order must be gone
        if (o->price != lvl.price) ok = false;    // and be at its level's price
        sum += o->qty;
        ++n;
        prev = o;
        if (n > 100000) break;                    // cycle guard
      }
      if (prev != lvl.tail) ok = false;
      if (n != lvl.order_count) ok = false;
      if (sum != lvl.total_qty) ok = false;
      counted_orders += n;
    });
  }
  // The id map and the ladder must agree on how many orders exist.
  if (counted_orders != book.open_orders()) ok = false;
  return ok;
}

// Runs every stage. `sound` is cleared if a structural invariant broke.
// The return value is derived from the input purely so no stage can be
// optimized away.
inline std::uint64_t fuzz_one(const std::uint8_t* data, std::size_t size, bool* sound) {
  using namespace pricetime::itch;
  std::uint64_t sink = 0;
  if (sound != nullptr) *sound = true;

  // Stage 1: framed reading, the BinaryFILE layout.
  const ReadResult framed = for_each_framed_message(
      data, size, [&](const std::uint8_t* msg, std::size_t len) {
        sink ^= decode_message(msg, len);
      });
  sink ^= framed.messages ^ framed.offset ^ static_cast<std::uint64_t>(framed.status);

  // Stage 2: unframed reading, the MoldUDP64 payload layout.
  const ReadResult raw = for_each_raw_message(
      data, size, [&](const std::uint8_t* msg, std::size_t len) {
        sink ^= decode_message(msg, len);
      });
  sink ^= raw.messages ^ raw.offset ^ static_cast<std::uint64_t>(raw.status);

  // Stage 3: bounded book reconstruction.
  if (size <= kReplayByteLimit) {
    MultiBook<MapLadder> books;
    Replayer<MapLadder> rep(books);
    for_each_framed_message(data, size, [&](const std::uint8_t* msg, std::size_t len) {
      rep.apply(msg, len);
    });
    books.for_each_book([&](const Symbol&, const auto& book) {
      if (!book_structurally_sound(book) && sound != nullptr) *sound = false;
      sink ^= book.state_hash();
    });
  }
  return sink;
}

}  // namespace pricetime::fuzz
