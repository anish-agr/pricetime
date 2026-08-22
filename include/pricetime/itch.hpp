#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "pricetime/symbol.hpp"
#include "pricetime/types.hpp"

// NASDAQ TotalView-ITCH 5.0 decoding.
//
// Wire-format facts that drive this code:
//
//  - Every integer field is BIG-endian. x86 is little-endian, so every
//    multi-byte field needs a byte swap. This is the most common source of
//    silent corruption when reading ITCH: the wrong endianness still parses,
//    it just produces nonsense prices.
//  - Messages are not self-describing in length. The leading type byte
//    determines the length from a fixed table. A single wrong entry
//    desynchronizes the entire remainder of the file, so an unknown type is
//    treated as fatal rather than skipped.
//  - Prices are 4-byte unsigned integers with four implied decimal places:
//    $12.3400 arrives as 123400. They stay integer ticks all the way through.
//    Converting to double anywhere in this pipeline would be a correctness
//    bug, not merely a performance one.
//  - Timestamps are 6 bytes: nanoseconds since midnight US/Eastern.
//  - Fields are not aligned to their own size within a message, so every read
//    goes through byte assembly rather than a reinterpret_cast, which would
//    be undefined behaviour and genuinely faults on stricter targets.

namespace pricetime::itch {

// --- big-endian field readers --------------------------------------------

[[nodiscard]] inline std::uint16_t be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) |
                                    static_cast<std::uint16_t>(p[1]));
}

[[nodiscard]] inline std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

[[nodiscard]] inline std::uint64_t be48(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(p[0]) << 40) | (static_cast<std::uint64_t>(p[1]) << 32) |
         (static_cast<std::uint64_t>(p[2]) << 24) | (static_cast<std::uint64_t>(p[3]) << 16) |
         (static_cast<std::uint64_t>(p[4]) << 8) | static_cast<std::uint64_t>(p[5]);
}

[[nodiscard]] inline std::uint64_t be64(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(be32(p)) << 32) | static_cast<std::uint64_t>(be32(p + 4));
}

// --- message types --------------------------------------------------------

enum class MsgType : char {
  SystemEvent = 'S',
  StockDirectory = 'R',
  StockTradingAction = 'H',
  RegSho = 'Y',
  MarketParticipantPosition = 'L',
  MwcbDeclineLevel = 'V',
  MwcbStatus = 'W',
  IpoQuotingPeriod = 'K',
  LuldAuctionCollar = 'J',
  OperationalHalt = 'h',
  AddOrder = 'A',
  AddOrderMpid = 'F',
  OrderExecuted = 'E',
  OrderExecutedWithPrice = 'C',
  OrderCancel = 'X',
  OrderDelete = 'D',
  OrderReplace = 'U',
  TradeNonCross = 'P',
  CrossTrade = 'Q',
  BrokenTrade = 'B',
  Noii = 'I',
  Rpii = 'N',
  DirectListingCapitalRaise = 'O',
};

// Message length INCLUDING the leading type byte, per the ITCH 5.0 spec.
// Returns 0 for an unrecognized type; callers must treat that as fatal,
// because guessing a length silently corrupts everything downstream.
[[nodiscard]] inline std::size_t message_length(char type) noexcept {
  switch (static_cast<MsgType>(type)) {
    case MsgType::SystemEvent: return 12;
    case MsgType::StockDirectory: return 39;
    case MsgType::StockTradingAction: return 25;
    case MsgType::RegSho: return 20;
    case MsgType::MarketParticipantPosition: return 26;
    case MsgType::MwcbDeclineLevel: return 35;
    case MsgType::MwcbStatus: return 12;
    case MsgType::IpoQuotingPeriod: return 28;
    case MsgType::LuldAuctionCollar: return 35;
    case MsgType::OperationalHalt: return 21;
    case MsgType::AddOrder: return 36;
    case MsgType::AddOrderMpid: return 40;
    case MsgType::OrderExecuted: return 31;
    case MsgType::OrderExecutedWithPrice: return 36;
    case MsgType::OrderCancel: return 23;
    case MsgType::OrderDelete: return 19;
    case MsgType::OrderReplace: return 35;
    case MsgType::TradeNonCross: return 44;
    case MsgType::CrossTrade: return 40;
    case MsgType::BrokenTrade: return 19;
    case MsgType::Noii: return 50;
    case MsgType::Rpii: return 20;
    case MsgType::DirectListingCapitalRaise: return 48;
  }
  return 0;
}

// --- decoded messages -----------------------------------------------------
// Decoding copies only the scalar fields out; nothing allocates, and the
// caller keeps owning the underlying bytes.

struct AddOrder {
  std::uint64_t timestamp = 0;
  OrderId reference = 0;
  Side side = Side::Bid;
  Qty shares = 0;
  Symbol symbol;
  Price price = 0;
};

struct OrderExecuted {
  std::uint64_t timestamp = 0;
  OrderId reference = 0;
  Qty shares = 0;
  std::uint64_t match_number = 0;
  bool has_price = false;  // set for 'C', which carries its own print price
  bool printable = true;   // 'C' can mark an execution as non-printable
  Price price = 0;
};

struct OrderCancel {
  std::uint64_t timestamp = 0;
  OrderId reference = 0;
  Qty shares = 0;  // partial cancel: this many shares leave the order
};

struct OrderDelete {
  std::uint64_t timestamp = 0;
  OrderId reference = 0;
};

struct OrderReplace {
  std::uint64_t timestamp = 0;
  OrderId original_reference = 0;
  OrderId new_reference = 0;
  Qty shares = 0;
  Price price = 0;
};

struct SystemEvent {
  std::uint64_t timestamp = 0;
  // 'O' start of messages, 'S' start of system hours, 'Q' start of market
  // hours, 'M' end of market hours, 'E' end of system hours, 'C' end of
  // messages.
  char code = ' ';
};

struct StockDirectory {
  std::uint64_t timestamp = 0;
  Symbol symbol;
  char market_category = ' ';
};

// Offsets are from the ITCH 5.0 spec, measured from the type byte.
// 'A': type(1) locate(2) tracking(2) ts(6) ref(8) side(1) shares(4)
//      stock(8) price(4) = 36
[[nodiscard]] inline AddOrder decode_add_order(const std::uint8_t* p) noexcept {
  AddOrder m;
  m.timestamp = be48(p + 5);
  m.reference = be64(p + 11);
  m.side = p[19] == 'B' ? Side::Bid : Side::Ask;
  m.shares = be32(p + 20);
  m.symbol = Symbol::from_wire(reinterpret_cast<const char*>(p + 24));
  m.price = static_cast<Price>(be32(p + 32));
  return m;
}

// 'E': type(1) locate(2) tracking(2) ts(6) ref(8) shares(4) match(8)   = 31
// 'C': the same, then printable(1) price(4)                            = 36
[[nodiscard]] inline OrderExecuted decode_order_executed(const std::uint8_t* p) noexcept {
  OrderExecuted m;
  m.timestamp = be48(p + 5);
  m.reference = be64(p + 11);
  m.shares = be32(p + 19);
  m.match_number = be64(p + 23);
  if (static_cast<char>(p[0]) == static_cast<char>(MsgType::OrderExecutedWithPrice)) {
    m.has_price = true;
    m.printable = p[31] == 'Y';
    m.price = static_cast<Price>(be32(p + 32));
  }
  return m;
}

// 'X': type(1) locate(2) tracking(2) ts(6) ref(8) shares(4) = 23
[[nodiscard]] inline OrderCancel decode_order_cancel(const std::uint8_t* p) noexcept {
  OrderCancel m;
  m.timestamp = be48(p + 5);
  m.reference = be64(p + 11);
  m.shares = be32(p + 19);
  return m;
}

// 'D': type(1) locate(2) tracking(2) ts(6) ref(8) = 19
[[nodiscard]] inline OrderDelete decode_order_delete(const std::uint8_t* p) noexcept {
  OrderDelete m;
  m.timestamp = be48(p + 5);
  m.reference = be64(p + 11);
  return m;
}

// 'U': type(1) locate(2) tracking(2) ts(6) orig(8) new(8) shares(4) price(4)
//      = 35
[[nodiscard]] inline OrderReplace decode_order_replace(const std::uint8_t* p) noexcept {
  OrderReplace m;
  m.timestamp = be48(p + 5);
  m.original_reference = be64(p + 11);
  m.new_reference = be64(p + 19);
  m.shares = be32(p + 27);
  m.price = static_cast<Price>(be32(p + 31));
  return m;
}

// 'S': type(1) locate(2) tracking(2) ts(6) event(1) = 12
[[nodiscard]] inline SystemEvent decode_system_event(const std::uint8_t* p) noexcept {
  SystemEvent m;
  m.timestamp = be48(p + 5);
  m.code = static_cast<char>(p[11]);
  return m;
}

// 'R': type(1) locate(2) tracking(2) ts(6) stock(8) category(1) ... = 39
[[nodiscard]] inline StockDirectory decode_stock_directory(const std::uint8_t* p) noexcept {
  StockDirectory m;
  m.timestamp = be48(p + 5);
  m.symbol = Symbol::from_wire(reinterpret_cast<const char*>(p + 11));
  m.market_category = static_cast<char>(p[19]);
  return m;
}

}  // namespace pricetime::itch
