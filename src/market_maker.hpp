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

  // Strategy parameters
  double base_spread_ = 0.01;
  double min_spread_ = 0.01;
  double max_spread_ = 0.10;
  uint32_t base_quote_size_ = 100;
  double max_position_ = 1000.0;
  double tick_size_ = 0.01;

  double inventory_skew_coefficient_ = 0.005;
  double toxicity_spread_multiplier_ = 2.0;

  MarketMakerStats stats_;

  // Helper methods
  [[nodiscard]] double round_to_tick(double price) const noexcept;
  [[nodiscard]] double calculate_toxicity_adjusted_spread(double base_spread_val) const;
  [[nodiscard]] double calculate_inventory_skew() const noexcept;
};
