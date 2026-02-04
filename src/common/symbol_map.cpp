#include "symbol_map.hpp"
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace xdp {

namespace {

// Helper to parse CSV fields (handles quoted fields with commas)
std::vector<std::string> parse_csv_line(const std::string &line) {
  std::vector<std::string> result;
  std::string field;
  bool in_quotes = false;

  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];

    if (c == '"') {
      in_quotes = !in_quotes;
    } else if (c == ',' && !in_quotes) {
      result.push_back(field);
      field.clear();
    } else {
      field += c;
    }
  }
  result.push_back(field); // Add last field
  return result;
}

// Trim whitespace from string
std::string trim(const std::string &s) {
  size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
    ++start;
  }
  size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(start, end - start);
}

} // namespace

size_t SymbolMap::load(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Warning: Could not open symbol file: " << filename
              << std::endl;
    return 0;
  }

  symbols_.clear();
  std::string line;
  size_t count = 0;
  bool first_line = true;

  while (std::getline(file, line)) {
    // Remove Windows carriage return if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Skip empty lines
    if (line.empty())
      continue;

    // Skip header row
    if (first_line) {
      first_line = false;
      // Check if this looks like a header (starts with "symbol")
      if (line.find("symbol") == 0 || line.find("Symbol") == 0) {
        continue;
      }
    }

    // Parse CSV fields
    std::vector<std::string> tokens = parse_csv_line(line);

    // We need at least 11 fields for the full format:
    // symbol,cqs_symbol,symbol_id,exchange_code,listed_market,ticker_designation,
    // lot_size,price_scale_code,system_id,asset_type,price_multiplier
    if (tokens.size() >= 11) {
      try {
        SymbolInfo info;
        info.symbol = trim(tokens[0]);
        info.cqs_symbol = trim(tokens[1]);
        info.symbol_id = std::stoul(trim(tokens[2]));
        info.exchange_code = trim(tokens[3]);
        info.listed_market = trim(tokens[4]);
        info.ticker_designation = trim(tokens[5]);
        info.lot_size = std::stoul(trim(tokens[6]));
        info.price_scale_code = static_cast<uint8_t>(std::stoul(trim(tokens[7])));
        info.system_id = std::stoul(trim(tokens[8]));
        info.asset_type = trim(tokens[9]);
        info.price_multiplier = std::stod(trim(tokens[10]));

        // Store in map using symbol_id as key
        symbols_[info.symbol_id] = std::move(info);
        count++;

      } catch (const std::exception &e) {
        // Silently skip invalid lines
        continue;
      }
    }
  }

  std::cout << "Loaded " << count << " symbol mappings from " << filename
            << std::endl;

  return count;
}

std::string SymbolMap::get_symbol(uint32_t index) const {
  auto it = symbols_.find(index);
  if (it != symbols_.end()) {
    return it->second.symbol;
  }
  return std::to_string(index);
}

std::optional<SymbolInfo> SymbolMap::get_symbol_info(uint32_t index) const {
  auto it = symbols_.find(index);
  if (it != symbols_.end()) {
    return it->second;
  }
  return std::nullopt;
}

double SymbolMap::get_price_multiplier(uint32_t index) const {
  auto it = symbols_.find(index);
  if (it != symbols_.end()) {
    return it->second.price_multiplier;
  }
  // Default multiplier (1e-6 = divide by 1,000,000)
  return 1e-6;
}

std::optional<std::string> SymbolMap::find_symbol(uint32_t index) const {
  auto it = symbols_.find(index);
  if (it != symbols_.end()) {
    return it->second.symbol;
  }
  return std::nullopt;
}

bool SymbolMap::contains(uint32_t index) const {
  return symbols_.find(index) != symbols_.end();
}

// Global symbol map instance
SymbolMap &get_global_symbol_map() {
  static SymbolMap instance;
  return instance;
}

} // namespace xdp
