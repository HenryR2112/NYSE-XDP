#include "market_maker.hpp"
#include "order_book.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <pcap.h>
#include <sstream>
#include <unordered_map>
#include <vector>

std::unordered_map<uint32_t, std::string> symbol_map;
std::string filter_ticker = "";

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

double parse_price(uint32_t price_raw) {
  // NYSE XDP prices are typically in ten-thousandths (1/10000) of a dollar.
  // Some captures/sources appear to be scaled 100x higher; apply a safe
  // heuristic so equities like AAPL don't show up as $17,000+.
  double p = static_cast<double>(price_raw) / 10000.0;
  if (p > 10000.0) {
    p = static_cast<double>(price_raw) / 1000000.0;
  }
  return p;
}

static uint32_t read_symbol_index(uint16_t msg_type, const uint8_t *data,
                                  size_t max_len) {
  // Handle messages with non-standard header structure (106, 223)
  if (msg_type == 106 || msg_type == 223) {
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

std::string get_symbol(uint32_t symbol_index) {
  auto it = symbol_map.find(symbol_index);
  if (it != symbol_map.end()) {
    return it->second;
  }
  return "UNKNOWN";
}

void load_symbol_map(const char *filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Warning: Could not open symbol file: " << filename
              << std::endl;
    return;
  }

  std::string line;
  int count = 0;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;

    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    while (!line.empty() && std::isspace(line.back())) {
      line.pop_back();
    }

    if (line.empty())
      continue;

    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(line);

    while (std::getline(iss, token, '|')) {
      tokens.push_back(token);
    }

    if (tokens.size() >= 3) {
      try {
        uint32_t index = std::stoul(tokens[2]);
        std::string symbol = tokens[0];
        symbol_map[index] = symbol;
        count++;
      } catch (const std::exception &e) {
        continue;
      }
    }
  }

  std::cout << "Loaded " << symbol_map.size() << " symbols" << std::endl;
}

struct PerSymbolSim {
  OrderBook order_book_baseline;
  OrderBook order_book_toxicity;
  MarketMakerStrategy mm_baseline;
  MarketMakerStrategy mm_toxicity;

  // Exchange order_id -> resting side ('B'/'S') for that symbol
  std::unordered_map<uint64_t, char> order_side;

  PerSymbolSim()
      : order_book_baseline(), order_book_toxicity(),
        mm_baseline(order_book_baseline, false),
        mm_toxicity(order_book_toxicity, true), order_side() {}

  void update_quotes() {
    mm_baseline.update_market_data();
    mm_toxicity.update_market_data();
  }

  void on_add(uint64_t order_id, double price, uint32_t volume, char side) {
    order_side[order_id] = side;
    order_book_baseline.add_order(order_id, price, volume, side);
    order_book_toxicity.add_order(order_id, price, volume, side);
  }

  void on_modify(uint64_t order_id, double price, uint32_t volume) {
    order_book_baseline.modify_order(order_id, price, volume);
    order_book_toxicity.modify_order(order_id, price, volume);
  }

  void on_delete(uint64_t order_id) {
    order_side.erase(order_id);
    order_book_baseline.delete_order(order_id);
    order_book_toxicity.delete_order(order_id);
  }

  void on_replace(uint64_t old_order_id, uint64_t new_order_id, double price,
                  uint32_t volume, char side) {
    order_side.erase(old_order_id);
    order_side[new_order_id] = side;

    order_book_baseline.delete_order(old_order_id);
    order_book_baseline.add_order(new_order_id, price, volume, side);
    order_book_toxicity.delete_order(old_order_id);
    order_book_toxicity.add_order(new_order_id, price, volume, side);
  }

  void maybe_fill_on_execution(char resting_side, double exec_price,
                               uint32_t exec_qty) {
    // "Paper MM" fill model: if the execution price would have crossed our
    // quoted price, count a fill (up to our quote size).
    update_quotes();

    const MarketMakerQuote q_base = mm_baseline.get_current_quotes();
    const MarketMakerQuote q_tox = mm_toxicity.get_current_quotes();

    if (resting_side == 'B') {
      if (q_base.bid_price > 0.0 && q_base.bid_size > 0 &&
          q_base.bid_price >= exec_price) {
        mm_baseline.on_order_filled(true, exec_price,
                                    std::min(exec_qty, q_base.bid_size));
      }
      if (q_tox.bid_price > 0.0 && q_tox.bid_size > 0 &&
          q_tox.bid_price >= exec_price) {
        mm_toxicity.on_order_filled(true, exec_price,
                                    std::min(exec_qty, q_tox.bid_size));
      }
      return;
    }

    if (resting_side == 'S') {
      if (q_base.ask_price > 0.0 && q_base.ask_size > 0 &&
          q_base.ask_price <= exec_price) {
        mm_baseline.on_order_filled(false, exec_price,
                                    std::min(exec_qty, q_base.ask_size));
      }
      if (q_tox.ask_price > 0.0 && q_tox.ask_size > 0 &&
          q_tox.ask_price <= exec_price) {
        mm_toxicity.on_order_filled(false, exec_price,
                                    std::min(exec_qty, q_tox.ask_size));
      }
    }
  }

  void on_execute(uint64_t order_id, uint32_t exec_qty, double exec_price) {
    auto it = order_side.find(order_id);
    if (it != order_side.end()) {
      maybe_fill_on_execution(it->second, exec_price, exec_qty);
    }

    order_book_baseline.execute_order(order_id, exec_qty, exec_price);
    order_book_toxicity.execute_order(order_id, exec_qty, exec_price);
    // No need to recompute quotes here; our paper-fill decision happens
    // immediately around the execution event.
  }
};

static std::unordered_map<uint32_t, PerSymbolSim> sims;
static uint64_t total_executions = 0;

void process_xdp_message(const uint8_t *data, size_t max_len,
                         uint16_t msg_type) {
  if (max_len < 4)
    return;

  uint32_t symbol_index = read_symbol_index(msg_type, data, max_len);
  if (symbol_index == 0)
    return;
  std::string ticker = get_symbol(symbol_index);

  if (!filter_ticker.empty() && ticker != filter_ticker) {
    return;
  }

  PerSymbolSim &sim = sims[symbol_index];

  switch (msg_type) {
  case 100: {
    if (max_len >= 39) {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      uint8_t side = data[32];
      double price = parse_price(price_raw);
      char side_char = (side == 'B' || side == 1) ? 'B' : 'S';

      sim.on_add(order_id, price, volume, side_char);
    }
    break;
  }

  case 101: {
    if (max_len >= 35) {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      double price = parse_price(price_raw);

      sim.on_modify(order_id, price, volume);
    }
    break;
  }

  case 102: {
    if (max_len >= 25) {
      uint64_t order_id = read_le64(data + 16);
      sim.on_delete(order_id);
    }
    break;
  }

  case 103: {
    if (max_len >= 42) {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 28);
      uint32_t volume = read_le32(data + 32);
      double price = parse_price(price_raw);

      total_executions++;
      sim.on_execute(order_id, volume, price);
    }
    break;
  }

  case 104: {
    if (max_len >= 42) {
      uint64_t old_order_id = read_le64(data + 16);
      uint64_t new_order_id = read_le64(data + 24);
      uint32_t price_raw = read_le32(data + 32);
      uint32_t volume = read_le32(data + 36);
      double price = parse_price(price_raw);
      uint8_t side = data[40];
      char side_char = (side == 'B' || side == 1) ? 'B' : 'S';

      sim.on_replace(old_order_id, new_order_id, price, volume, side_char);
    }
    break;
  }
  }
}

void parse_xdp_packet(const uint8_t *data, size_t length) {
  if (length < 16)
    return;

  uint8_t num_messages = data[3];
  size_t offset = 16;

  for (uint8_t i = 0; i < num_messages && offset < length; i++) {
    if (offset + 2 > length)
      break;

    uint16_t msg_size = read_le16(data + offset);
    if (msg_size < 2 || offset + msg_size > length)
      break;

    uint16_t msg_type = read_le16(data + offset + 2);
    process_xdp_message(data + offset, msg_size, msg_type);

    offset += msg_size;
  }
}

void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr,
                    const u_char *packet) {
  (void)user_data;

  if (pkthdr->caplen < 14)
    return;

  uint16_t eth_type = ntohs(*(uint16_t *)(packet + 12));
  size_t eth_header_len = 14;

  if (eth_type == 0x8100 || eth_type == 0x88A8) {
    if (pkthdr->caplen < 18)
      return;
    eth_type = ntohs(*(uint16_t *)(packet + 16));
    eth_header_len = 18;
  }

  if (eth_type != 0x0800)
    return;

  if (pkthdr->caplen < eth_header_len + 20)
    return;
  const uint8_t *ip_header = packet + eth_header_len;
  uint8_t ip_ver_ihl = ip_header[0];
  uint8_t ip_header_len = (ip_ver_ihl & 0x0F) * 4;
  uint8_t protocol = ip_header[9];

  if (protocol != 17)
    return;

  size_t udp_offset = eth_header_len + ip_header_len;
  if (pkthdr->caplen < udp_offset + 8)
    return;

  const uint8_t *udp_header = packet + udp_offset;
  uint16_t udp_len = ntohs(*(uint16_t *)(udp_header + 4));

  const uint8_t *udp_payload = udp_header + 8;
  size_t payload_len = udp_len - 8;

  if (payload_len > pkthdr->caplen - udp_offset - 8) {
    payload_len = pkthdr->caplen - udp_offset - 8;
  }

  if (payload_len < 16)
    return;

  parse_xdp_packet(udp_payload, payload_len);
}

void print_results() {
  struct Row {
    uint32_t symbol_index;
    std::string ticker;
    double baseline_total;
    double toxicity_total;
    double improvement;
    int64_t baseline_fills;
    int64_t toxicity_fills;
  };

  std::vector<Row> rows;
  rows.reserve(sims.size());

  double portfolio_baseline = 0.0;
  double portfolio_toxicity = 0.0;

  for (const auto &kv : sims) {
    const uint32_t symbol_index = kv.first;
    const PerSymbolSim &sim = kv.second;

    const MarketMakerStats baseline_stats = sim.mm_baseline.get_stats();
    const MarketMakerStats toxicity_stats = sim.mm_toxicity.get_stats();

    const double baseline_total =
        baseline_stats.realized_pnl + baseline_stats.unrealized_pnl;
    const double toxicity_total =
        toxicity_stats.realized_pnl + toxicity_stats.unrealized_pnl;
    const double improvement = toxicity_total - baseline_total;

    portfolio_baseline += baseline_total;
    portfolio_toxicity += toxicity_total;

    rows.push_back(Row{symbol_index, get_symbol(symbol_index), baseline_total,
                       toxicity_total, improvement, baseline_stats.total_fills,
                       toxicity_stats.total_fills});
  }

  std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
    return a.improvement > b.improvement;
  });

  const double portfolio_improvement = portfolio_toxicity - portfolio_baseline;
  const double portfolio_improvement_pct =
      portfolio_baseline != 0.0
          ? (portfolio_improvement / std::abs(portfolio_baseline)) * 100.0
          : 0.0;

  std::cout << "\n=== MARKET MAKER PORTFOLIO RESULTS ===" << std::endl;
  std::cout << "Symbols simulated: " << rows.size() << std::endl;
  std::cout << "Total executions processed: " << total_executions << std::endl;

  std::cout << "\n--- PORTFOLIO TOTALS ---" << std::endl;
  std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
            << portfolio_baseline << std::endl;
  std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
            << portfolio_toxicity << std::endl;
  std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
            << portfolio_improvement << " (" << std::fixed
            << std::setprecision(2) << portfolio_improvement_pct << "%)"
            << std::endl;

  std::cout << "\n--- TOP 5 SYMBOLS BY IMPROVEMENT (Toxicity - Baseline) ---"
            << std::endl;
  const size_t top_n = std::min<size_t>(5, rows.size());
  for (size_t i = 0; i < top_n; i++) {
    const Row &r = rows[i];
    std::cout << (i + 1) << ". " << r.ticker << " (index " << r.symbol_index
              << "): $" << std::fixed << std::setprecision(2) << r.improvement
              << " | baseline $" << r.baseline_total << " | tox $"
              << r.toxicity_total << " | fills " << r.baseline_fills << " vs "
              << r.toxicity_fills << std::endl;
  }

  if (!filter_ticker.empty() && rows.size() == 1) {
    const Row &r = rows[0];
    std::cout << "\n--- SINGLE SYMBOL DETAIL (" << r.ticker << ") ---"
              << std::endl;
    std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
              << r.baseline_total << std::endl;
    std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
              << r.toxicity_total << std::endl;
    std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
              << r.improvement << std::endl;
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0]
              << " <pcap_file> [symbol_file] [-t ticker]" << std::endl;
    return 1;
  }

  std::string pcap_file = argv[1];
  std::string symbol_file = "data/symbol_nyse.txt";

  for (int i = 2; i < argc; i++) {
    if (std::string(argv[i]) == "-t" && i + 1 < argc) {
      filter_ticker = argv[i + 1];
      i++;
    } else {
      symbol_file = argv[i];
    }
  }

  load_symbol_map(symbol_file.c_str());

  if (!filter_ticker.empty()) {
    std::cout << "Filtering for ticker: " << filter_ticker << std::endl;
  }

  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t *handle = pcap_open_offline(pcap_file.c_str(), errbuf);

  if (handle == nullptr) {
    std::cerr << "Error opening PCAP file: " << errbuf << std::endl;
    return 1;
  }

  std::cout << "Processing PCAP file: " << pcap_file << std::endl;
  std::cout << "Running baseline and toxicity-aware strategies..." << std::endl;

  uint64_t packet_count = 0;
  struct pcap_pkthdr header;
  const u_char *packet;

  while ((packet = pcap_next(handle, &header)) != nullptr) {
    packet_handler(nullptr, &header, packet);
    packet_count++;

    if (packet_count % 100000 == 0) {
      std::cout << "Processed " << packet_count << " packets..." << std::endl;
    }
  }

  pcap_close(handle);

  std::cout << "\nFinished processing " << packet_count << " packets"
            << std::endl;

  print_results();

  return 0;
}
