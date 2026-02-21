// reader.cpp - NYSE XDP (Exchange Data Protocol) Market Data Parser
// Parses XDP Integrated Feed messages from PCAP files
// Usage: ./reader <pcap_file> [verbose] [symbol_file] [-t ticker] [-m message_type]

#include "common/pcap_reader.hpp"
#include "common/symbol_map.hpp"
#include "common/xdp_types.hpp"
#include "common/xdp_utils.hpp"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

// Configuration
int g_verbose_mode = 0;
std::string g_filter_ticker;
std::string g_filter_message;
std::unordered_map<uint32_t, uint32_t> g_symbol_msg_counters;

// Check if message passes filters
bool passes_filter(const std::string &ticker, uint16_t msg_type) {
  if (!g_filter_ticker.empty() && ticker != g_filter_ticker) {
    return false;
  }
  if (!g_filter_message.empty()) {
    auto type_name = xdp::get_message_type_name(msg_type);
    if (g_filter_message != type_name) {
      return false;
    }
  }
  return true;
}

// Print message-specific fields for a single message type.
// The verbose flag controls whether to emit compact one-line or multi-line
// labeled output.  The ticker and msg_num arguments are only used by simple
// mode (verbose mode prints them in its own header section).
void print_message_fields(const uint8_t *data, uint16_t msg_size,
                          uint16_t msg_type, bool verbose,
                          const std::string &ticker, uint32_t msg_num) {
  switch (msg_type) {
  case 100: { // Add Order
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::ADD_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t price = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      uint8_t side = data[32];
      if (verbose) {
        char firm_id[6] = {0};
        std::memcpy(firm_id, data + 33, 5);
        std::cout << "      OrderID: " << order_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
        std::cout << "      Side: " << (side == 'B' ? "BUY" : "SELL") << '\n';
        std::cout << "      FirmID: '" << firm_id << "'\n";
      } else {
        std::cout << " OrderID=" << order_id << " $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(price) << " "
                  << volume << " " << xdp::get_side_abbr(side);
      }
    }
    break;
  }

  case 101: { // Modify Order
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::MODIFY_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t price = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      uint8_t position_change = data[32];
      if (verbose) {
        std::cout << "      OrderID: " << order_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
        std::cout << "      Position Change: "
                  << (position_change == 0 ? "Kept position" : "Lost position")
                  << '\n';
      } else {
        std::cout << " OrderID=" << order_id << " $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(price) << " "
                  << volume
                  << " Pos=" << (position_change == 0 ? "Kept" : "Lost");
      }
    }
    break;
  }

  case 102: { // Delete Order
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::DELETE_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      if (verbose) {
        std::cout << "      OrderID: " << order_id << '\n';
      } else {
        std::cout << " OrderID=" << order_id;
      }
    }
    break;
  }

  case 103: { // Execute Order
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::EXECUTE_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t trade_id = xdp::read_le32(data + 24);
      uint32_t price = xdp::read_le32(data + 28);
      uint32_t volume = xdp::read_le32(data + 32);
      uint8_t printable_flag = data[36];
      if (verbose) {
        std::cout << "      OrderID: " << order_id << '\n';
        std::cout << "      TradeID: " << trade_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
        std::cout << "      Printable Flag: "
                  << (printable_flag == 1 ? "Printed to SIP"
                                          : "Not Printed to SIP")
                  << '\n';
      } else {
        std::cout << " OrderID=" << order_id << " TradeID=" << trade_id << " $"
                  << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << " Qty=" << volume;
        if (printable_flag == 0) {
          std::cout << " (NotPrinted)";
        }
      }
    }
    break;
  }

  case 104: { // Replace Order
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::REPLACE_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint64_t new_order_id = xdp::read_le64(data + 24);
      uint32_t price = xdp::read_le32(data + 32);
      uint32_t volume = xdp::read_le32(data + 36);
      if (verbose) {
        std::cout << "      Old OrderID: " << order_id << '\n';
        std::cout << "      New OrderID: " << new_order_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
      } else {
        std::cout << " OldOrderID=" << order_id
                  << " NewOrderID=" << new_order_id << " $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(price) << " "
                  << volume;
      }
    }
    break;
  }

  case 105: { // Imbalance Message
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::IMBALANCE) {
      uint32_t reference_price = xdp::read_le32(data + 16);
      uint32_t paired_qty = xdp::read_le32(data + 20);
      uint32_t imbalance_qty = xdp::read_le32(data + 24);
      uint8_t imbalance_side = data[28];
      uint32_t indicative_match_price = xdp::read_le32(data + 38);
      if (verbose) {
        std::cout << "      Reference Price: $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(reference_price)
                  << '\n';
        std::cout << "      Paired Quantity: " << paired_qty << '\n';
        std::cout << "      Imbalance Quantity: " << imbalance_qty << '\n';
        std::cout << "      Imbalance Side: "
                  << (imbalance_side == 'B' ? "BUY" : "SELL") << '\n';
        std::cout << "      Indicative Match Price: $" << std::fixed
                  << std::setprecision(4)
                  << xdp::parse_price(indicative_match_price) << '\n';
      } else {
        uint8_t unpaired_side = data[71];
        uint8_t significant_imbalance = data[72];
        std::cout << " RefPrice=$" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(reference_price)
                  << " Paired=" << paired_qty
                  << " Imbalance=" << imbalance_qty
                  << " Side=" << static_cast<char>(imbalance_side)
                  << " IndicativeMatch=$"
                  << xdp::parse_price(indicative_match_price);
        if (unpaired_side != ' ') {
          std::cout << " UnpairedSide=" << static_cast<char>(unpaired_side);
        }
        if (significant_imbalance == 'Y') {
          std::cout << " SignificantImbalance=Y";
        }
      }
    }
    break;
  }

  case 106: { // Add Order Refresh
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::ADD_ORDER_REFRESH) {
      uint64_t order_id = xdp::read_le64(data + 20);
      uint32_t price = xdp::read_le32(data + 28);
      uint32_t volume = xdp::read_le32(data + 32);
      uint8_t side = data[36];
      if (verbose) {
        char firm_id[6] = {0};
        std::memcpy(firm_id, data + 37, 5);
        std::cout << "      OrderID: " << order_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
        std::cout << "      Side: " << (side == 'B' ? "BUY" : "SELL") << '\n';
        std::cout << "      FirmID: '" << firm_id << "'\n";
      } else {
        std::cout << " OrderID=" << order_id << " $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(price) << " "
                  << volume << " " << xdp::get_side_abbr(side);
      }
    }
    break;
  }

  case 110: { // Non-Displayed Trade
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::NON_DISPLAYED_TRADE) {
      uint64_t trade_id = xdp::read_le64(data + 16);
      uint32_t price = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      if (verbose) {
        std::cout << "      TradeID: " << trade_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
      } else {
        std::cout << " TradeID=" << trade_id << " $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(price)
                  << " Qty=" << volume;
      }
    }
    break;
  }

  case 111: { // Cross Trade
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::CROSS_TRADE) {
      uint64_t cross_id = xdp::read_le64(data + 16);
      uint32_t price = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      uint32_t cross_type = xdp::read_le32(data + 32);
      if (verbose) {
        std::cout << "      CrossID: " << cross_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
        std::cout << "      Cross Type: " << cross_type << '\n';
      } else {
        std::cout << " CrossID=" << cross_id << " $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(price)
                  << " Qty=" << volume << " Type=" << cross_type;
      }
    }
    break;
  }

  case 112: { // Trade Cancel
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::TRADE_CANCEL) {
      uint64_t trade_id = xdp::read_le64(data + 16);
      uint32_t price = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      if (verbose) {
        std::cout << "      TradeID: " << trade_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
      } else {
        std::cout << " TradeID=" << trade_id << " $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(price)
                  << " Qty=" << volume;
      }
    }
    break;
  }

  case 113: { // Cross Correction
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::CROSS_CORRECTION) {
      uint64_t cross_id = xdp::read_le64(data + 16);
      uint32_t price = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      uint32_t cross_type = xdp::read_le32(data + 32);
      if (verbose) {
        std::cout << "      CrossID: " << cross_id << '\n';
        std::cout << "      Price: $" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(price) << '\n';
        std::cout << "      Volume: " << volume << '\n';
        std::cout << "      Cross Type: " << cross_type << '\n';
      } else {
        std::cout << " CrossID=" << cross_id << " $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(price)
                  << " Qty=" << volume << " Type=" << cross_type;
      }
    }
    break;
  }

  case 114: { // Retail Price Improvement
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::RETAIL_PRICE_IMPROVEMENT) {
      uint8_t rpi_indicator = data[16];
      if (verbose) {
        std::cout << "      RPI Indicator: ";
        switch (rpi_indicator) {
        case ' ':
          std::cout << "' ' (No retail interest)\n";
          break;
        case 'A':
          std::cout << "'A' (Retail interest on bid side)\n";
          break;
        case 'B':
          std::cout << "'B' (Retail interest on offer side)\n";
          break;
        case 'C':
          std::cout << "'C' (Retail interest on both sides)\n";
          break;
        default:
          std::cout << "'" << static_cast<char>(rpi_indicator)
                    << "' (Unknown)\n";
          break;
        }
      } else {
        std::cout << " RPI=";
        switch (rpi_indicator) {
        case ' ':
          std::cout << "None";
          break;
        case 'A':
          std::cout << "Bid";
          break;
        case 'B':
          std::cout << "Offer";
          break;
        case 'C':
          std::cout << "Both";
          break;
        default:
          std::cout << "'" << static_cast<char>(rpi_indicator) << "'";
          break;
        }
      }
    }
    break;
  }

  case 223: { // Stock Summary
    if (!verbose)
      std::cout << ticker << " " << msg_num;
    if (msg_size >= xdp::MessageSize::STOCK_SUMMARY) {
      uint32_t high_price = xdp::read_le32(data + 16);
      uint32_t low_price = xdp::read_le32(data + 20);
      uint32_t open_price = xdp::read_le32(data + 24);
      uint32_t close_price = xdp::read_le32(data + 28);
      uint32_t total_volume = xdp::read_le32(data + 32);
      if (verbose) {
        std::cout << "      High Price: $" << std::fixed
                  << std::setprecision(4) << xdp::parse_price(high_price)
                  << '\n';
        std::cout << "      Low Price: $" << xdp::parse_price(low_price)
                  << '\n';
        std::cout << "      Open Price: $" << xdp::parse_price(open_price)
                  << '\n';
        std::cout << "      Close Price: $" << xdp::parse_price(close_price)
                  << '\n';
        std::cout << "      Total Volume: " << total_volume << '\n';
      } else {
        std::cout << " High=$" << std::fixed << std::setprecision(4)
                  << xdp::parse_price(high_price)
                  << " Low=$" << xdp::parse_price(low_price)
                  << " Open=$" << xdp::parse_price(open_price)
                  << " Close=$" << xdp::parse_price(close_price)
                  << " Volume=" << total_volume;
      }
    }
    break;
  }

  default:
    if (verbose) {
      std::cout << "      Unknown message type, size: " << msg_size
                << " bytes\n";
    } else {
      std::cout << ticker << " Type=" << msg_type << " Size=" << msg_size;
    }
    break;
  }
}

// Parse and output message in simplified format
void parse_message_simple(const uint8_t *data, size_t max_len,
                          uint32_t packet_send_time,
                          uint32_t packet_send_time_ns) {
  if (max_len < xdp::MESSAGE_HEADER_SIZE)
    return;

  uint16_t msg_size = xdp::read_le16(data);
  uint16_t msg_type = xdp::read_le16(data + 2);

  if (msg_size < xdp::MESSAGE_HEADER_SIZE || msg_size > max_len)
    return;

  std::string ticker;
  uint32_t msg_num = 0;
  uint32_t source_time = packet_send_time;
  uint32_t source_time_ns = packet_send_time_ns;

  // Handle messages with non-standard header structure
  if (xdp::has_non_standard_header(msg_type)) {
    if (msg_size < 16)
      return;
    source_time = xdp::read_le32(data + 4);
    source_time_ns = xdp::read_le32(data + 8);
    uint32_t symbol_index = xdp::read_le32(data + 12);
    ticker = xdp::get_symbol(symbol_index);

    if (!passes_filter(ticker, msg_type))
      return;

    std::cout << xdp::format_time_micro(source_time, source_time_ns) << " "
              << xdp::get_message_type_name(msg_type) << " ";
    msg_num = ++g_symbol_msg_counters[symbol_index];
  } else {
    if (msg_size < xdp::COMMON_MSG_HEADER_SIZE)
      return;
    source_time_ns = xdp::read_le32(data + 4);
    uint32_t symbol_index = xdp::read_le32(data + 8);
    ticker = xdp::get_symbol(symbol_index);

    if (!passes_filter(ticker, msg_type))
      return;

    std::cout << xdp::format_time_micro(packet_send_time, packet_send_time_ns)
              << " " << xdp::get_message_type_name(msg_type) << " ";
    msg_num = ++g_symbol_msg_counters[symbol_index];
  }

  print_message_fields(data, msg_size, msg_type, false, ticker, msg_num);
  std::cout << '\n';
}

// Parse and output message in verbose format
void parse_message_verbose(const uint8_t *data, size_t max_len, int msg_num) {
  if (max_len < xdp::MESSAGE_HEADER_SIZE) {
    std::cout << "  [" << msg_num << "] Too short for message header\n";
    return;
  }

  uint16_t msg_size = xdp::read_le16(data);
  uint16_t msg_type = xdp::read_le16(data + 2);

  std::cout << "  [" << msg_num << "] Type: " << msg_type << " ("
            << xdp::get_message_type_name(msg_type) << ")\n";
  std::cout << "      Size: " << msg_size << " bytes\n";

  if (msg_size > max_len) {
    std::cout << "      ERROR: Message size (" << msg_size
              << ") exceeds remaining data (" << max_len << ")!\n";
    return;
  }

  std::string ticker;

  // Parse common header based on message type
  if (xdp::has_non_standard_header(msg_type)) {
    if (msg_size < 16)
      return;
    uint32_t source_time = xdp::read_le32(data + 4);
    uint32_t source_time_ns = xdp::read_le32(data + 8);
    uint32_t symbol_index = xdp::read_le32(data + 12);
    ticker = xdp::get_symbol(symbol_index);

    if (!passes_filter(ticker, msg_type))
      return;

    std::cout << "      SourceTime: " << source_time << " seconds\n";
    std::cout << "      SourceTimeNS: " << source_time_ns << '\n';
    std::cout << "      SymbolIndex: " << symbol_index << " (" << ticker
              << ")\n";
  } else {
    if (msg_size < xdp::COMMON_MSG_HEADER_SIZE)
      return;
    uint32_t source_time_ns = xdp::read_le32(data + 4);
    uint32_t symbol_index = xdp::read_le32(data + 8);
    uint32_t symbol_seq = xdp::read_le32(data + 12);
    ticker = xdp::get_symbol(symbol_index);

    if (!passes_filter(ticker, msg_type))
      return;

    std::cout << "      SourceTimeNS: " << source_time_ns << '\n';
    std::cout << "      SymbolIndex: " << symbol_index << " (" << ticker
              << ")\n";
    std::cout << "      SymbolSeqNum: " << symbol_seq << '\n';
  }

  print_message_fields(data, msg_size, msg_type, true, ticker, 0);
}

// Parse XDP packet in verbose mode
void parse_packet_verbose(const uint8_t *data, size_t length, uint64_t pkt_num,
                          const xdp::NetworkPacketInfo &info) {
  std::cout << "\n=== Packet " << pkt_num << " ===\n";
  std::cout << "Source: " << info.src_ip << " -> Multicast: " << info.dst_ip
            << ":" << info.dst_port << '\n';
  std::cout << "Total length: " << length << " bytes\n";

  if (length < xdp::PACKET_HEADER_SIZE) {
    std::cout << "ERROR: Packet too short for XDP header\n";
    return;
  }

  xdp::PacketHeader header;
  if (!xdp::parse_packet_header(data, length, header))
    return;

  std::cout << "\nXDP Packet Header:\n";
  std::cout << "  Packet Size: " << header.packet_size << " bytes\n";
  std::cout << "  Delivery Flag: " << static_cast<int>(header.delivery_flag)
            << '\n';
  std::cout << "  Message Count: " << static_cast<int>(header.num_messages)
            << '\n';
  std::cout << "  Sequence Number: " << header.seq_num << '\n';
  std::cout << "  Send Time: "
            << xdp::format_time_micro(header.send_time, header.send_time_ns)
            << '\n';

  std::cout << "\nMessages (" << static_cast<int>(header.num_messages)
            << " expected):\n";

  size_t offset = xdp::PACKET_HEADER_SIZE;
  int msg_count = 0;

  while (offset + xdp::MESSAGE_HEADER_SIZE <= length &&
         msg_count < header.num_messages) {
    parse_message_verbose(data + offset, length - offset, msg_count + 1);

    uint16_t msg_size = xdp::read_le16(data + offset);
    if (msg_size < xdp::MESSAGE_HEADER_SIZE || msg_size > length - offset)
      break;

    offset += msg_size;
    msg_count++;
  }

  std::cout << "\nParsed " << msg_count << " of "
            << static_cast<int>(header.num_messages) << " messages\n";
}

// Parse XDP packet in simple mode
void parse_packet_simple(const uint8_t *data, size_t length, uint64_t,
                         const xdp::NetworkPacketInfo &) {
  if (length < xdp::PACKET_HEADER_SIZE)
    return;

  xdp::PacketHeader header;
  if (!xdp::parse_packet_header(data, length, header))
    return;

  size_t offset = xdp::PACKET_HEADER_SIZE;
  int msg_count = 0;

  while (offset + xdp::MESSAGE_HEADER_SIZE <= length &&
         msg_count < header.num_messages) {
    parse_message_simple(data + offset, length - offset, header.send_time,
                         header.send_time_ns);

    uint16_t msg_size = xdp::read_le16(data + offset);
    if (msg_size < xdp::MESSAGE_HEADER_SIZE || msg_size > length - offset)
      break;

    offset += msg_size;
    msg_count++;
  }
}

void print_usage(const char *program) {
  std::cerr
      << "Usage: " << program
      << " <pcap_file> [verbose] [symbol_file] [-t ticker] [-m message_type]\n"
      << "  verbose: 0 = simplified output (default)\n"
      << "           1 = detailed output with headers\n"
      << "  symbol_file: TXT file with symbol mapping (optional)\n"
      << "  -t ticker: Filter messages for specific ticker symbol (optional)\n"
      << "  -m message_type: Filter messages by type (e.g., ADD_ORDER, "
         "MODIFY_ORDER, etc.)\n\n"
      << "Examples:\n"
      << "  " << program << " nyse_xdp_data.pcap 0 symbols.txt\n"
      << "  " << program << " nyse_xdp_data.pcap 1 symbols.txt\n"
      << "  " << program << " nyse_xdp_data.pcap 0 symbols.txt -t AAPL\n"
      << "  " << program << " nyse_xdp_data.pcap 0 symbols.txt -m ADD_ORDER\n";
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  const char *pcap_file = argv[1];
  const char *symbol_file = nullptr;

  // Parse command line arguments
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "-t") == 0) {
      if (i + 1 < argc) {
        g_filter_ticker = argv[++i];
      } else {
        std::cerr << "Error: -t requires a ticker symbol\n";
        return 1;
      }
    } else if (std::strcmp(argv[i], "-m") == 0 ||
               std::strcmp(argv[i], "--message") == 0) {
      if (i + 1 < argc) {
        g_filter_message = argv[++i];
      } else {
        std::cerr << "Error: -m requires a message type\n";
        return 1;
      }
    } else if (std::strcmp(argv[i], "0") == 0 ||
               std::strcmp(argv[i], "1") == 0) {
      g_verbose_mode = std::atoi(argv[i]);
    } else if (symbol_file == nullptr) {
      symbol_file = argv[i];
    }
  }

  // Load symbol mapping if provided
  if (symbol_file) {
    xdp::load_symbol_map(symbol_file);
  }

  // Open PCAP file
  xdp::PcapReader reader;
  if (!reader.open(pcap_file)) {
    std::cerr << "Error opening pcap file: " << reader.error() << '\n';
    return 1;
  }

  // Print header
  if (g_verbose_mode) {
    std::cout << "Parsing NYSE XDP Market Data from: " << pcap_file << '\n';
    std::cout << "Mode: VERBOSE\n";
    std::cout << "Symbols loaded: " << xdp::get_global_symbol_map().size()
              << '\n';
    if (!g_filter_ticker.empty()) {
      std::cout << "Filtering for ticker: " << g_filter_ticker << '\n';
    }
    if (!g_filter_message.empty()) {
      std::cout << "Filtering for message type: " << g_filter_message << '\n';
    }
    std::cout << "==================================================\n";
  } else {
    std::cout << "Parsing NYSE XDP Market Data\n";
    if (symbol_file) {
      std::cout << "Using symbol mapping from: " << symbol_file << '\n';
    }
    if (!g_filter_ticker.empty()) {
      std::cout << "Filtering for ticker: " << g_filter_ticker << '\n';
    }
    if (!g_filter_message.empty()) {
      std::cout << "Filtering for message type: " << g_filter_message << '\n';
    }
    std::cout << "Format: Time Type Ticker [Price Qty Side]\n";
    std::cout << "================================================\n";
  }

  // Process packets
  auto callback = g_verbose_mode ? parse_packet_verbose : parse_packet_simple;
  int result = reader.process_all(callback);

  if (result < 0) {
    std::cerr << "Error reading packets: " << reader.error() << '\n';
    return 1;
  }

  std::cout << "\nParsing complete\n";
  return 0;
}
