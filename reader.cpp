// nyse_xdp_parser_final.cpp
// Parser for NYSE XDP (Exchange Data Protocol) Market Data
// Compile: g++ -std=c++11 reader.cpp -lpcap -o reader
// Usage: ./reader <pcap_file> [verbose] [symbol_file] [-t ticker]
//        verbose: 0 = simplified output, 1 = detailed output
//        symbol_file: TXT file with symbol mapping
//        -t ticker: Filter messages for specific ticker symbol

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <pcap.h>
#include <sstream>
#include <unordered_map>
#include <vector>

#pragma pack(push, 1)

// XDP Packet Header (16 bytes)
struct XdpPacketHeader {
  uint16_t
      packet_size; // Total packet size including this header (LITTLE-ENDIAN)
  uint8_t delivery_flag; // Delivery flag
  uint8_t num_messages;  // Number of messages in packet
  uint32_t seq_num;      // Sequence number of first message (LITTLE-ENDIAN)
  uint32_t send_time;    // Send time seconds (LITTLE-ENDIAN)
  uint32_t send_time_ns; // Send time nanoseconds (LITTLE-ENDIAN)
};

// XDP Message Header (4 bytes)
struct XdpMessageHeader {
  uint16_t msg_size; // Message size (LITTLE-ENDIAN)
  uint16_t msg_type; // Message type (LITTLE-ENDIAN)
};

#pragma pack(pop)

// Configuration
int verbose_mode = 0;           // 0 = simplified, 1 = verbose
std::string filter_ticker = ""; // Empty string means no filter
std::unordered_map<uint32_t, std::string> symbol_map;
std::unordered_map<uint32_t, uint32_t> symbol_msg_counters;

// Helper functions for little-endian
uint16_t read_le16(const uint8_t *p) { return p[0] | (p[1] << 8); }

uint32_t read_le32(const uint8_t *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

uint64_t read_le64(const uint8_t *p) {
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}
bool test_symbol_lookup() {
  std::cout << "\n=== Testing Symbol Lookup ===" << std::endl;

  // Test index 9 (should be AAPL based on your file)
  uint32_t test_index = 9;
  auto it = symbol_map.find(test_index);
  if (it != symbol_map.end()) {
    std::cout << "✓ Index " << test_index << " -> " << it->second
              << " (SUCCESS)" << std::endl;
  } else {
    std::cout << "✗ Index " << test_index << " -> NOT FOUND" << std::endl;
    return false;
  }

  // Test a few more known symbols
  uint32_t test_indices[] = {4541, 54151, 6, 8, 4550};
  for (uint32_t index : test_indices) {
    it = symbol_map.find(index);
    if (it != symbol_map.end()) {
      std::cout << "✓ Index " << index << " -> " << it->second << std::endl;
    } else {
      std::cout << "✗ Index " << index << " -> NOT FOUND" << std::endl;
    }
  }

  // If 4624 appears in output but isn't showing as ticker, check for it
  if (symbol_map.find(4624) != symbol_map.end()) {
    std::cout << "\n✓ Found index 4624 in symbol file: " << symbol_map[4624]
              << std::endl;
  } else {
    std::cout << "\n✗ Index 4624 NOT FOUND in symbol file" << std::endl;
    std::cout << "Symbols near 4624:" << std::endl;
    for (const auto &pair : symbol_map) {
      if (pair.first >= 4620 && pair.first <= 4630) {
        std::cout << "  " << pair.first << " -> " << pair.second << std::endl;
      }
    }
  }

  return true;
}
// Load symbol mapping from file
bool load_symbol_map(const char *filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Warning: Could not open symbol file: " << filename
              << std::endl;
    return false;
  }

  std::string line;
  int count = 0;
  int line_num = 0;

  while (std::getline(file, line)) {
    line_num++;

    // Skip empty lines
    if (line.empty())
      continue;

    // Remove Windows carriage return if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Trim trailing whitespace
    while (!line.empty() && std::isspace(line.back())) {
      line.pop_back();
    }

    // Skip if line is now empty
    if (line.empty())
      continue;

    // DEBUG: Check for specific line
    if (line.find("49102") != std::string::npos) {
      std::cout << "DEBUG: Found line with 49102: '" << line << "'"
                << std::endl;
    }

    // Split by pipe delimiter
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(line);

    while (std::getline(iss, token, '|')) {
      tokens.push_back(token);
    }

    // We need at least 3 fields
    if (tokens.size() >= 3) {
      try {
        // The index is in the 3rd field (0-based, so tokens[2])
        uint32_t index = std::stoul(tokens[2]);

        // The symbol is in the 1st field (tokens[0])
        std::string symbol = tokens[0];

        // DEBUG: Check for index 49102
        if (index == 49102) {
          std::cout << "DEBUG: Loading index 49102 -> '" << symbol << "'"
                    << std::endl;
          std::cout << "DEBUG: Token[2] = '" << tokens[2] << "'" << std::endl;
          std::cout << "DEBUG: Full line: " << line << std::endl;
        }

        // Store in map
        symbol_map[index] = symbol;
        count++;

      } catch (const std::exception &e) {
        std::cerr << "Warning: Could not parse line " << line_num
                  << " (error: " << e.what() << "): " << line << std::endl;
      }
    } else {
      std::cerr << "Warning: Line " << line_num << " has insufficient fields ("
                << tokens.size() << "): " << line << std::endl;
    }
  }

  std::cout << "Loaded " << count << " symbol mappings from " << filename
            << std::endl;

  // Verify 49102 was loaded
  auto it = symbol_map.find(49102);
  if (it != symbol_map.end()) {
    std::cout << "✓ Verified: Index 49102 -> '" << it->second << "'"
              << std::endl;
  } else {
    std::cout << "✗ ERROR: Index 49102 not loaded!" << std::endl;
  }

  return true;
}

std::string get_symbol(uint32_t index) {
  auto it = symbol_map.find(index);
  if (it != symbol_map.end()) {
    return it->second;
  } else {
    return std::to_string(index); // fallback
  }
}

// Format time to microsecond precision
std::string format_time_micro(uint32_t seconds, uint32_t nanoseconds) {
  char buffer[64];
  time_t t = seconds;
  struct tm *tm_info = localtime(&t);

  // Format: HH:MM:SS.mmmmmm
  strftime(buffer, sizeof(buffer), "%H:%M:%S", tm_info);

  char result[80];
  uint32_t microseconds = nanoseconds / 1000;
  snprintf(result, sizeof(result), "%s.%06u", buffer, microseconds);
  return std::string(result);
}

// Parse price (divide by 10,000 for dollar value)
double parse_price(uint32_t price_raw) { return price_raw / 10000.0; }

// Get message type name
const char *get_message_type_name(uint16_t type) {
  switch (type) {
  // Integrated Feed messages
  case 100:
    return "ADD_ORDER";
  case 101:
    return "MODIFY_ORDER";
  case 102:
    return "DELETE_ORDER";
  case 103:
    return "EXECUTE_ORDER";
  case 104:
    return "REPLACE_ORDER";
  case 105:
    return "IMBALANCE";
  case 106:
    return "ADD_ORDER_REFRESH";
  case 110:
    return "NON_DISPLAYED_TRADE";
  case 111:
    return "CROSS_TRADE";
  case 112:
    return "TRADE_CANCEL";
  case 113:
    return "CROSS_CORRECTION";
  case 114:
    return "RETAIL_PRICE_IMPROVEMENT";
  case 223:
    return "STOCK_SUMMARY";
  default:
    return "UNKNOWN";
  }
}

// Get order type abbreviation
const char *get_order_type_abbr(uint16_t type) {
  switch (type) {
  case 100:
    return "A"; // Add
  case 102:
    return "D"; // Delete
  case 104:
    return "R"; // Replace
  case 101:
    return "M"; // Modify
  case 103:
    return "E"; // Execute
  case 105:
    return "I"; // Imbalance
  case 106:
    return "AR"; // Add Order Refresh
  case 110:
    return "NDT"; // Non-Displayed Trade
  case 111:
    return "X"; // Cross Trade
  case 112:
    return "TC"; // Trade Cancel
  case 113:
    return "XC"; // Cross Correction
  case 114:
    return "RPI"; // Retail Price Improvement
  case 223:
    return "SS"; // Stock Summary
  default:
    return "?"; // Other
  }
}

// Get side abbreviation
const char *get_side_abbr(uint8_t side) {
  return (side == 'B') ? "B" : (side == 'S') ? "S" : "?";
}

// Parse individual XDP message and output in simplified format
void parse_xdp_message_simple(const uint8_t *data, size_t max_len,
                              uint32_t packet_send_time,
                              uint32_t packet_send_time_ns) {

  if (max_len < 4)
    return;

  uint16_t msg_size = read_le16(data);
  uint16_t msg_type = read_le16(data + 2);

  if (msg_size < 4 || msg_size > max_len)
    return;

  // Parse message-specific fields
  // Messages with common header (SourceTimeNS, SymbolIndex, SymbolSeqNum at
  // offset 4, 8, 12)
  if (msg_size >= 16) {
    uint32_t source_time_ns = read_le32(data + 4);
    uint32_t symbol_index = read_le32(data + 8);
    uint32_t symbol_seq = read_le32(data + 12);
    std::string ticker = get_symbol(symbol_index);

    // Filter by ticker if specified (for messages with symbol_index)
    if (!filter_ticker.empty() && ticker != filter_ticker) {
      return;
    }

    // Output timestamp and message type (after filter check)
    std::cout << format_time_micro(packet_send_time, packet_send_time_ns)
              << " ";
    std::cout << get_message_type_name(msg_type) << " ";

    uint32_t msg_num = ++symbol_msg_counters[symbol_index];

    switch (msg_type) {
    case 100: { // Add Order (39 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 39) {
        uint64_t order_id = read_le64(data + 16);
        uint32_t price = read_le32(data + 24);
        uint32_t volume = read_le32(data + 28);
        uint8_t side = data[32];
        std::cout << " OrderID=" << order_id;
        std::cout << " $" << std::fixed << std::setprecision(4)
                  << parse_price(price);
        std::cout << " " << volume;
        std::cout << " " << get_side_abbr(side);
      }
      break;
    }

    case 101: { // Modify Order (35 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 35) {
        uint64_t order_id = read_le64(data + 16);
        uint32_t price = read_le32(data + 24);
        uint32_t volume = read_le32(data + 28);
        uint8_t position_change = data[32];
        std::cout << " OrderID=" << order_id;
        std::cout << " $" << std::fixed << std::setprecision(4)
                  << parse_price(price);
        std::cout << " " << volume;
        std::cout << " Pos=" << (position_change == 0 ? "Kept" : "Lost");
      } else {
        std::cout << " [truncated, size=" << msg_size << "]";
      }
      break;
    }

    case 102: { // Delete Order (25 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 25) {
        uint64_t order_id = read_le64(data + 16);
        std::cout << " OrderID=" << order_id;
      }
      break;
    }

    case 103: { // Execute Order (32 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 32) {
        uint64_t order_id = read_le64(data + 16);
        uint32_t exec_volume = read_le32(data + 24);
        uint32_t price = read_le32(data + 28);
        std::cout << " OrderID=" << order_id;
        std::cout << " $" << std::fixed << std::setprecision(4)
                  << parse_price(price);
        std::cout << " Qty=" << exec_volume;
      }
      break;
    }

    case 104: { // Replace Order (42 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 42) {
        uint64_t order_id = read_le64(data + 16);
        uint64_t new_order_id = read_le64(data + 24);
        uint32_t price = read_le32(data + 32);
        uint32_t volume = read_le32(data + 36);
        uint8_t prev_price_parity_splits = data[40];
        uint8_t new_price_parity_splits = data[41];
        std::cout << " OldOrderID=" << order_id;
        std::cout << " NewOrderID=" << new_order_id;
        std::cout << " $" << std::fixed << std::setprecision(4)
                  << parse_price(price);
        std::cout << " " << volume;
      }
      break;
    }

    case 105: { // Imbalance Message
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 60) {
        uint32_t reference_price = read_le32(data + 16);
        uint32_t paired_qty = read_le32(data + 20);
        uint32_t imbalance_qty = read_le32(data + 24);
        uint8_t imbalance_side = data[28];
        uint32_t continuous_book_clearing_price = read_le32(data + 29);
        uint32_t closing_only_clearing_price = read_le32(data + 33);
        uint8_t ssr_filing_price = data[37];
        uint32_t indicative_match_price = read_le32(data + 38);
        std::cout << " RefPrice=$" << std::fixed << std::setprecision(4)
                  << parse_price(reference_price);
        std::cout << " Paired=" << paired_qty;
        std::cout << " Imbalance=" << imbalance_qty;
        std::cout << " Side=" << (char)imbalance_side;
        std::cout << " IndicativeMatch=$"
                  << parse_price(indicative_match_price);
      }
      break;
    }

    case 106: { // Add Order Refresh (39 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 39) {
        uint64_t order_id = read_le64(data + 16);
        uint32_t price = read_le32(data + 24);
        uint32_t volume = read_le32(data + 28);
        uint8_t side = data[32];
        std::cout << " OrderID=" << order_id;
        std::cout << " $" << std::fixed << std::setprecision(4)
                  << parse_price(price);
        std::cout << " " << volume;
        std::cout << " " << get_side_abbr(side);
      }
      break;
    }

    case 110: { // Non-Displayed Trade (32 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 32) {
        uint64_t trade_id = read_le64(data + 16);
        uint32_t price = read_le32(data + 24);
        uint32_t volume = read_le32(data + 28);
        std::cout << " TradeID=" << trade_id;
        std::cout << " $" << std::fixed << std::setprecision(4)
                  << parse_price(price);
        std::cout << " Qty=" << volume;
      }
      break;
    }

    case 111: { // Cross Trade (40 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 25) {
        uint64_t cross_id = read_le64(data + 16);
        std::cout << " CrossID=" << cross_id;
        if (msg_size >= 29) {
          uint32_t price = read_le32(data + 24);
          std::cout << " $" << std::fixed << std::setprecision(4)
                    << parse_price(price);
          if (msg_size >= 33) {
            uint32_t volume = read_le32(data + 28);
            std::cout << " Qty=" << volume;
            if (msg_size >= 37) {
              uint32_t cross_type = read_le32(data + 32);
              std::cout << " Type=" << cross_type;
            }
          }
        }
      } else {
        std::cout << " [truncated, size=" << msg_size << "]";
      }
      break;
    }

    case 112: { // Trade Cancel (32 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 32) {
        uint64_t trade_id = read_le64(data + 16);
        uint32_t price = read_le32(data + 24);
        uint32_t volume = read_le32(data + 28);
        std::cout << " TradeID=" << trade_id;
        std::cout << " $" << std::fixed << std::setprecision(4)
                  << parse_price(price);
        std::cout << " Qty=" << volume;
      }
      break;
    }

    case 113: { // Cross Correction (40 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 40) {
        uint64_t cross_id = read_le64(data + 16);
        uint32_t price = read_le32(data + 24);
        uint32_t volume = read_le32(data + 28);
        uint32_t cross_type = read_le32(data + 32);
        std::cout << " CrossID=" << cross_id;
        std::cout << " $" << std::fixed << std::setprecision(4)
                  << parse_price(price);
        std::cout << " Qty=" << volume;
        std::cout << " Type=" << cross_type;
      }
      break;
    }

    case 114: { // Retail Price Improvement (17 bytes)
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 17) {
        uint8_t rpi_indicator = data[16];
        std::cout << " RPI=";
        if (rpi_indicator == ' ') {
          std::cout << "None";
        } else if (rpi_indicator == 'A') {
          std::cout << "Bid";
        } else if (rpi_indicator == 'B') {
          std::cout << "Offer";
        } else if (rpi_indicator == 'C') {
          std::cout << "Both";
        } else {
          std::cout << "'" << (char)rpi_indicator << "'";
        }
      } else {
        std::cout << " [truncated, size=" << msg_size << "]";
      }
      break;
    }

    case 223: { // Stock Summary
      std::cout << ticker << " " << msg_num;
      if (msg_size >= 40) {
        uint32_t high_price = read_le32(data + 16);
        uint32_t low_price = read_le32(data + 20);
        uint32_t open_price = read_le32(data + 24);
        uint32_t close_price = read_le32(data + 28);
        uint64_t total_volume = read_le64(data + 32);
        std::cout << " High=$" << std::fixed << std::setprecision(4)
                  << parse_price(high_price);
        std::cout << " Low=$" << parse_price(low_price);
        std::cout << " Open=$" << parse_price(open_price);
        std::cout << " Close=$" << parse_price(close_price);
        std::cout << " Volume=" << total_volume;
      }
      break;
    }

    default:
      std::cout << ticker << " Type=" << msg_type << " Size=" << msg_size;
      break;
    }
    std::cout << std::endl;
  } else {
    // Messages without common header - skip (integrated feed messages should
    // have common header)
    return;
  }
}

std::string get_ticker(uint32_t index) {
  auto it = symbol_map.find(index);
  if (it != symbol_map.end()) {
    return it->second;
  }
  return std::to_string(index); // Just return the index number as string
}

// Parse individual XDP message for verbose mode
void parse_xdp_message_verbose(const uint8_t *data, size_t max_len,
                               int msg_num) {
  if (max_len < 4) {
    std::cout << "  [" << msg_num << "] Too short for message header"
              << std::endl;
    return;
  }

  uint16_t msg_size = read_le16(data);
  uint16_t msg_type = read_le16(data + 2);

  const char *type_name = get_message_type_name(msg_type);

  std::cout << "  [" << msg_num << "] Type: " << msg_type << " (" << type_name
            << ")" << std::endl;
  std::cout << "      Size: " << msg_size << " bytes" << std::endl;

  if (msg_size > max_len) {
    std::cout << "      ERROR: Message size (" << msg_size
              << ") exceeds remaining data (" << max_len << ")!" << std::endl;
    return;
  }

  // Parse common fields
  if (msg_size >= 16) {
    uint32_t source_time_ns = read_le32(data + 4);
    uint32_t symbol_index = read_le32(data + 8);
    uint32_t symbol_seq = read_le32(data + 12);

    // Get ticker symbol
    std::string ticker = get_symbol(symbol_index);

    // Filter by ticker if specified
    if (!filter_ticker.empty() && ticker != filter_ticker) {
      return;
    }

    std::cout << "      SourceTimeNS: " << source_time_ns << std::endl;
    std::cout << "      SymbolIndex: " << symbol_index;

    // Show ticker symbol if available
    auto it = symbol_map.find(symbol_index);
    if (it != symbol_map.end()) {
      std::cout << " (" << it->second << ")";
    }
    std::cout << std::endl;

    std::cout << "      SymbolSeqNum: " << symbol_seq << std::endl;
  }

  // Parse message-specific fields
  switch (msg_type) {
  case 100: { // Add Order (39 bytes)
    if (msg_size >= 39) {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      uint8_t side = data[32];
      char firm_id[6] = {0};
      memcpy(firm_id, data + 33, 5);

      std::cout << "      OrderID: " << order_id << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
      std::cout << "      Volume: " << volume << std::endl;
      std::cout << "      Side: " << (side == 'B' ? "BUY" : "SELL")
                << std::endl;
      std::cout << "      FirmID: '" << firm_id << "'" << std::endl;
    }
    break;
  }

  case 101: { // Modify Order (35 bytes)
    if (msg_size >= 35) {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      uint8_t position_change = data[32];
      uint8_t prev_price_parity_splits = data[33];
      uint8_t new_price_parity_splits = data[34];

      std::cout << "      OrderID: " << order_id << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
      std::cout << "      Volume: " << volume << std::endl;
      std::cout << "      Position Change: "
                << (position_change == 0 ? "Kept position" : "Lost position")
                << std::endl;
      std::cout << "      Prev Price Parity Splits: "
                << (int)prev_price_parity_splits << std::endl;
      std::cout << "      New Price Parity Splits: "
                << (int)new_price_parity_splits << std::endl;
    }
    break;
  }

  case 102: { // Delete Order (25 bytes)
    if (msg_size >= 25) {
      uint64_t order_id = read_le64(data + 16);
      std::cout << "      OrderID: " << order_id << std::endl;
    }
    break;
  }

  case 103: { // Execute Order (32 bytes)
    if (msg_size >= 32) {
      uint64_t order_id = read_le64(data + 16);
      uint32_t exec_volume = read_le32(data + 24);
      uint32_t price = read_le32(data + 28);

      std::cout << "      OrderID: " << order_id << std::endl;
      std::cout << "      Executed Volume: " << exec_volume << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
    }
    break;
  }

  case 104: { // Replace Order (42 bytes)
    if (msg_size >= 42) {
      uint64_t order_id = read_le64(data + 16);
      uint64_t new_order_id = read_le64(data + 24);
      uint32_t price = read_le32(data + 32);
      uint32_t volume = read_le32(data + 36);
      uint8_t prev_price_parity_splits = data[40];
      uint8_t new_price_parity_splits = data[41];

      std::cout << "      Old OrderID: " << order_id << std::endl;
      std::cout << "      New OrderID: " << new_order_id << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
      std::cout << "      Volume: " << volume << std::endl;
      std::cout << "      Prev Price Parity Splits: "
                << (int)prev_price_parity_splits << std::endl;
      std::cout << "      New Price Parity Splits: "
                << (int)new_price_parity_splits << std::endl;
    }
    break;
  }

  case 105: { // Imbalance Message
    if (msg_size >= 60) {
      uint32_t reference_price = read_le32(data + 16);
      uint32_t paired_qty = read_le32(data + 20);
      uint32_t imbalance_qty = read_le32(data + 24);
      uint8_t imbalance_side = data[28];
      uint32_t continuous_book_clearing_price = read_le32(data + 29);
      uint32_t closing_only_clearing_price = read_le32(data + 33);
      uint8_t ssr_filing_price = data[37];
      uint32_t indicative_match_price = read_le32(data + 38);

      std::cout << "      Reference Price: $" << std::fixed
                << std::setprecision(4) << parse_price(reference_price)
                << std::endl;
      std::cout << "      Paired Quantity: " << paired_qty << std::endl;
      std::cout << "      Imbalance Quantity: " << imbalance_qty << std::endl;
      std::cout << "      Imbalance Side: "
                << (imbalance_side == 'B' ? "BUY" : "SELL") << std::endl;
      std::cout << "      Continuous Book Clearing Price: $" << std::fixed
                << std::setprecision(4)
                << parse_price(continuous_book_clearing_price) << std::endl;
      std::cout << "      Closing Only Clearing Price: $" << std::fixed
                << std::setprecision(4)
                << parse_price(closing_only_clearing_price) << std::endl;
      std::cout << "      SSR Filing Price: " << (int)ssr_filing_price
                << std::endl;
      std::cout << "      Indicative Match Price: $" << std::fixed
                << std::setprecision(4) << parse_price(indicative_match_price)
                << std::endl;
    }
    break;
  }

  case 106: { // Add Order Refresh (39 bytes)
    if (msg_size >= 39) {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      uint8_t side = data[32];
      char firm_id[6] = {0};
      memcpy(firm_id, data + 33, 5);

      std::cout << "      OrderID: " << order_id << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
      std::cout << "      Volume: " << volume << std::endl;
      std::cout << "      Side: " << (side == 'B' ? "BUY" : "SELL")
                << std::endl;
      std::cout << "      FirmID: '" << firm_id << "'" << std::endl;
    }
    break;
  }

  case 110: { // Non-Displayed Trade (32 bytes)
    if (msg_size >= 32) {
      uint64_t trade_id = read_le64(data + 16);
      uint32_t price = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);

      std::cout << "      TradeID: " << trade_id << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
      std::cout << "      Volume: " << volume << std::endl;
    }
    break;
  }

  case 111: { // Cross Trade (40 bytes)
    if (msg_size >= 40) {
      uint64_t cross_id = read_le64(data + 16);
      uint32_t price = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      uint32_t cross_type = read_le32(data + 32);

      std::cout << "      CrossID: " << cross_id << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
      std::cout << "      Volume: " << volume << std::endl;
      std::cout << "      Cross Type: " << cross_type << std::endl;
    }
    break;
  }

  case 112: { // Trade Cancel (32 bytes)
    if (msg_size >= 32) {
      uint64_t trade_id = read_le64(data + 16);
      uint32_t price = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);

      std::cout << "      TradeID: " << trade_id << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
      std::cout << "      Volume: " << volume << std::endl;
    }
    break;
  }

  case 113: { // Cross Correction (40 bytes)
    if (msg_size >= 40) {
      uint64_t cross_id = read_le64(data + 16);
      uint32_t price = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      uint32_t cross_type = read_le32(data + 32);

      std::cout << "      CrossID: " << cross_id << std::endl;
      std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                << parse_price(price) << std::endl;
      std::cout << "      Volume: " << volume << std::endl;
      std::cout << "      Cross Type: " << cross_type << std::endl;
    }
    break;
  }

  case 114: { // Retail Price Improvement (17 bytes)
    if (msg_size >= 17) {
      uint8_t rpi_indicator = data[16];
      std::cout << "      RPI Indicator: ";
      if (rpi_indicator == ' ') {
        std::cout << "' ' (No retail interest)" << std::endl;
      } else if (rpi_indicator == 'A') {
        std::cout << "'A' (Retail interest on bid side)" << std::endl;
      } else if (rpi_indicator == 'B') {
        std::cout << "'B' (Retail interest on offer side)" << std::endl;
      } else if (rpi_indicator == 'C') {
        std::cout << "'C' (Retail interest on both sides)" << std::endl;
      } else {
        std::cout << "'" << (char)rpi_indicator << "' (Unknown)" << std::endl;
      }
    } else {
      std::cout << "      ERROR: Message too short (needs 17, has " << msg_size
                << ")" << std::endl;
    }
    break;
  }

  case 223: { // Stock Summary
    if (msg_size >= 40) {
      uint32_t high_price = read_le32(data + 16);
      uint32_t low_price = read_le32(data + 20);
      uint32_t open_price = read_le32(data + 24);
      uint32_t close_price = read_le32(data + 28);
      uint64_t total_volume = read_le64(data + 32);

      std::cout << "      High Price: $" << std::fixed << std::setprecision(4)
                << parse_price(high_price) << std::endl;
      std::cout << "      Low Price: $" << std::fixed << std::setprecision(4)
                << parse_price(low_price) << std::endl;
      std::cout << "      Open Price: $" << std::fixed << std::setprecision(4)
                << parse_price(open_price) << std::endl;
      std::cout << "      Close Price: $" << std::fixed << std::setprecision(4)
                << parse_price(close_price) << std::endl;
      std::cout << "      Total Volume: " << total_volume << std::endl;
    }
    break;
  }

  default: {
    std::cout << "      Unknown message type, size: " << msg_size << " bytes"
              << std::endl;
    break;
  }
  }
}

// Parse XDP packet in verbose mode
void parse_xdp_packet_verbose(const uint8_t *data, size_t length,
                              int packet_num, const char *src_ip,
                              const char *dst_ip, uint16_t dst_port) {
  std::cout << "\n=== Packet " << packet_num << " ===" << std::endl;
  std::cout << "Source: " << src_ip << " -> Multicast: " << dst_ip << ":"
            << dst_port << std::endl;
  std::cout << "Total length: " << length << " bytes" << std::endl;

  if (length < sizeof(XdpPacketHeader)) {
    std::cout << "ERROR: Packet too short for XDP header (needs 16, has "
              << length << ")" << std::endl;
    return;
  }

  // Parse XDP header
  uint16_t packet_size = read_le16(data);
  uint8_t delivery_flag = data[2];
  uint8_t num_messages = data[3];
  uint32_t seq_num = read_le32(data + 4);
  uint32_t send_time = read_le32(data + 8);
  uint32_t send_time_ns = read_le32(data + 12);

  std::cout << "\nXDP Packet Header:" << std::endl;
  std::cout << "  Packet Size: " << packet_size << " bytes" << std::endl;
  std::cout << "  Delivery Flag: " << (int)delivery_flag << std::endl;
  std::cout << "  Message Count: " << (int)num_messages << std::endl;
  std::cout << "  Sequence Number: " << seq_num << std::endl;
  std::cout << "  Send Time: " << format_time_micro(send_time, send_time_ns)
            << std::endl;

  // Parse messages
  size_t offset = 16;
  int msg_count = 0;

  std::cout << "\nMessages (" << (int)num_messages
            << " expected):" << std::endl;

  while (offset + 4 <= length && msg_count < num_messages) {
    size_t remaining = length - offset;
    parse_xdp_message_verbose(data + offset, remaining, msg_count + 1);

    uint16_t msg_size = read_le16(data + offset);
    if (msg_size < 4 || msg_size > remaining) {
      std::cout << "  ERROR: Invalid message size " << msg_size
                << ", stopping parse" << std::endl;
      break;
    }

    offset += msg_size;
    msg_count++;
  }

  std::cout << "\nParsed " << msg_count << " of " << (int)num_messages
            << " messages" << std::endl;
}

// Parse XDP packet in simple mode
void parse_xdp_packet_simple(const uint8_t *data, size_t length,
                             int packet_num) {
  if (length < sizeof(XdpPacketHeader))
    return;

  // Parse XDP header
  uint16_t packet_size = read_le16(data);
  uint8_t num_messages = data[3];
  uint32_t send_time = read_le32(data + 8);
  uint32_t send_time_ns = read_le32(data + 12);

  // Parse messages
  size_t offset = 16;
  int msg_count = 0;

  while (offset + 4 <= length && msg_count < num_messages) {
    size_t remaining = length - offset;
    parse_xdp_message_simple(data + offset, remaining, send_time, send_time_ns);

    uint16_t msg_size = read_le16(data + offset);
    if (msg_size < 4 || msg_size > remaining)
      break;

    offset += msg_size;
    msg_count++;
  }
}

// PCAP packet handler
void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr,
                    const uint8_t *packet_data) {
  static int packet_count = 0;
  packet_count++;

  // In verbose mode, limit to first 10 packets for demo
  if (verbose_mode && packet_count > 100000)
    return;

  if (pkthdr->caplen < 14)
    return;

  // Parse Ethernet header
  uint16_t eth_type = ntohs(*(uint16_t *)(packet_data + 12));
  size_t eth_header_len = 14;

  // Skip VLAN tag if present
  if (eth_type == 0x8100 || eth_type == 0x88A8) {
    if (pkthdr->caplen < 18)
      return;
    eth_type = ntohs(*(uint16_t *)(packet_data + 16));
    eth_header_len = 18;
  }

  // Only process IPv4
  if (eth_type != 0x0800)
    return;

  // Parse IP header
  if (pkthdr->caplen < eth_header_len + 20)
    return;

  const uint8_t *ip_header = packet_data + eth_header_len;
  uint8_t ip_ver_ihl = ip_header[0];
  uint8_t ip_header_len = (ip_ver_ihl & 0x0F) * 4;
  uint8_t protocol = ip_header[9];

  // Extract IP addresses for verbose mode
  char src_ip[INET_ADDRSTRLEN], dst_ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, ip_header + 12, src_ip, sizeof(src_ip));
  inet_ntop(AF_INET, ip_header + 16, dst_ip, sizeof(dst_ip));

  // Check for UDP
  if (protocol != 17)
    return;

  // Parse UDP header
  size_t udp_offset = eth_header_len + ip_header_len;
  if (pkthdr->caplen < udp_offset + 8)
    return;

  const uint8_t *udp_header = packet_data + udp_offset;
  uint16_t dst_port = ntohs(*(uint16_t *)(udp_header + 2));
  uint16_t udp_len = ntohs(*(uint16_t *)(udp_header + 4));

  // Extract UDP payload (XDP data)
  const uint8_t *udp_payload = udp_header + 8;
  size_t payload_len = udp_len - 8;

  if (payload_len > pkthdr->caplen - udp_offset - 8) {
    payload_len = pkthdr->caplen - udp_offset - 8;
  }

  // Parse XDP packet based on mode
  if (verbose_mode) {
    parse_xdp_packet_verbose(udp_payload, payload_len, packet_count, src_ip,
                             dst_ip, dst_port);
  } else {
    parse_xdp_packet_simple(udp_payload, payload_len, packet_count);
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <pcap_file> [verbose] [symbol_file] [-t ticker]"
              << std::endl;
    std::cerr << "  verbose: 0 = simplified output (default)" << std::endl;
    std::cerr << "           1 = detailed output with headers" << std::endl;
    std::cerr << "  symbol_file: CSV file with symbol mapping (optional)"
              << std::endl;
    std::cerr
        << "  -t ticker: Filter messages for specific ticker symbol (optional)"
        << std::endl;
    std::cerr << std::endl;
    std::cerr << "Examples:" << std::endl;
    std::cerr << "  " << argv[0] << " nyse_xdp_data.pcap 0 symbols.csv"
              << std::endl;
    std::cerr << "  " << argv[0] << " nyse_xdp_data.pcap 1 symbols.csv"
              << std::endl;
    std::cerr << "  " << argv[0] << " nyse_xdp_data.pcap 0 symbols.csv -t AAPL"
              << std::endl;
    std::cerr << "  " << argv[0] << " nyse_xdp_data.pcap 0" << std::endl;
    return 1;
  }

  // Parse command line arguments
  const char *pcap_file = argv[1];
  const char *symbol_file = nullptr;

  // Parse arguments (handle both old style and new -t flag)
  for (int i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-t") == 0) {
      if (i + 1 < argc) {
        filter_ticker = argv[i + 1];
        i++; // Skip the ticker argument
      } else {
        std::cerr << "Error: -t requires a ticker symbol" << std::endl;
        return 1;
      }
    } else if (argv[i][0] != '-') {
      // Non-flag argument: could be verbose or symbol_file
      if (strcmp(argv[i], "0") == 0 || strcmp(argv[i], "1") == 0) {
        // This looks like a verbose flag
        if (verbose_mode == 0) { // Only set if not already set
          verbose_mode = atoi(argv[i]);
        } else {
          // Already set verbose, treat as symbol_file
          if (symbol_file == nullptr) {
            symbol_file = argv[i];
          }
        }
      } else {
        // Treat as symbol_file
        if (symbol_file == nullptr) {
          symbol_file = argv[i];
        } else {
          // Both verbose and symbol_file already set, this is unexpected
          std::cerr << "Warning: Unexpected argument: " << argv[i] << std::endl;
        }
      }
    }
  }

  // Load symbol mapping if provided
  if (symbol_file && !load_symbol_map(symbol_file)) {
    std::cerr << "Continuing without symbol mapping..." << std::endl;
  } else if (symbol_file) {
    // Test the symbol lookup
    test_symbol_lookup();
  }

  std::cout << "\n=== Quick Symbol Lookup Test ===" << std::endl;
  uint32_t test_indices[] = {42340, 10008, 5671, 5666};
  for (uint32_t idx : test_indices) {
    auto it = symbol_map.find(idx);
    if (it != symbol_map.end()) {
      std::cout << "Index " << idx << " -> '" << it->second << "'" << std::endl;
    } else {
      std::cout << "Index " << idx << " -> NOT FOUND" << std::endl;
    }
  }
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t *pcap = pcap_open_offline(pcap_file, errbuf);

  if (!pcap) {
    std::cerr << "Error opening pcap file: " << errbuf << std::endl;
    return 1;
  }

  if (verbose_mode) {
    std::cout << "Parsing NYSE XDP Market Data from: " << pcap_file
              << std::endl;
    std::cout << "Mode: VERBOSE" << std::endl;
    std::cout << "Symbols loaded: " << symbol_map.size() << std::endl;
    if (!filter_ticker.empty()) {
      std::cout << "Filtering for ticker: " << filter_ticker << std::endl;
    }
    std::cout << "=================================================="
              << std::endl;
  } else {
    std::cout << "Parsing NYSE XDP Market Data" << std::endl;
    if (symbol_file) {
      std::cout << "Using symbol mapping from: " << symbol_file << std::endl;
    }
    if (!filter_ticker.empty()) {
      std::cout << "Filtering for ticker: " << filter_ticker << std::endl;
    }
    std::cout << "Format: Time Type Ticker [Price Qty Side]" << std::endl;
    std::cout << "Example: 06:30:00.821165 D 68482" << std::endl;
    std::cout << "Example: 06:30:00.821165 A 68482 $45.2300 100 B" << std::endl;
    std::cout << "================================================"
              << std::endl;
  }

  int result = pcap_loop(pcap, 0, packet_handler, nullptr);

  if (result == -1) {
    std::cerr << "Error reading packets: " << pcap_geterr(pcap) << std::endl;
  }

  pcap_close(pcap);

  std::cout << "\nParsing complete" << std::endl;

  return 0;
}