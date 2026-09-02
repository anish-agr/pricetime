#pragma once

#include <cstdint>

#include "pricetime/book.hpp"
#include "pricetime/types.hpp"

namespace pricetime {

// A single book operation, decoupled from wherever it came from.
//
// This is the seam that makes the ITCH pipeline tractable: an ITCH 5.0 parser becomes just
// another producer of Ops, and the replay driver, the property tests, and the
// benchmark all consume the same type. It also means a failing random stream
// can be serialized, shrunk, and replayed verbatim.
enum class OpType : std::uint8_t {
  AddLimit = 0,
  AddIoc = 1,
  AddFok = 2,
  AddMarket = 3,
  Cancel = 4,
  Replace = 5,
};

struct Op {
  OpType type = OpType::AddLimit;
  Side side = Side::Bid;
  OrderId id = 0;      // order acted on; for Replace, the order being replaced
  OrderId new_id = 0;  // Replace only
  Price price = 0;
  Qty qty = 0;
};

template <class Book, class OnExec>
Result apply(Book& book, const Op& op, OnExec&& on_exec) {
  switch (op.type) {
    case OpType::AddLimit:
      return book.add_limit(op.id, op.side, op.price, op.qty, on_exec);
    case OpType::AddIoc:
      return book.add_ioc(op.id, op.side, op.price, op.qty, on_exec);
    case OpType::AddFok:
      return book.add_fok(op.id, op.side, op.price, op.qty, on_exec);
    case OpType::AddMarket:
      return book.add_market(op.id, op.side, op.qty, on_exec);
    case OpType::Cancel:
      return book.cancel(op.id);
    case OpType::Replace:
      return book.replace(op.id, op.new_id, op.price, op.qty, on_exec);
  }
  return Result::RejectedBadId;  // unreachable for well-formed Ops
}

template <class Book>
Result apply(Book& book, const Op& op) {
  return apply(book, op, [](const Execution&) {});
}

}  // namespace pricetime
