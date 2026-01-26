// order_book.hpp
#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <queue>
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
  } stats;

public:
  OrderBook() = default;

  void clear() {
    std::lock_guard<std::mutex> lock(mtx);
    bids.clear();
    asks.clear();
    active_orders.clear();
    last_traded_price = 0.0;
    last_traded_volume = 0;
    update_stats();
  }

  void add_order(uint64_t order_id, double price, uint32_t volume, char side) {
    std::lock_guard<std::mutex> lock(mtx);

    if (side == 'B') {
      bids[price] += volume;
    } else if (side == 'S') {
      asks[price] += volume;
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
        if (bids[order.price] == 0)
          bids.erase(order.price);
      } else {
        asks[order.price] -= order.volume;
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