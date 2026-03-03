#pragma once

#include "execution_model.hpp"
#include "order_book.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// ---- Online Toxicity Learning Model ----

static constexpr int N_TOXICITY_FEATURES = 15;

struct ToxicityFeatureVector {
  std::array<double, N_TOXICITY_FEATURES> features = {};
  // --- Order-book microstructure (per-level averages, top 3 bid + ask) ---
  // [0]  cancel_ratio           Cancel events / total events
  // [1]  ping_ratio             Sub-10-share "ping" orders / total events
  // [2]  odd_lot_ratio          Non-round-lot orders / total events
  // [3]  precision_ratio        Sub-penny prices / total events
  // [4]  resistance_ratio       Psychological price levels / total events
  // --- Temporal dynamics (circular buffer trackers) ---
  // [5]  trade_flow_imbalance   (buy_vol - sell_vol) / total_vol (VPIN signed)
  // [6]  spread_change_rate     Spread widening rate over 50 observations
  // [7]  price_momentum         Mid-price change rate over 50 observations
  // --- Structural features (book-wide and volume-weighted) ---
  // [8]  cancel_vol_intensity   Volume cancelled / volume added (volume-weighted OTR)
  // [9]  top_of_book_conc       Best-level qty / total qty (book fragility)
  // [10] depth_imbalance        (bid_qty - ask_qty) / total_qty (OFI proxy)
  // [11] level_asymmetry        (bid_levels - ask_levels) / total_levels
  // [12] abs_trade_imbalance    |buy_vol - sell_vol| / total_vol (VPIN magnitude)
  // [13] large_order_ratio      Large orders (>200 shares) / total events
  // [14] normalized_spread      Spread / mid_price (relative transaction cost)
};

struct OnlineToxicityModel {
  std::array<double, N_TOXICITY_FEATURES> weights = {
      0.40, 0.20, 0.15, 0.15, 0.10,   // book ratios (cancel_ratio dominant per empirical analysis)
      0.0, 0.0, 0.0,                    // temporal (SGD learns from gradient)
      0.0, 0.0, 0.0, 0.0,              // structural (SGD learns)
      0.0, 0.0, 0.0                     // VPIN magnitude, large orders, norm spread (SGD learns)
  };
  double bias = 0.0;
  double base_learning_rate;
  int n_updates = 0;
  int warmup_fills;

  // Running z-score normalization (Welford's algorithm)
  std::array<double, N_TOXICITY_FEATURES> feat_mean = {};
  std::array<double, N_TOXICITY_FEATURES> feat_m2 = {};
  int feat_count = 0;

  // Walk-forward: frozen weights from prior window for out-of-sample prediction
  struct WeightSnapshot {
    std::array<double, N_TOXICITY_FEATURES> weights;
    double bias;
    std::array<double, N_TOXICITY_FEATURES> feat_mean;
    std::array<double, N_TOXICITY_FEATURES> feat_m2;
    int feat_count;
    int n_updates;
  };

  bool has_frozen = false;
  WeightSnapshot frozen_snap = {};

  explicit OnlineToxicityModel(double lr = 0.01, int warmup = 50)
      : base_learning_rate(lr), warmup_fills(warmup) {}

  double predict(const ToxicityFeatureVector& fv) const;
  void update(const ToxicityFeatureVector& fv, bool was_adverse);
  void update_normalization(const ToxicityFeatureVector& fv);
  bool in_warmup() const { return n_updates < warmup_fills; }
  double current_lr() const { return base_learning_rate / (1.0 + static_cast<double>(n_updates) / 1000.0); }

  // Walk-forward methods
  WeightSnapshot snapshot() const;              // Capture current learned state
  void apply_frozen(const WeightSnapshot& snap); // Set frozen weights for prediction
  void reset_for_new_window();                   // Reset learning state, keep normalization
  double predict_frozen(const ToxicityFeatureVector& fv) const; // Predict using frozen weights
};

// ---- EWMA Adaptive Threshold Filter ----
// Bollinger-band-style filter: tracks distribution of cancel ratios per symbol,
// flags current observation as toxic when it exceeds ewma_mean + k * ewma_std.

struct EWMAFilter {
  double ewma_mean = 0.0;
  double ewma_var = 0.0;
  int n_observations = 0;
  double alpha;
  double threshold_k;
  int min_observations;

  explicit EWMAFilter(double a = 0.05, double k = 1.5, int min_obs = 20);

  // Update EWMA stats with new cancel ratio observation
  void update(double cancel_ratio);

  // Get toxicity score: normalized z-score of current cancel ratio, clipped to [0,1]
  double predict(double cancel_ratio) const;

  // Is current cancel ratio above adaptive threshold?
  bool is_toxic(double cancel_ratio) const;

  // Is filter still in warmup (insufficient observations)?
  bool in_warmup() const;

  // Get current adaptive threshold value
  double get_threshold() const;

  // Get current EWMA standard deviation
  double get_std() const;
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
  int64_t unwind_crosses = 0;      // Number of active inventory unwind crosses
  double unwind_cost = 0.0;        // Total cost of unwind crosses
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
  void set_taker_fee_per_share(double fee);
  void set_base_spread(double spread);
  void set_toxicity_multiplier(double multiplier);
  void set_toxicity_threshold(double threshold);
  void set_override_toxicity(double toxicity);
  void clear_override_toxicity();
  void set_ablation_mode(mmsim::AblationMode mode);
  void set_epsilon_min(double eps);

  // Active inventory management: cross the spread to unwind excess inventory
  void try_unwind_inventory();
  // Force close entire position (EOD liquidation)
  void force_close_position();

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

  // Strategy parameters - HFT MM (top-of-book priority)
  double base_spread_ = 0.01;         // Penny spread at NBBO
  double min_spread_ = 0.01;          // Allow penny spread
  double max_spread_ = 0.10;          // Cap widening
  uint32_t base_quote_size_ = 300;    // 300 shares per side
  double max_position_ = 300.0;       // Tight max inventory
  double tick_size_ = 0.01;

  double inventory_skew_coefficient_ = 0.25;  // Very aggressive skew
  double toxicity_spread_multiplier_ = 3.0;   // Strong widening: T=0.3→$0.019, T=0.5→$0.025
  double toxicity_quote_threshold_ = 0.50;    // Suppress quoting in toxic conditions (T>0.50)

  MarketMakerStats stats_;

  // Execution model parameters
  double mu_adverse_ = 0.015;      // Empirical adverse cost ~$0.012/share; moderate filter
  double gamma_risk_ = 0.0000002;  // Soft suppression at ~75 shares inventory
  double fill_probability_ = 0.25; // Expected fill rate
  double epsilon_min_ = 0.0;       // Accept any non-negative E[PnL]

  // Taker fee (for active unwind crosses)
  double taker_fee_per_share_ = 0.003;
  // Inventory unwind: actively cross spread when |inventory| exceeds this
  int64_t unwind_threshold_ = 150;   // Trim excess above 150 shares via periodic unwind

  // Take-profit: close position when mark moves this far in our favor
  double take_profit_threshold_ = 0.20;  // $0.20 favorable move triggers close
  // Stop-loss: close position when mark moves this far against us (0 = disabled)
  double stop_loss_threshold_ = 0.0;

  // Online model override for toxicity score
  bool use_override_toxicity_ = false;
  double override_toxicity_ = 0.0;

  // Ablation mode: which toxicity components are active
  mmsim::AblationMode ablation_mode_ = mmsim::AblationMode::FULL;

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
  [[nodiscard]] static double calculate_obi_snap(const OrderBook::BookSnapshot& snap);
};
