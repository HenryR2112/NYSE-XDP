#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace xdp {

// Enhanced symbol information from parsed CSV
struct SymbolInfo {
  std::string symbol;           // Trading symbol (e.g., "AAPL")
  std::string cqs_symbol;       // CQS symbol (e.g., "AAPL")
  uint32_t symbol_id;           // Unique symbol index
  std::string exchange_code;    // Exchange code (e.g., "NYSE")
  std::string listed_market;    // Listed market (e.g., "NASDAQ", "NYSE Arca")
  std::string ticker_designation; // Tape designation (e.g., "Tape A", "Tape B", "Tape C")
  uint32_t lot_size;            // Round lot size (typically 100)
  uint8_t price_scale_code;     // Price scale code for multiplier
  uint32_t system_id;           // System ID
  std::string asset_type;       // Asset type (e.g., "Common Stock", "ETF")
  double price_multiplier;      // Multiplier to convert raw price to actual price
};

class SymbolMap {
public:
  SymbolMap() = default;

  // Load symbol mappings from a CSV file
  // Format: symbol,cqs_symbol,symbol_id,exchange_code,listed_market,ticker_designation,lot_size,price_scale_code,system_id,asset_type,price_multiplier
  // Returns number of symbols loaded, or 0 on failure
  [[nodiscard]] size_t load(const std::string &filename);

  // Get symbol for an index
  // Returns the symbol string, or the index as string if not found
  [[nodiscard]] std::string get_symbol(uint32_t index) const;

  // Get full symbol info for an index
  // Returns nullopt if not found
  [[nodiscard]] std::optional<SymbolInfo> get_symbol_info(uint32_t index) const;

  // Get price multiplier for a symbol index
  // Returns 1e-6 (default) if not found
  [[nodiscard]] double get_price_multiplier(uint32_t index) const;

  // Get symbol as optional (returns nullopt if not found)
  [[nodiscard]] std::optional<std::string> find_symbol(uint32_t index) const;

  // Check if a symbol index exists
  [[nodiscard]] bool contains(uint32_t index) const;

  // Get the number of loaded symbols
  [[nodiscard]] size_t size() const noexcept { return symbols_.size(); }

  // Check if the map is empty
  [[nodiscard]] bool empty() const noexcept { return symbols_.empty(); }

  // Clear all mappings
  void clear() noexcept { symbols_.clear(); }

  // Get the underlying map (for iteration)
  [[nodiscard]] const std::unordered_map<uint32_t, SymbolInfo> &
  get_map() const noexcept {
    return symbols_;
  }

private:
  std::unordered_map<uint32_t, SymbolInfo> symbols_;
};

// Global symbol map instance for backward compatibility
// Individual modules can use their own instances if needed
SymbolMap &get_global_symbol_map();

// Convenience function to get symbol using global map
[[nodiscard]] inline std::string get_symbol(uint32_t index) {
  return get_global_symbol_map().get_symbol(index);
}

// Convenience function to load symbols into global map
[[nodiscard]] inline size_t load_symbol_map(const std::string &filename) {
  return get_global_symbol_map().load(filename);
}

} // namespace xdp
