#pragma once

#include <array>

namespace mmsim {

// Circular buffer trackers for online learning temporal features.
// All use std::array (no heap allocation) for cache-friendly access.

struct TradeFlowTracker {
  static constexpr int WINDOW = 100;
  struct Trade { bool is_buy; uint32_t volume; };
  std::array<Trade, WINDOW> buffer = {};
  int head = 0;
  int count = 0;

  void record_trade(bool is_buy, uint32_t volume) {
    buffer[head] = {is_buy, volume};
    head = (head + 1) % WINDOW;
    if (count < WINDOW) count++;
  }

  double get_imbalance() const {
    if (count == 0) return 0.0;
    double buy_vol = 0.0, sell_vol = 0.0;
    for (int i = 0; i < count; i++) {
      const auto& t = buffer[i];
      if (t.is_buy) buy_vol += t.volume;
      else sell_vol += t.volume;
    }
    double total = buy_vol + sell_vol;
    return (total > 0) ? (buy_vol - sell_vol) / total : 0.0;
  }
};

struct SpreadTracker {
  static constexpr int WINDOW = 50;
  std::array<double, WINDOW> buffer = {};
  int head = 0;
  int count = 0;

  void record_spread(double spread) {
    buffer[head] = spread;
    head = (head + 1) % WINDOW;
    if (count < WINDOW) count++;
  }

  double get_spread_change_rate() const {
    if (count < 2) return 0.0;
    double current = buffer[(head - 1 + WINDOW) % WINDOW];
    int oldest_idx = (count < WINDOW) ? 0 : head;
    double oldest = buffer[oldest_idx];
    return (oldest > 1e-10) ? (current - oldest) / oldest : 0.0;
  }
};

struct MomentumTracker {
  static constexpr int WINDOW = 50;
  std::array<double, WINDOW> buffer = {};
  int head = 0;
  int count = 0;

  void record_mid(double mid) {
    buffer[head] = mid;
    head = (head + 1) % WINDOW;
    if (count < WINDOW) count++;
  }

  double get_momentum() const {
    if (count < 2) return 0.0;
    double current = buffer[(head - 1 + WINDOW) % WINDOW];
    int oldest_idx = (count < WINDOW) ? 0 : head;
    double oldest = buffer[oldest_idx];
    return (oldest > 1e-10) ? (current - oldest) / oldest : 0.0;
  }
};

} // namespace mmsim
