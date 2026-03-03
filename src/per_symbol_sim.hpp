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
  EWMAFilter ewma_filter;
  TradeFlowTracker trade_flow;
  SpreadTracker spread_tracker;
  MomentumTracker momentum_tracker;

  // Walk-forward analysis state (per-symbol window tracking)
  int current_wf_window = 0;
  uint64_t wf_window_start_ns = 0;
  uint64_t wf_window_duration_ns = 0;
  bool wf_initialized = false;

  // Per-window metrics for walk-forward reporting
  struct WFWindowMetrics {
    int window_id = 0;
    double toxicity_pnl = 0.0;
    double baseline_pnl = 0.0;
    int64_t fills = 0;
    int64_t suppressed = 0;
  };
  std::vector<WFWindowMetrics> wf_window_metrics;

  // Fill pipeline diagnostics — counts where potential fills are lost
  struct FillDiagnostics {
    uint64_t exec_total = 0;           // Total execution messages for this symbol
    uint64_t exec_no_order_info = 0;   // Order ID not found (cleaned up or unknown)
    uint64_t exec_not_eligible = 0;    // Symbol not eligible to trade
    uint64_t try_fill_calls = 0;       // Total calls to try_fill_one
    uint64_t rejected_halted = 0;      // Risk limits breached
    uint64_t rejected_not_live = 0;    // Virtual order not live or remaining=0
    uint64_t rejected_latency = 0;     // Still in latency window
    uint64_t rejected_price = 0;       // Price not eligible (Cross mode)
    uint64_t rejected_queue = 0;       // Queue ahead not consumed
    uint64_t fill_succeeded = 0;       // Actual fills
    uint64_t quote_resets = 0;         // Virtual order queue resets (price/size change)
  };
  FillDiagnostics diag_baseline;
  FillDiagnostics diag_toxicity;

  // End-of-day liquidation state
  bool eod_liquidated = false;

  // Per-symbol blacklisting: stop trading after persistent losses
  bool blacklisted = false;
  int64_t blacklist_check_fills = 0;  // Fills at last blacklist check

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
  bool check_eligibility() const;

  // Check if we've hit loss limits
  bool check_risk_limits(SymbolRiskState& risk) const;

  // Build current feature vector from order book and trackers
  ToxicityFeatureVector build_feature_vector() const;

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
                    FillDiagnostics& diag,
                    bool is_bid_side, double exec_price, uint32_t exec_qty,
                    uint64_t now_ns);

  // Check both strategies for fills on an execution
  void maybe_fill_on_execution(char resting_side, double exec_price,
                               uint32_t exec_qty, uint64_t now_ns);
};

} // namespace mmsim
