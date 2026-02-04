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

// =============================================================================
// HFT Market Maker Execution Model
// Assumes: Colocation at exchange, bottom-tier HFT infrastructure
// =============================================================================

struct ExecutionModelConfig {
  uint64_t seed = 42;

  // --- Latency Model (HFT Colocation) ---
  // 20μs one-way = network + matching engine
  // 5μs jitter from kernel scheduling, NIC, etc.
  double latency_us_mean = 20.0;        // 20 microseconds one-way
  double latency_us_jitter = 5.0;       // Low jitter with colo
  uint64_t quote_update_interval_us = 50;  // Can update quotes every 50μs

  // --- Queue Position Model ---
  // At a liquid stock's NBBO, typical queue depth is 1000-5000 shares
  // Assume we're a decent-tier HFT with reasonable queue position
  double queue_position_fraction = 0.2; // We're ~20% back in queue (better infra)
  double queue_position_variance = 0.4; // Moderate variance

  // --- Adverse Selection Model ---
  // After we get filled, price often moves against us (informed flow)
  // Lookforward window to measure adverse selection
  // Note: Not all adverse movement is realized as loss if we can exit
  uint64_t adverse_lookforward_us = 500;   // 500μs lookforward (faster measurement)
  double adverse_selection_multiplier = 0.10; // 10% of adverse move charged (aggressive hedging)

  // --- Quote Exposure Window ---
  // During cancel-replace, our stale quote is exposed to adverse selection
  // exposure_window = round_trip_latency = 2 * one_way_latency
  uint64_t quote_exposure_window_us = 50;  // 50μs exposure during updates

  // --- Fee/Rebate Structure (NYSE Tier 1 maker) ---
  double maker_rebate_per_share = 0.002;   // Receive $0.002/share (top-tier)
  double taker_fee_per_share = 0.003;      // Pay $0.003/share if we cross
  double clearing_fee_per_share = 0.00015; // Clearing + regulatory fees (lower tier)

  // --- Risk Limits ---
  double max_position_per_symbol = 2500.0; // Max 2500 shares per symbol (5 fills of 500)
  double max_daily_loss_per_symbol = 500.0; // Stop trading after $500 loss
  double max_portfolio_loss = 50000.0;     // Kill switch at $50k loss

  // --- Symbol Selection Criteria ---
  // Profitable spread requirements
  double min_spread_to_trade = 0.03;       // Only trade if spread >= $0.03 (good edge)
  double max_spread_to_trade = 0.06;       // Moderate max - balance liquidity/edge
  uint32_t min_depth_to_trade = 400;       // Moderate depth requirement

  // Legacy compatibility
  enum class FillMode { Cross, Match } fill_mode = FillMode::Cross;
};

ExecutionModelConfig g_exec;

struct VirtualOrder {
  double price = 0.0;
  uint32_t size = 0;
  uint32_t remaining = 0;
  uint64_t active_at_ns = 0;       // When order becomes active (after latency)
  uint64_t exposed_until_ns = 0;  // Stale quote exposure window
  uint32_t queue_ahead = 0;
  bool live = false;
};

struct StrategyExecState {
  VirtualOrder bid;
  VirtualOrder ask;
};

// Track fills for adverse selection measurement
struct FillRecord {
  uint64_t fill_time_ns;
  double fill_price;
  uint32_t fill_qty;
  bool is_buy;
  double mid_price_at_fill;
  bool adverse_measured = false;
  double adverse_pnl = 0.0;
};

// Per-symbol risk state
struct SymbolRiskState {
  double realized_pnl = 0.0;
  double unrealized_pnl = 0.0;
  bool halted = false;  // Stopped due to loss limit
  int64_t total_fills = 0;
  double total_adverse_pnl = 0.0;
  int64_t adverse_fills = 0;
};

struct PerSymbolSim {
  OrderBook order_book_baseline;
  OrderBook order_book_toxicity;
  MarketMakerStrategy mm_baseline;
  MarketMakerStrategy mm_toxicity;

  std::unordered_map<uint64_t, char> order_side;

  bool initialized = false;
  bool eligible_to_trade = true;  // Passes symbol selection criteria
  uint32_t symbol_index = 0;
  std::mt19937_64 rng;
  std::normal_distribution<double> latency_us_dist;
  std::uniform_real_distribution<double> uni01;

  StrategyExecState baseline_state;
  StrategyExecState toxicity_state;
  uint64_t last_quote_update_ns = 0;

  // Risk tracking
  SymbolRiskState baseline_risk;
  SymbolRiskState toxicity_risk;

  // Adverse selection tracking - store recent fills to measure post-fill movement
  std::vector<FillRecord> baseline_pending_fills;
  std::vector<FillRecord> toxicity_pending_fills;

  PerSymbolSim()
      : order_book_baseline(), order_book_toxicity(),
        mm_baseline(order_book_baseline, false),
        mm_toxicity(order_book_toxicity, true), order_side(),
        latency_us_dist(0.0, 1.0), uni01(0.0, 1.0) {}

  void ensure_init(uint32_t idx) {
    if (initialized)
      return;
    initialized = true;
    symbol_index = idx;

    const uint64_t seed =
        g_exec.seed ^ (static_cast<uint64_t>(idx) * 0x9E3779B97F4A7C15ULL);
    rng.seed(seed);

    // Microsecond latency distribution for HFT
    latency_us_dist = std::normal_distribution<double>(
        g_exec.latency_us_mean, g_exec.latency_us_jitter);

    // Net fee = maker_rebate - clearing_fee (we receive rebate, pay clearing)
    double net_fee = -(g_exec.maker_rebate_per_share - g_exec.clearing_fee_per_share);
    mm_baseline.set_fee_per_share(net_fee);
    mm_toxicity.set_fee_per_share(net_fee);
  }

  uint64_t sample_latency_ns() {
    double us = latency_us_dist(rng);
    if (us < 5.0) us = 5.0;  // Minimum 5μs even with colo
    return static_cast<uint64_t>(us * 1000.0);  // Convert μs to ns
  }

  // Calculate queue position based on visible depth at price level
  uint32_t calculate_queue_position(double price, char side) {
    auto stats = (side == 'B') ? order_book_toxicity.get_stats()
                               : order_book_toxicity.get_stats();

    uint32_t visible_depth = 0;
    if (side == 'B') {
      auto bids = order_book_toxicity.get_bids();
      auto it = bids.find(price);
      if (it != bids.end()) visible_depth = it->second;
    } else {
      auto asks = order_book_toxicity.get_asks();
      auto it = asks.find(price);
      if (it != asks.end()) visible_depth = it->second;
    }

    if (visible_depth == 0) return 0;

    // Our queue position is a fraction of visible depth with variance
    double base_position = visible_depth * g_exec.queue_position_fraction;
    double variance = base_position * g_exec.queue_position_variance;
    std::normal_distribution<double> pos_dist(base_position, variance);
    double pos = pos_dist(rng);
    return static_cast<uint32_t>(std::max(0.0, pos));
  }

  // Check if symbol meets eligibility criteria
  bool check_eligibility() {
    auto stats = order_book_toxicity.get_stats();

    // Need valid BBO
    if (stats.best_bid <= 0 || stats.best_ask <= 0) return false;

    // Check spread requirements
    if (stats.spread < g_exec.min_spread_to_trade) return false;
    if (stats.spread > g_exec.max_spread_to_trade) return false;

    // Check depth requirements
    if (stats.total_bid_qty < g_exec.min_depth_to_trade) return false;
    if (stats.total_ask_qty < g_exec.min_depth_to_trade) return false;

    return true;
  }

  // Check if we've hit loss limits
  bool check_risk_limits(SymbolRiskState& risk) {
    double total_pnl = risk.realized_pnl + risk.unrealized_pnl + risk.total_adverse_pnl;
    if (total_pnl < -g_exec.max_daily_loss_per_symbol) {
      risk.halted = true;
      return false;
    }
    return true;
  }

  // Measure adverse selection on pending fills
  void measure_adverse_selection(std::vector<FillRecord>& fills,
                                  SymbolRiskState& risk,
                                  uint64_t now_ns) {
    auto stats = order_book_toxicity.get_stats();
    double current_mid = stats.mid_price;
    if (current_mid <= 0) return;

    for (auto& fill : fills) {
      if (fill.adverse_measured) continue;

      // Check if enough time has passed
      uint64_t elapsed_us = (now_ns - fill.fill_time_ns) / 1000;
      if (elapsed_us < g_exec.adverse_lookforward_us) continue;

      // Measure adverse price movement
      double price_change = current_mid - fill.mid_price_at_fill;

      // For buys: adverse if price went down after we bought
      // For sells: adverse if price went up after we sold
      double adverse_move = fill.is_buy ? -price_change : price_change;

      if (adverse_move > 0) {
        // We got adversely selected - charge a fraction of the move
        fill.adverse_pnl = -adverse_move * fill.fill_qty *
                           g_exec.adverse_selection_multiplier;
        risk.total_adverse_pnl += fill.adverse_pnl;
        risk.adverse_fills++;
      }

      fill.adverse_measured = true;
    }

    // Clean up old measured fills
    fills.erase(
        std::remove_if(fills.begin(), fills.end(),
                       [](const FillRecord& f) { return f.adverse_measured; }),
        fills.end());
  }

  bool eligible_for_fill(double quote_px, double exec_px,
                         bool is_bid_side) const {
    if (g_exec.fill_mode == ExecutionModelConfig::FillMode::Match) {
      return std::abs(quote_px - exec_px) < 1e-12;
    }
    return is_bid_side ? (quote_px >= exec_px) : (quote_px <= exec_px);
  }

  void update_virtual_order(VirtualOrder &vo, double price, uint32_t size,
                            char side, uint64_t now_ns) {
    const bool price_changed = vo.price != price;
    const bool changed = (!vo.live) || price_changed ||
                         (vo.size != size) || (vo.remaining == 0);
    if (!changed)
      return;

    uint64_t latency_ns = sample_latency_ns();

    // If we're changing price, there's an exposure window where stale quote is live
    if (vo.live && price_changed) {
      vo.exposed_until_ns = now_ns + g_exec.quote_exposure_window_us * 1000;
    }

    vo.price = price;
    vo.size = size;
    vo.remaining = size;
    // Calculate queue position based on current visible depth at this price
    vo.queue_ahead = calculate_queue_position(price, side);
    vo.active_at_ns = now_ns + latency_ns;
    vo.live = (price > 0.0 && size > 0);
  }

  void update_quotes(uint64_t now_ns) {
    uint64_t quote_interval_ns = g_exec.quote_update_interval_us * 1000;
    if (now_ns - last_quote_update_ns < quote_interval_ns)
      return;
    last_quote_update_ns = now_ns;

    // Measure adverse selection on any pending fills
    measure_adverse_selection(baseline_pending_fills, baseline_risk, now_ns);
    measure_adverse_selection(toxicity_pending_fills, toxicity_risk, now_ns);

    // Check eligibility and risk limits
    eligible_to_trade = check_eligibility();
    if (!eligible_to_trade) return;

    if (!check_risk_limits(baseline_risk) || !check_risk_limits(toxicity_risk)) {
      return;
    }

    mm_baseline.update_market_data();
    mm_toxicity.update_market_data();

    const MarketMakerQuote q_base = mm_baseline.get_current_quotes();
    const MarketMakerQuote q_tox = mm_toxicity.get_current_quotes();

    update_virtual_order(baseline_state.bid, q_base.bid_price, q_base.bid_size,
                         'B', now_ns);
    update_virtual_order(baseline_state.ask, q_base.ask_price, q_base.ask_size,
                         'S', now_ns);

    update_virtual_order(toxicity_state.bid, q_tox.bid_price, q_tox.bid_size,
                         'B', now_ns);
    update_virtual_order(toxicity_state.ask, q_tox.ask_price, q_tox.ask_size,
                         'S', now_ns);
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
                    std::vector<FillRecord>& pending_fills,
                    SymbolRiskState& risk,
                    bool is_bid_side, double exec_price, uint32_t exec_qty,
                    uint64_t now_ns) {
    // Check if halted due to loss limits
    if (risk.halted) return;

    VirtualOrder &vo = is_bid_side ? st.bid : st.ask;
    if (!vo.live || vo.remaining == 0)
      return;

    // Order must be active (past latency period)
    if (now_ns < vo.active_at_ns)
      return;

    // Check price eligibility
    if (!eligible_for_fill(vo.price, exec_price, is_bid_side))
      return;

    // During quote exposure window, we're more vulnerable to adverse fills
    // Model this as higher fill probability (bad fills get through)
    bool in_exposure_window = (now_ns < vo.exposed_until_ns);

    // Queue position logic - must wait our turn
    uint32_t qty_left = exec_qty;
    if (vo.queue_ahead > 0 && !in_exposure_window) {
      // Normal case: consume queue ahead of us
      const uint32_t consume = std::min(vo.queue_ahead, qty_left);
      vo.queue_ahead -= consume;
      qty_left -= consume;
    }
    // During exposure window, skip queue logic (we get adversely picked off)

    if (qty_left == 0)
      return;

    const uint32_t fill_qty = std::min(vo.remaining, qty_left);
    if (fill_qty == 0)
      return;

    // Execute the fill
    vo.remaining -= fill_qty;
    mm.on_order_filled(is_bid_side, vo.price, fill_qty);
    risk.total_fills++;

    // Record fill for adverse selection measurement
    auto stats = order_book_toxicity.get_stats();
    FillRecord record;
    record.fill_time_ns = now_ns;
    record.fill_price = vo.price;
    record.fill_qty = fill_qty;
    record.is_buy = is_bid_side;
    record.mid_price_at_fill = stats.mid_price;
    pending_fills.push_back(record);
  }

  void maybe_fill_on_execution(char resting_side, double exec_price,
                               uint32_t exec_qty, uint64_t now_ns) {
    update_quotes(now_ns);

    // Skip if not eligible to trade this symbol
    if (!eligible_to_trade) return;

    if (resting_side == 'B') {
      try_fill_one(mm_baseline, baseline_state, baseline_pending_fills,
                   baseline_risk, true, exec_price, exec_qty, now_ns);
      try_fill_one(mm_toxicity, toxicity_state, toxicity_pending_fills,
                   toxicity_risk, true, exec_price, exec_qty, now_ns);
      return;
    }

    if (resting_side == 'S') {
      try_fill_one(mm_baseline, baseline_state, baseline_pending_fills,
                   baseline_risk, false, exec_price, exec_qty, now_ns);
      try_fill_one(mm_toxicity, toxicity_state, toxicity_pending_fills,
                   toxicity_risk, false, exec_price, exec_qty, now_ns);
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
    int64_t quotes_suppressed;
    double adverse_pnl;
    int64_t adverse_fills;
  };

  std::vector<Row> rows;
  rows.reserve(g_sims.size());

  double portfolio_baseline = 0.0;
  double portfolio_toxicity = 0.0;
  double portfolio_adverse = 0.0;
  int64_t total_baseline_fills = 0;
  int64_t total_toxicity_fills = 0;
  int64_t total_quotes_suppressed = 0;
  int64_t total_adverse_fills = 0;
  int64_t symbols_halted = 0;
  int64_t symbols_ineligible = 0;

  for (const auto &kv : g_sims) {
    const uint32_t symbol_index = kv.first;
    const PerSymbolSim &sim = kv.second;

    if (!sim.eligible_to_trade) {
      symbols_ineligible++;
      continue;
    }
    if (sim.toxicity_risk.halted) {
      symbols_halted++;
    }

    const MarketMakerStats baseline_stats = sim.mm_baseline.get_stats();
    const MarketMakerStats toxicity_stats = sim.mm_toxicity.get_stats();

    // Include adverse selection penalty in PnL
    const double baseline_total =
        baseline_stats.realized_pnl + baseline_stats.unrealized_pnl +
        sim.baseline_risk.total_adverse_pnl;
    const double toxicity_total =
        toxicity_stats.realized_pnl + toxicity_stats.unrealized_pnl +
        sim.toxicity_risk.total_adverse_pnl;
    const double improvement = toxicity_total - baseline_total;

    portfolio_baseline += baseline_total;
    portfolio_toxicity += toxicity_total;
    portfolio_adverse += sim.toxicity_risk.total_adverse_pnl;
    total_baseline_fills += sim.baseline_risk.total_fills;
    total_toxicity_fills += sim.toxicity_risk.total_fills;
    total_quotes_suppressed += toxicity_stats.quotes_suppressed;
    total_adverse_fills += sim.toxicity_risk.adverse_fills;

    rows.push_back(Row{symbol_index, xdp::get_symbol(symbol_index),
                       baseline_total, toxicity_total, improvement,
                       sim.baseline_risk.total_fills, sim.toxicity_risk.total_fills,
                       toxicity_stats.quotes_suppressed,
                       sim.toxicity_risk.total_adverse_pnl,
                       sim.toxicity_risk.adverse_fills});
  }

  std::sort(rows.begin(), rows.end(),
            [](const Row &a, const Row &b) { return a.improvement > b.improvement; });

  const double portfolio_improvement = portfolio_toxicity - portfolio_baseline;
  const double portfolio_improvement_pct =
      portfolio_baseline != 0.0
          ? (portfolio_improvement / std::abs(portfolio_baseline)) * 100.0
          : 0.0;

  std::cout << "\n=== HFT MARKET MAKER SIMULATION RESULTS ===\n";
  std::cout << "Latency: " << g_exec.latency_us_mean << "μs (colo)\n";
  std::cout << "Symbols traded: " << rows.size() << '\n';
  std::cout << "Symbols ineligible: " << symbols_ineligible << '\n';
  std::cout << "Symbols halted (loss limit): " << symbols_halted << '\n';
  std::cout << "Total executions processed: " << g_total_executions << '\n';

  std::cout << "\n--- PORTFOLIO TOTALS (incl. adverse selection) ---\n";
  std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
            << portfolio_baseline << '\n';
  std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
            << portfolio_toxicity << '\n';
  std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
            << portfolio_improvement << " (" << std::fixed
            << std::setprecision(2) << portfolio_improvement_pct << "%)\n";

  std::cout << "\n--- ADVERSE SELECTION ANALYSIS ---\n";
  std::cout << "Total adverse selection penalty: $" << std::fixed << std::setprecision(2)
            << portfolio_adverse << '\n';
  std::cout << "Fills with adverse movement: " << total_adverse_fills << " / "
            << total_toxicity_fills << '\n';
  if (total_adverse_fills > 0) {
    std::cout << "Avg adverse penalty per fill: $" << std::fixed << std::setprecision(4)
              << (portfolio_adverse / total_adverse_fills) << '\n';
  }

  std::cout << "\n--- EXECUTION STATS ---\n";
  std::cout << "Baseline fills: " << total_baseline_fills << '\n';
  std::cout << "Toxicity fills: " << total_toxicity_fills << '\n';
  std::cout << "Quotes suppressed (toxicity): " << total_quotes_suppressed << '\n';
  if (total_baseline_fills > 0) {
    std::cout << "Avg PnL per fill (baseline): $" << std::fixed << std::setprecision(4)
              << (portfolio_baseline / total_baseline_fills) << '\n';
  }
  if (total_toxicity_fills > 0) {
    std::cout << "Avg PnL per fill (toxicity): $" << std::fixed << std::setprecision(4)
              << (portfolio_toxicity / total_toxicity_fills) << '\n';
  }

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

  // Show worst performers too
  std::cout << "\n--- BOTTOM 5 SYMBOLS (WORST) ---\n";
  const size_t bottom_start = rows.size() > 5 ? rows.size() - 5 : 0;
  for (size_t i = rows.size(); i > bottom_start; i--) {
    const Row &r = rows[i - 1];
    std::cout << (rows.size() - i + 1) << ". " << r.ticker << " (index " << r.symbol_index
              << "): $" << std::fixed << std::setprecision(2) << r.toxicity_total
              << " | fills " << r.toxicity_fills << '\n';
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
    std::cout << "Quotes suppressed: " << r.quotes_suppressed << '\n';
  }
}

void print_usage(const char *program) {
  std::cerr << "HFT Market Maker Simulation\n\n"
            << "Usage: " << program << " <pcap_file> [symbol_file] [options]\n\n"
            << "Options:\n"
            << "  -t TICKER           Filter to single ticker\n"
            << "  --seed N            Random seed\n"
            << "  --latency-us M      One-way latency in microseconds (default: 20)\n"
            << "  --latency-jitter-us J  Latency jitter (default: 5)\n"
            << "  --queue-fraction F  Queue position as fraction of depth (default: 0.3)\n"
            << "  --adverse-lookforward-us U  Adverse selection lookforward (default: 1000)\n"
            << "  --adverse-multiplier M  Adverse selection penalty multiplier (default: 0.5)\n"
            << "  --maker-rebate R    Maker rebate per share (default: 0.0015)\n"
            << "  --max-position P    Max position per symbol (default: 500)\n"
            << "  --max-loss L        Max daily loss per symbol (default: 100)\n"
            << "  --quote-interval-us Q  Quote update interval (default: 50)\n"
            << "  --fill-mode M       Fill mode: cross or match (default: cross)\n";
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::string pcap_file = argv[1];
  std::string symbol_file = "data/symbol_nyse_parsed.csv";

  for (int i = 2; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "-t" && i + 1 < argc) {
      g_filter_ticker = argv[++i];
    } else if (arg == "--seed" && i + 1 < argc) {
      g_exec.seed = std::stoull(argv[++i]);
    } else if (arg == "--latency-us" && i + 1 < argc) {
      g_exec.latency_us_mean = std::stod(argv[++i]);
    } else if (arg == "--latency-jitter-us" && i + 1 < argc) {
      g_exec.latency_us_jitter = std::stod(argv[++i]);
    } else if (arg == "--queue-fraction" && i + 1 < argc) {
      g_exec.queue_position_fraction = std::stod(argv[++i]);
    } else if (arg == "--adverse-lookforward-us" && i + 1 < argc) {
      g_exec.adverse_lookforward_us = std::stoull(argv[++i]);
    } else if (arg == "--adverse-multiplier" && i + 1 < argc) {
      g_exec.adverse_selection_multiplier = std::stod(argv[++i]);
    } else if (arg == "--maker-rebate" && i + 1 < argc) {
      g_exec.maker_rebate_per_share = std::stod(argv[++i]);
    } else if (arg == "--max-position" && i + 1 < argc) {
      g_exec.max_position_per_symbol = std::stod(argv[++i]);
    } else if (arg == "--max-loss" && i + 1 < argc) {
      g_exec.max_daily_loss_per_symbol = std::stod(argv[++i]);
    } else if (arg == "--fill-mode" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode == "match") {
        g_exec.fill_mode = ExecutionModelConfig::FillMode::Match;
      } else {
        g_exec.fill_mode = ExecutionModelConfig::FillMode::Cross;
      }
    } else if (arg == "--quote-interval-us" && i + 1 < argc) {
      g_exec.quote_update_interval_us = std::stoull(argv[++i]);
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
