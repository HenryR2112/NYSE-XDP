#include "market_maker.hpp"
#include <algorithm>
#include <cmath>

// ---- EWMAFilter implementation ----

EWMAFilter::EWMAFilter(double a, double k, int min_obs)
    : alpha(a), threshold_k(k), min_observations(min_obs) {}

void EWMAFilter::update(double cancel_ratio) {
  n_observations++;
  if (n_observations == 1) {
    ewma_mean = cancel_ratio;
    ewma_var = 0.0;
  } else {
    double delta = cancel_ratio - ewma_mean;
    ewma_mean = (1.0 - alpha) * ewma_mean + alpha * cancel_ratio;
    // EWMA variance: exponentially weighted variance
    ewma_var = (1.0 - alpha) * (ewma_var + alpha * delta * delta);
  }
}

double EWMAFilter::get_std() const {
  return std::sqrt(std::max(ewma_var, 1e-10));
}

double EWMAFilter::get_threshold() const {
  return ewma_mean + threshold_k * get_std();
}

bool EWMAFilter::in_warmup() const {
  return n_observations < min_observations;
}

double EWMAFilter::predict(double cancel_ratio) const {
  if (in_warmup()) return 0.0;  // No signal during warmup
  double z = (cancel_ratio - ewma_mean) / get_std();
  // Normalize so that threshold_k maps to 0.5, clip to [0, 1]
  // Negative z means below-average cancel ratio (safe) -> 0
  return std::max(0.0, std::min(1.0, z / (2.0 * threshold_k)));
}

bool EWMAFilter::is_toxic(double cancel_ratio) const {
  if (in_warmup()) return false;
  return cancel_ratio > get_threshold();
}

// ---- MarketMakerStrategy implementation ----

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
  constexpr int levels_to_check = 3;

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

void MarketMakerStrategy::set_ablation_mode(mmsim::AblationMode mode) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  ablation_mode_ = mode;
}

void MarketMakerStrategy::set_epsilon_min(double eps) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  epsilon_min_ = eps;
}

void MarketMakerStrategy::clear_override_toxicity() {
  use_override_toxicity_ = false;
  override_toxicity_ = 0.0;
}

// Snapshot-based helpers: operate on pre-fetched data (no additional lock acquisitions)

double MarketMakerStrategy::get_average_toxicity_snap(const OrderBook::BookSnapshot& snap) const {
  if (use_override_toxicity_) {
    return override_toxicity_;
  }
  double toxicity_sum = 0.0;
  int toxicity_count = 0;
  for (int i = 0; i < snap.num_bid_levels; i++) {
    toxicity_sum += snap.bid_levels[i].toxicity_score;
    toxicity_count++;
  }
  for (int i = 0; i < snap.num_ask_levels; i++) {
    toxicity_sum += snap.ask_levels[i].toxicity_score;
    toxicity_count++;
  }
  return (toxicity_count > 0) ? toxicity_sum / toxicity_count : 0.0;
}

double MarketMakerStrategy::calculate_toxicity_adjusted_spread_snap(
    double base_spread_val, const OrderBook::BookSnapshot& snap) const {
  if (!use_toxicity_screen_) {
    return base_spread_val;
  }
  if (snap.stats.best_bid == 0.0 || snap.stats.best_ask == 0.0) {
    return base_spread_val;
  }
  double avg_toxicity = get_average_toxicity_snap(snap);
  double adjusted_spread =
      base_spread_val * (1.0 + avg_toxicity * toxicity_spread_multiplier_);
  return std::clamp(adjusted_spread, min_spread_, max_spread_);
}

double MarketMakerStrategy::calculate_obi_snap(const OrderBook::BookSnapshot& snap) {
  double total_qty = static_cast<double>(snap.stats.total_bid_qty + snap.stats.total_ask_qty);
  if (total_qty < 1.0) return 0.0;
  return (static_cast<double>(snap.stats.total_bid_qty) -
          static_cast<double>(snap.stats.total_ask_qty)) / total_qty;
}

void MarketMakerStrategy::update_market_data() {
  // Single lock acquisition: capture all needed book state at once
  auto snap = order_book_.get_snapshot();

  // Calculate OBI and toxicity from snapshot (no further lock acquisitions)
  double avg_toxicity = 0.0;
  double obi = 0.0;

  if (use_toxicity_screen_) {
    avg_toxicity = get_average_toxicity_snap(snap);
    obi = calculate_obi_snap(snap);
  }

  std::lock_guard<std::mutex> lock(strategy_mutex_);

  if (snap.stats.best_bid == 0.0 || snap.stats.best_ask == 0.0) {
    current_quotes_.is_quoted = false;
    return;
  }

  double mid_price = snap.stats.mid_price;

  // Spread widening: active in FULL and SPREAD_ONLY modes
  const bool apply_spread =
      use_toxicity_screen_ &&
      (ablation_mode_ == mmsim::AblationMode::FULL ||
       ablation_mode_ == mmsim::AblationMode::SPREAD_ONLY);
  double spread = apply_spread
      ? calculate_toxicity_adjusted_spread_snap(base_spread_, snap)
      : base_spread_;

  double half_spread = spread / 2.0;
  double inventory_skew = calculate_inventory_skew();

  // PnL filter: active in FULL and PNL_FILTER_ONLY modes
  const bool apply_pnl_filter =
      use_toxicity_screen_ &&
      (ablation_mode_ == mmsim::AblationMode::FULL ||
       ablation_mode_ == mmsim::AblationMode::PNL_FILTER_ONLY);

  if (use_toxicity_screen_) {
    stats_.avg_toxicity = avg_toxicity;
  }

  if (apply_pnl_filter) {
    // Skip quoting entirely if toxicity is too high
    if (avg_toxicity > toxicity_quote_threshold_) {
      stats_.quotes_suppressed++;
      current_quotes_.is_quoted = false;
      current_quotes_.bid_size = 0;
      current_quotes_.ask_size = 0;
      return;
    }

    // Calculate expected PnL using BASE spread (not toxicity-adjusted).
    // Fills near NBBO capture approximately base spread, regardless of our wider quote.
    // The wider spread reduces fill rate but doesn't increase per-fill income proportionally.
    double inventory_risk = gamma_risk_ * inventory_ * inventory_;
    double expected_pnl = calculate_expected_pnl(base_spread_, avg_toxicity, inventory_risk);

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

  // Quote at full size — maximize fill opportunities
  current_quotes_.bid_size = base_quote_size_;
  current_quotes_.ask_size = base_quote_size_;

  // Inventory-proportional size skew: aggressively favor offsetting side
  double inventory_pct = static_cast<double>(inventory_) / max_position_;
  if (inventory_pct > 0.8) {
    // Very long: stop buying entirely, sell at 3x
    current_quotes_.bid_size = 0;
    current_quotes_.ask_size = base_quote_size_ * 3;
  } else if (inventory_pct > 0.3) {
    // Long: ramp down buying, ramp up selling (0→1 over 0.3..0.8 range)
    double ratio = (inventory_pct - 0.3) / 0.5;  // 0..1
    current_quotes_.bid_size = static_cast<uint32_t>(base_quote_size_ * (1.0 - 0.8 * ratio));
    current_quotes_.ask_size = static_cast<uint32_t>(base_quote_size_ * (1.0 + ratio));
  } else if (inventory_pct < -0.8) {
    // Very short: stop selling entirely, buy at 3x
    current_quotes_.bid_size = base_quote_size_ * 3;
    current_quotes_.ask_size = 0;
  } else if (inventory_pct < -0.3) {
    // Short: ramp down selling, ramp up buying
    double ratio = (-inventory_pct - 0.3) / 0.5;
    current_quotes_.bid_size = static_cast<uint32_t>(base_quote_size_ * (1.0 + ratio));
    current_quotes_.ask_size = static_cast<uint32_t>(base_quote_size_ * (1.0 - 0.8 * ratio));
  }

  // OBI-based asymmetric spread: active in FULL and OBI_ONLY modes
  // Continuous adjustment: tighten the safe side, widen the risky side
  const bool apply_obi =
      use_toxicity_screen_ &&
      (ablation_mode_ == mmsim::AblationMode::FULL ||
       ablation_mode_ == mmsim::AblationMode::OBI_ONLY);
  if (apply_obi && std::abs(obi) > 0.01) {
    // Positive OBI (more bids, price going up): buying safe, selling risky
    double obi_factor = std::clamp(obi * 0.5, -0.3, 0.3);
    double bid_half = half_spread * (1.0 - obi_factor);  // tighter when OBI>0
    double ask_half = half_spread * (1.0 + obi_factor);  // wider when OBI>0
    current_quotes_.bid_price = round_to_tick(mid_price - bid_half + inventory_skew);
    current_quotes_.ask_price = round_to_tick(mid_price + ask_half + inventory_skew);
    // Ensure bid < ask
    if (current_quotes_.bid_price >= current_quotes_.ask_price) {
      current_quotes_.bid_price = round_to_tick(mid_price - tick_size_);
      current_quotes_.ask_price = round_to_tick(mid_price + tick_size_);
    }
  }

  current_quotes_.is_quoted = (current_quotes_.bid_size > 0 || current_quotes_.ask_size > 0);

  // Update unrealized PnL
  const double mark = (snap.last_traded_price > 0.0) ? snap.last_traded_price : mid_price;

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

void MarketMakerStrategy::set_taker_fee_per_share(double fee) {
  std::lock_guard<std::mutex> lock(strategy_mutex_);
  taker_fee_per_share_ = fee;
}

void MarketMakerStrategy::try_unwind_inventory() {
  // Two triggers for crossing the spread to close:
  // 1. Take-profit: mark has moved favorably by take_profit_threshold_
  // 2. Safety unwind: |inventory| exceeds unwind_threshold_

  // Get snapshot without holding strategy_mutex_ (order_book has its own lock)
  auto snap = order_book_.get_snapshot();
  if (snap.stats.best_bid <= 0 || snap.stats.best_ask <= 0) return;

  std::lock_guard<std::mutex> lock(strategy_mutex_);

  if (inventory_ == 0) return;

  int64_t abs_inv = std::abs(inventory_);
  int64_t unwind_qty = 0;

  if (inventory_ > 0) {
    double mark = snap.stats.best_bid;
    double unrealized_per_share = mark - avg_entry_price_;

    if (unrealized_per_share >= take_profit_threshold_) {
      // Take profit: close entire position
      unwind_qty = inventory_;
    } else if (abs_inv > unwind_threshold_) {
      // Safety: trim excess only
      unwind_qty = abs_inv - unwind_threshold_;
    }

    if (unwind_qty <= 0) return;

    double qty_d = static_cast<double>(unwind_qty);
    int64_t close_qty = std::min(unwind_qty, inventory_);
    realized_pnl_ += (mark - avg_entry_price_) * static_cast<double>(close_qty);
    inventory_ -= close_qty;
    if (inventory_ == 0) avg_entry_price_ = 0.0;
    realized_pnl_ -= taker_fee_per_share_ * qty_d;
    stats_.sell_fills++;
    stats_.total_fills++;
    stats_.total_volume_traded += static_cast<uint32_t>(unwind_qty);
    stats_.unwind_crosses++;
    stats_.unwind_cost += taker_fee_per_share_ * qty_d;
  } else {
    double mark = snap.stats.best_ask;
    double unrealized_per_share = avg_entry_price_ - mark;

    if (unrealized_per_share >= take_profit_threshold_) {
      // Take profit: close entire position
      unwind_qty = -inventory_;
    } else if (abs_inv > unwind_threshold_) {
      // Safety: trim excess only
      unwind_qty = abs_inv - unwind_threshold_;
    }

    if (unwind_qty <= 0) return;

    double qty_d = static_cast<double>(unwind_qty);
    int64_t close_qty = std::min(unwind_qty, -inventory_);
    realized_pnl_ += (avg_entry_price_ - mark) * static_cast<double>(close_qty);
    inventory_ += close_qty;
    if (inventory_ == 0) avg_entry_price_ = 0.0;
    realized_pnl_ -= taker_fee_per_share_ * qty_d;
    stats_.buy_fills++;
    stats_.total_fills++;
    stats_.total_volume_traded += static_cast<uint32_t>(unwind_qty);
    stats_.unwind_crosses++;
    stats_.unwind_cost += taker_fee_per_share_ * qty_d;
  }
}

void MarketMakerStrategy::force_close_position() {
  auto snap = order_book_.get_snapshot();
  if (snap.stats.best_bid <= 0 || snap.stats.best_ask <= 0) return;

  std::lock_guard<std::mutex> lock(strategy_mutex_);
  if (inventory_ == 0) return;

  int64_t abs_inv = std::abs(inventory_);
  double qty_d = static_cast<double>(abs_inv);

  if (inventory_ > 0) {
    double mark = snap.stats.best_bid;
    realized_pnl_ += (mark - avg_entry_price_) * static_cast<double>(inventory_);
    stats_.sell_fills++;
  } else {
    double mark = snap.stats.best_ask;
    realized_pnl_ += (avg_entry_price_ - mark) * static_cast<double>(-inventory_);
    stats_.buy_fills++;
  }

  inventory_ = 0;
  avg_entry_price_ = 0.0;
  unrealized_pnl_ = 0.0;
  realized_pnl_ -= taker_fee_per_share_ * qty_d;
  stats_.total_fills++;
  stats_.total_volume_traded += static_cast<uint32_t>(abs_inv);
  stats_.unwind_crosses++;
  stats_.unwind_cost += taker_fee_per_share_ * qty_d;

  // Stop quoting
  current_quotes_.is_quoted = false;
  current_quotes_.bid_size = 0;
  current_quotes_.ask_size = 0;
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
}

double MarketMakerStrategy::sigmoid(double x) const noexcept {
  return 1.0 / (1.0 + std::exp(-x));
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
  return expected_pnl > epsilon_min_ && std::abs(inventory_) < max_position_;
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

OnlineToxicityModel::WeightSnapshot OnlineToxicityModel::snapshot() const {
  WeightSnapshot snap;
  snap.weights = weights;
  snap.bias = bias;
  snap.feat_mean = feat_mean;
  snap.feat_m2 = feat_m2;
  snap.feat_count = feat_count;
  snap.n_updates = n_updates;
  return snap;
}

void OnlineToxicityModel::apply_frozen(const WeightSnapshot& snap) {
  frozen_snap = snap;
  has_frozen = true;
}

void OnlineToxicityModel::reset_for_new_window() {
  // Reset learning state to initial weights (all N_TOXICITY_FEATURES)
  weights = {0.40, 0.20, 0.15, 0.15, 0.10,   // book ratios
             0.0, 0.0, 0.0,                    // temporal
             0.0, 0.0, 0.0, 0.0,              // structural
             0.0, 0.0, 0.0};                   // VPIN mag, large orders, norm spread
  bias = 0.0;
  n_updates = 0;
  // Keep normalization stats (feat_mean, feat_m2, feat_count) continuous
  // so z-score normalization remains stable across windows
}

double OnlineToxicityModel::predict_frozen(const ToxicityFeatureVector& fv) const {
  if (!has_frozen) {
    // No frozen weights yet (window 1) - fall back to live predict
    return predict(fv);
  }

  // If frozen snapshot didn't pass warmup, use raw weighted sum (like predict during warmup)
  if (frozen_snap.n_updates < warmup_fills) {
    double score = 0.0;
    for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
      score += frozen_snap.weights[i] * fv.features[i];
    }
    score += frozen_snap.bias;
    return std::min(std::max(score, 0.0), 1.0);
  }

  // Post-warmup: normalize features using frozen normalization stats, then sigmoid
  double z = frozen_snap.bias;
  for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
    double std_dev = (frozen_snap.feat_count > 1)
        ? std::sqrt(frozen_snap.feat_m2[i] / static_cast<double>(frozen_snap.feat_count - 1))
        : 1.0;
    double x_norm = (std_dev > 1e-10)
        ? (fv.features[i] - frozen_snap.feat_mean[i]) / std_dev
        : 0.0;
    z += frozen_snap.weights[i] * x_norm;
  }
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
