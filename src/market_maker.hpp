#pragma once

#include "order_book.hpp"
#include <chrono>
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
private:
  OrderBook &order_book;
  bool use_toxicity_screen;

  int64_t inventory;
  double realized_pnl;
  double unrealized_pnl;
  // Average entry price for the current position.
  // - If inventory > 0 (long): avg_entry_price is the average buy price.
  // - If inventory < 0 (short): avg_entry_price is the average sell(short)
  // price.
  double avg_entry_price;

  MarketMakerQuote current_quotes;
  std::unordered_map<uint64_t, bool> our_order_ids;
  mutable std::mutex strategy_mutex;

  double base_spread;
  double min_spread;
  double max_spread;
  uint32_t base_quote_size;
  double max_position;
  double tick_size;

  double inventory_skew_coefficient;
  double toxicity_spread_multiplier;

  MarketMakerStats stats;

  double round_to_tick(double price) const;
  double calculate_toxicity_adjusted_spread(double base_spread_val) const;
  double calculate_inventory_skew() const;

public:
  MarketMakerStrategy(OrderBook &ob, bool use_toxicity = false);

  void update_market_data();
  MarketMakerQuote get_current_quotes() const;
  void on_order_filled(bool is_buy, double price, uint32_t size);
  void register_our_order(uint64_t order_id);
  bool is_our_order(uint64_t order_id) const;
  void on_order_cancelled(uint64_t order_id);
  MarketMakerStats get_stats() const;
  double get_inventory() const;
  void set_base_spread(double spread);
  void set_toxicity_multiplier(double multiplier);
  void reset();
};
