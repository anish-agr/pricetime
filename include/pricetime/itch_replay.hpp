#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

#include "pricetime/itch.hpp"
#include "pricetime/multi_book.hpp"
#include "pricetime/symbol.hpp"

namespace pricetime::itch {

struct ReplayStats {
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;
  std::uint64_t adds = 0;
  std::uint64_t executions = 0;
  std::uint64_t cancels = 0;
  std::uint64_t deletes = 0;
  std::uint64_t replaces = 0;
  std::uint64_t trades = 0;         // 'P' non-cross trades (never enter the book)
  std::uint64_t skipped_symbol = 0; // adds filtered out by the symbol filter
  std::uint64_t unknown_reference = 0;
  std::uint64_t clamped = 0;        // feed asked to remove more than was resting
  std::uint64_t shares_added = 0;
  std::uint64_t shares_executed = 0;
  std::uint64_t last_timestamp = 0;
  char session_state = ' ';
};

// Drives a MultiBook from a decoded ITCH stream.
//
// The important semantics, which are easy to get wrong:
//
//  - Adds are inserted PASSIVELY. The exchange already ran its matching
//    engine; these messages describe the book that resulted. Feeding them to
//    add_limit() would re-match orders that were never meant to cross and the
//    reconstruction would diverge on the first busy symbol.
//  - Execute / cancel / delete / replace messages carry only an order
//    reference, with no symbol. The order-to-book index in MultiBook is what
//    makes them routable.
//  - An unknown reference is normal, not an error, whenever a symbol filter
//    is active: the file is full of orders belonging to symbols we chose not
//    to track. It is counted, not warned about.
//  - 'P' (non-cross trade) messages report trades of orders that were never
//    displayed, so they must not touch the book at all. Applying them is a
//    classic double-count bug.
template <class Ladder, class IdMap = OpenAddressIdMap>
class Replayer {
 public:
  using Books = MultiBook<Ladder, IdMap>;

  explicit Replayer(Books& books) : books_(books) {}

  // Restricts reconstruction to these symbols. Empty means track everything.
  void track_only(const std::vector<Symbol>& symbols) {
    filter_.clear();
    for (const Symbol& s : symbols) filter_.insert(s.bits());
  }

  [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }

  // Applies one complete message. `len` is the message length from
  // message_length(), already validated by the caller.
  void apply(const std::uint8_t* msg, std::size_t len) {
    ++stats_.messages;
    stats_.bytes += len;
    const char type = static_cast<char>(msg[0]);
    switch (static_cast<MsgType>(type)) {
      case MsgType::AddOrder:
      case MsgType::AddOrderMpid: on_add(decode_add_order(msg)); break;
      case MsgType::OrderExecuted:
      case MsgType::OrderExecutedWithPrice: on_executed(decode_order_executed(msg)); break;
      case MsgType::OrderCancel: on_cancel(decode_order_cancel(msg)); break;
      case MsgType::OrderDelete: on_delete(decode_order_delete(msg)); break;
      case MsgType::OrderReplace: on_replace(decode_order_replace(msg)); break;
      case MsgType::TradeNonCross:
        // Reported for completeness of the tape; never affects the book.
        ++stats_.trades;
        stats_.last_timestamp = be48(msg + 5);
        break;
      case MsgType::SystemEvent: {
        const SystemEvent ev = decode_system_event(msg);
        stats_.session_state = ev.code;
        stats_.last_timestamp = ev.timestamp;
        break;
      }
      default: break;  // administrative messages carry no book state
    }
  }

 private:
  [[nodiscard]] bool tracked(const Symbol& s) const {
    return filter_.empty() || filter_.count(s.bits()) != 0;
  }

  void on_add(const AddOrder& m) {
    stats_.last_timestamp = m.timestamp;
    if (!tracked(m.symbol)) {
      ++stats_.skipped_symbol;
      return;
    }
    auto& book = books_.book(m.symbol);
    if (book.insert_passive(m.reference, m.side, m.price, m.shares) == Result::Ok) {
      books_.note_order(m.reference, book);
      ++stats_.adds;
      stats_.shares_added += m.shares;
    }
  }

  void on_executed(const OrderExecuted& m) {
    stats_.last_timestamp = m.timestamp;
    auto* book = books_.book_for_order(m.reference);
    if (book == nullptr) {
      ++stats_.unknown_reference;
      return;
    }
    Qty taken = 0;
    book->execute_resting(m.reference, m.shares, &taken);
    if (taken < m.shares) ++stats_.clamped;
    ++stats_.executions;
    stats_.shares_executed += taken;
    if (book->find_order(m.reference) == nullptr) books_.forget_order(m.reference);
  }

  void on_cancel(const OrderCancel& m) {
    stats_.last_timestamp = m.timestamp;
    auto* book = books_.book_for_order(m.reference);
    if (book == nullptr) {
      ++stats_.unknown_reference;
      return;
    }
    Qty taken = 0;
    book->reduce_resting(m.reference, m.shares, &taken);
    if (taken < m.shares) ++stats_.clamped;
    ++stats_.cancels;
    if (book->find_order(m.reference) == nullptr) books_.forget_order(m.reference);
  }

  void on_delete(const OrderDelete& m) {
    stats_.last_timestamp = m.timestamp;
    auto* book = books_.book_for_order(m.reference);
    if (book == nullptr) {
      ++stats_.unknown_reference;
      return;
    }
    book->cancel(m.reference);
    books_.forget_order(m.reference);
    ++stats_.deletes;
  }

  // ITCH replace does not repeat the side, so it has to be read off the
  // original order before that order is destroyed.
  void on_replace(const OrderReplace& m) {
    stats_.last_timestamp = m.timestamp;
    auto* book = books_.book_for_order(m.original_reference);
    if (book == nullptr) {
      ++stats_.unknown_reference;
      return;
    }
    const Order* original = book->find_order(m.original_reference);
    if (original == nullptr) {
      ++stats_.unknown_reference;
      return;
    }
    const Side side = original->side;
    book->cancel(m.original_reference);
    books_.forget_order(m.original_reference);
    if (book->insert_passive(m.new_reference, side, m.price, m.shares) == Result::Ok) {
      books_.note_order(m.new_reference, *book);
      stats_.shares_added += m.shares;
    }
    ++stats_.replaces;
  }

  Books& books_;
  std::unordered_set<std::uint64_t> filter_;
  ReplayStats stats_;
};

}  // namespace pricetime::itch
