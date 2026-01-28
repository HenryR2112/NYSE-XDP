#include "market_maker.hpp"
#include "order_book.hpp"
#include <cmath>

MarketMakerStrategy::MarketMakerStrategy(OrderBook &ob, bool use_toxicity)
    : order_book(ob), use_toxicity_screen(use_toxicity), inventory(0),
      realized_pnl(0.0), unrealized_pnl(0.0), avg_entry_price(0.0),
      base_spread(0.01), min_spread(0.01), max_spread(0.10),
      base_quote_size(100), max_position(1000.0), tick_size(0.01),
      inventory_skew_coefficient(0.005), toxicity_spread_multiplier(2.0) {
  stats.start_time = std::chrono::steady_clock::now();
}

double MarketMakerStrategy::round_to_tick(double price) const {
  return std::round(price / tick_size) * tick_size;
}

double MarketMakerStrategy::calculate_toxicity_adjusted_spread(
    double base_spread_val) const {
  if (!use_toxicity_screen) {
    return base_spread_val;
  }

  auto book_stats = order_book.get_stats();
  if (book_stats.best_bid == 0.0 || book_stats.best_ask == 0.0) {
    return base_spread_val;
  }

  double toxicity_sum = 0.0;
  int toxicity_count = 0;

  auto bids = order_book.get_bids();
  auto asks = order_book.get_asks();

  int levels_to_check = 5;
  int bid_levels = 0;
  for (const auto &bid : bids) {
    if (bid_levels >= levels_to_check)
      break;
    double toxicity = order_book.get_toxicity(bid.first, 'B');
    toxicity_sum += toxicity;
    toxicity_count++;
    bid_levels++;
  }

  int ask_levels = 0;
  for (const auto &ask : asks) {
    if (ask_levels >= levels_to_check)
      break;
    double toxicity = order_book.get_toxicity(ask.first, 'S');
    toxicity_sum += toxicity;
    toxicity_count++;
    ask_levels++;
  }

  double avg_toxicity = 0.0;
  if (toxicity_count > 0) {
    avg_toxicity = toxicity_sum / toxicity_count;
  }

  double adjusted_spread =
      base_spread_val * (1.0 + avg_toxicity * toxicity_spread_multiplier);
  return std::min(std::max(adjusted_spread, min_spread), max_spread);
}

double MarketMakerStrategy::calculate_inventory_skew() const {
  double inventory_ratio = static_cast<double>(inventory) / max_position;
  return -inventory_ratio * inventory_skew_coefficient;
}

void MarketMakerStrategy::update_market_data() {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  auto book_stats = order_book.get_stats();

  if (book_stats.best_bid == 0.0 || book_stats.best_ask == 0.0) {
    return;
  }

  double mid_price = book_stats.mid_price;
  double spread = calculate_toxicity_adjusted_spread(base_spread);
  double half_spread = spread / 2.0;
  double inventory_skew = calculate_inventory_skew();

  current_quotes.bid_price =
      round_to_tick(mid_price - half_spread + inventory_skew);
  current_quotes.ask_price =
      round_to_tick(mid_price + half_spread + inventory_skew);

  if (current_quotes.bid_price >= current_quotes.ask_price) {
    current_quotes.bid_price = round_to_tick(mid_price - tick_size);
    current_quotes.ask_price = round_to_tick(mid_price + tick_size);
  }

  if (inventory > max_position * 0.5) {
    current_quotes.bid_size = base_quote_size / 2;
    current_quotes.ask_size = base_quote_size * 2;
  } else if (inventory < -max_position * 0.5) {
    current_quotes.bid_size = base_quote_size * 2;
    current_quotes.ask_size = base_quote_size / 2;
  } else {
    current_quotes.bid_size = base_quote_size;
    current_quotes.ask_size = base_quote_size;
  }

  double last_trade = order_book.get_last_trade();
  const double mark = (last_trade > 0.0) ? last_trade : mid_price;
  if (inventory > 0) {
    unrealized_pnl = (mark - avg_entry_price) * static_cast<double>(inventory);
  } else if (inventory < 0) {
    unrealized_pnl = (avg_entry_price - mark) * static_cast<double>(-inventory);
  } else {
    unrealized_pnl = 0.0;
  }
}

MarketMakerQuote MarketMakerStrategy::get_current_quotes() const {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  return current_quotes;
}

void MarketMakerStrategy::on_order_filled(bool is_buy, double price,
                                          uint32_t size) {
  std::lock_guard<std::mutex> lock(strategy_mutex);

  const int64_t qty = static_cast<int64_t>(size);

  if (is_buy) {
    // BUY: increases long, or reduces short (realize PnL on the covered part).
    if (inventory >= 0) {
      const int64_t new_pos = inventory + qty;
      if (new_pos != 0) {
        avg_entry_price = (avg_entry_price * static_cast<double>(inventory) +
                           price * static_cast<double>(qty)) /
                          static_cast<double>(new_pos);
      } else {
        avg_entry_price = 0.0;
      }
      inventory = new_pos;
    } else {
      // Covering a short
      const int64_t cover_qty = std::min<int64_t>(qty, -inventory);
      realized_pnl +=
          (avg_entry_price - price) * static_cast<double>(cover_qty);
      inventory += cover_qty; // toward zero

      const int64_t remaining = qty - cover_qty;
      if (inventory == 0 && remaining > 0) {
        // Flip to long with remaining
        inventory = remaining;
        avg_entry_price = price;
      } else if (inventory == 0) {
        avg_entry_price = 0.0;
      }
    }

    stats.buy_fills++;
    stats.avg_fill_price_buy =
        (stats.avg_fill_price_buy * (stats.buy_fills - 1) + price) /
        stats.buy_fills;
  } else {
    // SELL: reduces long (realize PnL on sold part), or increases short.
    if (inventory <= 0) {
      const int64_t new_short_abs = (-inventory) + qty;
      if (new_short_abs != 0) {
        avg_entry_price = (avg_entry_price * static_cast<double>(-inventory) +
                           price * static_cast<double>(qty)) /
                          static_cast<double>(new_short_abs);
      } else {
        avg_entry_price = 0.0;
      }
      inventory -= qty; // more negative
    } else {
      const int64_t close_qty = std::min<int64_t>(qty, inventory);
      realized_pnl +=
          (price - avg_entry_price) * static_cast<double>(close_qty);
      inventory -= close_qty;

      const int64_t remaining = qty - close_qty;
      if (inventory == 0 && remaining > 0) {
        // Flip to short with remaining
        inventory = -remaining;
        avg_entry_price = price;
      } else if (inventory == 0) {
        avg_entry_price = 0.0;
      }
    }

    stats.sell_fills++;
    stats.avg_fill_price_sell =
        (stats.avg_fill_price_sell * (stats.sell_fills - 1) + price) /
        stats.sell_fills;
  }

  stats.total_fills++;
  stats.total_volume_traded += size;

  if (static_cast<double>(inventory) > stats.max_inventory) {
    stats.max_inventory = static_cast<double>(inventory);
  }
  if (static_cast<double>(inventory) < stats.min_inventory) {
    stats.min_inventory = static_cast<double>(inventory);
  }
}

void MarketMakerStrategy::register_our_order(uint64_t order_id) {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  our_order_ids[order_id] = true;
}

bool MarketMakerStrategy::is_our_order(uint64_t order_id) const {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  return our_order_ids.find(order_id) != our_order_ids.end();
}

void MarketMakerStrategy::on_order_cancelled(uint64_t order_id) {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  our_order_ids.erase(order_id);
}

MarketMakerStats MarketMakerStrategy::get_stats() const {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  MarketMakerStats s = stats;
  s.realized_pnl = realized_pnl;
  s.unrealized_pnl = unrealized_pnl;
  return s;
}

double MarketMakerStrategy::get_inventory() const {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  return static_cast<double>(inventory);
}

void MarketMakerStrategy::set_base_spread(double spread) {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  base_spread = spread;
}

void MarketMakerStrategy::set_toxicity_multiplier(double multiplier) {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  toxicity_spread_multiplier = multiplier;
}

void MarketMakerStrategy::reset() {
  std::lock_guard<std::mutex> lock(strategy_mutex);
  inventory = 0;
  realized_pnl = 0.0;
  unrealized_pnl = 0.0;
  avg_entry_price = 0.0;
  current_quotes = MarketMakerQuote();
  our_order_ids.clear();
  stats = MarketMakerStats();
  stats.start_time = std::chrono::steady_clock::now();
}
