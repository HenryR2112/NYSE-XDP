#pragma once

#include <cstdint>
#include <string_view>

namespace xdp {

// XDP Message Types (NYSE XDP Integrated Feed v2.3a)
enum class MessageType : uint16_t {
  ADD_ORDER = 100,
  MODIFY_ORDER = 101,
  DELETE_ORDER = 102,
  EXECUTE_ORDER = 103,
  REPLACE_ORDER = 104,
  IMBALANCE = 105,
  ADD_ORDER_REFRESH = 106,
  NON_DISPLAYED_TRADE = 110,
  CROSS_TRADE = 111,
  TRADE_CANCEL = 112,
  CROSS_CORRECTION = 113,
  RETAIL_PRICE_IMPROVEMENT = 114,
  STOCK_SUMMARY = 223
};

// Order side
enum class Side : char { BUY = 'B', SELL = 'S', UNKNOWN = '?' };

// Message sizes (per XDP spec)
namespace MessageSize {
constexpr size_t ADD_ORDER = 39;
constexpr size_t MODIFY_ORDER = 35;
constexpr size_t DELETE_ORDER = 25;
constexpr size_t EXECUTE_ORDER = 42;
constexpr size_t REPLACE_ORDER = 42;
constexpr size_t IMBALANCE = 73;
constexpr size_t ADD_ORDER_REFRESH = 43;
constexpr size_t NON_DISPLAYED_TRADE = 32;
constexpr size_t CROSS_TRADE = 40;
constexpr size_t TRADE_CANCEL = 32;
constexpr size_t CROSS_CORRECTION = 40;
constexpr size_t RETAIL_PRICE_IMPROVEMENT = 17;
constexpr size_t STOCK_SUMMARY = 36;
} // namespace MessageSize

// Header sizes
constexpr size_t PACKET_HEADER_SIZE = 16;
constexpr size_t MESSAGE_HEADER_SIZE = 4;
constexpr size_t COMMON_MSG_HEADER_SIZE = 16;

#pragma pack(push, 1)

// XDP Packet Header (16 bytes)
struct PacketHeader {
  uint16_t packet_size;    // Total packet size including this header
  uint8_t delivery_flag;   // Delivery flag
  uint8_t num_messages;    // Number of messages in packet
  uint32_t seq_num;        // Sequence number of first message
  uint32_t send_time;      // Send time seconds
  uint32_t send_time_ns;   // Send time nanoseconds
};

// XDP Message Header (4 bytes)
struct MessageHeader {
  uint16_t msg_size; // Message size
  uint16_t msg_type; // Message type
};

#pragma pack(pop)

// Get human-readable message type name
[[nodiscard]] constexpr std::string_view get_message_type_name(
    uint16_t type) noexcept {
  switch (type) {
  case static_cast<uint16_t>(MessageType::ADD_ORDER):
    return "ADD_ORDER";
  case static_cast<uint16_t>(MessageType::MODIFY_ORDER):
    return "MODIFY_ORDER";
  case static_cast<uint16_t>(MessageType::DELETE_ORDER):
    return "DELETE_ORDER";
  case static_cast<uint16_t>(MessageType::EXECUTE_ORDER):
    return "EXECUTE_ORDER";
  case static_cast<uint16_t>(MessageType::REPLACE_ORDER):
    return "REPLACE_ORDER";
  case static_cast<uint16_t>(MessageType::IMBALANCE):
    return "IMBALANCE";
  case static_cast<uint16_t>(MessageType::ADD_ORDER_REFRESH):
    return "ADD_ORDER_REFRESH";
  case static_cast<uint16_t>(MessageType::NON_DISPLAYED_TRADE):
    return "NON_DISPLAYED_TRADE";
  case static_cast<uint16_t>(MessageType::CROSS_TRADE):
    return "CROSS_TRADE";
  case static_cast<uint16_t>(MessageType::TRADE_CANCEL):
    return "TRADE_CANCEL";
  case static_cast<uint16_t>(MessageType::CROSS_CORRECTION):
    return "CROSS_CORRECTION";
  case static_cast<uint16_t>(MessageType::RETAIL_PRICE_IMPROVEMENT):
    return "RETAIL_PRICE_IMPROVEMENT";
  case static_cast<uint16_t>(MessageType::STOCK_SUMMARY):
    return "STOCK_SUMMARY";
  default:
    return "UNKNOWN";
  }
}

// Get abbreviated message type name
[[nodiscard]] constexpr std::string_view get_message_type_abbr(
    uint16_t type) noexcept {
  switch (type) {
  case static_cast<uint16_t>(MessageType::ADD_ORDER):
    return "A";
  case static_cast<uint16_t>(MessageType::MODIFY_ORDER):
    return "M";
  case static_cast<uint16_t>(MessageType::DELETE_ORDER):
    return "D";
  case static_cast<uint16_t>(MessageType::EXECUTE_ORDER):
    return "E";
  case static_cast<uint16_t>(MessageType::REPLACE_ORDER):
    return "R";
  case static_cast<uint16_t>(MessageType::IMBALANCE):
    return "I";
  case static_cast<uint16_t>(MessageType::ADD_ORDER_REFRESH):
    return "AR";
  case static_cast<uint16_t>(MessageType::NON_DISPLAYED_TRADE):
    return "NDT";
  case static_cast<uint16_t>(MessageType::CROSS_TRADE):
    return "X";
  case static_cast<uint16_t>(MessageType::TRADE_CANCEL):
    return "TC";
  case static_cast<uint16_t>(MessageType::CROSS_CORRECTION):
    return "XC";
  case static_cast<uint16_t>(MessageType::RETAIL_PRICE_IMPROVEMENT):
    return "RPI";
  case static_cast<uint16_t>(MessageType::STOCK_SUMMARY):
    return "SS";
  default:
    return "?";
  }
}

// Check if message type uses non-standard header (SourceTime@4, SourceTimeNS@8,
// SymbolIndex@12)
[[nodiscard]] constexpr bool has_non_standard_header(uint16_t type) noexcept {
  return type == static_cast<uint16_t>(MessageType::ADD_ORDER_REFRESH) ||
         type == static_cast<uint16_t>(MessageType::STOCK_SUMMARY);
}

// Get side abbreviation
[[nodiscard]] constexpr std::string_view get_side_abbr(char side) noexcept {
  switch (side) {
  case 'B':
    return "B";
  case 'S':
    return "S";
  default:
    return "?";
  }
}

// Convert raw side byte to Side enum
[[nodiscard]] constexpr Side parse_side(uint8_t raw_side) noexcept {
  if (raw_side == 'B' || raw_side == 1)
    return Side::BUY;
  if (raw_side == 'S' || raw_side == 2)
    return Side::SELL;
  return Side::UNKNOWN;
}

// Convert Side enum to char
[[nodiscard]] constexpr char side_to_char(Side side) noexcept {
  return static_cast<char>(side);
}

} // namespace xdp
