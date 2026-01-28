// order_book.hpp
#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <queue>
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
private:
  std::map<double, uint32_t, std::greater<double>> bids; // Price descending
  std::map<double, uint32_t, std::less<double>> asks;    // Price ascending
  std::unordered_map<uint64_t, Order> active_orders;
  mutable std::mutex mtx; // mutable allows locking in const methods

  double last_traded_price = 0.0;
  uint32_t last_traded_volume = 0;
  std::chrono::system_clock::time_point last_update;

public:
  // Toxicity tracking: detailed metrics per price level
  struct ToxicityMetrics {
    uint32_t adds = 0;
    uint32_t cancels = 0;
    uint32_t total_volume_added = 0;
    uint32_t total_volume_cancelled = 0;
    uint32_t ping_count = 0;        // Orders with volume < 10
    uint32_t large_order_count = 0; // Orders with volume > 1000
    uint32_t odd_lot_count = 0;     // Orders with volume not divisible by 100
    uint32_t high_precision_price_count = 0; // Prices with > 2 decimal places
    uint32_t resistance_level_count =
        0; // Prices ending in .95, .99, .98, .01, .05

    double get_toxicity_score() const {
      if (adds + cancels == 0)
        return 0.0;

      double score = 0.0;
      uint32_t total_events = adds + cancels;

      // Factor 1: Cancel/Add ratio (0.0 to 1.0, weighted 40%)
      double cancel_ratio = (double)cancels / (double)total_events;
      score += cancel_ratio * 0.4;

      // Factor 2: Ping ratio (small orders < 10, weighted 20%)
      double ping_ratio = (double)ping_count / (double)total_events;
      score += ping_ratio * 0.2;

      // Factor 3: Odd lot ratio (non-round volumes, weighted 15%)
      double odd_lot_ratio = (double)odd_lot_count / (double)total_events;
      score += odd_lot_ratio * 0.15;

      // Factor 4: High precision price ratio (weighted 15%)
      double precision_ratio =
          (double)high_precision_price_count / (double)total_events;
      score += precision_ratio * 0.15;

      // Factor 5: Resistance level ratio (weighted 10%)
      double resistance_ratio =
          (double)resistance_level_count / (double)total_events;
      score += resistance_ratio * 0.1;

      return std::min(score, 1.0); // Cap at 1.0
    }

    std::string get_explanation() const {
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

private:
  std::map<double, ToxicityMetrics, std::greater<double>> bid_toxicity;
  std::map<double, ToxicityMetrics, std::less<double>> ask_toxicity;

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
  } stats;

public:
  OrderBook() = default;

  void clear() {
    std::lock_guard<std::mutex> lock(mtx);
    bids.clear();
    asks.clear();
    active_orders.clear();
    bid_toxicity.clear();
    ask_toxicity.clear();
    last_traded_price = 0.0;
    last_traded_volume = 0;
    update_stats();
  }

  void add_order(uint64_t order_id, double price, uint32_t volume, char side) {
    std::lock_guard<std::mutex> lock(mtx);

    if (side == 'B') {
      bids[price] += volume;
      ToxicityMetrics &metrics = bid_toxicity[price];
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
    } else if (side == 'S') {
      asks[price] += volume;
      ToxicityMetrics &metrics = ask_toxicity[price];
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

    active_orders[order_id] = {order_id, price, volume, side,
                               std::chrono::system_clock::now()};
    update_stats();
  }

  void modify_order(uint64_t order_id, double new_price, uint32_t new_volume) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = active_orders.find(order_id);
    if (it != active_orders.end()) {
      Order &order = it->second;

      // Remove from old price level
      if (order.side == 'B') {
        bids[order.price] -= order.volume;
        if (bids[order.price] == 0)
          bids.erase(order.price);
      } else {
        asks[order.price] -= order.volume;
        if (asks[order.price] == 0)
          asks.erase(order.price);
      }

      // Update order and add to new level
      order.price = new_price;
      order.volume = new_volume;
      order.timestamp = std::chrono::system_clock::now();

      if (order.side == 'B') {
        bids[new_price] += new_volume;
      } else {
        asks[new_price] += new_volume;
      }

      update_stats();
    }
  }

  void delete_order(uint64_t order_id) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = active_orders.find(order_id);
    if (it != active_orders.end()) {
      const Order &order = it->second;

      if (order.side == 'B') {
        bids[order.price] -= order.volume;
        ToxicityMetrics &metrics = bid_toxicity[order.price];
        metrics.cancels++;
        metrics.total_volume_cancelled += order.volume;
        if (bids[order.price] == 0)
          bids.erase(order.price);
      } else {
        asks[order.price] -= order.volume;
        ToxicityMetrics &metrics = ask_toxicity[order.price];
        metrics.cancels++;
        metrics.total_volume_cancelled += order.volume;
        if (asks[order.price] == 0)
          asks.erase(order.price);
      }

      active_orders.erase(it);
      update_stats();
    }
  }

  void execute_order(uint64_t order_id, uint32_t executed_qty,
                     double trade_price) {
    std::lock_guard<std::mutex> lock(mtx);

    auto it = active_orders.find(order_id);
    if (it != active_orders.end()) {
      Order &order = it->second;

      if (order.volume > executed_qty) {
        // Partial fill
        order.volume -= executed_qty;
        if (order.side == 'B') {
          bids[order.price] -= executed_qty;
        } else {
          asks[order.price] -= executed_qty;
        }
      } else {
        // Full fill
        if (order.side == 'B') {
          bids[order.price] -= order.volume;
          if (bids[order.price] == 0)
            bids.erase(order.price);
        } else {
          asks[order.price] -= order.volume;
          if (asks[order.price] == 0)
            asks.erase(order.price);
        }
        active_orders.erase(it);
      }

      last_traded_price = trade_price;
      last_traded_volume = executed_qty;
      update_stats();
    }
  }

  // Thread-safe getters that return copies (snapshots) to avoid race conditions
  BookStats get_stats() const {
    std::lock_guard<std::mutex> lock(mtx);
    return stats;
  }

  std::map<double, uint32_t, std::greater<double>> get_bids() const {
    std::lock_guard<std::mutex> lock(mtx);
    return bids; // Return a copy
  }

  std::map<double, uint32_t, std::less<double>> get_asks() const {
    std::lock_guard<std::mutex> lock(mtx);
    return asks; // Return a copy
  }

  double get_last_trade() const {
    std::lock_guard<std::mutex> lock(mtx);
    return last_traded_price;
  }

  // Get toxicity score for a price level (0.0 to 1.0)
  double get_toxicity(double price, char side) const {
    std::lock_guard<std::mutex> lock(mtx);

    if (side == 'B') {
      auto it = bid_toxicity.find(price);
      if (it != bid_toxicity.end()) {
        return it->second.get_toxicity_score();
      }
    } else {
      auto it = ask_toxicity.find(price);
      if (it != ask_toxicity.end()) {
        return it->second.get_toxicity_score();
      }
    }
    return 0.0;
  }

  // Get detailed toxicity metrics for a price level
  ToxicityMetrics get_toxicity_metrics(double price, char side) const {
    std::lock_guard<std::mutex> lock(mtx);

    if (side == 'B') {
      auto it = bid_toxicity.find(price);
      if (it != bid_toxicity.end()) {
        return it->second;
      }
    } else {
      auto it = ask_toxicity.find(price);
      if (it != ask_toxicity.end()) {
        return it->second;
      }
    }
    return ToxicityMetrics(); // Return empty metrics
  }

private:
  void update_stats() {
    stats.bid_levels = bids.size();
    stats.ask_levels = asks.size();
    stats.total_bid_qty = 0;
    stats.total_ask_qty = 0;

    if (!bids.empty()) {
      stats.best_bid = bids.begin()->first;
      for (const auto &pair : bids) {
        stats.total_bid_qty += pair.second;
      }
    }

    if (!asks.empty()) {
      stats.best_ask = asks.begin()->first;
      for (const auto &pair : asks) {
        stats.total_ask_qty += pair.second;
      }
    }

    if (stats.best_bid > 0 && stats.best_ask > 0) {
      stats.spread = stats.best_ask - stats.best_bid;
      stats.mid_price = (stats.best_bid + stats.best_ask) / 2.0;
    }

    last_update = std::chrono::system_clock::now();
  }
};