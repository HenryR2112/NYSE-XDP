// market_maker_sim.cpp - Market Maker Simulation with PCAP playback
// Simulates market making strategies on historical XDP data

#include "common/pcap_reader.hpp"
#include "common/symbol_map.hpp"
#include "common/xdp_types.hpp"
#include "common/xdp_utils.hpp"
#include "market_maker.hpp"
#include "order_book.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

std::string g_filter_ticker;

struct ExecutionModelConfig {
  uint64_t seed = 1;
  double latency_ms_mean = 0.0;
  double latency_ms_jitter = 0.0;
  uint32_t queue_ahead_mean = 0;
  double queue_ahead_cv = 0.0;
  double miss_prob = 0.0;
  double fee_per_share = 0.0;
  enum class FillMode { Cross, Match } fill_mode = FillMode::Cross;
  uint64_t quote_interval_ns = 0;
};

ExecutionModelConfig g_exec;

struct VirtualOrder {
  double price = 0.0;
  uint32_t size = 0;
  uint32_t remaining = 0;
  uint64_t active_at_ns = 0;
  uint32_t queue_ahead = 0;
  bool live = false;
};

struct StrategyExecState {
  VirtualOrder bid;
  VirtualOrder ask;
};

struct PerSymbolSim {
  OrderBook order_book_baseline;
  OrderBook order_book_toxicity;
  MarketMakerStrategy mm_baseline;
  MarketMakerStrategy mm_toxicity;

  std::unordered_map<uint64_t, char> order_side;

  bool initialized = false;
  uint32_t symbol_index = 0;
  std::mt19937_64 rng;
  std::normal_distribution<double> latency_ms_dist;
  std::lognormal_distribution<double> queue_ahead_logn_dist;
  std::uniform_real_distribution<double> uni01;

  StrategyExecState baseline_state;
  StrategyExecState toxicity_state;
  uint64_t last_quote_update_ns = 0;

  PerSymbolSim()
      : order_book_baseline(), order_book_toxicity(),
        mm_baseline(order_book_baseline, false),
        mm_toxicity(order_book_toxicity, true), order_side(),
        latency_ms_dist(0.0, 1.0), queue_ahead_logn_dist(0.0, 1.0),
        uni01(0.0, 1.0) {}

  void ensure_init(uint32_t idx) {
    if (initialized)
      return;
    initialized = true;
    symbol_index = idx;

    const uint64_t seed =
        g_exec.seed ^ (static_cast<uint64_t>(idx) * 0x9E3779B97F4A7C15ULL);
    rng.seed(seed);

    const double jitter = std::max(0.0, g_exec.latency_ms_jitter);
    latency_ms_dist =
        std::normal_distribution<double>(g_exec.latency_ms_mean, jitter);

    const double m =
        std::max(1.0, static_cast<double>(g_exec.queue_ahead_mean));
    const double c = std::max(0.0, g_exec.queue_ahead_cv);
    const double sigma2 = std::log(1.0 + c * c);
    const double sigma = std::sqrt(sigma2);
    const double mu = std::log(m) - 0.5 * sigma2;
    queue_ahead_logn_dist = std::lognormal_distribution<double>(mu, sigma);

    mm_baseline.set_fee_per_share(g_exec.fee_per_share);
    mm_toxicity.set_fee_per_share(g_exec.fee_per_share);
  }

  uint64_t sample_latency_ns() {
    double ms = latency_ms_dist(rng);
    if (ms < 0.0)
      ms = 0.0;
    return static_cast<uint64_t>(ms * 1000000.0);
  }

  uint32_t sample_queue_ahead() {
    double v = queue_ahead_logn_dist(rng);
    if (v < 0.0)
      v = 0.0;
    if (v > static_cast<double>(std::numeric_limits<uint32_t>::max()))
      v = static_cast<double>(std::numeric_limits<uint32_t>::max());
    return static_cast<uint32_t>(v);
  }

  bool eligible_for_fill(double quote_px, double exec_px,
                         bool is_bid_side) const {
    if (g_exec.fill_mode == ExecutionModelConfig::FillMode::Match) {
      return std::abs(quote_px - exec_px) < 1e-12;
    }
    return is_bid_side ? (quote_px >= exec_px) : (quote_px <= exec_px);
  }

  void update_virtual_order(VirtualOrder &vo, double price, uint32_t size,
                            uint64_t now_ns) {
    const bool changed = (!vo.live) || (vo.price != price) ||
                         (vo.size != size) || (vo.remaining == 0);
    if (!changed)
      return;
    vo.price = price;
    vo.size = size;
    vo.remaining = size;
    vo.queue_ahead = sample_queue_ahead();
    vo.active_at_ns = now_ns + sample_latency_ns();
    vo.live = (price > 0.0 && size > 0);
  }

  void update_quotes(uint64_t now_ns) {
    if (now_ns - last_quote_update_ns < g_exec.quote_interval_ns)
      return;
    last_quote_update_ns = now_ns;

    mm_baseline.update_market_data();
    mm_toxicity.update_market_data();

    const MarketMakerQuote q_base = mm_baseline.get_current_quotes();
    const MarketMakerQuote q_tox = mm_toxicity.get_current_quotes();

    update_virtual_order(baseline_state.bid, q_base.bid_price, q_base.bid_size,
                         now_ns);
    update_virtual_order(baseline_state.ask, q_base.ask_price, q_base.ask_size,
                         now_ns);

    update_virtual_order(toxicity_state.bid, q_tox.bid_price, q_tox.bid_size,
                         now_ns);
    update_virtual_order(toxicity_state.ask, q_tox.ask_price, q_tox.ask_size,
                         now_ns);
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

  void try_fill_one(MarketMakerStrategy &mm, StrategyExecState &st,
                    bool is_bid_side, double exec_price, uint32_t exec_qty,
                    uint64_t now_ns) {
    VirtualOrder &vo = is_bid_side ? st.bid : st.ask;
    if (!vo.live || vo.remaining == 0)
      return;
    if (now_ns < vo.active_at_ns)
      return;
    if (!eligible_for_fill(vo.price, exec_price, is_bid_side))
      return;

    uint32_t qty_left = exec_qty;
    if (vo.queue_ahead > 0) {
      const uint32_t consume = std::min(vo.queue_ahead, qty_left);
      vo.queue_ahead -= consume;
      qty_left -= consume;
    }
    if (qty_left == 0)
      return;

    const double u = uni01(rng);
    if (u < std::max(0.0, std::min(1.0, g_exec.miss_prob)))
      return;

    const uint32_t fill_qty = std::min(vo.remaining, qty_left);
    if (fill_qty == 0)
      return;

    vo.remaining -= fill_qty;
    mm.on_order_filled(is_bid_side, vo.price, fill_qty);
  }

  void maybe_fill_on_execution(char resting_side, double exec_price,
                               uint32_t exec_qty, uint64_t now_ns) {
    update_quotes(now_ns);

    if (resting_side == 'B') {
      try_fill_one(mm_baseline, baseline_state, true, exec_price, exec_qty,
                   now_ns);
      try_fill_one(mm_toxicity, toxicity_state, true, exec_price, exec_qty,
                   now_ns);
      return;
    }

    if (resting_side == 'S') {
      try_fill_one(mm_baseline, baseline_state, false, exec_price, exec_qty,
                   now_ns);
      try_fill_one(mm_toxicity, toxicity_state, false, exec_price, exec_qty,
                   now_ns);
    }
  }

  void on_execute(uint64_t order_id, uint32_t exec_qty, double exec_price,
                  uint64_t now_ns) {
    auto it = order_side.find(order_id);
    if (it != order_side.end()) {
      maybe_fill_on_execution(it->second, exec_price, exec_qty, now_ns);
    }

    order_book_baseline.execute_order(order_id, exec_qty, exec_price);
    order_book_toxicity.execute_order(order_id, exec_qty, exec_price);
  }
};

std::unordered_map<uint32_t, PerSymbolSim> g_sims;
uint64_t g_total_executions = 0;

void process_xdp_message(const uint8_t *data, size_t max_len, uint16_t msg_type,
                         uint64_t now_ns) {
  if (max_len < xdp::MESSAGE_HEADER_SIZE)
    return;

  uint32_t symbol_index = xdp::read_symbol_index(msg_type, data, max_len);
  if (symbol_index == 0)
    return;

  std::string ticker = xdp::get_symbol(symbol_index);
  if (!g_filter_ticker.empty() && ticker != g_filter_ticker)
    return;

  PerSymbolSim &sim = g_sims[symbol_index];
  sim.ensure_init(symbol_index);

  switch (msg_type) {
  case static_cast<uint16_t>(xdp::MessageType::ADD_ORDER): {
    if (max_len >= xdp::MessageSize::ADD_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t price_raw = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      uint8_t side = data[32];
      double price = xdp::parse_price(price_raw);
      char side_char = xdp::side_to_char(xdp::parse_side(side));
      sim.on_add(order_id, price, volume, side_char);
    }
    break;
  }

  case static_cast<uint16_t>(xdp::MessageType::MODIFY_ORDER): {
    if (max_len >= xdp::MessageSize::MODIFY_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t price_raw = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      double price = xdp::parse_price(price_raw);
      sim.on_modify(order_id, price, volume);
    }
    break;
  }

  case static_cast<uint16_t>(xdp::MessageType::DELETE_ORDER): {
    if (max_len >= xdp::MessageSize::DELETE_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      sim.on_delete(order_id);
    }
    break;
  }

  case static_cast<uint16_t>(xdp::MessageType::EXECUTE_ORDER): {
    if (max_len >= xdp::MessageSize::EXECUTE_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t price_raw = xdp::read_le32(data + 28);
      uint32_t volume = xdp::read_le32(data + 32);
      double price = xdp::parse_price(price_raw);
      g_total_executions++;
      sim.on_execute(order_id, volume, price, now_ns);
    }
    break;
  }

  case static_cast<uint16_t>(xdp::MessageType::REPLACE_ORDER): {
    if (max_len >= xdp::MessageSize::REPLACE_ORDER) {
      uint64_t old_order_id = xdp::read_le64(data + 16);
      uint64_t new_order_id = xdp::read_le64(data + 24);
      uint32_t price_raw = xdp::read_le32(data + 32);
      uint32_t volume = xdp::read_le32(data + 36);
      double price = xdp::parse_price(price_raw);
      uint8_t side = data[40];
      char side_char = xdp::side_to_char(xdp::parse_side(side));
      sim.on_replace(old_order_id, new_order_id, price, volume, side_char);
    }
    break;
  }
  default:
    break;
  }
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
  rows.reserve(g_sims.size());

  double portfolio_baseline = 0.0;
  double portfolio_toxicity = 0.0;

  for (const auto &kv : g_sims) {
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

    rows.push_back(Row{symbol_index, xdp::get_symbol(symbol_index),
                       baseline_total, toxicity_total, improvement,
                       baseline_stats.total_fills, toxicity_stats.total_fills});
  }

  std::sort(rows.begin(), rows.end(),
            [](const Row &a, const Row &b) { return a.improvement > b.improvement; });

  const double portfolio_improvement = portfolio_toxicity - portfolio_baseline;
  const double portfolio_improvement_pct =
      portfolio_baseline != 0.0
          ? (portfolio_improvement / std::abs(portfolio_baseline)) * 100.0
          : 0.0;

  std::cout << "\n=== MARKET MAKER PORTFOLIO RESULTS ===\n";
  std::cout << "Symbols simulated: " << rows.size() << '\n';
  std::cout << "Total executions processed: " << g_total_executions << '\n';

  std::cout << "\n--- PORTFOLIO TOTALS ---\n";
  std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
            << portfolio_baseline << '\n';
  std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
            << portfolio_toxicity << '\n';
  std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
            << portfolio_improvement << " (" << std::fixed
            << std::setprecision(2) << portfolio_improvement_pct << "%)\n";

  std::cout << "\n--- TOP 5 SYMBOLS BY IMPROVEMENT ---\n";
  const size_t top_n = std::min<size_t>(5, rows.size());
  for (size_t i = 0; i < top_n; i++) {
    const Row &r = rows[i];
    std::cout << (i + 1) << ". " << r.ticker << " (index " << r.symbol_index
              << "): $" << std::fixed << std::setprecision(2) << r.improvement
              << " | baseline $" << r.baseline_total << " | tox $"
              << r.toxicity_total << " | fills " << r.baseline_fills << " vs "
              << r.toxicity_fills << '\n';
  }

  if (!g_filter_ticker.empty() && rows.size() == 1) {
    const Row &r = rows[0];
    std::cout << "\n--- SINGLE SYMBOL DETAIL (" << r.ticker << ") ---\n";
    std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
              << r.baseline_total << '\n';
    std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
              << r.toxicity_total << '\n';
    std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
              << r.improvement << '\n';
  }
}

void print_usage(const char *program) {
  std::cerr << "Usage: " << program
            << " <pcap_file> [symbol_file] [-t ticker] "
               "[--seed N] [--latency-ms M] [--latency-jitter-ms J] "
               "[--queue-ahead MEAN] [--queue-ahead-cv CV] "
               "[--miss-prob P] [--fee-per-share F] "
               "[--fill-mode cross|match] [--quote-interval-ms Q]\n";
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string pcap_file = argv[1];
  std::string symbol_file = "data/symbol_nyse.txt";

  for (int i = 2; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "-t" && i + 1 < argc) {
      g_filter_ticker = argv[++i];
    } else if (arg == "--seed" && i + 1 < argc) {
      g_exec.seed = std::stoull(argv[++i]);
    } else if (arg == "--latency-ms" && i + 1 < argc) {
      g_exec.latency_ms_mean = std::stod(argv[++i]);
    } else if (arg == "--latency-jitter-ms" && i + 1 < argc) {
      g_exec.latency_ms_jitter = std::stod(argv[++i]);
    } else if (arg == "--queue-ahead" && i + 1 < argc) {
      g_exec.queue_ahead_mean = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (arg == "--queue-ahead-cv" && i + 1 < argc) {
      g_exec.queue_ahead_cv = std::stod(argv[++i]);
    } else if (arg == "--miss-prob" && i + 1 < argc) {
      g_exec.miss_prob = std::stod(argv[++i]);
    } else if (arg == "--fee-per-share" && i + 1 < argc) {
      g_exec.fee_per_share = std::stod(argv[++i]);
    } else if (arg == "--fill-mode" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode == "match") {
        g_exec.fill_mode = ExecutionModelConfig::FillMode::Match;
      } else {
        g_exec.fill_mode = ExecutionModelConfig::FillMode::Cross;
      }
    } else if (arg == "--quote-interval-ms" && i + 1 < argc) {
      const double ms = std::max(0.0, std::stod(argv[++i]));
      g_exec.quote_interval_ns = static_cast<uint64_t>(ms * 1000000.0);
    } else {
      symbol_file = argv[i];
    }
  }

  (void)xdp::load_symbol_map(symbol_file);

  if (!g_filter_ticker.empty()) {
    std::cout << "Filtering for ticker: " << g_filter_ticker << '\n';
  }

  xdp::PcapReader reader;
  if (!reader.open(pcap_file)) {
    std::cerr << "Error opening PCAP file: " << reader.error() << '\n';
    return 1;
  }

  std::cout << "Processing PCAP file: " << pcap_file << '\n';
  std::cout << "Running baseline and toxicity-aware strategies...\n";

  uint64_t packet_count = 0;

  auto packet_callback = [&packet_count](const uint8_t *data, size_t length,
                                          uint64_t, const xdp::NetworkPacketInfo &info) {
    packet_count++;

    if (length < xdp::PACKET_HEADER_SIZE)
      return;

    xdp::PacketHeader pkt_header;
    if (!xdp::parse_packet_header(data, length, pkt_header))
      return;

    size_t offset = xdp::PACKET_HEADER_SIZE;

    for (uint8_t i = 0; i < pkt_header.num_messages && offset < length; i++) {
      if (offset + xdp::MESSAGE_HEADER_SIZE > length)
        break;

      uint16_t msg_size = xdp::read_le16(data + offset);
      if (msg_size < xdp::MESSAGE_HEADER_SIZE || offset + msg_size > length)
        break;

      uint16_t msg_type = xdp::read_le16(data + offset + 2);
      process_xdp_message(data + offset, msg_size, msg_type, info.timestamp_ns);

      offset += msg_size;
    }

    if (packet_count % 100000 == 0) {
      std::cout << "Processed " << packet_count << " packets...\n";
    }
  };

  reader.process_all(packet_callback);

  std::cout << "\nFinished processing " << packet_count << " packets\n";

  print_results();

  return 0;
}
