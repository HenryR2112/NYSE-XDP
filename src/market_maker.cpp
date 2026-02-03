#include "market_maker.hpp"
#include <algorithm>
#include <cmath>

MarketMakerStrategy::MarketMakerStrategy(OrderBook &ob, bool use_toxicity)
    : order_book_(ob), use_toxicity_screen_(use_toxicity) {
  stats_.start_time = std::chrono::steady_clock::now();
}

double MarketMakerStrategy::round_to_tick(double price) const noexcept {
  return std::round(price / tick_size_) * tick_size_;
}

double MarketMakerStrategy::calculate_toxicity_adjusted_spread(
    double base_spread_val) const {
  if (!use_toxicity_screen_) {
    return base_spread_val;
  }

  auto book_stats = order_book_.get_stats();
  if (book_stats.best_bid == 0.0 || book_stats.best_ask == 0.0) {
    return base_spread_val;
  }

  double toxicity_sum = 0.0;
  int toxicity_count = 0;
  constexpr int levels_to_check = 5;

  auto bids = order_book_.get_bids();
  auto asks = order_book_.get_asks();

  int bid_levels = 0;
  for (const auto &[price, qty] : bids) {
    if (bid_levels >= levels_to_check)
      break;
    toxicity_sum += order_book_.get_toxicity(price, 'B');
    toxicity_count++;
    bid_levels++;
  }

  int ask_levels = 0;
  for (const auto &[price, qty] : asks) {
    if (ask_levels >= levels_to_check)
      break;
    toxicity_sum += order_book_.get_toxicity(price, 'S');
    toxicity_count++;
    ask_levels++;
  }

  double avg_toxicity = (toxicity_count > 0) ? toxicity_sum / toxicity_count : 0.0;

  double adjusted_spread =
      base_spread_val * (1.0 + avg_toxicity * toxicity_spread_multiplier_);
  return std::clamp(adjusted_spread, min_spread_, max_spread_);
}

double MarketMakerStrategy::calculate_inventory_skew() const noexcept {
  double inventory_ratio = static_cast<double>(inventory_) / max_position_;
  return -inventory_ratio * inventory_skew_coefficient_;
}

void MarketMakerStrategy::update_market_data() {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  auto book_stats = order_book_.get_stats();

  if (book_stats.best_bid == 0.0 || book_stats.best_ask == 0.0) {
    return;
  }

  double mid_price = book_stats.mid_price;
  double spread = calculate_toxicity_adjusted_spread(base_spread_);
  double half_spread = spread / 2.0;
  double inventory_skew = calculate_inventory_skew();

  current_quotes_.bid_price =
      round_to_tick(mid_price - half_spread + inventory_skew);
  current_quotes_.ask_price =
      round_to_tick(mid_price + half_spread + inventory_skew);

  // Ensure bid < ask
  if (current_quotes_.bid_price >= current_quotes_.ask_price) {
    current_quotes_.bid_price = round_to_tick(mid_price - tick_size_);
    current_quotes_.ask_price = round_to_tick(mid_price + tick_size_);
  }

  // Adjust quote sizes based on inventory
  if (inventory_ > max_position_ * 0.5) {
    current_quotes_.bid_size = base_quote_size_ / 2;
    current_quotes_.ask_size = base_quote_size_ * 2;
  } else if (inventory_ < -max_position_ * 0.5) {
    current_quotes_.bid_size = base_quote_size_ * 2;
    current_quotes_.ask_size = base_quote_size_ / 2;
  } else {
    current_quotes_.bid_size = base_quote_size_;
    current_quotes_.ask_size = base_quote_size_;
  }

  // Update unrealized PnL
  double last_trade = order_book_.get_last_trade();
  const double mark = (last_trade > 0.0) ? last_trade : mid_price;

  if (inventory_ > 0) {
    unrealized_pnl_ = (mark - avg_entry_price_) * static_cast<double>(inventory_);
  } else if (inventory_ < 0) {
    unrealized_pnl_ = (avg_entry_price_ - mark) * static_cast<double>(-inventory_);
  } else {
    unrealized_pnl_ = 0.0;
  }
}

MarketMakerQuote MarketMakerStrategy::get_current_quotes() const {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  return current_quotes_;
}

void MarketMakerStrategy::on_order_filled(bool is_buy, double price,
                                          uint32_t size) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);

  const int64_t qty = static_cast<int64_t>(size);

  if (is_buy) {
    // BUY: increases long, or reduces short
    if (inventory_ >= 0) {
      const int64_t new_pos = inventory_ + qty;
      if (new_pos != 0) {
        avg_entry_price_ = (avg_entry_price_ * static_cast<double>(inventory_) +
                            price * static_cast<double>(qty)) /
                           static_cast<double>(new_pos);
      } else {
        avg_entry_price_ = 0.0;
      }
      inventory_ = new_pos;
    } else {
      // Covering a short
      const int64_t cover_qty = std::min(qty, -inventory_);
      realized_pnl_ +=
          (avg_entry_price_ - price) * static_cast<double>(cover_qty);
      inventory_ += cover_qty;

      const int64_t remaining = qty - cover_qty;
      if (inventory_ == 0 && remaining > 0) {
        inventory_ = remaining;
        avg_entry_price_ = price;
      } else if (inventory_ == 0) {
        avg_entry_price_ = 0.0;
      }
    }

    stats_.buy_fills++;
    stats_.avg_fill_price_buy =
        (stats_.avg_fill_price_buy * (stats_.buy_fills - 1) + price) /
        stats_.buy_fills;
  } else {
    // SELL: reduces long, or increases short
    if (inventory_ <= 0) {
      const int64_t new_short_abs = (-inventory_) + qty;
      if (new_short_abs != 0) {
        avg_entry_price_ = (avg_entry_price_ * static_cast<double>(-inventory_) +
                            price * static_cast<double>(qty)) /
                           static_cast<double>(new_short_abs);
      } else {
        avg_entry_price_ = 0.0;
      }
      inventory_ -= qty;
    } else {
      const int64_t close_qty = std::min(qty, inventory_);
      realized_pnl_ +=
          (price - avg_entry_price_) * static_cast<double>(close_qty);
      inventory_ -= close_qty;

      const int64_t remaining = qty - close_qty;
      if (inventory_ == 0 && remaining > 0) {
        inventory_ = -remaining;
        avg_entry_price_ = price;
      } else if (inventory_ == 0) {
        avg_entry_price_ = 0.0;
      }
    }

    stats_.sell_fills++;
    stats_.avg_fill_price_sell =
        (stats_.avg_fill_price_sell * (stats_.sell_fills - 1) + price) /
        stats_.sell_fills;
  }

  stats_.total_fills++;
  stats_.total_volume_traded += size;

  // Fees/rebates applied on every fill
  realized_pnl_ -= fee_per_share_ * static_cast<double>(size);

  // Update inventory extremes
  double inv = static_cast<double>(inventory_);
  stats_.max_inventory = std::max(stats_.max_inventory, inv);
  stats_.min_inventory = std::min(stats_.min_inventory, inv);
}

void MarketMakerStrategy::set_fee_per_share(double fee) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  fee_per_share_ = fee;
}

void MarketMakerStrategy::register_our_order(uint64_t order_id) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  our_order_ids_[order_id] = true;
}

bool MarketMakerStrategy::is_our_order(uint64_t order_id) const {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  return our_order_ids_.find(order_id) != our_order_ids_.end();
}

void MarketMakerStrategy::on_order_cancelled(uint64_t order_id) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  our_order_ids_.erase(order_id);
}

MarketMakerStats MarketMakerStrategy::get_stats() const {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  MarketMakerStats s = stats_;
  s.realized_pnl = realized_pnl_;
  s.unrealized_pnl = unrealized_pnl_;
  return s;
}

double MarketMakerStrategy::get_inventory() const {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  return static_cast<double>(inventory_);
}

void MarketMakerStrategy::set_base_spread(double spread) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  base_spread_ = spread;
}

void MarketMakerStrategy::set_toxicity_multiplier(double multiplier) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  toxicity_spread_multiplier_ = multiplier;
}

void MarketMakerStrategy::reset() {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  inventory_ = 0;
  realized_pnl_ = 0.0;
  unrealized_pnl_ = 0.0;
  fee_per_share_ = 0.0;
  avg_entry_price_ = 0.0;
  current_quotes_ = MarketMakerQuote();
  our_order_ids_.clear();
  stats_ = MarketMakerStats();
  stats_.start_time = std::chrono::steady_clock::now();
}
