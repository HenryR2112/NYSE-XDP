// market_maker_sim.cpp - Market Maker Simulation with PCAP playback
// Simulates market making strategies on historical XDP data
// PARALLELIZED VERSION - Uses all available CPU cores for maximum throughput

#include "common/mmap_pcap_reader.hpp"
#include "common/pcap_reader.hpp"
#include "common/symbol_map.hpp"
#include "common/thread_pool.hpp"
#include "common/xdp_types.hpp"
#include "common/xdp_utils.hpp"
#include "market_maker.hpp"
#include "order_book.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <random>
#include <regex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

std::string g_filter_ticker;
bool g_use_parallel = true;  // Enable parallel processing by default
bool g_use_hybrid = true;    // Enable hybrid multi-process mode by default
size_t g_num_threads = 0;    // 0 = auto-detect (use all cores)
size_t g_files_per_group = 0; // 0 = auto (num_files / num_threads)

// =============================================================================
// HFT Market Maker Execution Model
// Assumes: Elite HFT with FPGA, microwave links, and top-of-book priority
// =============================================================================

struct ExecutionModelConfig {
  uint64_t seed = 42;

  // --- Latency Model (Elite HFT - Sub-10μs) ---
  // 5μs one-way = FPGA + kernel bypass + direct exchange feed
  // 1μs jitter (hardware timestamping, isolated cores)
  double latency_us_mean = 5.0;           // 5 microseconds one-way
  double latency_us_jitter = 1.0;         // Minimal jitter
  uint64_t quote_update_interval_us = 10; // Can update quotes every 10μs

  // --- Queue Position Model ---
  // Elite HFT: effectively at front of queue
  // 0.5% = if 5000 shares at level, only ~25 shares ahead
  double queue_position_fraction = 0.005; // Top 0.5% (near front of queue)
  double queue_position_variance = 0.1;   // Very consistent

  // --- Adverse Selection Model ---
  // Excellent ability to detect and hedge adverse flow
  uint64_t adverse_lookforward_us = 250;  // 250μs lookforward (faster reaction)
  double adverse_selection_multiplier = 0.03; // Only 3% realized (excellent hedging)

  // --- Quote Exposure Window ---
  uint64_t quote_exposure_window_us = 10; // 10μs exposure

  // --- Fee/Rebate Structure (NYSE Tier 1 maker) ---
  double maker_rebate_per_share = 0.0025;  // $0.0025/share (top-tier volume)
  double taker_fee_per_share = 0.003;      // Pay $0.003/share if we cross
  double clearing_fee_per_share = 0.00008; // Volume discount on clearing

  // --- Risk Limits ---
  double max_position_per_symbol = 50000.0; // Max 50k shares per symbol
  double max_daily_loss_per_symbol = 5000.0; // Stop trading after $5k loss
  double max_portfolio_loss = 500000.0;     // Kill switch at $500k loss

  // --- Symbol Selection Criteria ---
  double min_spread_to_trade = 0.01;       // Trade penny spreads
  double max_spread_to_trade = 0.20;       // Trade wider spreads too (more edge)
  uint32_t min_depth_to_trade = 100;       // Lower threshold (more symbols)

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

  // Track order details for queue position updates on cancel/execute
  struct OrderInfo {
    char side;
    double price;
    uint32_t volume;
    uint64_t add_time_ns;  // Track when order was added for cleanup
  };
  std::unordered_map<uint64_t, OrderInfo> order_info;
  uint64_t last_cleanup_ns = 0;

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
        mm_toxicity(order_book_toxicity, true), order_info(),
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

    for (auto& fill : fills) {
      if (fill.adverse_measured) continue;

      // Check if enough time has passed
      uint64_t elapsed_us = (now_ns - fill.fill_time_ns) / 1000;
      if (elapsed_us < g_exec.adverse_lookforward_us) continue;

      // Mark as measured even if we can't calculate (to allow cleanup)
      fill.adverse_measured = true;

      // Skip adverse calculation if no valid mid price
      if (current_mid <= 0) continue;

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
    }

    // Clean up old measured fills - also force cleanup if vector is too large
    if (fills.size() > 10000) {
      // Emergency cleanup - mark old fills as measured
      for (size_t i = 0; i < fills.size() - 5000; i++) {
        fills[i].adverse_measured = true;
      }
    }

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

  void on_add(uint64_t order_id, double price, uint32_t volume, char side, uint64_t now_ns) {
    order_info[order_id] = {side, price, volume, now_ns};
    order_book_baseline.add_order(order_id, price, volume, side);
    order_book_toxicity.add_order(order_id, price, volume, side);

    // Periodic cleanup of stale orders (every 10 seconds of market time)
    constexpr uint64_t CLEANUP_INTERVAL_NS = 10ULL * 1000000000ULL;  // 10 seconds
    constexpr uint64_t MAX_ORDER_AGE_NS = 60ULL * 1000000000ULL;     // 60 seconds max age
    if (now_ns - last_cleanup_ns > CLEANUP_INTERVAL_NS) {
      last_cleanup_ns = now_ns;
      // Remove orders older than MAX_ORDER_AGE_NS
      for (auto it = order_info.begin(); it != order_info.end(); ) {
        if (now_ns - it->second.add_time_ns > MAX_ORDER_AGE_NS) {
          it = order_info.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  void on_modify(uint64_t order_id, double price, uint32_t volume) {
    auto it = order_info.find(order_id);
    if (it != order_info.end()) {
      // If price changed, treat old price level as cancel for queue purposes
      if (std::abs(it->second.price - price) > 0.0001) {
        update_queue_on_cancel(it->second.price, it->second.volume, it->second.side);
      }
      it->second.price = price;
      it->second.volume = volume;
    }
    order_book_baseline.modify_order(order_id, price, volume);
    order_book_toxicity.modify_order(order_id, price, volume);
  }

  // Helper to update queue positions when orders at our quote price cancel
  void update_queue_on_cancel(double price, uint32_t volume, char side) {
    auto update_vo = [&](VirtualOrder& vo, bool is_bid) {
      if (!vo.live || vo.queue_ahead == 0) return;
      // Only update if cancel is at our quote price and same side
      if ((is_bid && side == 'B') || (!is_bid && side == 'S')) {
        if (std::abs(vo.price - price) < 0.0001) {
          // Order ahead of us cancelled - improve our queue position
          vo.queue_ahead = (vo.queue_ahead > volume) ? (vo.queue_ahead - volume) : 0;
        }
      }
    };

    update_vo(baseline_state.bid, true);
    update_vo(baseline_state.ask, false);
    update_vo(toxicity_state.bid, true);
    update_vo(toxicity_state.ask, false);
  }

  void on_delete(uint64_t order_id) {
    auto it = order_info.find(order_id);
    if (it != order_info.end()) {
      // Update queue positions before removing order info
      update_queue_on_cancel(it->second.price, it->second.volume, it->second.side);
      order_info.erase(it);
    }
    order_book_baseline.delete_order(order_id);
    order_book_toxicity.delete_order(order_id);
  }

  void on_replace(uint64_t old_order_id, uint64_t new_order_id, double price,
                  uint32_t volume, char side, uint64_t now_ns) {
    auto it = order_info.find(old_order_id);
    if (it != order_info.end()) {
      // Old order leaving queue - update queue positions
      update_queue_on_cancel(it->second.price, it->second.volume, it->second.side);
      order_info.erase(it);
    }
    order_info[new_order_id] = {side, price, volume, now_ns};

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
    auto it = order_info.find(order_id);
    if (it != order_info.end()) {
      maybe_fill_on_execution(it->second.side, exec_price, exec_qty, now_ns);

      // Update volume tracking (partial fills reduce remaining volume)
      if (it->second.volume > exec_qty) {
        it->second.volume -= exec_qty;
      } else {
        order_info.erase(it);
      }
    }

    order_book_baseline.execute_order(order_id, exec_qty, exec_price);
    order_book_toxicity.execute_order(order_id, exec_qty, exec_price);
  }
};

// Thread-safe symbol simulation storage
// Pre-allocated array for lock-free access (no hash map lookups during processing)
constexpr uint32_t MAX_SYMBOLS = 100000;
std::unique_ptr<PerSymbolSim*[]> g_sims_array;       // Raw pointer array
std::unique_ptr<std::atomic<bool>[]> g_sims_initialized;  // Atomic flags (raw array)

// Sharded locks to reduce contention (64 shards = good balance)
constexpr size_t NUM_LOCK_SHARDS = 64;
std::array<std::mutex, NUM_LOCK_SHARDS> g_shard_mutexes;

std::atomic<uint64_t> g_total_executions{0};
std::atomic<uint64_t> g_total_packets{0};
std::atomic<uint64_t> g_total_messages{0};
std::atomic<size_t> g_files_completed{0};
std::atomic<size_t> g_active_symbols{0};

// Initialize pre-allocated storage (call once at startup)
void init_symbol_storage() {
  g_sims_array = std::make_unique<PerSymbolSim*[]>(MAX_SYMBOLS);
  g_sims_initialized = std::make_unique<std::atomic<bool>[]>(MAX_SYMBOLS);
  for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
    g_sims_array[i] = nullptr;
    g_sims_initialized[i].store(false, std::memory_order_relaxed);
  }
}

// Get shard mutex for a symbol (distributes lock contention)
inline std::mutex& get_shard_mutex(uint32_t symbol_index) {
  return g_shard_mutexes[symbol_index % NUM_LOCK_SHARDS];
}

// Get or create PerSymbolSim - lock-free fast path
inline PerSymbolSim* get_or_create_sim_fast(uint32_t symbol_index) {
  if (symbol_index >= MAX_SYMBOLS) return nullptr;

  // Fast path: already initialized (lock-free check)
  if (g_sims_initialized[symbol_index].load(std::memory_order_acquire)) {
    return g_sims_array[symbol_index];
  }

  // Slow path: need to initialize (use sharded lock)
  std::lock_guard<std::mutex> lock(get_shard_mutex(symbol_index));

  // Double-check after acquiring lock
  if (g_sims_initialized[symbol_index].load(std::memory_order_acquire)) {
    return g_sims_array[symbol_index];
  }

  g_sims_array[symbol_index] = new PerSymbolSim();
  g_sims_initialized[symbol_index].store(true, std::memory_order_release);
  g_active_symbols.fetch_add(1, std::memory_order_relaxed);

  return g_sims_array[symbol_index];
}

// Periodically report memory stats (lock-free read of atomics)
void report_memory_stats() {
  std::cout << " [syms: " << g_active_symbols.load() << "]" << std::flush;
}

void process_xdp_message(const uint8_t *data, size_t max_len, uint16_t msg_type,
                         uint64_t now_ns) {
  if (max_len < xdp::MESSAGE_HEADER_SIZE)
    return;

  uint32_t symbol_index = xdp::read_symbol_index(msg_type, data, max_len);
  if (symbol_index == 0)
    return;

  // Bounds check: NYSE has ~8000 symbols, anything > 100k is invalid
  constexpr uint32_t MAX_VALID_SYMBOL_INDEX = 100000;
  if (symbol_index > MAX_VALID_SYMBOL_INDEX)
    return;

  std::string ticker = xdp::get_symbol(symbol_index);
  if (!g_filter_ticker.empty() && ticker != g_filter_ticker)
    return;

  // Only create simulation state for known symbols
  if (ticker.empty())
    return;

  g_total_messages.fetch_add(1, std::memory_order_relaxed);

  // Lock-free fast path for symbol lookup, sharded lock for updates
  PerSymbolSim* sim_ptr = get_or_create_sim_fast(symbol_index);
  if (!sim_ptr) return;

  PerSymbolSim& sim = *sim_ptr;

  // Use sharded lock for this symbol's updates
  std::lock_guard<std::mutex> sym_lock(get_shard_mutex(symbol_index));

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
      sim.on_add(order_id, price, volume, side_char, now_ns);
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
      sim.on_replace(old_order_id, new_order_id, price, volume, side_char, now_ns);
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
  rows.reserve(g_active_symbols.load());

  double portfolio_baseline = 0.0;
  double portfolio_toxicity = 0.0;
  double portfolio_adverse = 0.0;
  int64_t total_baseline_fills = 0;
  int64_t total_toxicity_fills = 0;
  int64_t total_quotes_suppressed = 0;
  int64_t total_adverse_fills = 0;
  int64_t symbols_halted = 0;
  int64_t symbols_ineligible = 0;

  // Iterate over pre-allocated array (no lock needed - single-threaded at results time)
  for (uint32_t symbol_index = 0; symbol_index < MAX_SYMBOLS; ++symbol_index) {
    if (!g_sims_initialized[symbol_index].load(std::memory_order_relaxed)) continue;
    PerSymbolSim* sim_ptr = g_sims_array[symbol_index];
    if (!sim_ptr) continue;
    const PerSymbolSim &sim = *sim_ptr;

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
  std::cerr << "HFT Market Maker Simulation (HYBRID MULTI-PROCESS VERSION)\n\n"
            << "Usage: " << program << " <pcap_file(s)> [options]\n\n"
            << "Processes PCAP files using hybrid multi-process architecture:\n"
            << "- Files grouped by time window, each group processed by separate process\n"
            << "- Sequential processing within groups (maintains order book state)\n"
            << "- Zero lock contention between groups (separate memory spaces)\n\n"
            << "Options:\n"
            << "  -t TICKER           Filter to single ticker\n"
            << "  -s, --symbols FILE  Symbol map file (default: data/symbol_nyse_parsed.csv)\n"
            << "  --seed N            Random seed\n"
            << "  --latency-us M      One-way latency in microseconds (default: 20)\n"
            << "  --latency-jitter-us J  Latency jitter (default: 5)\n"
            << "  --queue-fraction F  Queue position as fraction of depth (default: 0.05)\n"
            << "  --adverse-lookforward-us U  Adverse selection lookforward (default: 500)\n"
            << "  --adverse-multiplier M  Adverse selection penalty multiplier (default: 0.10)\n"
            << "  --maker-rebate R    Maker rebate per share (default: 0.002)\n"
            << "  --max-position P    Max position per symbol (default: 5000)\n"
            << "  --max-loss L        Max daily loss per symbol (default: 500)\n"
            << "  --quote-interval-us Q  Quote update interval (default: 50)\n"
            << "  --fill-mode M       Fill mode: cross or match (default: cross)\n"
            << "\nParallel Processing Options:\n"
            << "  --threads N         Number of processes (default: auto-detect all cores)\n"
            << "  --files-per-group N Files per process group (default: auto)\n"
            << "  --no-hybrid         Disable hybrid mode (use threaded mode instead)\n"
            << "  --sequential        Disable all parallelism (single-threaded)\n\n"
            << "Example (full day, hybrid):\n"
            << "  " << program << " data/pcaps/*.pcap --threads 14\n";
}

// =============================================================================
// HYBRID MULTI-PROCESS ARCHITECTURE
// Groups files by time window, spawns process per group, aggregates results
// =============================================================================

// Shared memory structure for inter-process result aggregation
struct ProcessResults {
  double baseline_pnl;
  double toxicity_pnl;
  double adverse_pnl;
  int64_t baseline_fills;
  int64_t toxicity_fills;
  int64_t quotes_suppressed;
  int64_t adverse_fills;
  uint64_t packets_processed;
  uint64_t messages_processed;
  uint64_t symbols_active;
  bool completed;
  char padding[7];  // Align to 8 bytes
};

// Get file size in bytes
size_t get_file_size(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    return static_cast<size_t>(st.st_size);
  }
  return 0;
}

// Group files by total size for balanced load distribution
// Uses greedy algorithm: assign each file to the group with smallest total size
std::vector<std::vector<std::string>> group_files(
    const std::vector<std::string>& files, size_t num_groups) {
  std::vector<std::vector<std::string>> groups(num_groups);
  std::vector<size_t> group_sizes(num_groups, 0);

  if (files.empty() || num_groups == 0) return groups;

  // Get sizes for all files and sort by size (largest first for better balancing)
  std::vector<std::pair<std::string, size_t>> file_sizes;
  file_sizes.reserve(files.size());
  for (const auto& f : files) {
    file_sizes.push_back({f, get_file_size(f)});
  }

  // Sort by size descending (largest files first = better load balancing)
  std::sort(file_sizes.begin(), file_sizes.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });

  // Greedy assignment: assign each file to the group with smallest total size
  for (const auto& [file, size] : file_sizes) {
    // Find group with minimum total size
    size_t min_idx = 0;
    size_t min_size = group_sizes[0];
    for (size_t i = 1; i < num_groups; ++i) {
      if (group_sizes[i] < min_size) {
        min_size = group_sizes[i];
        min_idx = i;
      }
    }

    groups[min_idx].push_back(file);
    group_sizes[min_idx] += size;
  }

  // Sort files within each group by name (chronological order)
  for (auto& group : groups) {
    std::sort(group.begin(), group.end());
  }

  // Remove empty groups
  groups.erase(
      std::remove_if(groups.begin(), groups.end(),
                     [](const std::vector<std::string>& g) { return g.empty(); }),
      groups.end());

  // Print load distribution for debugging
  std::cout << "Load distribution (MB per group):\n";
  for (size_t i = 0; i < groups.size(); ++i) {
    double mb = group_sizes[i] / (1024.0 * 1024.0);
    std::cout << "  Group " << (i+1) << ": " << std::fixed << std::setprecision(1)
              << mb << " MB (" << groups[i].size() << " files)\n";
  }

  return groups;
}

// Process a group of files sequentially (called in child process)
void process_file_group(const std::vector<std::string>& files,
                        ProcessResults* results,
                        const std::string& symbol_file,
                        size_t group_idx) {
  // Debug: confirm child started
  std::cerr << "[Group " << (group_idx+1) << "] Starting with " << files.size() << " files\n" << std::flush;

  // Re-initialize symbol storage in child process
  init_symbol_storage();
  if (!xdp::load_symbol_map(symbol_file)) {
    std::cerr << "[Group " << (group_idx+1) << "] WARNING: Failed to load symbol map\n";
  }

  // Reset counters for this process
  g_total_packets.store(0);
  g_total_messages.store(0);
  g_active_symbols.store(0);

  auto packet_callback = [](const uint8_t *data, size_t length,
                            uint64_t, const xdp::NetworkPacketInfo &info) {
    g_total_packets.fetch_add(1, std::memory_order_relaxed);

    if (length < xdp::PACKET_HEADER_SIZE) return;

    xdp::PacketHeader pkt_header;
    if (!xdp::parse_packet_header(data, length, pkt_header)) return;

    size_t offset = xdp::PACKET_HEADER_SIZE;
    for (uint8_t i = 0; i < pkt_header.num_messages && offset < length; i++) {
      if (offset + xdp::MESSAGE_HEADER_SIZE > length) break;
      uint16_t msg_size = xdp::read_le16(data + offset);
      if (msg_size < xdp::MESSAGE_HEADER_SIZE || offset + msg_size > length) break;
      uint16_t msg_type = xdp::read_le16(data + offset + 2);
      process_xdp_message(data + offset, msg_size, msg_type, info.timestamp_ns);
      offset += msg_size;
    }
  };

  // Process files sequentially within group (maintains state)
  size_t file_num = 0;
  for (const auto& pcap_file : files) {
    file_num++;
    xdp::MmapPcapReader reader;
    if (!reader.open(pcap_file)) {
      std::cerr << "[Group " << (group_idx+1) << "] Failed to open: " << pcap_file << "\n";
      continue;
    }
    reader.preload();

    uint64_t pkts_before = g_total_packets.load();
    reader.process_all(packet_callback);
    uint64_t pkts_in_file = g_total_packets.load() - pkts_before;

    // Progress every 10 files or at the end
    if (file_num % 10 == 0 || file_num == files.size()) {
      std::cerr << "[Group " << (group_idx+1) << "] File " << file_num << "/" << files.size()
                << " (" << pkts_in_file << " pkts, total " << g_total_packets.load() << ")\n" << std::flush;
    }
  }

  // Aggregate results from this process
  double baseline_pnl = 0.0, toxicity_pnl = 0.0, adverse_pnl = 0.0;
  int64_t baseline_fills = 0, toxicity_fills = 0, quotes_suppressed = 0, adverse_fills = 0;

  for (uint32_t idx = 0; idx < MAX_SYMBOLS; ++idx) {
    if (!g_sims_initialized[idx].load(std::memory_order_relaxed)) continue;
    PerSymbolSim* sim = g_sims_array[idx];
    if (!sim || !sim->eligible_to_trade) continue;

    const auto& bs = sim->mm_baseline.get_stats();
    const auto& ts = sim->mm_toxicity.get_stats();

    baseline_pnl += bs.realized_pnl + bs.unrealized_pnl + sim->baseline_risk.total_adverse_pnl;
    toxicity_pnl += ts.realized_pnl + ts.unrealized_pnl + sim->toxicity_risk.total_adverse_pnl;
    adverse_pnl += sim->toxicity_risk.total_adverse_pnl;
    baseline_fills += sim->baseline_risk.total_fills;
    toxicity_fills += sim->toxicity_risk.total_fills;
    quotes_suppressed += ts.quotes_suppressed;
    adverse_fills += sim->toxicity_risk.adverse_fills;
  }

  // Debug: confirm aggregation complete
  std::cerr << "[Group " << (group_idx+1) << "] Aggregation done: "
            << g_total_packets.load() << " pkts, "
            << g_active_symbols.load() << " syms, "
            << "baseline $" << baseline_pnl << ", toxicity $" << toxicity_pnl << "\n" << std::flush;

  // Write results to shared memory
  results->baseline_pnl = baseline_pnl;
  results->toxicity_pnl = toxicity_pnl;
  results->adverse_pnl = adverse_pnl;
  results->baseline_fills = baseline_fills;
  results->toxicity_fills = toxicity_fills;
  results->quotes_suppressed = quotes_suppressed;
  results->adverse_fills = adverse_fills;
  results->packets_processed = g_total_packets.load();
  results->messages_processed = g_total_messages.load();
  results->symbols_active = g_active_symbols.load();
  results->completed = true;

  std::cerr << "[Group " << (group_idx+1) << "] Results written to shared memory\n" << std::flush;
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc < 2) {
    print_usage(argv[0]);
    return 1;
  }

  std::vector<std::string> pcap_files;
  std::string symbol_file = "data/symbol_nyse_parsed.csv";

  // Parse arguments - collect PCAP files and options
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "-t" && i + 1 < argc) {
      g_filter_ticker = argv[++i];
    } else if ((arg == "-s" || arg == "--symbols") && i + 1 < argc) {
      symbol_file = argv[++i];
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
    } else if (arg == "--threads" && i + 1 < argc) {
      g_num_threads = std::stoull(argv[++i]);
    } else if (arg == "--files-per-group" && i + 1 < argc) {
      g_files_per_group = std::stoull(argv[++i]);
    } else if (arg == "--sequential") {
      g_use_parallel = false;
      g_use_hybrid = false;
    } else if (arg == "--no-hybrid") {
      g_use_hybrid = false;
    } else if (arg == "--mmap") {
      // mmap is now default, this flag is kept for compatibility
    } else if (arg[0] != '-') {
      // Assume it's a PCAP file
      pcap_files.push_back(arg);
    }
  }

  if (pcap_files.empty()) {
    std::cerr << "Error: No PCAP files specified\n";
    print_usage(argv[0]);
    return 1;
  }

  // Sort PCAP files by name to ensure chronological order
  std::sort(pcap_files.begin(), pcap_files.end());

  // Determine number of processes/threads
  size_t num_procs = g_num_threads;
  if (num_procs == 0) {
    num_procs = std::thread::hardware_concurrency();
    if (num_procs == 0) num_procs = 4;
  }

  // Determine mode string
  std::string mode_str = "SEQUENTIAL";
  if (g_use_hybrid && g_use_parallel && pcap_files.size() > 1) {
    mode_str = "HYBRID MULTI-PROCESS";
  } else if (g_use_parallel && pcap_files.size() > 1) {
    mode_str = "THREADED";
  }

  auto start_time = std::chrono::high_resolution_clock::now();

  // ==========================================================================
  // HYBRID MULTI-PROCESS MODE
  // ==========================================================================
  if (g_use_hybrid && g_use_parallel && pcap_files.size() > 1) {
    // Group files for parallel processing
    auto file_groups = group_files(pcap_files, num_procs);
    size_t actual_groups = file_groups.size();

    // Print header BEFORE fork to avoid duplicate output
    std::cout << "=== HFT Market Maker Simulation (HYBRID) ===\n";
    std::cout << "PCAP files: " << pcap_files.size() << '\n';
    std::cout << "Process groups: " << actual_groups << '\n';
    for (size_t i = 0; i < actual_groups; ++i) {
      std::cout << "  Group " << (i+1) << ": " << file_groups[i].size() << " files\n";
    }
    std::cout << "\nSpawning child processes...\n" << std::flush;

    // Allocate shared memory for results
    size_t shm_size = sizeof(ProcessResults) * actual_groups;
    ProcessResults* shared_results = static_cast<ProcessResults*>(
        mmap(nullptr, shm_size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));

    if (shared_results == MAP_FAILED) {
      std::cerr << "Failed to allocate shared memory\n";
      return 1;
    }

    // Initialize shared memory
    std::memset(shared_results, 0, shm_size);

    // Fork child processes
    std::vector<pid_t> children;
    for (size_t group_idx = 0; group_idx < actual_groups; ++group_idx) {
      pid_t pid = fork();

      if (pid < 0) {
        std::cerr << "Fork failed for group " << group_idx << "\n";
        continue;
      }

      if (pid == 0) {
        // Child process
        process_file_group(file_groups[group_idx],
                           &shared_results[group_idx],
                           symbol_file,
                           group_idx);

        // Print progress from child (use stderr to avoid buffering issues)
        std::cerr << "[Group " << (group_idx + 1) << "/" << actual_groups << "] "
                  << "Completed: " << shared_results[group_idx].packets_processed
                  << " packets, " << shared_results[group_idx].messages_processed
                  << " msgs\n" << std::flush;

        _exit(0);  // Exit child without calling destructors
      }

      children.push_back(pid);
    }

    // Parent: wait for all children with proper error checking
    std::cout << "Waiting for " << children.size() << " child processes...\n" << std::flush;

    size_t children_completed = 0;
    size_t children_crashed = 0;

    for (size_t i = 0; i < children.size(); ++i) {
      pid_t child = children[i];
      int status = 0;
      pid_t result = waitpid(child, &status, 0);

      if (result < 0) {
        std::cerr << "waitpid failed for child " << child << " (group " << (i+1) << "): "
                  << strerror(errno) << "\n";
        continue;
      }

      if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
          children_completed++;
        } else {
          std::cerr << "Group " << (i+1) << " exited with code " << exit_code << "\n";
          children_crashed++;
        }
      } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        std::cerr << "Group " << (i+1) << " killed by signal " << sig;
        if (sig == SIGSEGV) std::cerr << " (segmentation fault)";
        else if (sig == SIGBUS) std::cerr << " (bus error)";
        else if (sig == SIGKILL) std::cerr << " (killed - OOM?)";
        else if (sig == SIGABRT) std::cerr << " (abort)";
        std::cerr << "\n";
        children_crashed++;
      } else {
        std::cerr << "Group " << (i+1) << " ended with unknown status\n";
        children_crashed++;
      }
    }

    std::cout << "\nChild processes finished: " << children_completed << " completed, "
              << children_crashed << " failed\n";
    std::cout << "Aggregating results...\n";

    // Aggregate results from all processes
    double total_baseline_pnl = 0.0, total_toxicity_pnl = 0.0, total_adverse_pnl = 0.0;
    int64_t total_baseline_fills = 0, total_toxicity_fills = 0;
    int64_t total_quotes_suppressed = 0, total_adverse_fills = 0;
    uint64_t total_packets = 0, total_messages = 0, total_symbols = 0;
    size_t groups_with_results = 0;

    for (size_t i = 0; i < actual_groups; ++i) {
      if (shared_results[i].completed) {
        groups_with_results++;
        total_baseline_pnl += shared_results[i].baseline_pnl;
        total_toxicity_pnl += shared_results[i].toxicity_pnl;
        total_adverse_pnl += shared_results[i].adverse_pnl;
        total_baseline_fills += shared_results[i].baseline_fills;
        total_toxicity_fills += shared_results[i].toxicity_fills;
        total_quotes_suppressed += shared_results[i].quotes_suppressed;
        total_adverse_fills += shared_results[i].adverse_fills;
        total_packets += shared_results[i].packets_processed;
        total_messages += shared_results[i].messages_processed;
        total_symbols += shared_results[i].symbols_active;
      } else {
        std::cerr << "Warning: Group " << (i+1) << " did not write results to shared memory\n";
      }
    }

    std::cout << "Groups with valid results: " << groups_with_results << "/" << actual_groups << '\n';

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double seconds = duration.count() / 1000.0;

    // Print aggregated results
    std::cout << "\n=== PERFORMANCE STATISTICS ===\n";
    std::cout << "Total processing time: " << std::fixed << std::setprecision(2)
              << seconds << " seconds\n";
    std::cout << "Total packets: " << total_packets << '\n';
    std::cout << "Total messages: " << total_messages << '\n';
    std::cout << "Throughput: " << std::fixed << std::setprecision(0)
              << (total_packets / seconds) << " packets/sec, "
              << (total_messages / seconds) << " msgs/sec\n";
    std::cout << "Process groups: " << actual_groups << '\n';
    std::cout << "Unique symbols (sum): " << total_symbols << '\n';

    double improvement = total_toxicity_pnl - total_baseline_pnl;
    double improvement_pct = total_baseline_pnl != 0.0
        ? (improvement / std::abs(total_baseline_pnl)) * 100.0 : 0.0;

    std::cout << "\n=== AGGREGATED SIMULATION RESULTS ===\n";
    std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
              << total_baseline_pnl << '\n';
    std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
              << total_toxicity_pnl << '\n';
    std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
              << improvement << " (" << improvement_pct << "%)\n";
    std::cout << "\nBaseline fills: " << total_baseline_fills << '\n';
    std::cout << "Toxicity fills: " << total_toxicity_fills << '\n';
    std::cout << "Quotes suppressed: " << total_quotes_suppressed << '\n';
    std::cout << "Adverse fills: " << total_adverse_fills << '\n';
    std::cout << "Total adverse penalty: $" << std::fixed << std::setprecision(2)
              << total_adverse_pnl << '\n';

    // Cleanup shared memory
    munmap(shared_results, shm_size);

    return 0;
  }

  // ==========================================================================
  // NON-HYBRID MODES (threaded or sequential)
  // ==========================================================================
  std::cout << "=== HFT Market Maker Simulation (" << mode_str << ") ===\n";
  std::cout << "PCAP files to process: " << pcap_files.size() << '\n';
  std::cout << "Parallel units: " << num_procs << '\n';
  if (!g_filter_ticker.empty()) {
    std::cout << "Filtering for ticker: " << g_filter_ticker << '\n';
  }
  std::cout << "Running baseline and toxicity-aware strategies...\n\n";

  (void)xdp::load_symbol_map(symbol_file);
  init_symbol_storage();

  // Thread-safe packet processing callback
  auto packet_callback = [](const uint8_t *data, size_t length,
                            uint64_t, const xdp::NetworkPacketInfo &info) {
    g_total_packets++;

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
  };

  if (g_use_parallel && pcap_files.size() > 1) {
    // =====================================================================
    // PARALLEL PROCESSING MODE
    // Process multiple files concurrently using thread pool
    // =====================================================================
    std::cout << "Starting parallel processing with " << num_procs << " threads...\n";

    xdp::ThreadPool pool(num_procs);
    std::mutex progress_mutex;
    std::vector<std::future<size_t>> futures;
    futures.reserve(pcap_files.size());

    // Submit all files to thread pool
    for (size_t file_idx = 0; file_idx < pcap_files.size(); file_idx++) {
      const std::string& pcap_file = pcap_files[file_idx];

      futures.push_back(pool.enqueue([&pcap_file, &progress_mutex,
                                       total_files = pcap_files.size(),
                                       &packet_callback]() -> size_t {
        // Use memory-mapped reader for maximum throughput
        xdp::MmapPcapReader reader;
        if (!reader.open(pcap_file)) {
          std::lock_guard<std::mutex> lock(progress_mutex);
          std::cerr << "Warning: Error opening PCAP file " << pcap_file
                    << ": " << reader.error() << " - skipping\n";
          return 0;
        }

        // Pre-load file into memory
        reader.preload();

        size_t file_packets = reader.process_all(packet_callback);

        // Report progress
        size_t completed = ++g_files_completed;
        {
          std::lock_guard<std::mutex> lock(progress_mutex);
          size_t last_slash = pcap_file.find_last_of("/\\");
          std::string filename = (last_slash != std::string::npos)
                                     ? pcap_file.substr(last_slash + 1)
                                     : pcap_file;
          std::cout << "[" << completed << "/" << total_files << "] "
                    << filename << " - " << file_packets << " packets"
                    << " (total: " << g_total_packets.load() << " packets, "
                    << g_total_messages.load() << " msgs)\n" << std::flush;
        }

        return file_packets;
      }));
    }

    // Wait for all files to complete
    for (auto& f : futures) {
      f.wait();
    }

    std::cout << "\nAll files processed.\n";
  } else {
    // =====================================================================
    // SEQUENTIAL PROCESSING MODE (single file or --sequential flag)
    // =====================================================================
    std::cout << "Starting sequential processing...\n";

    for (size_t file_idx = 0; file_idx < pcap_files.size(); file_idx++) {
      const std::string& pcap_file = pcap_files[file_idx];

      // Use memory-mapped reader for faster I/O
      xdp::MmapPcapReader reader;
      if (!reader.open(pcap_file)) {
        std::cerr << "Warning: Error opening PCAP file " << pcap_file
                  << ": " << reader.error() << " - skipping\n";
        continue;
      }

      // Extract filename for progress display
      size_t last_slash = pcap_file.find_last_of("/\\");
      std::string filename = (last_slash != std::string::npos)
                                 ? pcap_file.substr(last_slash + 1)
                                 : pcap_file;

      std::cout << "[" << (file_idx + 1) << "/" << pcap_files.size() << "] "
                << filename << "..." << std::flush;

      // Pre-load for better performance
      reader.preload();

      uint64_t packets_before = g_total_packets.load();
      reader.process_all(packet_callback);
      uint64_t file_packets = g_total_packets.load() - packets_before;

      std::cout << " " << file_packets << " packets";
      report_memory_stats();
      std::cout << "\n";
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
  double seconds = duration.count() / 1000.0;
  double packets_per_sec = g_total_packets.load() / seconds;
  double msgs_per_sec = g_total_messages.load() / seconds;

  std::cout << "\n=== PERFORMANCE STATISTICS ===\n";
  std::cout << "Total processing time: " << std::fixed << std::setprecision(2)
            << seconds << " seconds\n";
  std::cout << "Total packets: " << g_total_packets.load() << '\n';
  std::cout << "Total messages: " << g_total_messages.load() << '\n';
  std::cout << "Throughput: " << std::fixed << std::setprecision(0)
            << packets_per_sec << " packets/sec, "
            << msgs_per_sec << " msgs/sec\n";
  std::cout << "Files processed: " << pcap_files.size() << '\n';

  print_results();

  return 0;
}
