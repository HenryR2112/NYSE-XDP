#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct Order {
  uint64_t order_id;
  double price;
  uint32_t volume;
  char side; // 'B' or 'S'
  std::chrono::system_clock::time_point timestamp;
};

class OrderBook {
public:
  // Toxicity tracking: detailed metrics per price level
  struct ToxicityMetrics {
    uint32_t adds = 0;
    uint32_t cancels = 0;
    uint32_t total_volume_added = 0;
    uint32_t total_volume_cancelled = 0;
    uint32_t ping_count = 0;        // Orders with volume < 10
    uint32_t large_order_count = 0; // Orders with volume > 200
    uint32_t odd_lot_count = 0;     // Orders with volume not divisible by 100
    uint32_t high_precision_price_count = 0; // Prices with > 2 decimal places
    uint32_t resistance_level_count = 0;     // Prices ending in .95, .99, .98, .01, .05

    struct FeatureRatios {
      double cancel_ratio = 0.0;
      double ping_ratio = 0.0;
      double odd_lot_ratio = 0.0;
      double precision_ratio = 0.0;
      double resistance_ratio = 0.0;
    };

    [[nodiscard]] FeatureRatios get_feature_ratios() const noexcept {
      FeatureRatios fr;
      uint32_t total_events = adds + cancels;
      if (total_events == 0)
        return fr;
      double te = static_cast<double>(total_events);
      fr.cancel_ratio = static_cast<double>(cancels) / te;
      fr.ping_ratio = static_cast<double>(ping_count) / te;
      fr.odd_lot_ratio = static_cast<double>(odd_lot_count) / te;
      fr.precision_ratio = static_cast<double>(high_precision_price_count) / te;
      fr.resistance_ratio = static_cast<double>(resistance_level_count) / te;
      return fr;
    }

    [[nodiscard]] double get_toxicity_score() const noexcept {
      auto fr = get_feature_ratios();
      double score = fr.cancel_ratio * 0.4 + fr.ping_ratio * 0.2 +
                     fr.odd_lot_ratio * 0.15 + fr.precision_ratio * 0.15 +
                     fr.resistance_ratio * 0.1;
      return std::min(score, 1.0);
    }

    [[nodiscard]] std::string get_explanation() const {
      if (adds + cancels == 0)
        return "No activity";

      std::stringstream ss;
      ss << "Events: " << adds << " adds, " << cancels << " cancels";
      if (ping_count > 0)
        ss << " | Pings: " << ping_count;
      if (odd_lot_count > 0)
        ss << " | Odd lots: " << odd_lot_count;
      if (high_precision_price_count > 0)
        ss << " | High precision: " << high_precision_price_count;
      if (resistance_level_count > 0)
        ss << " | Resistance levels: " << resistance_level_count;
      return ss.str();
    }
  };

  // Statistics
  struct BookStats {
    double best_bid = 0.0;
    double best_ask = 0.0;
    double spread = 0.0;
    double mid_price = 0.0;
    uint32_t total_bid_qty = 0;
    uint32_t total_ask_qty = 0;
    int bid_levels = 0;
    int ask_levels = 0;
  };

  // Lightweight snapshot for strategy quote updates - captures only
  // what the market-making strategies need in a single lock acquisition.
  struct BookSnapshot {
    BookStats stats;
    double last_traded_price = 0.0;
    // Top N levels with toxicity scores (price descending for bids, ascending for asks)
    static constexpr int MAX_LEVELS = 3;
    struct Level {
      double price = 0.0;
      uint32_t qty = 0;
      double toxicity_score = 0.0;
    };
    Level bid_levels[MAX_LEVELS] = {};
    Level ask_levels[MAX_LEVELS] = {};
    int num_bid_levels = 0;
    int num_ask_levels = 0;
  };

  [[nodiscard]] BookSnapshot get_snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    BookSnapshot snap;
    snap.stats = stats_;
    snap.last_traded_price = last_traded_price_;

    int i = 0;
    for (auto it = bids_.begin(); it != bids_.end() && i < BookSnapshot::MAX_LEVELS; ++it, ++i) {
      snap.bid_levels[i].price = it->first;
      snap.bid_levels[i].qty = it->second;
      auto tox_it = bid_toxicity_.find(it->first);
      if (tox_it != bid_toxicity_.end()) {
        snap.bid_levels[i].toxicity_score = tox_it->second.get_toxicity_score();
      }
    }
    snap.num_bid_levels = i;

    i = 0;
    for (auto it = asks_.begin(); it != asks_.end() && i < BookSnapshot::MAX_LEVELS; ++it, ++i) {
      snap.ask_levels[i].price = it->first;
      snap.ask_levels[i].qty = it->second;
      auto tox_it = ask_toxicity_.find(it->first);
      if (tox_it != ask_toxicity_.end()) {
        snap.ask_levels[i].toxicity_score = tox_it->second.get_toxicity_score();
      }
    }
    snap.num_ask_levels = i;

    return snap;
  }

  OrderBook() = default;

  void clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    bids_.clear();
    asks_.clear();
    active_orders_.clear();
    bid_toxicity_.clear();
    ask_toxicity_.clear();
    last_traded_price_ = 0.0;
    last_traded_volume_ = 0;
    total_bid_volume_ = 0;
    total_ask_volume_ = 0;
    update_stats();
  }

  void add_order(uint64_t order_id, double price, uint32_t volume, char side) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (side == 'B') {
      bids_[price] += volume;
      total_bid_volume_ += volume;
      update_toxicity_on_add(bid_toxicity_[price], price, volume);
    } else {
      asks_[price] += volume;
      total_ask_volume_ += volume;
      update_toxicity_on_add(ask_toxicity_[price], price, volume);
    }

    active_orders_[order_id] = {order_id, price, volume, side,
                                std::chrono::system_clock::now()};
    update_stats();
  }

  void modify_order(uint64_t order_id, double new_price, uint32_t new_volume) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = active_orders_.find(order_id);
    if (it == active_orders_.end())
      return;

    Order &order = it->second;

    // Remove from old price level (remove_volume_from_* updates running totals)
    if (order.side == 'B') {
      remove_volume_from_bids(order.price, order.volume);
      bids_[new_price] += new_volume;
      total_bid_volume_ += new_volume;
    } else {
      remove_volume_from_asks(order.price, order.volume);
      asks_[new_price] += new_volume;
      total_ask_volume_ += new_volume;
    }

    // Update order
    order.price = new_price;
    order.volume = new_volume;
    order.timestamp = std::chrono::system_clock::now();

    update_stats();
  }

  void delete_order(uint64_t order_id) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = active_orders_.find(order_id);
    if (it == active_orders_.end())
      return;

    const Order &order = it->second;

    if (order.side == 'B') {
      bid_toxicity_[order.price].cancels++;
      bid_toxicity_[order.price].total_volume_cancelled += order.volume;
      remove_volume_from_bids(order.price, order.volume);
    } else {
      ask_toxicity_[order.price].cancels++;
      ask_toxicity_[order.price].total_volume_cancelled += order.volume;
      remove_volume_from_asks(order.price, order.volume);
    }

    active_orders_.erase(it);
    update_stats();
  }

  void execute_order(uint64_t order_id, uint32_t executed_qty, double trade_price) {
    std::lock_guard<std::mutex> lock(mtx_);

    auto it = active_orders_.find(order_id);
    if (it == active_orders_.end())
      return;

    Order &order = it->second;

    if (order.volume > executed_qty) {
      // Partial fill
      order.volume -= executed_qty;
      if (order.side == 'B') {
        bids_[order.price] -= executed_qty;
        total_bid_volume_ -= executed_qty;
      } else {
        asks_[order.price] -= executed_qty;
        total_ask_volume_ -= executed_qty;
      }
    } else {
      // Full fill (remove_volume_from_* updates running totals)
      if (order.side == 'B') {
        remove_volume_from_bids(order.price, order.volume);
      } else {
        remove_volume_from_asks(order.price, order.volume);
      }
      active_orders_.erase(it);
    }

    last_traded_price_ = trade_price;
    last_traded_volume_ = executed_qty;
    update_stats();
  }

  // Atomic snapshot - captures all state in a single lock acquisition for consistent rendering
  struct AtomicSnapshot {
    BookStats stats;
    std::map<double, uint32_t, std::greater<double>> bids;
    std::map<double, uint32_t, std::less<double>> asks;
    std::unordered_map<uint64_t, Order> active_orders;
    double last_traded_price;
    uint32_t last_traded_volume;
  };

  [[nodiscard]] AtomicSnapshot get_atomic_snapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    AtomicSnapshot snapshot;
    snapshot.stats = stats_;
    snapshot.bids = bids_;
    snapshot.asks = asks_;
    snapshot.active_orders = active_orders_;
    snapshot.last_traded_price = last_traded_price_;
    snapshot.last_traded_volume = last_traded_volume_;
    return snapshot;
  }

  // Restore order book state from a snapshot (for checkpoint-based seeking)
  void restore_from_snapshot(const std::map<double, uint32_t, std::greater<double>> &bids,
                             const std::map<double, uint32_t, std::less<double>> &asks,
                             const std::unordered_map<uint64_t, Order> &active_orders) {
    std::lock_guard<std::mutex> lock(mtx_);
    bids_ = bids;
    asks_ = asks;
    active_orders_ = active_orders;
    // Clear toxicity metrics since we're restoring from checkpoint
    bid_toxicity_.clear();
    ask_toxicity_.clear();
    // Recompute running totals from restored state
    total_bid_volume_ = 0;
    for (const auto& [p, v] : bids_) total_bid_volume_ += v;
    total_ask_volume_ = 0;
    for (const auto& [p, v] : asks_) total_ask_volume_ += v;
    update_stats();
  }

  // Thread-safe getters that return copies (snapshots) to avoid race conditions
  [[nodiscard]] BookStats get_stats() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return stats_;
  }

  [[nodiscard]] std::map<double, uint32_t, std::greater<double>> get_bids() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return bids_;
  }

  [[nodiscard]] std::map<double, uint32_t, std::less<double>> get_asks() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return asks_;
  }

  [[nodiscard]] double get_last_trade() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return last_traded_price_;
  }

  // Get toxicity score for a price level (0.0 to 1.0)
  [[nodiscard]] double get_toxicity(double price, char side) const {
    std::lock_guard<std::mutex> lock(mtx_);

    if (side == 'B') {
      auto it = bid_toxicity_.find(price);
      if (it != bid_toxicity_.end()) {
        return it->second.get_toxicity_score();
      }
    } else {
      auto it = ask_toxicity_.find(price);
      if (it != ask_toxicity_.end()) {
        return it->second.get_toxicity_score();
      }
    }
    return 0.0;
  }

  // Get raw feature ratios for a price level
  [[nodiscard]] ToxicityMetrics::FeatureRatios get_feature_ratios(double price, char side) const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (side == 'B') {
      auto it = bid_toxicity_.find(price);
      if (it != bid_toxicity_.end()) return it->second.get_feature_ratios();
    } else {
      auto it = ask_toxicity_.find(price);
      if (it != ask_toxicity_.end()) return it->second.get_feature_ratios();
    }
    return ToxicityMetrics::FeatureRatios();
  }

  // Get detailed toxicity metrics for a price level
  [[nodiscard]] ToxicityMetrics get_toxicity_metrics(double price, char side) const {
    std::lock_guard<std::mutex> lock(mtx_);

    if (side == 'B') {
      auto it = bid_toxicity_.find(price);
      if (it != bid_toxicity_.end()) {
        return it->second;
      }
    } else {
      auto it = ask_toxicity_.find(price);
      if (it != ask_toxicity_.end()) {
        return it->second;
      }
    }
    return ToxicityMetrics();
  }

private:
  std::map<double, uint32_t, std::greater<double>> bids_; // Price descending
  std::map<double, uint32_t, std::less<double>> asks_;    // Price ascending
  std::unordered_map<uint64_t, Order> active_orders_;
  mutable std::mutex mtx_;

  double last_traded_price_ = 0.0;
  uint32_t last_traded_volume_ = 0;
  std::chrono::system_clock::time_point last_update_;

  std::map<double, ToxicityMetrics, std::greater<double>> bid_toxicity_;
  std::map<double, ToxicityMetrics, std::less<double>> ask_toxicity_;

  BookStats stats_;

  // Running totals for O(1) volume/level queries
  uint32_t total_bid_volume_ = 0;
  uint32_t total_ask_volume_ = 0;

  // Helper to remove volume from bids (updates running totals)
  void remove_volume_from_bids(double price, uint32_t volume) {
    auto it = bids_.find(price);
    if (it != bids_.end()) {
      if (it->second <= volume) {
        total_bid_volume_ -= it->second;
        bids_.erase(it);
      } else {
        it->second -= volume;
        total_bid_volume_ -= volume;
      }
    }
  }

  // Helper to remove volume from asks (updates running totals)
  void remove_volume_from_asks(double price, uint32_t volume) {
    auto it = asks_.find(price);
    if (it != asks_.end()) {
      if (it->second <= volume) {
        total_ask_volume_ -= it->second;
        asks_.erase(it);
      } else {
        it->second -= volume;
        total_ask_volume_ -= volume;
      }
    }
  }

  // Helper to update toxicity metrics on order add
  void update_toxicity_on_add(ToxicityMetrics &metrics, double price, uint32_t volume) {
    metrics.adds++;
    metrics.total_volume_added += volume;

    // Analyze volume characteristics
    if (volume < 10)
      metrics.ping_count++;
    if (volume > 200)
      metrics.large_order_count++;
    if (volume % 100 != 0)
      metrics.odd_lot_count++;

    // Analyze price characteristics
    double decimal_part = price - std::floor(price);

    // Check for high precision (more than 2 decimal places)
    double rounded_2dec = std::round(price * 100.0) / 100.0;
    if (std::abs(price - rounded_2dec) > 0.0001) {
      metrics.high_precision_price_count++;
    }

    // Check for resistance levels
    double decimal_rounded = std::round(decimal_part * 100.0) / 100.0;
    if (decimal_rounded == 0.95 || decimal_rounded == 0.99 ||
        decimal_rounded == 0.98 || decimal_rounded == 0.01 ||
        decimal_rounded == 0.05) {
      metrics.resistance_level_count++;
    }
  }

  void update_stats() {
    // Level counts and volumes from running totals (O(1))
    stats_.bid_levels = static_cast<int>(bids_.size());
    stats_.ask_levels = static_cast<int>(asks_.size());
    stats_.total_bid_qty = total_bid_volume_;
    stats_.total_ask_qty = total_ask_volume_;

    // Best bid/ask from map begin (O(1) for std::map)
    stats_.best_bid = bids_.empty() ? 0.0 : bids_.begin()->first;
    stats_.best_ask = asks_.empty() ? 0.0 : asks_.begin()->first;

    if (stats_.best_bid > 0 && stats_.best_ask > 0) {
      stats_.spread = stats_.best_ask - stats_.best_bid;
      stats_.mid_price = (stats_.best_bid + stats_.best_ask) / 2.0;
    } else {
      stats_.spread = 0.0;
      stats_.mid_price = 0.0;
    }

    last_update_ = std::chrono::system_clock::now();
  }
};
