#pragma once

#include "xdp_types.hpp"
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

namespace xdp {

// Little-endian byte reading utilities
[[nodiscard]] inline uint16_t read_le16(const uint8_t *p) noexcept {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

[[nodiscard]] inline uint32_t read_le32(const uint8_t *p) noexcept {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

[[nodiscard]] inline uint64_t read_le64(const uint8_t *p) noexcept {
  return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
         (static_cast<uint64_t>(p[2]) << 16) |
         (static_cast<uint64_t>(p[3]) << 24) |
         (static_cast<uint64_t>(p[4]) << 32) |
         (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) |
         (static_cast<uint64_t>(p[7]) << 56);
}

// Price parsing using explicit multiplier from symbol data
// The multiplier converts raw integer price to actual dollar price
// Example: raw=150000000, multiplier=1e-6 -> price=$150.00
[[nodiscard]] inline double parse_price(uint32_t price_raw,
                                        double multiplier) noexcept {
  return static_cast<double>(price_raw) * multiplier;
}

// Legacy price parsing with heuristic (for backward compatibility)
// Prefers explicit multiplier version when symbol data is available
[[nodiscard]] inline double parse_price(uint32_t price_raw) noexcept {
  // Default to 1e-6 multiplier (divide by 1,000,000)
  // This matches price_scale_code=6 which is most common
  return static_cast<double>(price_raw) * 1e-6;
}

// Format timestamp with microsecond precision
[[nodiscard]] inline std::string format_time_micro(uint32_t seconds,
                                                   uint32_t nanoseconds) {
  char buffer[64];
  time_t t = static_cast<time_t>(seconds);
  struct tm tm_storage;
  localtime_r(&t, &tm_storage);

  strftime(buffer, sizeof(buffer), "%H:%M:%S", &tm_storage);

  char result[80];
  uint32_t microseconds = nanoseconds / 1000;
  snprintf(result, sizeof(result), "%s.%06u", buffer, microseconds);
  return std::string(result);
}

// Convert timeval to nanoseconds
[[nodiscard]] inline uint64_t timeval_to_ns(uint32_t tv_sec,
                                            uint32_t tv_usec) noexcept {
  return static_cast<uint64_t>(tv_sec) * 1000000000ULL +
         static_cast<uint64_t>(tv_usec) * 1000ULL;
}

// Read symbol index from message data based on message type
[[nodiscard]] inline uint32_t read_symbol_index(uint16_t msg_type,
                                                const uint8_t *data,
                                                size_t max_len) noexcept {
  if (has_non_standard_header(msg_type)) {
    // Messages 106 and 223: SourceTime@4, SourceTimeNS@8, SymbolIndex@12
    if (max_len < 16)
      return 0;
    return read_le32(data + 12);
  }

  // Standard messages: SourceTimeNS@4, SymbolIndex@8
  if (max_len < 12)
    return 0;
  return read_le32(data + 8);
}

// Parse packet header from raw data
[[nodiscard]] inline bool parse_packet_header(const uint8_t *data,
                                              size_t max_len,
                                              PacketHeader &header) noexcept {
  if (max_len < PACKET_HEADER_SIZE)
    return false;

  header.packet_size = read_le16(data);
  header.delivery_flag = data[2];
  header.num_messages = data[3];
  header.seq_num = read_le32(data + 4);
  header.send_time = read_le32(data + 8);
  header.send_time_ns = read_le32(data + 12);

  return true;
}

// Parse message header from raw data
[[nodiscard]] inline bool parse_message_header(const uint8_t *data,
                                               size_t max_len,
                                               MessageHeader &header) noexcept {
  if (max_len < MESSAGE_HEADER_SIZE)
    return false;

  header.msg_size = read_le16(data);
  header.msg_type = read_le16(data + 2);

  return true;
}

// Validate message size
[[nodiscard]] inline bool validate_message_size(uint16_t msg_size,
                                                size_t remaining) noexcept {
  return msg_size >= MESSAGE_HEADER_SIZE && msg_size <= remaining;
}

} // namespace xdp
