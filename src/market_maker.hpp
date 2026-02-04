#pragma once

#include "order_book.hpp"
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

struct MarketMakerStats {
  double realized_pnl = 0.0;
  double unrealized_pnl = 0.0;
  int64_t total_fills = 0;
  int64_t buy_fills = 0;
  int64_t sell_fills = 0;
  uint64_t total_volume_traded = 0;
  double avg_fill_price_buy = 0.0;
  double avg_fill_price_sell = 0.0;
  double max_inventory = 0.0;
  double min_inventory = 0.0;
  std::chrono::steady_clock::time_point start_time;

  // Strategy proposal metrics
  double sharpe_ratio = 0.0;
  double inventory_variance = 0.0;
  int64_t quotes_suppressed = 0;  // Quotes suppressed due to toxicity
  int64_t adverse_fills = 0;       // Fills followed by adverse price movement
  double avg_toxicity = 0.0;
};

struct MarketMakerQuote {
  double bid_price = 0.0;
  double ask_price = 0.0;
  uint32_t bid_size = 0;
  uint32_t ask_size = 0;
  uint64_t bid_order_id = 0;
  uint64_t ask_order_id = 0;
  bool is_quoted = false;
};

class MarketMakerStrategy {
public:
  explicit MarketMakerStrategy(OrderBook &ob, bool use_toxicity = false);

  // Non-copyable
  MarketMakerStrategy(const MarketMakerStrategy &) = delete;
  MarketMakerStrategy &operator=(const MarketMakerStrategy &) = delete;

  // Update market data and recalculate quotes
  void update_market_data();

  // Get current quotes (thread-safe)
  [[nodiscard]] MarketMakerQuote get_current_quotes() const;

  // Handle order fill events
  void on_order_filled(bool is_buy, double price, uint32_t size);

  // Order management
  void register_our_order(uint64_t order_id);
  [[nodiscard]] bool is_our_order(uint64_t order_id) const;
  void on_order_cancelled(uint64_t order_id);

  // Configuration setters
  void set_fee_per_share(double fee);
  void set_base_spread(double spread);
  void set_toxicity_multiplier(double multiplier);

  // Getters
  [[nodiscard]] MarketMakerStats get_stats() const;
  [[nodiscard]] double get_inventory() const;

  // Reset strategy state
  void reset();

  // Strategy proposal methods
  [[nodiscard]] double calculate_toxicity_score(double cancel_rate, double obi, double short_vol) const noexcept;
  [[nodiscard]] double calculate_expected_pnl(double spread, double toxicity, double inventory_risk) const noexcept;
  [[nodiscard]] bool should_quote(double expected_pnl) const noexcept;

private:
  OrderBook &order_book_;
  bool use_toxicity_screen_;

  int64_t inventory_ = 0;
  double realized_pnl_ = 0.0;
  double unrealized_pnl_ = 0.0;
  double fee_per_share_ = 0.0;
  double avg_entry_price_ = 0.0;

  MarketMakerQuote current_quotes_;
  std::unordered_map<uint64_t, bool> our_order_ids_;
  mutable std::mutex strategy_mutex_;

  // Strategy parameters - REALISTIC HFT MM (optimized for fill volume + profitability)
  // Balance between getting fills and avoiding adverse selection
  double base_spread_ = 0.02;         // Tighter spread for more fills
  double min_spread_ = 0.01;          // Allow penny spread
  double max_spread_ = 0.10;          // Allow wider in toxic conditions
  uint32_t base_quote_size_ = 200;    // Smaller quotes = more fill opportunities
  double max_position_ = 5000.0;      // Allow larger inventory (25 fills of 200)
  double tick_size_ = 0.01;

  double inventory_skew_coefficient_ = 0.08;  // Moderate inventory management
  double toxicity_spread_multiplier_ = 2.0;   // Moderate toxicity response
  double toxicity_quote_threshold_ = 0.55;    // Less aggressive filtering (more fills)
  double obi_threshold_ = 0.35;               // Higher OBI threshold (less skipping)

  MarketMakerStats stats_;

  // Strategy proposal parameters - calibrated for realistic fill rates
  // Tuned to allow more fills while still filtering worst toxic flow
  double alpha1_ = 1.5;   // Cancel rate weight (slightly lower)
  double alpha2_ = 1.2;   // OBI weight - important predictor
  double alpha3_ = 0.5;   // Short volatility weight
  double mu_adverse_ = 0.008;  // Expected adverse movement (slightly lower)
  double gamma_risk_ = 0.002;  // Lower inventory risk penalty
  double fill_probability_ = 0.15;  // Higher fill probability expectation

  // Rolling window for toxicity metrics
  struct ToxicityWindow {
    std::chrono::steady_clock::time_point start_time;
    uint32_t cancel_count = 0;
    uint32_t add_count = 0;
    double sum_price_changes = 0.0;
    int num_price_samples = 0;
  };
  mutable ToxicityWindow toxicity_window_;
  double window_duration_ms_ = 1000.0;  // 1 second rolling window

  // Helper methods
  [[nodiscard]] double round_to_tick(double price) const noexcept;
  [[nodiscard]] double calculate_toxicity_adjusted_spread(double base_spread_val) const;
  [[nodiscard]] double calculate_inventory_skew() const noexcept;
  [[nodiscard]] double sigmoid(double x) const noexcept;
  [[nodiscard]] double calculate_obi() const;  // Order Book Imbalance
  [[nodiscard]] double get_average_toxicity() const;  // Average toxicity across levels
};
