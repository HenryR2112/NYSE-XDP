#include "per_symbol_sim.hpp"

#include "common/symbol_map.hpp"

#include <algorithm>
#include <cmath>

namespace mmsim {

PerSymbolSim::PerSymbolSim()
    : order_book(),
      mm_baseline(order_book, false),
      mm_toxicity(order_book, true), order_info(),
      latency_us_dist(0.0, 1.0), uni01(0.0, 1.0) {}

void PerSymbolSim::ensure_init(uint32_t idx, const SimConfig& config) {
  if (initialized)
    return;
  initialized = true;
  symbol_index = idx;
  config_ = &config;
  cached_ticker = xdp::get_symbol(idx);

  const uint64_t seed =
      config.exec.seed ^ (static_cast<uint64_t>(idx) * 0x9E3779B97F4A7C15ULL);
  rng.seed(seed);

  // Microsecond latency distribution for HFT
  latency_us_dist = std::normal_distribution<double>(
      config.exec.latency_us_mean, config.exec.latency_us_jitter);

  // Net fee = maker_rebate - clearing_fee (we receive rebate, pay clearing)
  double net_fee = -(config.exec.maker_rebate_per_share - config.exec.clearing_fee_per_share);
  mm_baseline.set_fee_per_share(net_fee);
  mm_toxicity.set_fee_per_share(net_fee);
  mm_baseline.set_taker_fee_per_share(config.exec.taker_fee_per_share);
  mm_toxicity.set_taker_fee_per_share(config.exec.taker_fee_per_share);

  // Apply CLI-overridden toxicity parameters
  if (config.toxicity_threshold > 0.0) {
    mm_toxicity.set_toxicity_threshold(config.toxicity_threshold);
  }
  if (config.toxicity_multiplier > 0.0) {
    mm_toxicity.set_toxicity_multiplier(config.toxicity_multiplier);
  }
  mm_toxicity.set_ablation_mode(config.ablation_mode);
  mm_toxicity.set_epsilon_min(config.epsilon_min);

  // Initialize online learning model with CLI params
  if (config.online_learning) {
    online_model = OnlineToxicityModel(config.learning_rate, config.warmup_fills);
  }

  // Initialize EWMA filter if selected
  if (config.filter_type == FilterType::EWMA) {
    ewma_filter = EWMAFilter(config.ewma_alpha, config.ewma_threshold_k, config.ewma_min_obs);
  }
}

uint64_t PerSymbolSim::sample_latency_ns() {
  double us = latency_us_dist(rng);
  if (us < 5.0) us = 5.0;  // Minimum 5us even with colo
  return static_cast<uint64_t>(us * 1000.0);  // Convert us to ns
}

uint32_t PerSymbolSim::calculate_queue_position(double price, char side) {
  uint32_t visible_depth = 0;
  if (side == 'B') {
    auto bids = order_book.get_bids();
    auto it = bids.find(price);
    if (it != bids.end()) visible_depth = it->second;
  } else {
    auto asks = order_book.get_asks();
    auto it = asks.find(price);
    if (it != asks.end()) visible_depth = it->second;
  }

  if (visible_depth == 0) return 0;

  // Our queue position is a fraction of visible depth with variance
  double base_position = visible_depth * config_->exec.queue_position_fraction;
  double variance = base_position * config_->exec.queue_position_variance;
  std::normal_distribution<double> pos_dist(base_position, variance);
  double pos = pos_dist(rng);
  return static_cast<uint32_t>(std::max(0.0, pos));
}

bool PerSymbolSim::check_eligibility() const {
  auto stats = order_book.get_stats();

  // Need valid BBO
  if (stats.best_bid <= 0 || stats.best_ask <= 0) return false;

  // Check spread requirements
  if (stats.spread < config_->exec.min_spread_to_trade) return false;
  if (stats.spread > config_->exec.max_spread_to_trade) return false;

  // Check depth requirements
  if (stats.total_bid_qty < config_->exec.min_depth_to_trade) return false;
  if (stats.total_ask_qty < config_->exec.min_depth_to_trade) return false;

  return true;
}

bool PerSymbolSim::check_risk_limits(SymbolRiskState& risk) const {
  double total_pnl = risk.realized_pnl + risk.unrealized_pnl + risk.total_adverse_pnl;
  if (total_pnl < -config_->exec.max_daily_loss_per_symbol) {
    risk.halted = true;
    return false;
  }
  return true;
}

ToxicityFeatureVector PerSymbolSim::build_feature_vector() const {
  ToxicityFeatureVector fv;

  // Average the 5 order-book features across top 3 bid + 3 ask levels
  constexpr int levels = 3;
  int count = 0;
  auto bids = order_book.get_bids();
  auto asks = order_book.get_asks();

  // Also accumulate cancel volume intensity across levels
  double total_vol_cancelled = 0.0;
  double total_vol_added = 0.0;

  int bid_levels = 0;
  for (const auto& [price, qty] : bids) {
    if (bid_levels >= levels) break;
    auto fr = order_book.get_feature_ratios(price, 'B');
    fv.features[0] += fr.cancel_ratio;
    fv.features[1] += fr.ping_ratio;
    fv.features[2] += fr.odd_lot_ratio;
    fv.features[3] += fr.precision_ratio;
    fv.features[4] += fr.resistance_ratio;
    auto tm = order_book.get_toxicity_metrics(price, 'B');
    total_vol_cancelled += tm.total_volume_cancelled;
    total_vol_added += tm.total_volume_added;
    count++;
    bid_levels++;
  }
  int ask_levels = 0;
  for (const auto& [price, qty] : asks) {
    if (ask_levels >= levels) break;
    auto fr = order_book.get_feature_ratios(price, 'S');
    fv.features[0] += fr.cancel_ratio;
    fv.features[1] += fr.ping_ratio;
    fv.features[2] += fr.odd_lot_ratio;
    fv.features[3] += fr.precision_ratio;
    fv.features[4] += fr.resistance_ratio;
    auto tm = order_book.get_toxicity_metrics(price, 'S');
    total_vol_cancelled += tm.total_volume_cancelled;
    total_vol_added += tm.total_volume_added;
    count++;
    ask_levels++;
  }
  if (count > 0) {
    for (int i = 0; i < 5; i++) fv.features[i] /= count;
  }

  // Temporal dynamics
  fv.features[5] = trade_flow.get_imbalance();
  fv.features[6] = spread_tracker.get_spread_change_rate();
  fv.features[7] = momentum_tracker.get_momentum();

  // --- Structural features (book-wide) ---
  auto stats = order_book.get_stats();

  // [8] Cancel volume intensity: volume_cancelled / volume_added
  fv.features[8] = (total_vol_added > 0)
      ? total_vol_cancelled / total_vol_added
      : 0.0;

  // [9] Top-of-book concentration: avg fraction of total depth at best level
  double bid_conc = (stats.total_bid_qty > 0 && !bids.empty())
      ? static_cast<double>(bids.begin()->second) / static_cast<double>(stats.total_bid_qty)
      : 0.0;
  double ask_conc = (stats.total_ask_qty > 0 && !asks.empty())
      ? static_cast<double>(asks.begin()->second) / static_cast<double>(stats.total_ask_qty)
      : 0.0;
  fv.features[9] = (bid_conc + ask_conc) / 2.0;

  // [10] Depth imbalance: (bid_qty - ask_qty) / total_qty (OFI proxy)
  double total_qty = static_cast<double>(stats.total_bid_qty) + static_cast<double>(stats.total_ask_qty);
  fv.features[10] = (total_qty > 0)
      ? (static_cast<double>(stats.total_bid_qty) - static_cast<double>(stats.total_ask_qty)) / total_qty
      : 0.0;

  // [11] Level count asymmetry
  double total_levels = static_cast<double>(stats.bid_levels + stats.ask_levels);
  fv.features[11] = (total_levels > 0)
      ? static_cast<double>(stats.bid_levels - stats.ask_levels) / total_levels
      : 0.0;

  // [12] Absolute trade flow imbalance (VPIN magnitude — Easley, López de Prado & O'Hara)
  fv.features[12] = std::abs(fv.features[5]);

  // [13] Large order ratio: large_order_count / total_events across top levels
  double total_large = 0.0;
  double total_events_all = 0.0;
  {
    int lvl = 0;
    for (const auto& [price, qty] : bids) {
      if (lvl >= 3) break;
      auto tm = order_book.get_toxicity_metrics(price, 'B');
      total_large += tm.large_order_count;
      total_events_all += tm.adds + tm.cancels;
      lvl++;
    }
    lvl = 0;
    for (const auto& [price, qty] : asks) {
      if (lvl >= 3) break;
      auto tm = order_book.get_toxicity_metrics(price, 'S');
      total_large += tm.large_order_count;
      total_events_all += tm.adds + tm.cancels;
      lvl++;
    }
  }
  fv.features[13] = (total_events_all > 0) ? total_large / total_events_all : 0.0;

  // [14] Normalized spread: spread / mid_price (relative transaction cost)
  fv.features[14] = (stats.mid_price > 0)
      ? stats.spread / stats.mid_price
      : 0.0;

  return fv;
}

void PerSymbolSim::measure_adverse_selection(std::vector<FillRecord>& fills,
                                              std::vector<FillRecord>* completed,
                                              SymbolRiskState& risk,
                                              uint64_t now_ns) {
  auto stats = order_book.get_stats();
  double current_mid = stats.mid_price;

  for (auto& fill : fills) {
    if (fill.adverse_measured) continue;

    // Check if enough time has passed
    uint64_t elapsed_us = (now_ns - fill.fill_time_ns) / 1000;
    if (elapsed_us < config_->exec.adverse_lookforward_us) continue;

    // Mark as measured even if we can't calculate (to allow cleanup)
    fill.adverse_measured = true;

    // Skip adverse calculation if no valid mid price
    if (current_mid <= 0) continue;

    // Measure adverse price movement
    double price_change = current_mid - fill.mid_price_at_fill;

    // For buys: adverse if price went down after we bought
    // For sells: adverse if price went up after we sold
    double adverse_move = fill.is_buy ? -price_change : price_change;

    if (adverse_move > 0) {
      // We got adversely selected - charge a fraction of the move
      fill.adverse_pnl = -adverse_move * fill.fill_qty *
                         config_->exec.adverse_selection_multiplier;
      risk.total_adverse_pnl += fill.adverse_pnl;
      risk.adverse_fills++;
    }

    // Train online model: label = was there meaningful adverse selection?
    // Only train SGD for logistic filter; EWMA updates in update_quotes() instead.
    if (config_->online_learning && config_->filter_type == FilterType::LOGISTIC) {
      bool was_adverse = (adverse_move > 0.005);  // > half a cent threshold
      online_model.update(fill.features, was_adverse);
    }
  }

  // Clean up old measured fills - also force cleanup if vector is too large
  if (fills.size() > 10000) {
    // Emergency cleanup - mark old fills as measured
    for (size_t i = 0; i < fills.size() - 5000; i++) {
      fills[i].adverse_measured = true;
    }
  }

  // Move measured fills to completed vector (for CSV output) before erasing
  if (completed) {
    for (auto& f : fills) {
      if (f.adverse_measured) {
        completed->push_back(std::move(f));
      }
    }
  }

  fills.erase(
      std::remove_if(fills.begin(), fills.end(),
                     [](const FillRecord& f) { return f.adverse_measured; }),
      fills.end());
}

bool PerSymbolSim::eligible_for_fill(double quote_px, double exec_px,
                                      bool is_bid_side) const {
  if (config_->exec.fill_mode == ExecutionModelConfig::FillMode::Match) {
    return std::abs(quote_px - exec_px) < 1e-12;
  }
  return is_bid_side ? (quote_px >= exec_px) : (quote_px <= exec_px);
}

void PerSymbolSim::update_virtual_order(VirtualOrder& vo, double price,
                                         uint32_t size, char side,
                                         uint64_t now_ns) {
  const bool price_changed = vo.price != price;
  const bool changed = (!vo.live) || price_changed ||
                       (vo.size != size) || (vo.remaining == 0);
  if (!changed)
    return;

  uint64_t latency_ns = sample_latency_ns();

  // If we're changing price, there's an exposure window where stale quote is live
  if (vo.live && price_changed) {
    vo.exposed_until_ns = now_ns + config_->exec.quote_exposure_window_us * 1000;
  }

  vo.price = price;
  vo.size = size;
  vo.remaining = size;
  // Calculate queue position based on current visible depth at this price
  vo.queue_ahead = calculate_queue_position(price, side);
  vo.active_at_ns = now_ns + latency_ns;
  vo.live = (price > 0.0 && size > 0);
}

void PerSymbolSim::update_quotes(uint64_t now_ns) {
  uint64_t quote_interval_ns = config_->exec.quote_update_interval_us * 1000;
  if (now_ns - last_quote_update_ns < quote_interval_ns)
    return;
  last_quote_update_ns = now_ns;

  // Measure adverse selection on any pending fills
  // Pass completed vectors for CSV output when output directory is set
  auto* bc = config_->output_dir.empty() ? nullptr : &baseline_completed_fills;
  auto* tc = config_->output_dir.empty() ? nullptr : &toxicity_completed_fills;
  measure_adverse_selection(baseline_pending_fills, bc, baseline_risk, now_ns);
  measure_adverse_selection(toxicity_pending_fills, tc, toxicity_risk, now_ns);

  // Update spread and momentum trackers
  {
    auto book_stats = order_book.get_stats();
    if (book_stats.spread > 0) spread_tracker.record_spread(book_stats.spread);
    if (book_stats.mid_price > 0) momentum_tracker.record_mid(book_stats.mid_price);
  }

  // End-of-day liquidation: MUST come before eligibility check.
  // By 15:50 ET, book depth thins out and symbols may fail eligibility —
  // but we still need to force-close any open positions.
  // Uses gmtime_r (UTC) since PCAP timestamps are Unix epoch.
  // Aug 22 2023 is EDT: NYSE 15:50 ET = 19:50 UTC = 71400 seconds into UTC day
  if (!eod_liquidated) {
    uint64_t now_sec = now_ns / 1000000000ULL;
    time_t t = static_cast<time_t>(now_sec);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    int seconds_into_utc_day = tm_utc.tm_hour * 3600 + tm_utc.tm_min * 60 + tm_utc.tm_sec;
    if (seconds_into_utc_day >= 71400) {
      mm_baseline.force_close_position();
      mm_toxicity.force_close_position();
      eod_liquidated = true;
      eligible_to_trade = false;
      return;
    }
  } else {
    eligible_to_trade = false;
    return;
  }

  // Check eligibility and risk limits
  eligible_to_trade = check_eligibility();
  if (!eligible_to_trade) return;

  if (blacklisted) {
    eligible_to_trade = false;
    return;
  }

  if (!check_risk_limits(baseline_risk) || !check_risk_limits(toxicity_risk)) {
    return;
  }

  // Per-symbol blacklisting: check every 50 fills if this symbol is a persistent loser
  int64_t total_fills = baseline_risk.total_fills + toxicity_risk.total_fills;
  if (total_fills >= blacklist_check_fills + 50) {
    blacklist_check_fills = total_fills;
    double baseline_pnl = baseline_risk.realized_pnl + baseline_risk.total_adverse_pnl;
    double toxicity_pnl = toxicity_risk.realized_pnl + toxicity_risk.total_adverse_pnl;
    // Blacklist if BOTH strategies are losing badly after enough fills
    if (total_fills >= 100 && baseline_pnl < -500.0 && toxicity_pnl < -500.0) {
      blacklisted = true;
      eligible_to_trade = false;
      return;
    }
  }

  // Walk-forward window boundary detection
  if (config_->walk_forward && config_->online_learning) {
    if (!wf_initialized) {
      wf_window_duration_ns = static_cast<uint64_t>(config_->wf_window_minutes) * 60ULL * 1000000000ULL;
      wf_window_start_ns = now_ns;
      wf_initialized = true;
      current_wf_window = 0;
    }

    uint64_t elapsed = now_ns - wf_window_start_ns;
    int new_window = static_cast<int>(elapsed / wf_window_duration_ns);

    if (new_window > current_wf_window) {
      // Snapshot current window's PnL before transition
      const auto tox_stats = mm_toxicity.get_stats();
      const auto base_stats = mm_baseline.get_stats();
      WFWindowMetrics wm;
      wm.window_id = current_wf_window;
      wm.toxicity_pnl = tox_stats.realized_pnl + tox_stats.unrealized_pnl
                         + toxicity_risk.total_adverse_pnl;
      wm.baseline_pnl = base_stats.realized_pnl + base_stats.unrealized_pnl
                         + baseline_risk.total_adverse_pnl;
      wm.fills = toxicity_risk.total_fills;
      wm.suppressed = tox_stats.quotes_suppressed;
      wf_window_metrics.push_back(wm);

      // Window boundary crossed:
      // 1. Snapshot current learned weights
      auto snap = online_model.snapshot();
      // 2. Apply as frozen weights for next window predictions
      online_model.apply_frozen(snap);
      // 3. Reset learning state for new window (keeps normalization stats)
      online_model.reset_for_new_window();
      current_wf_window = new_window;
    }
  }

  // Feed toxicity prediction to toxicity strategy based on filter type
  if (config_->filter_type == FilterType::EWMA) {
    auto fv = build_feature_vector();
    double cancel_ratio = fv.features[0];  // cancel_ratio is feature[0]

    if (!ewma_filter.in_warmup()) {
      double predicted_toxicity = ewma_filter.predict(cancel_ratio);
      mm_toxicity.set_override_toxicity(predicted_toxicity);
    }

    // Update EWMA with each observation (even during warmup)
    ewma_filter.update(cancel_ratio);
  } else if (config_->online_learning && !online_model.in_warmup()) {
    // Logistic model path
    auto fv = build_feature_vector();
    double predicted_toxicity;
    if (config_->walk_forward && online_model.has_frozen) {
      // Walk-forward: use frozen weights from prior window
      predicted_toxicity = online_model.predict_frozen(fv);
    } else {
      predicted_toxicity = online_model.predict(fv);
    }
    mm_toxicity.set_override_toxicity(predicted_toxicity);
  } else if (config_->walk_forward && config_->online_learning && online_model.has_frozen) {
    // During warmup in walk-forward mode, still use frozen weights if available
    auto fv = build_feature_vector();
    double predicted_toxicity = online_model.predict_frozen(fv);
    mm_toxicity.set_override_toxicity(predicted_toxicity);
  }

  mm_baseline.update_market_data();
  mm_toxicity.update_market_data();

  // Active inventory management: cross the spread to unwind excess inventory
  mm_baseline.try_unwind_inventory();
  mm_toxicity.try_unwind_inventory();

  const MarketMakerQuote q_base = mm_baseline.get_current_quotes();
  const MarketMakerQuote q_tox = mm_toxicity.get_current_quotes();

  // Track queue resets: check if virtual order will change before updating
  auto check_reset = [](const VirtualOrder& vo, double price, uint32_t size) -> bool {
    return vo.live && (vo.price != price || vo.size != size);
  };
  if (check_reset(baseline_state.bid, q_base.bid_price, q_base.bid_size))
    diag_baseline.quote_resets++;
  if (check_reset(baseline_state.ask, q_base.ask_price, q_base.ask_size))
    diag_baseline.quote_resets++;
  if (check_reset(toxicity_state.bid, q_tox.bid_price, q_tox.bid_size))
    diag_toxicity.quote_resets++;
  if (check_reset(toxicity_state.ask, q_tox.ask_price, q_tox.ask_size))
    diag_toxicity.quote_resets++;

  update_virtual_order(baseline_state.bid, q_base.bid_price, q_base.bid_size,
                       'B', now_ns);
  update_virtual_order(baseline_state.ask, q_base.ask_price, q_base.ask_size,
                       'S', now_ns);

  update_virtual_order(toxicity_state.bid, q_tox.bid_price, q_tox.bid_size,
                       'B', now_ns);
  update_virtual_order(toxicity_state.ask, q_tox.ask_price, q_tox.ask_size,
                       'S', now_ns);
}

void PerSymbolSim::on_add(uint64_t order_id, double price, uint32_t volume,
                           char side, uint64_t now_ns) {
  order_info[order_id] = {side, price, volume, now_ns};
  order_book.add_order(order_id, price, volume, side);

  // Periodic cleanup of stale orders (every 60 seconds of market time)
  constexpr uint64_t CLEANUP_INTERVAL_NS = 60ULL * 1000000000ULL;  // 60 seconds
  constexpr uint64_t MAX_ORDER_AGE_NS = 600ULL * 1000000000ULL;    // 10 minutes max age
  if (now_ns - last_cleanup_ns > CLEANUP_INTERVAL_NS) {
    last_cleanup_ns = now_ns;
    // Remove orders older than MAX_ORDER_AGE_NS
    for (auto it = order_info.begin(); it != order_info.end(); ) {
      if (now_ns - it->second.add_time_ns > MAX_ORDER_AGE_NS) {
        it = order_info.erase(it);
      } else {
        ++it;
      }
    }
  }
}

void PerSymbolSim::on_modify(uint64_t order_id, double price, uint32_t volume) {
  auto it = order_info.find(order_id);
  if (it != order_info.end()) {
    // If price changed, treat old price level as cancel for queue purposes
    if (std::abs(it->second.price - price) > 0.0001) {
      update_queue_on_cancel(it->second.price, it->second.volume, it->second.side);
    }
    it->second.price = price;
    it->second.volume = volume;
  }
  order_book.modify_order(order_id, price, volume);
}

void PerSymbolSim::update_queue_on_cancel(double price, uint32_t volume, char side) {
  auto update_vo = [&](VirtualOrder& vo, bool is_bid) {
    if (!vo.live || vo.queue_ahead == 0) return;
    // Only update if cancel is at our quote price and same side
    if ((is_bid && side == 'B') || (!is_bid && side == 'S')) {
      if (std::abs(vo.price - price) < 0.0001) {
        // Order ahead of us cancelled - improve our queue position
        vo.queue_ahead = (vo.queue_ahead > volume) ? (vo.queue_ahead - volume) : 0;
      }
    }
  };

  update_vo(baseline_state.bid, true);
  update_vo(baseline_state.ask, false);
  update_vo(toxicity_state.bid, true);
  update_vo(toxicity_state.ask, false);
}

void PerSymbolSim::on_delete(uint64_t order_id) {
  auto it = order_info.find(order_id);
  if (it != order_info.end()) {
    // Update queue positions before removing order info
    update_queue_on_cancel(it->second.price, it->second.volume, it->second.side);
    order_info.erase(it);
  }
  order_book.delete_order(order_id);
}

void PerSymbolSim::on_replace(uint64_t old_order_id, uint64_t new_order_id,
                               double price, uint32_t volume, char side,
                               uint64_t now_ns) {
  auto it = order_info.find(old_order_id);
  if (it != order_info.end()) {
    // Old order leaving queue - update queue positions
    update_queue_on_cancel(it->second.price, it->second.volume, it->second.side);
    order_info.erase(it);
  }
  order_info[new_order_id] = {side, price, volume, now_ns};

  order_book.delete_order(old_order_id);
  order_book.add_order(new_order_id, price, volume, side);
}

void PerSymbolSim::try_fill_one(MarketMakerStrategy& mm, StrategyExecState& st,
                                 std::vector<FillRecord>& pending_fills,
                                 SymbolRiskState& risk,
                                 FillDiagnostics& diag,
                                 bool is_bid_side, double exec_price,
                                 uint32_t exec_qty, uint64_t now_ns) {
  diag.try_fill_calls++;

  // Check if halted due to loss limits
  if (risk.halted) { diag.rejected_halted++; return; }

  VirtualOrder& vo = is_bid_side ? st.bid : st.ask;
  if (!vo.live || vo.remaining == 0) {
    diag.rejected_not_live++;
    return;
  }

  // Order must be active (past latency period)
  if (now_ns < vo.active_at_ns) {
    diag.rejected_latency++;
    return;
  }

  // Check price eligibility
  if (!eligible_for_fill(vo.price, exec_price, is_bid_side)) {
    diag.rejected_price++;
    return;
  }

  // During quote exposure window, we're more vulnerable to adverse fills
  // Model this as higher fill probability (bad fills get through)
  bool in_exposure_window = (now_ns < vo.exposed_until_ns);

  // Queue position logic - must wait our turn
  uint32_t qty_left = exec_qty;
  if (vo.queue_ahead > 0 && !in_exposure_window) {
    // Normal case: consume queue ahead of us
    const uint32_t consume = std::min(vo.queue_ahead, qty_left);
    vo.queue_ahead -= consume;
    qty_left -= consume;
  }
  // During exposure window, skip queue logic (we get adversely picked off)

  if (qty_left == 0) {
    diag.rejected_queue++;
    return;
  }

  const uint32_t fill_qty = std::min(vo.remaining, qty_left);
  if (fill_qty == 0)
    return;

  // Execute the fill
  vo.remaining -= fill_qty;
  mm.on_order_filled(is_bid_side, vo.price, fill_qty);
  risk.total_fills++;
  diag.fill_succeeded++;

  // Let inventory accumulate; periodic unwind in update_quotes() handles excess.
  // Immediate unwind after fill is self-defeating: taker fees > rebate income.

  // Track inventory variance for hypothesis testing H3
  risk.update_inventory_variance(mm.get_inventory());

  // Record fill for adverse selection measurement
  auto stats = order_book.get_stats();
  FillRecord record;
  record.fill_time_ns = now_ns;
  record.fill_price = vo.price;
  record.fill_qty = fill_qty;
  record.is_buy = is_bid_side;
  record.mid_price_at_fill = stats.mid_price;

  // Build feature vector and store with fill
  record.features = build_feature_vector();

  if (config_->filter_type == FilterType::EWMA && !ewma_filter.in_warmup()) {
    record.toxicity_at_fill = ewma_filter.predict(record.features.features[0]);
  } else if (config_->online_learning && !online_model.in_warmup()) {
    record.toxicity_at_fill = online_model.predict(record.features);
  } else {
    record.toxicity_at_fill = mm.get_current_toxicity();
  }
  pending_fills.push_back(record);
}

void PerSymbolSim::maybe_fill_on_execution(char resting_side, double exec_price,
                                            uint32_t exec_qty, uint64_t now_ns) {
  // Try fills with EXISTING virtual orders first (before updating them).
  // In real trading, resting orders are already on the exchange when an
  // execution happens — the fill check should use the current state.
  // Updating quotes afterward adjusts prices for the NEXT execution.
  if (eligible_to_trade) {
    if (resting_side == 'B') {
      try_fill_one(mm_baseline, baseline_state, baseline_pending_fills,
                   baseline_risk, diag_baseline, true, exec_price, exec_qty, now_ns);
      try_fill_one(mm_toxicity, toxicity_state, toxicity_pending_fills,
                   toxicity_risk, diag_toxicity, true, exec_price, exec_qty, now_ns);
    } else if (resting_side == 'S') {
      try_fill_one(mm_baseline, baseline_state, baseline_pending_fills,
                   baseline_risk, diag_baseline, false, exec_price, exec_qty, now_ns);
      try_fill_one(mm_toxicity, toxicity_state, toxicity_pending_fills,
                   toxicity_risk, diag_toxicity, false, exec_price, exec_qty, now_ns);
    }
  } else {
    diag_baseline.exec_not_eligible++;
    diag_toxicity.exec_not_eligible++;
  }

  // THEN update quotes for the next execution cycle
  update_quotes(now_ns);
}

void PerSymbolSim::on_execute(uint64_t order_id, uint32_t exec_qty,
                               double exec_price, uint64_t now_ns) {
  diag_baseline.exec_total++;
  diag_toxicity.exec_total++;

  auto it = order_info.find(order_id);
  if (it != order_info.end()) {
    // Feed trade flow tracker with execution side
    bool is_buy = (it->second.side == 'B');
    trade_flow.record_trade(is_buy, exec_qty);

    maybe_fill_on_execution(it->second.side, exec_price, exec_qty, now_ns);

    // Update volume tracking (partial fills reduce remaining volume)
    if (it->second.volume > exec_qty) {
      it->second.volume -= exec_qty;
    } else {
      order_info.erase(it);
    }
  } else {
    diag_baseline.exec_no_order_info++;
    diag_toxicity.exec_no_order_info++;

    // Order ID not in our map (cross-group boundary or cleaned up).
    // Try both sides — price eligibility in try_fill_one prevents wrong-side fills.
    maybe_fill_on_execution('B', exec_price, exec_qty, now_ns);
    maybe_fill_on_execution('S', exec_price, exec_qty, now_ns);
  }

  order_book.execute_order(order_id, exec_qty, exec_price);
}

} // namespace mmsim
