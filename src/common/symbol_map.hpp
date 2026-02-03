#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace xdp {

class SymbolMap {
public:
  SymbolMap() = default;

  // Load symbol mappings from a pipe-delimited file
  // Format: SYMBOL|EXCHANGE|INDEX|...
  // Returns number of symbols loaded, or 0 on failure
  [[nodiscard]] size_t load(const std::string &filename);

  // Get symbol for an index
  // Returns the symbol string, or the index as string if not found
  [[nodiscard]] std::string get_symbol(uint32_t index) const;

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
  [[nodiscard]] const std::unordered_map<uint32_t, std::string> &
  get_map() const noexcept {
    return symbols_;
  }

private:
  std::unordered_map<uint32_t, std::string> symbols_;
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
