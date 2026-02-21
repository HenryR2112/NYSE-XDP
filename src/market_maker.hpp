#pragma once

#include "order_book.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// ---- Online Toxicity Learning Model ----

static constexpr int N_TOXICITY_FEATURES = 8;

struct ToxicityFeatureVector {
  std::array<double, N_TOXICITY_FEATURES> features = {};
  // [0] cancel_ratio     [1] ping_ratio       [2] odd_lot_ratio
  // [3] precision_ratio   [4] resistance_ratio  [5] trade_flow_imbalance
  // [6] spread_change_rate [7] price_momentum
};

struct OnlineToxicityModel {
  std::array<double, N_TOXICITY_FEATURES> weights = {0.4, 0.2, 0.15, 0.15, 0.1, 0.0, 0.0, 0.0};
  double bias = 0.0;
  double base_learning_rate;
  int n_updates = 0;
  int warmup_fills;

  // Running z-score normalization (Welford's algorithm)
  std::array<double, N_TOXICITY_FEATURES> feat_mean = {};
  std::array<double, N_TOXICITY_FEATURES> feat_m2 = {};
  int feat_count = 0;

  explicit OnlineToxicityModel(double lr = 0.01, int warmup = 50)
      : base_learning_rate(lr), warmup_fills(warmup) {}

  double predict(const ToxicityFeatureVector& fv) const;
  void update(const ToxicityFeatureVector& fv, bool was_adverse);
  void update_normalization(const ToxicityFeatureVector& fv);
  bool in_warmup() const { return n_updates < warmup_fills; }
  double current_lr() const { return base_learning_rate / (1.0 + static_cast<double>(n_updates) / 1000.0); }
};

// ---- Market Maker Stats / Types ----

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

  // Update market data and recalculate quotes (single lock acquisition via snapshot)
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
  void set_toxicity_threshold(double threshold);
  void set_override_toxicity(double toxicity);
  void clear_override_toxicity();

  // Getters
  [[nodiscard]] MarketMakerStats get_stats() const;
  [[nodiscard]] double get_inventory() const;
  [[nodiscard]] double get_current_toxicity() const;

  // Reset strategy state
  void reset();

  // Strategy proposal methods
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

  // Strategy parameters - ELITE HFT MM (top-of-book priority)
  // Assumes: Sub-5μs latency, front-of-queue, excellent flow prediction
  double base_spread_ = 0.01;         // Penny spread at NBBO
  double min_spread_ = 0.01;          // Allow penny spread
  double max_spread_ = 0.10;          // Moderate widening in toxic conditions
  uint32_t base_quote_size_ = 1000;   // 1000 shares per side
  double max_position_ = 100000.0;    // Max inventory: 100k shares
  double tick_size_ = 0.01;

  double inventory_skew_coefficient_ = 0.02;  // Very gentle skew (excellent risk mgmt)
  double toxicity_spread_multiplier_ = 1.0;   // Minimal spread widening
  double toxicity_quote_threshold_ = 0.75;    // Very high threshold (almost always quote)
  double obi_threshold_ = 0.50;               // Only skip on extreme OBI

  MarketMakerStats stats_;

  // Execution model parameters (calibrated for elite HFT)
  double mu_adverse_ = 0.003;  // Expected adverse selection cost per share
  double gamma_risk_ = 0.0005; // Inventory risk penalty
  double fill_probability_ = 0.35;  // Expected fill rate (front of queue)

  // Online model override for toxicity score
  bool use_override_toxicity_ = false;
  double override_toxicity_ = 0.0;

  // Helper methods
  [[nodiscard]] double round_to_tick(double price) const noexcept;
  [[nodiscard]] double calculate_toxicity_adjusted_spread(double base_spread_val) const;
  [[nodiscard]] double calculate_inventory_skew() const noexcept;
  [[nodiscard]] double sigmoid(double x) const noexcept;
  [[nodiscard]] double calculate_obi() const;  // Order Book Imbalance
  [[nodiscard]] double get_average_toxicity() const;  // Average toxicity across levels

  // Snapshot-based overloads (single lock acquisition)
  [[nodiscard]] double calculate_toxicity_adjusted_spread_snap(double base_spread_val,
                                                               const OrderBook::BookSnapshot& snap) const;
  [[nodiscard]] double get_average_toxicity_snap(const OrderBook::BookSnapshot& snap) const;
  [[nodiscard]] static double calculate_obi_snap(const OrderBook::BookSnapshot& snap) ;
};
