#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "pricetime/book.hpp"
#include "pricetime/symbol.hpp"

namespace pricetime {

// A set of per-symbol books sharing one global order-id space.
//
// The id space is global on purpose: NASDAQ ITCH order reference numbers are
// unique across the whole feed, not per instrument, and — more importantly —
// the messages that matter most (execute, cancel, delete) carry ONLY the
// order reference. They do not repeat the symbol. So a replay engine must be
// able to go from a bare id to the right book, which means one shared index
// from id to its owning book.
//
// Books are heap-allocated individually and never moved, so a BookRef handed
// out stays valid as symbols are added.
template <class Ladder, class IdMap = OpenAddressIdMap>
class MultiBook {
 public:
  using BookType = OrderBook<Ladder, IdMap>;

  // LadderArgs are forwarded to every per-symbol ladder (e.g. the price
  // bounds a DenseLadder needs). MapLadder takes none.
  template <class... LadderArgs>
  explicit MultiBook(LadderArgs... ladder_args)
      : make_book_([ladder_args...]() { return std::make_unique<BookType>(ladder_args...); }) {}

  // Returns the book for a symbol, creating it on first sight.
  BookType& book(const Symbol& s) {
    const auto it = books_.find(s);
    if (it != books_.end()) return *it->second;
    auto inserted = books_.emplace(s, make_book_());
    return *inserted.first->second;
  }

  // Returns nullptr if the symbol has never been seen.
  [[nodiscard]] const BookType* find(const Symbol& s) const {
    const auto it = books_.find(s);
    return it == books_.end() ? nullptr : it->second.get();
  }

  [[nodiscard]] std::size_t symbol_count() const noexcept { return books_.size(); }

  [[nodiscard]] std::vector<Symbol> symbols() const {
    std::vector<Symbol> out;
    out.reserve(books_.size());
    for (const auto& kv : books_) out.push_back(kv.first);
    return out;
  }

  // Routes an id-only operation (execute / cancel / delete / replace) to
  // whichever book owns that order. Returns nullptr when the id is unknown —
  // which on a real feed is normal, not an error: a full-day file references
  // orders for symbols the replay may have filtered out.
  BookType* book_for_order(OrderId id) {
    const auto it = owner_.find(id);
    return it == owner_.end() ? nullptr : it->second;
  }

  // Records that `id` now lives in `symbol`'s book. Called by the replay
  // driver after a successful add.
  void note_order(OrderId id, BookType& b) { owner_[id] = &b; }

  void forget_order(OrderId id) { owner_.erase(id); }

  [[nodiscard]] std::size_t tracked_orders() const noexcept { return owner_.size(); }

  template <class F>
  void for_each_book(F&& f) const {
    for (const auto& kv : books_) f(kv.first, *kv.second);
  }

 private:
  std::unordered_map<Symbol, std::unique_ptr<BookType>, SymbolHash> books_;
  std::unordered_map<OrderId, BookType*> owner_;
  std::function<std::unique_ptr<BookType>()> make_book_;
};

}  // namespace pricetime
