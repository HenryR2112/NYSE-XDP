#pragma once

#include <cstdint>
#include <string>

namespace mmsim {

// =============================================================================
// HFT Market Maker Execution Model
// Assumes: Elite HFT with FPGA, microwave links, and top-of-book priority
// =============================================================================

struct ExecutionModelConfig {
  uint64_t seed = 42;

  // --- Latency Model (Elite HFT - Sub-10us) ---
  // 5us one-way = FPGA + kernel bypass + direct exchange feed
  // 1us jitter (hardware timestamping, isolated cores)
  double latency_us_mean = 5.0;           // 5 microseconds one-way
  double latency_us_jitter = 1.0;         // Minimal jitter
  uint64_t quote_update_interval_us = 10; // Can update quotes every 10us

  // --- Queue Position Model ---
  // Elite HFT: effectively at front of queue
  // 0.5% = if 5000 shares at level, only ~25 shares ahead
  double queue_position_fraction = 0.005; // Top 0.5% (near front of queue)
  double queue_position_variance = 0.1;   // Very consistent

  // --- Adverse Selection Model ---
  // Excellent ability to detect and hedge adverse flow
  uint64_t adverse_lookforward_us = 250;  // 250us lookforward (faster reaction)
  double adverse_selection_multiplier = 0.03; // Only 3% realized (excellent hedging)

  // --- Quote Exposure Window ---
  uint64_t quote_exposure_window_us = 10; // 10us exposure

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

// Runtime simulation configuration (aggregates CLI-parsed parameters)
struct SimConfig {
  ExecutionModelConfig exec;
  std::string output_dir;       // Output directory for CSV files (empty = no CSV)
  bool online_learning = false; // Enable online SGD toxicity model
  double learning_rate = 0.01;  // Base learning rate for SGD
  int warmup_fills = 50;        // Fills before SGD kicks in
  double toxicity_threshold = 0.0;  // 0 = use default from market_maker.hpp
  double toxicity_multiplier = 0.0; // 0 = use default from market_maker.hpp
};

} // namespace mmsim
