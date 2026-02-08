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
  // Non-linear skew: more aggressive as position grows
  double skew = -inventory_ratio * inventory_skew_coefficient_;
  // Add quadratic term for larger positions
  skew -= 0.5 * inventory_ratio * std::abs(inventory_ratio) * inventory_skew_coefficient_;
  return skew;
}

double MarketMakerStrategy::calculate_obi() const {
  auto book_stats = order_book_.get_stats();
  double total_qty = static_cast<double>(book_stats.total_bid_qty + book_stats.total_ask_qty);
  if (total_qty < 1.0) return 0.0;
  // OBI ranges from -1 (all asks) to +1 (all bids)
  return (static_cast<double>(book_stats.total_bid_qty) -
          static_cast<double>(book_stats.total_ask_qty)) / total_qty;
}

double MarketMakerStrategy::get_average_toxicity() const {
  // If override is set (from online learning model), use it directly
  if (use_override_toxicity_) {
    return override_toxicity_;
  }

  double toxicity_sum = 0.0;
  int toxicity_count = 0;
  constexpr int levels_to_check = 3;

  auto bids = order_book_.get_bids();
  auto asks = order_book_.get_asks();

  int bid_levels = 0;
  for (const auto &[price, qty] : bids) {
    if (bid_levels >= levels_to_check) break;
    toxicity_sum += order_book_.get_toxicity(price, 'B');
    toxicity_count++;
    bid_levels++;
  }

  int ask_levels = 0;
  for (const auto &[price, qty] : asks) {
    if (ask_levels >= levels_to_check) break;
    toxicity_sum += order_book_.get_toxicity(price, 'S');
    toxicity_count++;
    ask_levels++;
  }

  return (toxicity_count > 0) ? toxicity_sum / toxicity_count : 0.0;
}

void MarketMakerStrategy::set_override_toxicity(double toxicity) {
  use_override_toxicity_ = true;
  override_toxicity_ = toxicity;
}

void MarketMakerStrategy::clear_override_toxicity() {
  use_override_toxicity_ = false;
  override_toxicity_ = 0.0;
}

void MarketMakerStrategy::update_market_data() {
  // Calculate OBI and toxicity before acquiring strategy lock
  // (these methods acquire the order book lock internally)
  double avg_toxicity = 0.0;
  double obi = 0.0;

  if (use_toxicity_screen_) {
    avg_toxicity = get_average_toxicity();
    obi = calculate_obi();
  }

  std::lock_guard<std::mutex> lock(strategy_mutex_);
  auto book_stats = order_book_.get_stats();

  if (book_stats.best_bid == 0.0 || book_stats.best_ask == 0.0) {
    current_quotes_.is_quoted = false;
    return;
  }

  double mid_price = book_stats.mid_price;
  double spread = calculate_toxicity_adjusted_spread(base_spread_);
  double half_spread = spread / 2.0;
  double inventory_skew = calculate_inventory_skew();

  if (use_toxicity_screen_) {
    stats_.avg_toxicity = avg_toxicity;

    // Skip quoting entirely if toxicity is too high
    if (avg_toxicity > toxicity_quote_threshold_) {
      stats_.quotes_suppressed++;
      current_quotes_.is_quoted = false;
      current_quotes_.bid_size = 0;
      current_quotes_.ask_size = 0;
      return;
    }

    // Calculate expected PnL and check if we should quote
    double inventory_risk = gamma_risk_ * inventory_ * inventory_;
    double expected_pnl = calculate_expected_pnl(spread, avg_toxicity, inventory_risk);

    if (!should_quote(expected_pnl)) {
      stats_.quotes_suppressed++;
      current_quotes_.is_quoted = false;
      current_quotes_.bid_size = 0;
      current_quotes_.ask_size = 0;
      return;
    }
  }

  current_quotes_.bid_price =
      round_to_tick(mid_price - half_spread + inventory_skew);
  current_quotes_.ask_price =
      round_to_tick(mid_price + half_spread + inventory_skew);

  // Ensure bid < ask
  if (current_quotes_.bid_price >= current_quotes_.ask_price) {
    current_quotes_.bid_price = round_to_tick(mid_price - tick_size_);
    current_quotes_.ask_price = round_to_tick(mid_price + tick_size_);
  }

  // Start with base sizes
  current_quotes_.bid_size = base_quote_size_;
  current_quotes_.ask_size = base_quote_size_;

  // Adjust quote sizes based on inventory (more aggressive)
  double inventory_pct = static_cast<double>(inventory_) / max_position_;
  if (inventory_pct > 0.7) {
    // Very long: stop buying, sell aggressively
    current_quotes_.bid_size = 0;
    current_quotes_.ask_size = base_quote_size_ * 3;
  } else if (inventory_pct > 0.3) {
    // Moderately long: reduce buying, increase selling
    current_quotes_.bid_size = base_quote_size_ / 2;
    current_quotes_.ask_size = base_quote_size_ * 2;
  } else if (inventory_pct < -0.7) {
    // Very short: stop selling, buy aggressively
    current_quotes_.bid_size = base_quote_size_ * 3;
    current_quotes_.ask_size = 0;
  } else if (inventory_pct < -0.3) {
    // Moderately short: reduce selling, increase buying
    current_quotes_.bid_size = base_quote_size_ * 2;
    current_quotes_.ask_size = base_quote_size_ / 2;
  }

  // OBI-based quote adjustment (only for toxicity-aware strategy)
  if (use_toxicity_screen_) {
    // Strong positive OBI (more bids) suggests price going up
    // -> safer to buy, riskier to sell
    if (obi > obi_threshold_) {
      // Price likely going up: reduce/skip sell side
      current_quotes_.ask_size = current_quotes_.ask_size / 2;
      // Widen ask price to avoid adverse selection
      current_quotes_.ask_price = round_to_tick(current_quotes_.ask_price + tick_size_);
    } else if (obi < -obi_threshold_) {
      // Price likely going down: reduce/skip buy side
      current_quotes_.bid_size = current_quotes_.bid_size / 2;
      // Widen bid price to avoid adverse selection
      current_quotes_.bid_price = round_to_tick(current_quotes_.bid_price - tick_size_);
    }
  }

  current_quotes_.is_quoted = (current_quotes_.bid_size > 0 || current_quotes_.ask_size > 0);

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

void MarketMakerStrategy::set_toxicity_threshold(double threshold) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  toxicity_quote_threshold_ = threshold;
}

double MarketMakerStrategy::get_current_toxicity() const {
  return get_average_toxicity();
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
  toxicity_window_ = ToxicityWindow();
  toxicity_window_.start_time = std::chrono::steady_clock::now();
}

double MarketMakerStrategy::sigmoid(double x) const noexcept {
  return 1.0 / (1.0 + std::exp(-x));
}

double MarketMakerStrategy::calculate_toxicity_score(double cancel_rate, double obi, double short_vol) const noexcept {
  // Equation (8) from strategy proposal
  // p_toxic = σ(α1 · r_cancel + α2 · |OBI| + α3 · σ_short)
  double score = alpha1_ * cancel_rate + alpha2_ * std::abs(obi) + alpha3_ * short_vol;
  return sigmoid(score);
}

double MarketMakerStrategy::calculate_expected_pnl(double spread, double toxicity, double inventory_risk) const noexcept {
  // Modified equation (14) from strategy proposal to include rebates
  // E[PnL] = P(fill) · (s/2 + rebate - p_toxic · μ_a) - γI²
  double half_spread_capture = spread / 2.0;
  double rebate_per_share = -fee_per_share_;  // Negative fee = rebate
  double expected_adverse = toxicity * mu_adverse_;
  double expected_pnl = fill_probability_ * (half_spread_capture + rebate_per_share - expected_adverse) - inventory_risk;
  return expected_pnl;
}

bool MarketMakerStrategy::should_quote(double expected_pnl) const noexcept {
  // Require slightly positive expected PnL - lowered threshold for more fills
  // while still filtering clearly unprofitable situations
  return expected_pnl > 0.0005 && std::abs(inventory_) < max_position_;
}

// ---- OnlineToxicityModel implementation ----

void OnlineToxicityModel::update_normalization(const ToxicityFeatureVector& fv) {
  feat_count++;
  for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
    double delta = fv.features[i] - feat_mean[i];
    feat_mean[i] += delta / static_cast<double>(feat_count);
    double delta2 = fv.features[i] - feat_mean[i];
    feat_m2[i] += delta * delta2;
  }
}

double OnlineToxicityModel::predict(const ToxicityFeatureVector& fv) const {
  // During warmup, use hardcoded weights (no normalization)
  if (n_updates < warmup_fills) {
    double score = 0.0;
    // Apply initial weights directly to raw features (same as old get_toxicity_score)
    for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
      score += weights[i] * fv.features[i];
    }
    score += bias;
    return std::min(std::max(score, 0.0), 1.0);
  }

  // Post-warmup: normalize features then apply sigmoid
  double z = bias;
  for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
    double std_dev = (feat_count > 1)
        ? std::sqrt(feat_m2[i] / static_cast<double>(feat_count - 1))
        : 1.0;
    double x_norm = (std_dev > 1e-10)
        ? (fv.features[i] - feat_mean[i]) / std_dev
        : 0.0;
    z += weights[i] * x_norm;
  }
  // Sigmoid
  return 1.0 / (1.0 + std::exp(-z));
}

void OnlineToxicityModel::update(const ToxicityFeatureVector& fv, bool was_adverse) {
  update_normalization(fv);

  if (n_updates < warmup_fills) {
    n_updates++;
    return;
  }

  double predicted = predict(fv);
  double label = was_adverse ? 1.0 : 0.0;
  double error = predicted - label;  // BCE gradient

  double lr = current_lr();

  for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
    double std_dev = (feat_count > 1)
        ? std::sqrt(feat_m2[i] / static_cast<double>(feat_count - 1))
        : 1.0;
    double x_norm = (std_dev > 1e-10)
        ? (fv.features[i] - feat_mean[i]) / std_dev
        : 0.0;
    weights[i] -= lr * error * x_norm;
    // Weight clipping for stability
    weights[i] = std::min(std::max(weights[i], -5.0), 5.0);
  }
  bias -= lr * error;
  bias = std::min(std::max(bias, -5.0), 5.0);

  n_updates++;
}
