#pragma once

#include "execution_model.hpp"
#include "feature_trackers.hpp"
#include "market_maker.hpp"
#include "order_book.hpp"
#include "sim_types.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmsim {

// Per-symbol simulation state: shared order book, dual strategies,
// feature trackers, risk tracking, and fill management.
struct PerSymbolSim {
  OrderBook order_book;
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
  std::string cached_ticker;  // Cached from symbol map during ensure_init()
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

  // Completed fills with measured adverse_pnl (for CSV output)
  std::vector<FillRecord> baseline_completed_fills;
  std::vector<FillRecord> toxicity_completed_fills;

  // Online learning feature trackers and model
  OnlineToxicityModel online_model;
  TradeFlowTracker trade_flow;
  SpreadTracker spread_tracker;
  MomentumTracker momentum_tracker;

  // Pointer to runtime configuration (set during ensure_init)
  const SimConfig* config_ = nullptr;

  PerSymbolSim();

  // Initialize simulation state for a given symbol index
  void ensure_init(uint32_t idx, const SimConfig& config);

  // Sample latency from the configured distribution
  uint64_t sample_latency_ns();

  // Calculate queue position based on visible depth at price level
  uint32_t calculate_queue_position(double price, char side);

  // Check if symbol meets eligibility criteria
  bool check_eligibility();

  // Check if we've hit loss limits
  bool check_risk_limits(SymbolRiskState& risk);

  // Build current feature vector from order book and trackers
  ToxicityFeatureVector build_feature_vector();

  // Measure adverse selection on pending fills
  void measure_adverse_selection(std::vector<FillRecord>& fills,
                                  std::vector<FillRecord>* completed,
                                  SymbolRiskState& risk,
                                  uint64_t now_ns);

  // Check if a fill is eligible at the given price
  bool eligible_for_fill(double quote_px, double exec_px,
                         bool is_bid_side) const;

  // Update a virtual order's price/size/queue position
  void update_virtual_order(VirtualOrder& vo, double price, uint32_t size,
                            char side, uint64_t now_ns);

  // Periodic quote update and adverse selection measurement
  void update_quotes(uint64_t now_ns);

  // Order book event handlers
  void on_add(uint64_t order_id, double price, uint32_t volume, char side,
              uint64_t now_ns);
  void on_modify(uint64_t order_id, double price, uint32_t volume);
  void on_delete(uint64_t order_id);
  void on_replace(uint64_t old_order_id, uint64_t new_order_id, double price,
                  uint32_t volume, char side, uint64_t now_ns);
  void on_execute(uint64_t order_id, uint32_t exec_qty, double exec_price,
                  uint64_t now_ns);

  // Helper to update queue positions when orders at our quote price cancel
  void update_queue_on_cancel(double price, uint32_t volume, char side);

  // Attempt to fill one side of a strategy
  void try_fill_one(MarketMakerStrategy& mm, StrategyExecState& st,
                    std::vector<FillRecord>& pending_fills,
                    SymbolRiskState& risk,
                    bool is_bid_side, double exec_price, uint32_t exec_qty,
                    uint64_t now_ns);

  // Check both strategies for fills on an execution
  void maybe_fill_on_execution(char resting_side, double exec_price,
                               uint32_t exec_qty, uint64_t now_ns);
};

} // namespace mmsim
