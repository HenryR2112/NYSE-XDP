#include "symbol_map.hpp"
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace xdp {

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
  int line_num = 0;

  while (std::getline(file, line)) {
    line_num++;

    // Skip empty lines
    if (line.empty())
      continue;

    // Remove Windows carriage return if present
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Trim trailing whitespace
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
      line.pop_back();
    }

    // Skip if line is now empty
    if (line.empty())
      continue;

    // Split by pipe delimiter
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(line);

    while (std::getline(iss, token, '|')) {
      tokens.push_back(token);
    }

    // We need at least 3 fields: SYMBOL|EXCHANGE|INDEX
    if (tokens.size() >= 3) {
      try {
        // The index is in the 3rd field (0-based, so tokens[2])
        uint32_t index = std::stoul(tokens[2]);

        // The symbol is in the 1st field (tokens[0])
        std::string symbol = tokens[0];

        // Store in map
        symbols_[index] = symbol;
        count++;

      } catch (const std::exception &) {
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
    return it->second;
  }
  return std::to_string(index);
}

std::optional<std::string> SymbolMap::find_symbol(uint32_t index) const {
  auto it = symbols_.find(index);
  if (it != symbols_.end()) {
    return it->second;
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
