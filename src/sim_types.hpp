#pragma once

#include "market_maker.hpp"
#include <cstdint>

namespace mmsim {

struct VirtualOrder {
  double price = 0.0;
  uint32_t size = 0;
  uint32_t remaining = 0;
  uint64_t active_at_ns = 0;       // When order becomes active (after latency)
  uint64_t exposed_until_ns = 0;   // Stale quote exposure window
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
  double toxicity_at_fill = 0.0;  // Toxicity score at time of fill
  bool adverse_measured = false;
  double adverse_pnl = 0.0;
  ToxicityFeatureVector features;  // Per-fill feature vector for online learning
};

// Per-symbol risk state with Welford's online inventory variance tracking
struct SymbolRiskState {
  double realized_pnl = 0.0;
  double unrealized_pnl = 0.0;
  bool halted = false;  // Stopped due to loss limit
  int64_t total_fills = 0;
  double total_adverse_pnl = 0.0;
  int64_t adverse_fills = 0;

  // Inventory variance tracking (Welford's online algorithm)
  double inv_mean = 0.0;
  double inv_m2 = 0.0;   // Sum of squared differences from mean
  int64_t inv_count = 0;  // Number of inventory samples

  void update_inventory_variance(double inventory) {
    inv_count++;
    double delta = inventory - inv_mean;
    inv_mean += delta / static_cast<double>(inv_count);
    double delta2 = inventory - inv_mean;
    inv_m2 += delta * delta2;
  }

  double get_inventory_variance() const {
    if (inv_count < 2) return 0.0;
    return inv_m2 / static_cast<double>(inv_count - 1);
  }
};

} // namespace mmsim
