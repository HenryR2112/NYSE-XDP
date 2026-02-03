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

    [[nodiscard]] double get_toxicity_score() const noexcept {
      uint32_t total_events = adds + cancels;
      if (total_events == 0)
        return 0.0;

      double score = 0.0;

      // Factor 1: Cancel/Add ratio (0.0 to 1.0, weighted 40%)
      double cancel_ratio = static_cast<double>(cancels) / static_cast<double>(total_events);
      score += cancel_ratio * 0.4;

      // Factor 2: Ping ratio (small orders < 10, weighted 20%)
      double ping_ratio = static_cast<double>(ping_count) / static_cast<double>(total_events);
      score += ping_ratio * 0.2;

      // Factor 3: Odd lot ratio (non-round volumes, weighted 15%)
      double odd_lot_ratio = static_cast<double>(odd_lot_count) / static_cast<double>(total_events);
      score += odd_lot_ratio * 0.15;

      // Factor 4: High precision price ratio (weighted 15%)
      double precision_ratio =
          static_cast<double>(high_precision_price_count) / static_cast<double>(total_events);
      score += precision_ratio * 0.15;

      // Factor 5: Resistance level ratio (weighted 10%)
      double resistance_ratio =
          static_cast<double>(resistance_level_count) / static_cast<double>(total_events);
      score += resistance_ratio * 0.1;

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
    double one_ms_toxicity = 0.0;
    double ten_ms_toxicity = 0.0;
    double hundred_ms_toxicity = 0.0;
    double one_second_toxicity = 0.0;
    double ten_seconds_toxicity = 0.0;
    double hundred_seconds_toxicity = 0.0;
    double one_minute_toxicity = 0.0;
    double ten_minutes_toxicity = 0.0;
  };

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
    update_stats();
  }

  void add_order(uint64_t order_id, double price, uint32_t volume, char side) {
    std::lock_guard<std::mutex> lock(mtx_);

    if (side == 'B') {
      bids_[price] += volume;
      update_toxicity_on_add(bid_toxicity_[price], price, volume);
    } else {
      asks_[price] += volume;
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

    // Remove from old price level
    if (order.side == 'B') {
      remove_volume_from_bids(order.price, order.volume);
      bids_[new_price] += new_volume;
    } else {
      remove_volume_from_asks(order.price, order.volume);
      asks_[new_price] += new_volume;
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
      } else {
        asks_[order.price] -= executed_qty;
      }
    } else {
      // Full fill
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

  // Helper to remove volume from bids
  void remove_volume_from_bids(double price, uint32_t volume) {
    auto it = bids_.find(price);
    if (it != bids_.end()) {
      if (it->second <= volume) {
        bids_.erase(it);
      } else {
        it->second -= volume;
      }
    }
  }

  // Helper to remove volume from asks
  void remove_volume_from_asks(double price, uint32_t volume) {
    auto it = asks_.find(price);
    if (it != asks_.end()) {
      if (it->second <= volume) {
        asks_.erase(it);
      } else {
        it->second -= volume;
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
    stats_.bid_levels = static_cast<int>(bids_.size());
    stats_.ask_levels = static_cast<int>(asks_.size());
    stats_.total_bid_qty = 0;
    stats_.total_ask_qty = 0;

    if (!bids_.empty()) {
      stats_.best_bid = bids_.begin()->first;
      for (const auto &pair : bids_) {
        stats_.total_bid_qty += pair.second;
      }
    } else {
      stats_.best_bid = 0.0;
    }

    if (!asks_.empty()) {
      stats_.best_ask = asks_.begin()->first;
      for (const auto &pair : asks_) {
        stats_.total_ask_qty += pair.second;
      }
    } else {
      stats_.best_ask = 0.0;
    }

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
