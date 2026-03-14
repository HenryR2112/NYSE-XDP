// market_maker_sim.cpp - Market Maker Simulation with PCAP playback
// Simulates market making strategies on historical XDP data
// PARALLELIZED VERSION - Uses all available CPU cores for maximum throughput

#include "per_symbol_sim.hpp"

#include "common/mmap_pcap_reader.hpp"
#include "common/pcap_reader.hpp"
#include "common/symbol_map.hpp"
#include "common/thread_pool.hpp"
#include "common/xdp_types.hpp"
#include "common/xdp_utils.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace mmsim;
using mmsim::AblationMode;

namespace {

// =============================================================================
// Global State
// =============================================================================

std::string g_filter_ticker;
bool g_use_parallel = true;  // Enable parallel processing by default
bool g_use_hybrid = true;    // Enable hybrid multi-process mode by default
size_t g_num_threads = 0;    // 0 = auto-detect (use all cores)
size_t g_files_per_group = 0; // 0 = auto (num_files / num_threads)

SimConfig g_config;  // Runtime simulation configuration

// =============================================================================
// Thread-safe symbol simulation storage
// Pre-allocated array for lock-free access (no hash map lookups during processing)
// =============================================================================

constexpr uint32_t MAX_SYMBOLS = 100000;
std::unique_ptr<PerSymbolSim*[]> g_sims_array;       // Raw pointer array
std::unique_ptr<std::atomic<bool>[]> g_sims_initialized;  // Atomic flags

// Sharded locks to reduce contention (64 shards = good balance)
constexpr size_t NUM_LOCK_SHARDS = 64;
std::array<std::mutex, NUM_LOCK_SHARDS> g_shard_mutexes;

std::atomic<uint64_t> g_total_executions{0};
std::atomic<uint64_t> g_total_packets{0};
std::atomic<uint64_t> g_total_messages{0};
std::atomic<size_t> g_files_completed{0};
std::atomic<size_t> g_active_symbols{0};

// Initialize pre-allocated storage (call once at startup)
void init_symbol_storage() {
  g_sims_array = std::make_unique<PerSymbolSim*[]>(MAX_SYMBOLS);
  g_sims_initialized = std::make_unique<std::atomic<bool>[]>(MAX_SYMBOLS);
  for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
    g_sims_array[i] = nullptr;
    g_sims_initialized[i].store(false, std::memory_order_relaxed);
  }
}

// Clean up allocated PerSymbolSim objects
void cleanup_symbol_storage() {
  if (!g_sims_array) return;
  for (size_t i = 0; i < MAX_SYMBOLS; ++i) {
    if (g_sims_initialized[i].load(std::memory_order_relaxed)) {
      delete g_sims_array[i];
      g_sims_array[i] = nullptr;
    }
  }
}

// Get shard mutex for a symbol (distributes lock contention)
inline std::mutex& get_shard_mutex(uint32_t symbol_index) {
  return g_shard_mutexes[symbol_index % NUM_LOCK_SHARDS];
}

// Get or create PerSymbolSim - lock-free fast path
inline PerSymbolSim* get_or_create_sim_fast(uint32_t symbol_index) {
  if (symbol_index >= MAX_SYMBOLS) return nullptr;

  // Fast path: already initialized (lock-free check)
  if (g_sims_initialized[symbol_index].load(std::memory_order_acquire)) {
    return g_sims_array[symbol_index];
  }

  // Slow path: need to initialize (use sharded lock)
  std::lock_guard<std::mutex> lock(get_shard_mutex(symbol_index));

  // Double-check after acquiring lock
  if (g_sims_initialized[symbol_index].load(std::memory_order_acquire)) {
    return g_sims_array[symbol_index];
  }

  g_sims_array[symbol_index] = new PerSymbolSim();
  g_sims_initialized[symbol_index].store(true, std::memory_order_release);
  g_active_symbols.fetch_add(1, std::memory_order_relaxed);

  return g_sims_array[symbol_index];
}

// Periodically report memory stats (lock-free read of atomics)
void report_memory_stats() {
  std::cout << " [syms: " << g_active_symbols.load() << "]" << std::flush;
}

// =============================================================================
// XDP Message Dispatch
// =============================================================================

void process_xdp_message(const uint8_t *data, size_t max_len, uint16_t msg_type,
                         uint64_t now_ns) {
  if (max_len < xdp::MESSAGE_HEADER_SIZE)
    return;

  uint32_t symbol_index = xdp::read_symbol_index(msg_type, data, max_len);
  if (symbol_index == 0)
    return;

  // Bounds check: NYSE has ~8000 symbols, anything > 100k is invalid
  constexpr uint32_t MAX_VALID_SYMBOL_INDEX = 100000;
  if (symbol_index > MAX_VALID_SYMBOL_INDEX)
    return;

  std::string ticker = xdp::get_symbol(symbol_index);
  if (!g_filter_ticker.empty() && ticker != g_filter_ticker)
    return;

  // Only create simulation state for known symbols
  if (ticker.empty())
    return;

  g_total_messages.fetch_add(1, std::memory_order_relaxed);

  // Lock-free fast path for symbol lookup, sharded lock for updates
  PerSymbolSim* sim_ptr = get_or_create_sim_fast(symbol_index);
  if (!sim_ptr) return;

  PerSymbolSim& sim = *sim_ptr;

  // Use sharded lock for this symbol's updates
  std::lock_guard<std::mutex> sym_lock(get_shard_mutex(symbol_index));

  sim.ensure_init(symbol_index, g_config);

  switch (msg_type) {
  case static_cast<uint16_t>(xdp::MessageType::ADD_ORDER): {
    if (max_len >= xdp::MessageSize::ADD_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t price_raw = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      uint8_t side = data[32];
      double price = xdp::parse_price(price_raw);
      char side_char = xdp::side_to_char(xdp::parse_side(side));
      sim.on_add(order_id, price, volume, side_char, now_ns);
    }
    break;
  }

  case static_cast<uint16_t>(xdp::MessageType::MODIFY_ORDER): {
    if (max_len >= xdp::MessageSize::MODIFY_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t price_raw = xdp::read_le32(data + 24);
      uint32_t volume = xdp::read_le32(data + 28);
      double price = xdp::parse_price(price_raw);
      sim.on_modify(order_id, price, volume);
    }
    break;
  }

  case static_cast<uint16_t>(xdp::MessageType::DELETE_ORDER): {
    if (max_len >= xdp::MessageSize::DELETE_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      sim.on_delete(order_id);
    }
    break;
  }

  case static_cast<uint16_t>(xdp::MessageType::EXECUTE_ORDER): {
    if (max_len >= xdp::MessageSize::EXECUTE_ORDER) {
      uint64_t order_id = xdp::read_le64(data + 16);
      uint32_t price_raw = xdp::read_le32(data + 28);
      uint32_t volume = xdp::read_le32(data + 32);
      double price = xdp::parse_price(price_raw);
      g_total_executions.fetch_add(1, std::memory_order_relaxed);
      sim.on_execute(order_id, volume, price, now_ns);
    }
    break;
  }

  case static_cast<uint16_t>(xdp::MessageType::REPLACE_ORDER): {
    if (max_len >= xdp::MessageSize::REPLACE_ORDER) {
      uint64_t old_order_id = xdp::read_le64(data + 16);
      uint64_t new_order_id = xdp::read_le64(data + 24);
      uint32_t price_raw = xdp::read_le32(data + 32);
      uint32_t volume = xdp::read_le32(data + 36);
      double price = xdp::parse_price(price_raw);
      uint8_t side = data[40];
      char side_char = xdp::side_to_char(xdp::parse_side(side));
      sim.on_replace(old_order_id, new_order_id, price, volume, side_char, now_ns);
    }
    break;
  }
  default:
    break;
  }
}

// =============================================================================
// Unified packet processing callback
// Used by all execution modes (hybrid, threaded, sequential)
// =============================================================================

void process_packet_callback(const uint8_t *data, size_t length,
                             uint64_t /*packet_num*/,
                             const xdp::NetworkPacketInfo &info) {
  g_total_packets.fetch_add(1, std::memory_order_relaxed);

  if (length < xdp::PACKET_HEADER_SIZE) return;

  xdp::PacketHeader pkt_header;
  if (!xdp::parse_packet_header(data, length, pkt_header)) return;

  size_t offset = xdp::PACKET_HEADER_SIZE;
  for (uint8_t i = 0; i < pkt_header.num_messages && offset < length; i++) {
    if (offset + xdp::MESSAGE_HEADER_SIZE > length) break;
    uint16_t msg_size = xdp::read_le16(data + offset);
    if (msg_size < xdp::MESSAGE_HEADER_SIZE || offset + msg_size > length) break;
    uint16_t msg_type = xdp::read_le16(data + offset + 2);
    process_xdp_message(data + offset, msg_size, msg_type, info.timestamp_ns);
    offset += msg_size;
  }
}

// =============================================================================
// Results Aggregation (non-hybrid mode)
// =============================================================================

void print_results() {
  struct Row {
    uint32_t symbol_index;
    std::string ticker;
    double baseline_total;
    double toxicity_total;
    double improvement;
    int64_t baseline_fills;
    int64_t toxicity_fills;
    int64_t quotes_suppressed;
    double adverse_pnl;
    int64_t adverse_fills;
  };

  std::vector<Row> rows;
  rows.reserve(g_active_symbols.load());

  double portfolio_baseline = 0.0;
  double portfolio_toxicity = 0.0;
  double portfolio_adverse = 0.0;
  int64_t total_baseline_fills = 0;
  int64_t total_toxicity_fills = 0;
  int64_t total_quotes_suppressed = 0;
  int64_t total_adverse_fills = 0;
  int64_t symbols_halted = 0;
  int64_t symbols_ineligible = 0;

  // Iterate over pre-allocated array (no lock needed - single-threaded at results time)
  for (uint32_t symbol_index = 0; symbol_index < MAX_SYMBOLS; ++symbol_index) {
    if (!g_sims_initialized[symbol_index].load(std::memory_order_relaxed)) continue;
    PerSymbolSim* sim_ptr = g_sims_array[symbol_index];
    if (!sim_ptr) continue;
    const PerSymbolSim &sim = *sim_ptr;

    if (!sim.eligible_to_trade) {
      symbols_ineligible++;
      continue;
    }
    if (sim.toxicity_risk.halted) {
      symbols_halted++;
    }

    const MarketMakerStats baseline_stats = sim.mm_baseline.get_stats();
    const MarketMakerStats toxicity_stats = sim.mm_toxicity.get_stats();

    // Include adverse selection penalty in PnL
    const double baseline_total =
        baseline_stats.realized_pnl + baseline_stats.unrealized_pnl +
        sim.baseline_risk.total_adverse_pnl;
    const double toxicity_total =
        toxicity_stats.realized_pnl + toxicity_stats.unrealized_pnl +
        sim.toxicity_risk.total_adverse_pnl;
    const double improvement = toxicity_total - baseline_total;

    portfolio_baseline += baseline_total;
    portfolio_toxicity += toxicity_total;
    portfolio_adverse += sim.toxicity_risk.total_adverse_pnl;
    total_baseline_fills += sim.baseline_risk.total_fills;
    total_toxicity_fills += sim.toxicity_risk.total_fills;
    total_quotes_suppressed += toxicity_stats.quotes_suppressed;
    total_adverse_fills += sim.toxicity_risk.adverse_fills;

    rows.push_back(Row{symbol_index, xdp::get_symbol(symbol_index),
                       baseline_total, toxicity_total, improvement,
                       sim.baseline_risk.total_fills, sim.toxicity_risk.total_fills,
                       toxicity_stats.quotes_suppressed,
                       sim.toxicity_risk.total_adverse_pnl,
                       sim.toxicity_risk.adverse_fills});
  }

  std::sort(rows.begin(), rows.end(),
            [](const Row &a, const Row &b) { return a.improvement > b.improvement; });

  const double portfolio_improvement = portfolio_toxicity - portfolio_baseline;
  const double portfolio_improvement_pct =
      portfolio_baseline != 0.0
          ? (portfolio_improvement / std::abs(portfolio_baseline)) * 100.0
          : 0.0;

  std::cout << "\n=== HFT MARKET MAKER SIMULATION RESULTS ===\n";
  std::cout << "Filter type: " << (g_config.filter_type == FilterType::EWMA ? "ewma" : "logistic") << '\n';
  std::cout << "Latency: " << g_config.exec.latency_us_mean << "μs (colo)\n";
  std::cout << "Symbols traded: " << rows.size() << '\n';
  std::cout << "Symbols ineligible: " << symbols_ineligible << '\n';
  std::cout << "Symbols halted (loss limit): " << symbols_halted << '\n';
  std::cout << "Total executions processed: " << g_total_executions << '\n';

  std::cout << "\n--- PORTFOLIO TOTALS (incl. adverse selection) ---\n";
  std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
            << portfolio_baseline << '\n';
  std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
            << portfolio_toxicity << '\n';
  std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
            << portfolio_improvement << " (" << std::fixed
            << std::setprecision(2) << portfolio_improvement_pct << "%)\n";

  std::cout << "\n--- ADVERSE SELECTION ANALYSIS ---\n";
  std::cout << "Total adverse selection penalty: $" << std::fixed << std::setprecision(2)
            << portfolio_adverse << '\n';
  std::cout << "Fills with adverse movement: " << total_adverse_fills << " / "
            << total_toxicity_fills << '\n';
  if (total_adverse_fills > 0) {
    std::cout << "Avg adverse penalty per fill: $" << std::fixed << std::setprecision(4)
              << (portfolio_adverse / total_adverse_fills) << '\n';
  }

  std::cout << "\n--- EXECUTION STATS ---\n";
  std::cout << "Baseline fills: " << total_baseline_fills << '\n';
  std::cout << "Toxicity fills: " << total_toxicity_fills << '\n';
  std::cout << "Quotes suppressed (toxicity): " << total_quotes_suppressed << '\n';
  if (total_baseline_fills > 0) {
    std::cout << "Avg PnL per fill (baseline): $" << std::fixed << std::setprecision(4)
              << (portfolio_baseline / total_baseline_fills) << '\n';
  }
  if (total_toxicity_fills > 0) {
    std::cout << "Avg PnL per fill (toxicity): $" << std::fixed << std::setprecision(4)
              << (portfolio_toxicity / total_toxicity_fills) << '\n';
  }

  std::cout << "\n--- TOP 5 SYMBOLS BY IMPROVEMENT ---\n";
  const size_t top_n = std::min<size_t>(5, rows.size());
  for (size_t i = 0; i < top_n; i++) {
    const Row &r = rows[i];
    std::cout << (i + 1) << ". " << r.ticker << " (index " << r.symbol_index
              << "): $" << std::fixed << std::setprecision(2) << r.improvement
              << " | baseline $" << r.baseline_total << " | tox $"
              << r.toxicity_total << " | fills " << r.baseline_fills << " vs "
              << r.toxicity_fills << '\n';
  }

  // Show worst performers too
  std::cout << "\n--- BOTTOM 5 SYMBOLS (WORST) ---\n";
  const size_t bottom_start = rows.size() > 5 ? rows.size() - 5 : 0;
  for (size_t i = rows.size(); i > bottom_start; i--) {
    const Row &r = rows[i - 1];
    std::cout << (rows.size() - i + 1) << ". " << r.ticker << " (index " << r.symbol_index
              << "): $" << std::fixed << std::setprecision(2) << r.toxicity_total
              << " | fills " << r.toxicity_fills << '\n';
  }

  if (!g_filter_ticker.empty() && rows.size() == 1) {
    const Row &r = rows[0];
    std::cout << "\n--- SINGLE SYMBOL DETAIL (" << r.ticker << ") ---\n";
    std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
              << r.baseline_total << '\n';
    std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
              << r.toxicity_total << '\n';
    std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
              << r.improvement << '\n';
    std::cout << "Quotes suppressed: " << r.quotes_suppressed << '\n';
  }

  // Walk-forward analysis summary (non-hybrid mode)
  if (g_config.walk_forward) {
    std::cout << "\n=== WALK-FORWARD ANALYSIS ===\n";
    std::cout << "Window size: " << g_config.wf_window_minutes << " minutes\n";
    std::cout << "Mode: learn in window N, apply frozen weights in window N+1\n";

    // Aggregate per-window metrics across all symbols
    std::map<int, double> window_tox_pnl, window_base_pnl;
    std::map<int, int64_t> window_fills, window_suppressed;
    for (uint32_t symbol_index = 0; symbol_index < MAX_SYMBOLS; ++symbol_index) {
      if (!g_sims_initialized[symbol_index].load(std::memory_order_relaxed)) continue;
      PerSymbolSim* sim_ptr = g_sims_array[symbol_index];
      if (!sim_ptr || !sim_ptr->eligible_to_trade) continue;
      for (const auto& wm : sim_ptr->wf_window_metrics) {
        window_tox_pnl[wm.window_id] += wm.toxicity_pnl;
        window_base_pnl[wm.window_id] += wm.baseline_pnl;
        window_fills[wm.window_id] += wm.fills;
        window_suppressed[wm.window_id] += wm.suppressed;
      }
    }

    if (!window_tox_pnl.empty()) {
      std::cout << "\nPer-window summary (cumulative PnL at window end):\n";
      std::cout << std::setw(8) << "Window" << std::setw(16) << "Baseline PnL"
                << std::setw(16) << "Toxicity PnL" << std::setw(16) << "Improvement"
                << std::setw(10) << "Fills" << std::setw(12) << "Suppressed" << '\n';
      for (const auto& [wid, tpnl] : window_tox_pnl) {
        double bpnl = window_base_pnl[wid];
        std::cout << std::setw(8) << wid
                  << std::setw(16) << std::fixed << std::setprecision(2) << bpnl
                  << std::setw(16) << tpnl
                  << std::setw(16) << (tpnl - bpnl)
                  << std::setw(10) << window_fills[wid]
                  << std::setw(12) << window_suppressed[wid] << '\n';
      }
    }
  }
}

// =============================================================================
// CLI Usage
// =============================================================================

constexpr const char *DEFAULT_DATA_DIR = "data/uncompressed-ny4-xnyx-pillar-a-20230822";

void print_usage(const char *program) {
  std::cerr << "HFT Market Maker Simulation (HYBRID MULTI-PROCESS VERSION)\n\n"
            << "Usage: " << program << " [pcap_file(s)] [options]\n\n"
            << "Processes PCAP files using hybrid multi-process architecture:\n"
            << "- Files grouped by time window, each group processed by separate process\n"
            << "- Sequential processing within groups (maintains order book state)\n"
            << "- Zero lock contention between groups (separate memory spaces)\n\n"
            << "If no PCAP files are specified, all *.pcap files in the default data\n"
            << "directory are used: " << DEFAULT_DATA_DIR << "/\n\n"
            << "Options:\n"
            << "  -t TICKER           Filter to single ticker\n"
            << "  -s, --symbols FILE  Symbol map file (default: data/symbol_nyse_parsed.csv)\n"
            << "  --data-dir DIR      Directory to scan for *.pcap files (default: " << DEFAULT_DATA_DIR << ")\n"
            << "  --seed N            Random seed\n"
            << "  --latency-us M      One-way latency in microseconds (default: 5)\n"
            << "  --latency-jitter-us J  Latency jitter (default: 1)\n"
            << "  --queue-fraction F  Queue position as fraction of depth (default: 0.005)\n"
            << "  --adverse-lookforward-us U  Adverse selection lookforward (default: 250)\n"
            << "  --adverse-multiplier M  Adverse selection penalty multiplier (default: 0.15)\n"
            << "  --maker-rebate R    Maker rebate per share (default: 0.0025)\n"
            << "  --max-position P    Max position per symbol (default: 50000)\n"
            << "  --max-loss L        Max daily loss per symbol (default: 5000)\n"
            << "  --quote-interval-us Q  Quote update interval (default: 10)\n"
            << "  --fill-mode M       Fill mode: cross or match (default: cross)\n"
            << "  --toxicity-threshold T  Toxicity threshold for quote suppression (default: 0.95)\n"
            << "  --toxicity-multiplier K  Toxicity spread multiplier (default: 1.0)\n"
            << "  --epsilon-min E     Minimum expected PnL per share to quote (default: 0.0003)\n"
            << "  --output-dir DIR    Output directory for per-fill/per-symbol CSV files\n"
            << "\nFilter Type Options:\n"
            << "  --filter-type TYPE  Toxicity filter: logistic or ewma (default: logistic)\n"
            << "  --ewma-alpha A      EWMA decay factor (default: 0.05)\n"
            << "  --ewma-k K          EWMA threshold multiplier in std devs (default: 1.5)\n"
            << "  --ewma-min-obs N    EWMA minimum observations before activation (default: 20)\n"
            << "\nOnline Learning Options (logistic filter):\n"
            << "  --online-learning   Enable online SGD for toxicity weights\n"
            << "  --learning-rate R   SGD base learning rate (default: 0.01)\n"
            << "  --warmup-fills N    Fills before SGD activates (default: 5)\n"
            << "\nWalk-Forward Analysis:\n"
            << "  --walk-forward      Enable walk-forward out-of-sample evaluation\n"
            << "  --wf-window-minutes N  Window size in minutes (default: 30)\n"
            << "\nParallel Processing Options:\n"
            << "  --threads N         Number of processes (default: auto-detect all cores)\n"
            << "  --files-per-group N Files per process group (default: auto)\n"
            << "  --no-hybrid         Disable hybrid mode (use threaded mode instead)\n"
            << "  --sequential        Disable all parallelism (single-threaded)\n\n"
            << "Examples:\n"
            << "  " << program << "                           # full day using default data dir\n"
            << "  " << program << " --data-dir path/to/pcaps  # full day from custom dir\n"
            << "  " << program << " file1.pcap file2.pcap     # specific files\n";
}

// =============================================================================
// HYBRID MULTI-PROCESS ARCHITECTURE
// Groups files by time window, spawns process per group, aggregates results
// =============================================================================

// Shared memory structure for inter-process result aggregation
struct ProcessResults {
  double baseline_pnl;
  double toxicity_pnl;
  double adverse_pnl;
  double baseline_adverse_pnl;  // For hypothesis testing H2
  double baseline_inv_variance; // For hypothesis testing H3
  double toxicity_inv_variance; // For hypothesis testing H3
  int64_t baseline_fills;
  int64_t toxicity_fills;
  int64_t quotes_suppressed;
  int64_t adverse_fills;
  int64_t baseline_adverse_fills;  // For hypothesis testing H2
  uint64_t packets_processed;
  uint64_t messages_processed;
  uint64_t symbols_active;
  // Unwind stats
  int64_t baseline_unwind_crosses;
  int64_t toxicity_unwind_crosses;
  double baseline_unwind_cost;
  double toxicity_unwind_cost;
  // PnL decomposition
  double toxicity_realized_pnl;
  double toxicity_unrealized_pnl;
  double baseline_realized_pnl;
  double baseline_unrealized_pnl;
  // Symbol-level diagnostics
  int64_t symbols_eod_liquidated;
  int64_t symbols_blacklisted;
  int64_t symbols_one_sided;         // Only buys or only sells
  int64_t symbols_with_fills;
  int64_t toxicity_buy_fills;
  int64_t toxicity_sell_fills;
  int64_t baseline_buy_fills;
  int64_t baseline_sell_fills;
  double avg_final_abs_inventory;
  // Fill pipeline diagnostics (toxicity strategy)
  uint64_t diag_exec_total;
  uint64_t diag_exec_no_order_info;
  uint64_t diag_exec_not_eligible;
  uint64_t diag_try_fill_calls;
  uint64_t diag_rejected_halted;
  uint64_t diag_rejected_not_live;
  uint64_t diag_rejected_latency;
  uint64_t diag_rejected_price;
  uint64_t diag_rejected_queue;
  uint64_t diag_fill_succeeded;
  uint64_t diag_quote_resets;
  bool completed;
  char padding[7];  // Align to 8 bytes
};

// Get file size in bytes
size_t get_file_size(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) == 0) {
    return static_cast<size_t>(st.st_size);
  }
  return 0;
}

// Group files by total size for balanced load distribution
// Uses greedy algorithm: assign each file to the group with smallest total size
std::vector<std::vector<std::string>> group_files(
    const std::vector<std::string>& files, size_t num_groups) {
  std::vector<std::vector<std::string>> groups(num_groups);

  if (files.empty() || num_groups == 0) return groups;

  // Sort files by name (chronological order for NYSE PCAP files)
  std::vector<std::string> sorted_files(files.begin(), files.end());
  std::sort(sorted_files.begin(), sorted_files.end());

  // Sequential chunking: assign contiguous time slices to each group
  // This ensures each group sees a complete slice of the trading day,
  // maintaining order book state (ADD → EXECUTE pairs stay together)
  size_t files_per_group = sorted_files.size() / num_groups;
  size_t remainder = sorted_files.size() % num_groups;
  size_t offset = 0;

  for (size_t g = 0; g < num_groups; ++g) {
    size_t count = files_per_group + (g < remainder ? 1 : 0);
    for (size_t i = 0; i < count; ++i) {
      groups[g].push_back(sorted_files[offset + i]);
    }
    offset += count;
  }

  // Remove empty groups
  groups.erase(
      std::remove_if(groups.begin(), groups.end(),
                     [](const std::vector<std::string>& g) { return g.empty(); }),
      groups.end());

  // Print distribution for debugging
  std::cout << "Sequential time-slice distribution:\n";
  for (size_t i = 0; i < groups.size(); ++i) {
    std::cout << "  Group " << (i+1) << ": " << groups[i].size() << " files\n";
  }

  return groups;
}

// Process a group of files sequentially (called in child process)
void process_file_group(const std::vector<std::string>& files,
                        ProcessResults* results,
                        const std::string& symbol_file,
                        size_t group_idx) {
  // Debug: confirm child started
  std::cerr << "[Group " << (group_idx+1) << "] Starting with " << files.size() << " files\n" << std::flush;

  // Re-initialize symbol storage in child process
  init_symbol_storage();
  if (!xdp::load_symbol_map(symbol_file)) {
    std::cerr << "[Group " << (group_idx+1) << "] WARNING: Failed to load symbol map\n";
  }

  // Reset counters for this process
  g_total_packets.store(0);
  g_total_messages.store(0);
  g_active_symbols.store(0);

  // Process files sequentially within group (maintains state)
  size_t file_num = 0;
  for (const auto& pcap_file : files) {
    file_num++;
    xdp::MmapPcapReader reader;
    if (!reader.open(pcap_file)) {
      std::cerr << "[Group " << (group_idx+1) << "] Failed to open: " << pcap_file << "\n";
      continue;
    }
    reader.preload();

    uint64_t pkts_before = g_total_packets.load();
    reader.process_all(process_packet_callback);
    uint64_t pkts_in_file = g_total_packets.load() - pkts_before;

    // Progress every 10 files or at the end
    if (file_num % 10 == 0 || file_num == files.size()) {
      std::cerr << "[Group " << (group_idx+1) << "] File " << file_num << "/" << files.size()
                << " (" << pkts_in_file << " pkts, total " << g_total_packets.load() << ")\n" << std::flush;
    }
  }

  // Aggregate results from this process
  double baseline_pnl = 0.0, toxicity_pnl = 0.0, adverse_pnl = 0.0, baseline_adverse_pnl = 0.0;
  double tox_realized = 0.0, tox_unrealized = 0.0;
  double base_realized = 0.0, base_unrealized = 0.0;
  double baseline_inv_variance_sum = 0.0, toxicity_inv_variance_sum = 0.0;
  int64_t baseline_fills = 0, toxicity_fills = 0, quotes_suppressed = 0, adverse_fills = 0, baseline_adverse_fills = 0;
  int64_t tox_buy_fills = 0, tox_sell_fills = 0, base_buy_fills = 0, base_sell_fills = 0;
  int64_t symbols_with_inv_data = 0;
  int64_t baseline_unwind_crosses = 0, toxicity_unwind_crosses = 0;
  double baseline_unwind_cost = 0.0, toxicity_unwind_cost = 0.0;
  int64_t n_eod_liquidated = 0, n_blacklisted = 0, n_one_sided = 0, n_with_fills = 0;
  double total_abs_final_inv = 0.0;

  // For worst-symbol dump
  struct SymDiag {
    std::string ticker;
    double tox_total, tox_realized, tox_unrealized, tox_adverse;
    double base_total;
    int64_t tox_buys, tox_sells, tox_unwinds;
    double tox_unwind_cost;
    double final_inv;
    bool eod, blacklisted;
  };
  std::vector<SymDiag> sym_diags;

  // Fill pipeline diagnostics (aggregate toxicity strategy across symbols)
  PerSymbolSim::FillDiagnostics diag_agg = {};

  for (uint32_t idx = 0; idx < MAX_SYMBOLS; ++idx) {
    if (!g_sims_initialized[idx].load(std::memory_order_relaxed)) continue;
    PerSymbolSim* sim = g_sims_array[idx];
    if (!sim || !sim->eligible_to_trade) continue;

    const auto& bs = sim->mm_baseline.get_stats();
    const auto& ts = sim->mm_toxicity.get_stats();

    baseline_pnl += bs.realized_pnl + bs.unrealized_pnl + sim->baseline_risk.total_adverse_pnl;
    toxicity_pnl += ts.realized_pnl + ts.unrealized_pnl + sim->toxicity_risk.total_adverse_pnl;
    tox_realized += ts.realized_pnl;
    tox_unrealized += ts.unrealized_pnl;
    base_realized += bs.realized_pnl;
    base_unrealized += bs.unrealized_pnl;
    adverse_pnl += sim->toxicity_risk.total_adverse_pnl;
    baseline_adverse_pnl += sim->baseline_risk.total_adverse_pnl;
    baseline_fills += sim->baseline_risk.total_fills;
    toxicity_fills += sim->toxicity_risk.total_fills;
    tox_buy_fills += ts.buy_fills;
    tox_sell_fills += ts.sell_fills;
    base_buy_fills += bs.buy_fills;
    base_sell_fills += bs.sell_fills;
    quotes_suppressed += ts.quotes_suppressed;
    adverse_fills += sim->toxicity_risk.adverse_fills;
    baseline_adverse_fills += sim->baseline_risk.adverse_fills;
    baseline_unwind_crosses += bs.unwind_crosses;
    toxicity_unwind_crosses += ts.unwind_crosses;
    baseline_unwind_cost += bs.unwind_cost;
    toxicity_unwind_cost += ts.unwind_cost;

    // Symbol-level diagnostics
    double tox_inv = sim->mm_toxicity.get_inventory();
    total_abs_final_inv += std::abs(tox_inv);
    if (ts.buy_fills > 0 || ts.sell_fills > 0) n_with_fills++;
    if ((ts.buy_fills > 0) != (ts.sell_fills > 0)) n_one_sided++;  // XOR: only one side
    if (sim->eod_liquidated) n_eod_liquidated++;
    if (sim->blacklisted) n_blacklisted++;

    // Collect per-symbol diagnostics for worst-symbol dump
    double t_total = ts.realized_pnl + ts.unrealized_pnl + sim->toxicity_risk.total_adverse_pnl;
    double b_total = bs.realized_pnl + bs.unrealized_pnl + sim->baseline_risk.total_adverse_pnl;
    sym_diags.push_back({sim->cached_ticker, t_total, ts.realized_pnl, ts.unrealized_pnl,
                         sim->toxicity_risk.total_adverse_pnl, b_total,
                         ts.buy_fills, ts.sell_fills, ts.unwind_crosses,
                         ts.unwind_cost, tox_inv, sim->eod_liquidated, sim->blacklisted});

    // Aggregate inventory variance (weighted sum by sample count)
    if (sim->baseline_risk.inv_count > 1 && sim->toxicity_risk.inv_count > 1) {
      baseline_inv_variance_sum += sim->baseline_risk.get_inventory_variance();
      toxicity_inv_variance_sum += sim->toxicity_risk.get_inventory_variance();
      symbols_with_inv_data++;
    }

    // Aggregate fill pipeline diagnostics (toxicity strategy)
    const auto& d = sim->diag_toxicity;
    diag_agg.exec_total += d.exec_total;
    diag_agg.exec_no_order_info += d.exec_no_order_info;
    diag_agg.exec_not_eligible += d.exec_not_eligible;
    diag_agg.try_fill_calls += d.try_fill_calls;
    diag_agg.rejected_halted += d.rejected_halted;
    diag_agg.rejected_not_live += d.rejected_not_live;
    diag_agg.rejected_latency += d.rejected_latency;
    diag_agg.rejected_price += d.rejected_price;
    diag_agg.rejected_queue += d.rejected_queue;
    diag_agg.fill_succeeded += d.fill_succeeded;
    diag_agg.quote_resets += d.quote_resets;
  }

  // Compute average inventory variance across symbols
  double baseline_inv_var_avg = (symbols_with_inv_data > 0) ? baseline_inv_variance_sum / symbols_with_inv_data : 0.0;
  double toxicity_inv_var_avg = (symbols_with_inv_data > 0) ? toxicity_inv_variance_sum / symbols_with_inv_data : 0.0;

  // Aggregate online model learned weights across symbols (weighted by n_updates)
  if (g_config.online_learning) {
    double avg_weights[N_TOXICITY_FEATURES] = {};
    double avg_bias = 0.0;
    int total_updates = 0;
    int models_trained = 0;
    for (uint32_t idx = 0; idx < MAX_SYMBOLS; ++idx) {
      if (!g_sims_initialized[idx].load(std::memory_order_relaxed)) continue;
      PerSymbolSim* sim = g_sims_array[idx];
      if (!sim || !sim->eligible_to_trade) continue;
      const auto& model = sim->online_model;
      if (model.n_updates > model.warmup_fills) {
        int effective = model.n_updates - model.warmup_fills;
        for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
          avg_weights[i] += model.weights[i] * effective;
        }
        avg_bias += model.bias * effective;
        total_updates += effective;
        models_trained++;
      }
    }
    if (total_updates > 0) {
      for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
        avg_weights[i] /= total_updates;
      }
      avg_bias /= total_updates;
    }
    std::cerr << "[Group " << (group_idx+1) << "] Online model: "
              << models_trained << " symbols trained, "
              << total_updates << " total updates, "
              << "weights=[";
    for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
      if (i > 0) std::cerr << ", ";
      std::cerr << std::fixed << std::setprecision(4) << avg_weights[i];
    }
    std::cerr << "], bias=" << std::fixed << std::setprecision(4) << avg_bias << "\n" << std::flush;

    // Write structured JSON weights file if output directory specified
    if (!g_config.output_dir.empty()) {
      std::string json_path = g_config.output_dir + "/learned_weights_group_" + std::to_string(group_idx + 1) + ".json";
      std::ofstream jout(json_path);
      if (jout.is_open()) {
        static const char* feature_names[N_TOXICITY_FEATURES] = {
          "cancel_ratio", "ping_ratio", "odd_lot_ratio", "precision_ratio",
          "resistance_ratio", "trade_flow_imbalance", "spread_change_rate", "price_momentum",
          "cancel_vol_intensity", "top_of_book_conc", "depth_imbalance", "level_asymmetry",
          "abs_trade_imbalance", "large_order_ratio", "normalized_spread"
        };
        jout << "{\n";
        jout << "  \"group\": " << (group_idx + 1) << ",\n";
        jout << "  \"models_trained\": " << models_trained << ",\n";
        jout << "  \"total_updates\": " << total_updates << ",\n";
        jout << "  \"aggregate_weights\": {\n";
        for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
          jout << "    \"" << feature_names[i] << "\": " << std::fixed << std::setprecision(6) << avg_weights[i];
          if (i < N_TOXICITY_FEATURES - 1) jout << ",";
          jout << "\n";
        }
        jout << "  },\n";
        jout << "  \"aggregate_bias\": " << std::fixed << std::setprecision(6) << avg_bias << ",\n";
        jout << "  \"per_symbol\": [\n";
        bool first_symbol = true;
        for (uint32_t idx = 0; idx < MAX_SYMBOLS; ++idx) {
          if (!g_sims_initialized[idx].load(std::memory_order_relaxed)) continue;
          PerSymbolSim* sim = g_sims_array[idx];
          if (!sim || !sim->eligible_to_trade) continue;
          const auto& model = sim->online_model;
          if (model.n_updates <= model.warmup_fills) continue;
          if (!first_symbol) jout << ",\n";
          first_symbol = false;
          jout << "    {\n";
          jout << "      \"symbol_index\": " << idx << ",\n";
          jout << "      \"ticker\": \"" << sim->cached_ticker << "\",\n";
          jout << "      \"n_updates\": " << model.n_updates << ",\n";
          jout << "      \"bias\": " << std::fixed << std::setprecision(6) << model.bias << ",\n";
          jout << "      \"weights\": {";
          for (int i = 0; i < N_TOXICITY_FEATURES; i++) {
            if (i > 0) jout << ", ";
            jout << "\"" << feature_names[i] << "\": " << std::fixed << std::setprecision(6) << model.weights[i];
          }
          jout << "}\n";
          jout << "    }";
        }
        jout << "\n  ]\n";
        jout << "}\n";
        jout.close();
        std::cerr << "[Group " << (group_idx+1) << "] Wrote learned weights JSON: " << json_path << "\n" << std::flush;
      }
    }
  }

  // ========== PnL DECOMPOSITION DUMP ==========
  double avg_inv = (n_with_fills > 0) ? total_abs_final_inv / n_with_fills : 0.0;
  std::cerr << "\n[Group " << (group_idx+1) << "] ===== PnL DECOMPOSITION (TOXICITY STRATEGY) =====\n"
            << "  Realized PnL:      $" << std::fixed << std::setprecision(2) << tox_realized << "\n"
            << "  Unrealized PnL:    $" << tox_unrealized << "\n"
            << "  Adverse penalty:   $" << adverse_pnl << "\n"
            << "  ─────────────────────\n"
            << "  TOTAL:             $" << toxicity_pnl << "\n"
            << "  Unwind crosses:    " << toxicity_unwind_crosses << " ($" << toxicity_unwind_cost << " cost)\n"
            << "  Fills:             " << toxicity_fills << " (buy: " << tox_buy_fills << ", sell: " << tox_sell_fills << ")\n"
            << "  Symbols w/ fills:  " << n_with_fills << "\n"
            << "  One-sided symbols: " << n_one_sided
            << " (" << (n_with_fills > 0 ? 100.0 * n_one_sided / n_with_fills : 0.0) << "%)\n"
            << "  EOD liquidated:    " << n_eod_liquidated << "\n"
            << "  Blacklisted:       " << n_blacklisted << "\n"
            << "  Avg |final inv|:   " << std::fixed << std::setprecision(1) << avg_inv << " shares\n"
            << std::flush;

  std::cerr << "[Group " << (group_idx+1) << "] ===== PnL DECOMPOSITION (BASELINE) =====\n"
            << "  Realized PnL:      $" << std::fixed << std::setprecision(2) << base_realized << "\n"
            << "  Unrealized PnL:    $" << base_unrealized << "\n"
            << "  Adverse penalty:   $" << baseline_adverse_pnl << "\n"
            << "  ─────────────────────\n"
            << "  TOTAL:             $" << baseline_pnl << "\n"
            << "  Fills:             " << baseline_fills << " (buy: " << base_buy_fills << ", sell: " << base_sell_fills << ")\n"
            << std::flush;

  // Worst 10 symbols by toxicity total PnL
  std::sort(sym_diags.begin(), sym_diags.end(),
            [](const SymDiag& a, const SymDiag& b) { return a.tox_total < b.tox_total; });
  std::cerr << "[Group " << (group_idx+1) << "] ===== WORST 10 SYMBOLS (TOXICITY) =====\n";
  size_t dump_n = std::min<size_t>(10, sym_diags.size());
  for (size_t i = 0; i < dump_n; i++) {
    const auto& s = sym_diags[i];
    std::cerr << "  " << std::setw(6) << s.ticker
              << "  total=$" << std::fixed << std::setprecision(2) << std::setw(10) << s.tox_total
              << "  real=$" << std::setw(10) << s.tox_realized
              << "  unreal=$" << std::setw(10) << s.tox_unrealized
              << "  adv=$" << std::setw(8) << s.tox_adverse
              << "  buys=" << s.tox_buys << " sells=" << s.tox_sells
              << "  unwinds=" << s.tox_unwinds << "($" << s.tox_unwind_cost << ")"
              << "  inv=" << std::setw(6) << s.final_inv
              << (s.eod ? " EOD" : "") << (s.blacklisted ? " BL" : "")
              << "\n";
  }

  // Best 5 symbols
  std::cerr << "[Group " << (group_idx+1) << "] ===== BEST 5 SYMBOLS (TOXICITY) =====\n";
  size_t best_n = std::min<size_t>(5, sym_diags.size());
  for (size_t i = sym_diags.size(); i > sym_diags.size() - best_n; i--) {
    const auto& s = sym_diags[i-1];
    std::cerr << "  " << std::setw(6) << s.ticker
              << "  total=$" << std::fixed << std::setprecision(2) << std::setw(10) << s.tox_total
              << "  real=$" << std::setw(10) << s.tox_realized
              << "  unreal=$" << std::setw(10) << s.tox_unrealized
              << "  buys=" << s.tox_buys << " sells=" << s.tox_sells
              << "  inv=" << std::setw(6) << s.final_inv
              << "\n";
  }
  std::cerr << std::flush;

  // Write results to shared memory
  results->baseline_pnl = baseline_pnl;
  results->toxicity_pnl = toxicity_pnl;
  results->adverse_pnl = adverse_pnl;
  results->baseline_adverse_pnl = baseline_adverse_pnl;
  results->baseline_inv_variance = baseline_inv_var_avg;
  results->toxicity_inv_variance = toxicity_inv_var_avg;
  results->baseline_fills = baseline_fills;
  results->toxicity_fills = toxicity_fills;
  results->quotes_suppressed = quotes_suppressed;
  results->adverse_fills = adverse_fills;
  results->baseline_adverse_fills = baseline_adverse_fills;
  results->baseline_unwind_crosses = baseline_unwind_crosses;
  results->toxicity_unwind_crosses = toxicity_unwind_crosses;
  results->baseline_unwind_cost = baseline_unwind_cost;
  results->toxicity_unwind_cost = toxicity_unwind_cost;
  results->toxicity_realized_pnl = tox_realized;
  results->toxicity_unrealized_pnl = tox_unrealized;
  results->baseline_realized_pnl = base_realized;
  results->baseline_unrealized_pnl = base_unrealized;
  results->symbols_eod_liquidated = n_eod_liquidated;
  results->symbols_blacklisted = n_blacklisted;
  results->symbols_one_sided = n_one_sided;
  results->symbols_with_fills = n_with_fills;
  results->toxicity_buy_fills = tox_buy_fills;
  results->toxicity_sell_fills = tox_sell_fills;
  results->baseline_buy_fills = base_buy_fills;
  results->baseline_sell_fills = base_sell_fills;
  results->avg_final_abs_inventory = avg_inv;
  results->packets_processed = g_total_packets.load();
  results->messages_processed = g_total_messages.load();
  results->symbols_active = g_active_symbols.load();
  results->diag_exec_total = diag_agg.exec_total;
  results->diag_exec_no_order_info = diag_agg.exec_no_order_info;
  results->diag_exec_not_eligible = diag_agg.exec_not_eligible;
  results->diag_try_fill_calls = diag_agg.try_fill_calls;
  results->diag_rejected_halted = diag_agg.rejected_halted;
  results->diag_rejected_not_live = diag_agg.rejected_not_live;
  results->diag_rejected_latency = diag_agg.rejected_latency;
  results->diag_rejected_price = diag_agg.rejected_price;
  results->diag_rejected_queue = diag_agg.rejected_queue;
  results->diag_fill_succeeded = diag_agg.fill_succeeded;
  results->diag_quote_resets = diag_agg.quote_resets;
  results->completed = true;

  std::cerr << "[Group " << (group_idx+1) << "] Results written to shared memory\n" << std::flush;

  // Write per-fill and per-symbol CSV output if output directory specified
  if (!g_config.output_dir.empty()) {
    // Per-fill CSV: toxicity score at fill time + realized adverse movement
    {
      std::string fill_path = g_config.output_dir + "/fills_group_" + std::to_string(group_idx + 1) + ".csv";
      std::ofstream fout(fill_path);
      if (fout.is_open()) {
        const char* filter_type_str = (g_config.filter_type == FilterType::EWMA) ? "ewma" : "logistic";
        fout << "group,symbol,ticker,strategy,filter_type,fill_time_ns,fill_price,fill_qty,is_buy,"
             << "mid_price_at_fill,toxicity_at_fill,adverse_measured,adverse_pnl,cumulative_pnl,"
             << "cancel_ratio,ping_ratio,odd_lot_ratio,precision_ratio,resistance_ratio,"
             << "trade_flow_imbalance,spread_change_rate,price_momentum,"
             << "cancel_vol_intensity,top_of_book_conc,depth_imbalance,level_asymmetry,"
             << "abs_trade_imbalance,large_order_ratio,normalized_spread,"
             << "wf_window\n";
        for (uint32_t idx = 0; idx < MAX_SYMBOLS; ++idx) {
          if (!g_sims_initialized[idx].load(std::memory_order_relaxed)) continue;
          PerSymbolSim* sim = g_sims_array[idx];
          if (!sim || !sim->eligible_to_trade) continue;
          std::string ticker = xdp::get_symbol(idx);
          // Lambda to write a fill row (includes filter_type and wf_window columns)
          auto write_fill = [&](const FillRecord& fill, const char* strategy) {
            fout << (group_idx+1) << ',' << idx << ',' << ticker << ',' << strategy << ','
                 << filter_type_str << ',' << fill.fill_time_ns << ',' << std::fixed << std::setprecision(4)
                 << fill.fill_price << ',' << fill.fill_qty << ','
                 << (fill.is_buy ? 1 : 0) << ',' << fill.mid_price_at_fill << ','
                 << fill.toxicity_at_fill << ',' << (fill.adverse_measured ? 1 : 0)
                 << ',' << fill.adverse_pnl << ',' << fill.cumulative_pnl;
            for (int fi = 0; fi < N_TOXICITY_FEATURES; fi++) {
              fout << ',' << fill.features.features[fi];
            }
            // Walk-forward window assignment
            int wf_win = -1;
            if (g_config.walk_forward && sim->wf_initialized && sim->wf_window_duration_ns > 0) {
              uint64_t fill_elapsed = fill.fill_time_ns - sim->wf_window_start_ns;
              wf_win = static_cast<int>(fill_elapsed / sim->wf_window_duration_ns);
            }
            fout << ',' << wf_win << '\n';
          };
          // Toxicity strategy: completed fills (with measured adverse_pnl) + remaining pending
          for (const auto& fill : sim->toxicity_completed_fills) write_fill(fill, "toxicity");
          for (const auto& fill : sim->toxicity_pending_fills)   write_fill(fill, "toxicity");
          // Baseline strategy: completed fills + remaining pending
          for (const auto& fill : sim->baseline_completed_fills) write_fill(fill, "baseline");
          for (const auto& fill : sim->baseline_pending_fills)   write_fill(fill, "baseline");
        }
        fout.close();
        std::cerr << "[Group " << (group_idx+1) << "] Wrote fills CSV: " << fill_path << "\n" << std::flush;
      }
    }

    // Per-symbol CSV: summary metrics per symbol (enhanced with PnL decomposition)
    {
      std::string sym_path = g_config.output_dir + "/symbols_group_" + std::to_string(group_idx + 1) + ".csv";
      std::ofstream fout(sym_path);
      if (fout.is_open()) {
        const char* sym_filter_str = (g_config.filter_type == FilterType::EWMA) ? "ewma" : "logistic";
        fout << "group,symbol_index,ticker,filter_type,"
             << "baseline_pnl,toxicity_pnl,improvement,"
             << "baseline_realized,baseline_unrealized,toxicity_realized,toxicity_unrealized,"
             << "baseline_fills,toxicity_fills,"
             << "tox_buy_fills,tox_sell_fills,base_buy_fills,base_sell_fills,"
             << "quotes_suppressed,"
             << "baseline_adverse_pnl,toxicity_adverse_pnl,"
             << "tox_unwind_crosses,tox_unwind_cost,base_unwind_crosses,base_unwind_cost,"
             << "tox_final_inventory,base_final_inventory,"
             << "tox_max_inventory,tox_min_inventory,"
             << "eod_liquidated,blacklisted,"
             << "baseline_inv_var,toxicity_inv_var\n";
        for (uint32_t idx = 0; idx < MAX_SYMBOLS; ++idx) {
          if (!g_sims_initialized[idx].load(std::memory_order_relaxed)) continue;
          PerSymbolSim* sim = g_sims_array[idx];
          if (!sim || !sim->eligible_to_trade) continue;
          const auto& bs = sim->mm_baseline.get_stats();
          const auto& ts = sim->mm_toxicity.get_stats();
          double b_pnl = bs.realized_pnl + bs.unrealized_pnl + sim->baseline_risk.total_adverse_pnl;
          double t_pnl = ts.realized_pnl + ts.unrealized_pnl + sim->toxicity_risk.total_adverse_pnl;
          fout << (group_idx+1) << ',' << idx << ',' << xdp::get_symbol(idx) << ','
               << sym_filter_str << ',' << std::fixed << std::setprecision(4)
               << b_pnl << ',' << t_pnl << ',' << (t_pnl - b_pnl) << ','
               << bs.realized_pnl << ',' << bs.unrealized_pnl << ','
               << ts.realized_pnl << ',' << ts.unrealized_pnl << ','
               << sim->baseline_risk.total_fills << ',' << sim->toxicity_risk.total_fills << ','
               << ts.buy_fills << ',' << ts.sell_fills << ','
               << bs.buy_fills << ',' << bs.sell_fills << ','
               << ts.quotes_suppressed << ','
               << sim->baseline_risk.total_adverse_pnl << ',' << sim->toxicity_risk.total_adverse_pnl << ','
               << ts.unwind_crosses << ',' << ts.unwind_cost << ','
               << bs.unwind_crosses << ',' << bs.unwind_cost << ','
               << sim->mm_toxicity.get_inventory() << ',' << sim->mm_baseline.get_inventory() << ','
               << ts.max_inventory << ',' << ts.min_inventory << ','
               << (sim->eod_liquidated ? 1 : 0) << ',' << (sim->blacklisted ? 1 : 0) << ','
               << sim->baseline_risk.get_inventory_variance() << ','
               << sim->toxicity_risk.get_inventory_variance() << '\n';
        }
        fout.close();
        std::cerr << "[Group " << (group_idx+1) << "] Wrote symbols CSV: " << sym_path << "\n" << std::flush;
      }
    }

    // Walk-forward per-window CSV: aggregate metrics per time window across symbols
    if (g_config.walk_forward) {
      std::string wf_path = g_config.output_dir + "/walk_forward_group_" + std::to_string(group_idx + 1) + ".csv";
      std::ofstream wfout(wf_path);
      if (wfout.is_open()) {
        wfout << "group,symbol_index,ticker,window_id,toxicity_pnl,baseline_pnl,fills,suppressed\n";
        for (uint32_t idx = 0; idx < MAX_SYMBOLS; ++idx) {
          if (!g_sims_initialized[idx].load(std::memory_order_relaxed)) continue;
          PerSymbolSim* sim = g_sims_array[idx];
          if (!sim || !sim->eligible_to_trade) continue;
          for (const auto& wm : sim->wf_window_metrics) {
            wfout << (group_idx+1) << ',' << idx << ',' << sim->cached_ticker << ','
                  << wm.window_id << ','
                  << std::fixed << std::setprecision(4)
                  << wm.toxicity_pnl << ',' << wm.baseline_pnl << ','
                  << wm.fills << ',' << wm.suppressed << '\n';
          }
        }
        wfout.close();
        std::cerr << "[Group " << (group_idx+1) << "] Wrote walk-forward CSV: " << wf_path << "\n" << std::flush;
      }
    }
  }
}

} // namespace

int main(int argc, char *argv[]) {
  std::vector<std::string> pcap_files;
  std::string symbol_file = "data/symbol_nyse_parsed.csv";
  std::string data_dir;

  // Parse arguments - collect PCAP files and options
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg == "-t" && i + 1 < argc) {
      g_filter_ticker = argv[++i];
    } else if ((arg == "-s" || arg == "--symbols") && i + 1 < argc) {
      symbol_file = argv[++i];
    } else if (arg == "--seed" && i + 1 < argc) {
      g_config.exec.seed = std::stoull(argv[++i]);
    } else if (arg == "--latency-us" && i + 1 < argc) {
      g_config.exec.latency_us_mean = std::stod(argv[++i]);
    } else if (arg == "--latency-jitter-us" && i + 1 < argc) {
      g_config.exec.latency_us_jitter = std::stod(argv[++i]);
    } else if (arg == "--queue-fraction" && i + 1 < argc) {
      g_config.exec.queue_position_fraction = std::stod(argv[++i]);
    } else if (arg == "--adverse-lookforward-us" && i + 1 < argc) {
      g_config.exec.adverse_lookforward_us = std::stoull(argv[++i]);
    } else if (arg == "--adverse-multiplier" && i + 1 < argc) {
      g_config.exec.adverse_selection_multiplier = std::stod(argv[++i]);
    } else if (arg == "--maker-rebate" && i + 1 < argc) {
      g_config.exec.maker_rebate_per_share = std::stod(argv[++i]);
    } else if (arg == "--max-position" && i + 1 < argc) {
      g_config.exec.max_position_per_symbol = std::stod(argv[++i]);
    } else if (arg == "--max-loss" && i + 1 < argc) {
      g_config.exec.max_daily_loss_per_symbol = std::stod(argv[++i]);
    } else if (arg == "--fill-mode" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode == "match") {
        g_config.exec.fill_mode = ExecutionModelConfig::FillMode::Match;
      } else {
        g_config.exec.fill_mode = ExecutionModelConfig::FillMode::Cross;
      }
    } else if (arg == "--quote-interval-us" && i + 1 < argc) {
      g_config.exec.quote_update_interval_us = std::stoull(argv[++i]);
    } else if (arg == "--threads" && i + 1 < argc) {
      g_num_threads = std::stoull(argv[++i]);
    } else if (arg == "--files-per-group" && i + 1 < argc) {
      g_files_per_group = std::stoull(argv[++i]);
    } else if (arg == "--toxicity-threshold" && i + 1 < argc) {
      g_config.toxicity_threshold = std::stod(argv[++i]);
    } else if (arg == "--toxicity-multiplier" && i + 1 < argc) {
      g_config.toxicity_multiplier = std::stod(argv[++i]);
    } else if (arg == "--epsilon-min" && i + 1 < argc) {
      g_config.epsilon_min = std::stod(argv[++i]);
    } else if (arg == "--output-dir" && i + 1 < argc) {
      g_config.output_dir = argv[++i];
    } else if (arg == "--filter-type" && i + 1 < argc) {
      const std::string ft = argv[++i];
      if (ft == "ewma") {
        g_config.filter_type = FilterType::EWMA;
      } else {
        g_config.filter_type = FilterType::LOGISTIC;
      }
    } else if (arg == "--ewma-alpha" && i + 1 < argc) {
      g_config.ewma_alpha = std::stod(argv[++i]);
    } else if (arg == "--ewma-k" && i + 1 < argc) {
      g_config.ewma_threshold_k = std::stod(argv[++i]);
    } else if (arg == "--ewma-min-obs" && i + 1 < argc) {
      g_config.ewma_min_obs = std::stoi(argv[++i]);
    } else if (arg == "--ablation" && i + 1 < argc) {
      const std::string mode = argv[++i];
      if (mode == "spread-only") {
        g_config.ablation_mode = AblationMode::SPREAD_ONLY;
      } else if (mode == "pnl-filter-only") {
        g_config.ablation_mode = AblationMode::PNL_FILTER_ONLY;
      } else if (mode == "obi-only") {
        g_config.ablation_mode = AblationMode::OBI_ONLY;
      } else {
        g_config.ablation_mode = AblationMode::FULL;
      }
    } else if (arg == "--online-learning") {
      g_config.online_learning = true;
    } else if (arg == "--learning-rate" && i + 1 < argc) {
      g_config.learning_rate = std::stod(argv[++i]);
    } else if (arg == "--warmup-fills" && i + 1 < argc) {
      g_config.warmup_fills = std::stoi(argv[++i]);
    } else if (arg == "--walk-forward") {
      g_config.walk_forward = true;
    } else if (arg == "--wf-window-minutes" && i + 1 < argc) {
      g_config.wf_window_minutes = std::stoi(argv[++i]);
    } else if (arg == "--sequential") {
      g_use_parallel = false;
      g_use_hybrid = false;
    } else if (arg == "--no-hybrid") {
      g_use_hybrid = false;
    } else if (arg == "--data-dir" && i + 1 < argc) {
      data_dir = argv[++i];
    } else if (arg == "--mmap") {
      // mmap is now default, this flag is kept for compatibility
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (arg[0] != '-') {
      // Assume it's a PCAP file
      pcap_files.push_back(arg);
    }
  }

  // If no PCAP files given explicitly, scan data directory for *.pcap
  if (pcap_files.empty()) {
    if (data_dir.empty()) data_dir = DEFAULT_DATA_DIR;
    namespace fs = std::filesystem;
    if (!fs::is_directory(data_dir)) {
      std::cerr << "Error: Data directory not found: " << data_dir << "\n";
      print_usage(argv[0]);
      return 1;
    }
    for (const auto &entry : fs::directory_iterator(data_dir)) {
      if (entry.is_regular_file() && entry.path().extension() == ".pcap") {
        pcap_files.push_back(entry.path().string());
      }
    }
    if (pcap_files.empty()) {
      std::cerr << "Error: No *.pcap files found in " << data_dir << "\n";
      return 1;
    }
    std::cerr << "Auto-discovered " << pcap_files.size() << " PCAP files from " << data_dir << "/\n";
  } else if (!data_dir.empty()) {
    std::cerr << "Warning: --data-dir ignored because PCAP files were specified explicitly\n";
  }

  // Sort PCAP files by name to ensure chronological order
  std::sort(pcap_files.begin(), pcap_files.end());

  // Determine number of processes/threads
  size_t num_procs = g_num_threads;
  if (num_procs == 0) {
    num_procs = std::thread::hardware_concurrency();
    if (num_procs == 0) num_procs = 4;
  }

  // Determine mode string
  std::string mode_str = "SEQUENTIAL";
  if (g_use_hybrid && g_use_parallel && pcap_files.size() > 1) {
    mode_str = "HYBRID MULTI-PROCESS";
  } else if (g_use_parallel && pcap_files.size() > 1) {
    mode_str = "THREADED";
  }

  // Log execution parameters for reproducibility
  std::cerr << "=== Simulation Parameters ===\n"
            << "Mode: " << mode_str << "\n"
            << "PCAP files: " << pcap_files.size() << "\n"
            << "Symbol file: " << symbol_file << "\n"
            << "Seed: " << g_config.exec.seed << "\n"
            << "Latency (us): " << g_config.exec.latency_us_mean
            << " +/- " << g_config.exec.latency_us_jitter << "\n"
            << "Queue fraction: " << g_config.exec.queue_position_fraction << "\n"
            << "Queue variance: " << g_config.exec.queue_position_variance << "\n"
            << "Adverse lookforward (us): " << g_config.exec.adverse_lookforward_us << "\n"
            << "Adverse multiplier: " << g_config.exec.adverse_selection_multiplier << "\n"
            << "Quote interval (us): " << g_config.exec.quote_update_interval_us << "\n"
            << "Maker rebate: " << g_config.exec.maker_rebate_per_share << "\n"
            << "Taker fee: " << g_config.exec.taker_fee_per_share << "\n"
            << "Clearing fee: " << g_config.exec.clearing_fee_per_share << "\n"
            << "Max position: " << g_config.exec.max_position_per_symbol << "\n"
            << "Max loss: " << g_config.exec.max_daily_loss_per_symbol << "\n"
            << "Fill mode: " << (g_config.exec.fill_mode == ExecutionModelConfig::FillMode::Cross ? "cross" : "match") << "\n"
            << "Filter type: " << (g_config.filter_type == FilterType::EWMA ? "ewma" : "logistic") << "\n";
  if (g_config.filter_type == FilterType::EWMA) {
    std::cerr << "  EWMA alpha: " << g_config.ewma_alpha << "\n"
              << "  EWMA threshold k: " << g_config.ewma_threshold_k << "\n"
              << "  EWMA min observations: " << g_config.ewma_min_obs << "\n";
  }
  std::cerr << "Online learning: " << (g_config.online_learning ? "enabled" : "disabled") << "\n"
            << "Ablation mode: " << (g_config.ablation_mode == AblationMode::SPREAD_ONLY ? "spread-only" :
                                     g_config.ablation_mode == AblationMode::PNL_FILTER_ONLY ? "pnl-filter-only" :
                                     g_config.ablation_mode == AblationMode::OBI_ONLY ? "obi-only" : "full") << "\n";
  if (g_config.online_learning) {
    std::cerr << "  Learning rate: " << g_config.learning_rate << "\n"
              << "  Warmup fills: " << g_config.warmup_fills << "\n";
  }
  if (g_config.walk_forward) {
    if (!g_config.online_learning) {
      std::cerr << "WARNING: --walk-forward requires --online-learning, enabling it\n";
      g_config.online_learning = true;
    }
    std::cerr << "Walk-forward: enabled\n"
              << "  Window size: " << g_config.wf_window_minutes << " minutes\n";
  }
  if (!g_filter_ticker.empty()) {
    std::cerr << "Ticker filter: " << g_filter_ticker << "\n";
  }
  if (!g_config.output_dir.empty()) {
    std::cerr << "Output dir: " << g_config.output_dir << "\n";
  }
  std::cerr << "Processes: " << num_procs << "\n"
            << "============================\n" << std::flush;

  auto start_time = std::chrono::high_resolution_clock::now();

  // ==========================================================================
  // HYBRID MULTI-PROCESS MODE
  // ==========================================================================
  if (g_use_hybrid && g_use_parallel && pcap_files.size() > 1) {
    // Group files for parallel processing
    auto file_groups = group_files(pcap_files, num_procs);
    size_t actual_groups = file_groups.size();

    // Print header BEFORE fork to avoid duplicate output
    std::cout << "=== HFT Market Maker Simulation (HYBRID) ===\n";
    std::cout << "PCAP files: " << pcap_files.size() << '\n';
    std::cout << "Process groups: " << actual_groups << '\n';
    for (size_t i = 0; i < actual_groups; ++i) {
      std::cout << "  Group " << (i+1) << ": " << file_groups[i].size() << " files\n";
    }
    std::cout << "\nSpawning child processes...\n" << std::flush;

    // Allocate shared memory for results
    size_t shm_size = sizeof(ProcessResults) * actual_groups;
    ProcessResults* shared_results = static_cast<ProcessResults*>(
        mmap(nullptr, shm_size, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0));

    if (shared_results == MAP_FAILED) {
      std::cerr << "Failed to allocate shared memory\n";
      return 1;
    }

    // Initialize shared memory
    std::memset(shared_results, 0, shm_size);

    // Fork child processes
    std::vector<pid_t> children;
    for (size_t group_idx = 0; group_idx < actual_groups; ++group_idx) {
      pid_t pid = fork();

      if (pid < 0) {
        std::cerr << "Fork failed for group " << group_idx << "\n";
        continue;
      }

      if (pid == 0) {
        // Child process
        process_file_group(file_groups[group_idx],
                           &shared_results[group_idx],
                           symbol_file,
                           group_idx);

        // Print progress from child (use stderr to avoid buffering issues)
        std::cerr << "[Group " << (group_idx + 1) << "/" << actual_groups << "] "
                  << "Completed: " << shared_results[group_idx].packets_processed
                  << " packets, " << shared_results[group_idx].messages_processed
                  << " msgs\n" << std::flush;

        _exit(0);  // Exit child without calling destructors
      }

      children.push_back(pid);
    }

    // Parent: wait for all children with proper error checking
    std::cout << "Waiting for " << children.size() << " child processes...\n" << std::flush;

    size_t children_completed = 0;
    size_t children_crashed = 0;

    for (size_t i = 0; i < children.size(); ++i) {
      pid_t child = children[i];
      int status = 0;
      pid_t result = waitpid(child, &status, 0);

      if (result < 0) {
        std::cerr << "waitpid failed for child " << child << " (group " << (i+1) << "): "
                  << strerror(errno) << "\n";
        continue;
      }

      if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
          children_completed++;
        } else {
          std::cerr << "Group " << (i+1) << " exited with code " << exit_code << "\n";
          children_crashed++;
        }
      } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        std::cerr << "Group " << (i+1) << " killed by signal " << sig;
        if (sig == SIGSEGV) std::cerr << " (segmentation fault)";
        else if (sig == SIGBUS) std::cerr << " (bus error)";
        else if (sig == SIGKILL) std::cerr << " (killed - OOM?)";
        else if (sig == SIGABRT) std::cerr << " (abort)";
        std::cerr << "\n";
        children_crashed++;
      } else {
        std::cerr << "Group " << (i+1) << " ended with unknown status\n";
        children_crashed++;
      }
    }

    std::cout << "\nChild processes finished: " << children_completed << " completed, "
              << children_crashed << " failed\n";
    std::cout << "Aggregating results...\n";

    // Aggregate results from all processes
    double total_baseline_pnl = 0.0, total_toxicity_pnl = 0.0, total_adverse_pnl = 0.0;
    double total_baseline_adverse_pnl = 0.0;
    double total_tox_realized = 0.0, total_tox_unrealized = 0.0;
    double total_base_realized = 0.0, total_base_unrealized = 0.0;
    double total_baseline_inv_var = 0.0, total_toxicity_inv_var = 0.0;
    int64_t total_baseline_fills = 0, total_toxicity_fills = 0;
    int64_t total_quotes_suppressed = 0, total_adverse_fills = 0, total_baseline_adverse_fills = 0;
    int64_t total_baseline_unwind = 0, total_toxicity_unwind = 0;
    double total_baseline_unwind_cost = 0.0, total_toxicity_unwind_cost = 0.0;
    int64_t total_eod = 0, total_blacklisted = 0, total_one_sided = 0, total_with_fills = 0;
    int64_t total_tox_buys = 0, total_tox_sells = 0;
    uint64_t total_packets = 0, total_messages = 0, total_symbols = 0;
    size_t groups_with_results = 0;

    for (size_t i = 0; i < actual_groups; ++i) {
      if (shared_results[i].completed) {
        groups_with_results++;
        total_baseline_pnl += shared_results[i].baseline_pnl;
        total_toxicity_pnl += shared_results[i].toxicity_pnl;
        total_adverse_pnl += shared_results[i].adverse_pnl;
        total_baseline_adverse_pnl += shared_results[i].baseline_adverse_pnl;
        total_tox_realized += shared_results[i].toxicity_realized_pnl;
        total_tox_unrealized += shared_results[i].toxicity_unrealized_pnl;
        total_base_realized += shared_results[i].baseline_realized_pnl;
        total_base_unrealized += shared_results[i].baseline_unrealized_pnl;
        total_baseline_inv_var += shared_results[i].baseline_inv_variance;
        total_toxicity_inv_var += shared_results[i].toxicity_inv_variance;
        total_baseline_fills += shared_results[i].baseline_fills;
        total_toxicity_fills += shared_results[i].toxicity_fills;
        total_tox_buys += shared_results[i].toxicity_buy_fills;
        total_tox_sells += shared_results[i].toxicity_sell_fills;
        total_quotes_suppressed += shared_results[i].quotes_suppressed;
        total_adverse_fills += shared_results[i].adverse_fills;
        total_baseline_adverse_fills += shared_results[i].baseline_adverse_fills;
        total_baseline_unwind += shared_results[i].baseline_unwind_crosses;
        total_toxicity_unwind += shared_results[i].toxicity_unwind_crosses;
        total_baseline_unwind_cost += shared_results[i].baseline_unwind_cost;
        total_toxicity_unwind_cost += shared_results[i].toxicity_unwind_cost;
        total_eod += shared_results[i].symbols_eod_liquidated;
        total_blacklisted += shared_results[i].symbols_blacklisted;
        total_one_sided += shared_results[i].symbols_one_sided;
        total_with_fills += shared_results[i].symbols_with_fills;
        total_packets += shared_results[i].packets_processed;
        total_messages += shared_results[i].messages_processed;
        total_symbols += shared_results[i].symbols_active;
      } else {
        std::cerr << "Warning: Group " << (i+1) << " did not write results to shared memory\n";
      }
    }

    std::cout << "Groups with valid results: " << groups_with_results << "/" << actual_groups << '\n';

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    double seconds = duration.count() / 1000.0;

    // Print aggregated results
    std::cout << "\n=== PERFORMANCE STATISTICS ===\n";
    std::cout << "Total processing time: " << std::fixed << std::setprecision(2)
              << seconds << " seconds\n";
    std::cout << "Total packets: " << total_packets << '\n';
    std::cout << "Total messages: " << total_messages << '\n';
    std::cout << "Throughput: " << std::fixed << std::setprecision(0)
              << (total_packets / seconds) << " packets/sec, "
              << (total_messages / seconds) << " msgs/sec\n";
    std::cout << "Process groups: " << actual_groups << '\n';
    std::cout << "Unique symbols (sum): " << total_symbols << '\n';

    double improvement = total_toxicity_pnl - total_baseline_pnl;
    double improvement_pct = total_baseline_pnl != 0.0
        ? (improvement / std::abs(total_baseline_pnl)) * 100.0 : 0.0;

    std::cout << "\n=== AGGREGATED SIMULATION RESULTS ===\n";
    std::cout << "Filter type: " << (g_config.filter_type == FilterType::EWMA ? "ewma" : "logistic") << '\n';
    std::cout << "Baseline Total PnL: $" << std::fixed << std::setprecision(2)
              << total_baseline_pnl << '\n';
    std::cout << "Toxicity Total PnL: $" << std::fixed << std::setprecision(2)
              << total_toxicity_pnl << '\n';
    std::cout << "PnL Improvement: $" << std::fixed << std::setprecision(2)
              << improvement << " (" << improvement_pct << "%)\n";

    std::cout << "\n--- PnL DECOMPOSITION (TOXICITY) ---\n";
    std::cout << "  Realized PnL:      $" << std::fixed << std::setprecision(2) << total_tox_realized << '\n';
    std::cout << "  Unrealized PnL:    $" << total_tox_unrealized << '\n';
    std::cout << "  Adverse penalty:   $" << total_adverse_pnl << '\n';
    std::cout << "  TOTAL:             $" << total_toxicity_pnl << '\n';

    std::cout << "\n--- PnL DECOMPOSITION (BASELINE) ---\n";
    std::cout << "  Realized PnL:      $" << std::fixed << std::setprecision(2) << total_base_realized << '\n';
    std::cout << "  Unrealized PnL:    $" << total_base_unrealized << '\n';
    std::cout << "  Adverse penalty:   $" << total_baseline_adverse_pnl << '\n';
    std::cout << "  TOTAL:             $" << total_baseline_pnl << '\n';

    std::cout << "\n--- FILL STATISTICS ---\n";
    std::cout << "Baseline fills: " << total_baseline_fills << '\n';
    std::cout << "Toxicity fills: " << total_toxicity_fills
              << " (buy: " << total_tox_buys << ", sell: " << total_tox_sells << ")\n";
    std::cout << "Quotes suppressed: " << total_quotes_suppressed << '\n';
    std::cout << "Adverse fills: " << total_adverse_fills << '\n';
    std::cout << "Baseline adverse fills: " << total_baseline_adverse_fills << '\n';

    std::cout << "\n--- INVENTORY MANAGEMENT ---\n";
    std::cout << "Baseline unwind crosses: " << total_baseline_unwind
              << " (cost: $" << std::fixed << std::setprecision(2) << total_baseline_unwind_cost << ")\n";
    std::cout << "Toxicity unwind crosses: " << total_toxicity_unwind
              << " (cost: $" << std::fixed << std::setprecision(2) << total_toxicity_unwind_cost << ")\n";
    std::cout << "Symbols with fills (sum): " << total_with_fills << '\n';
    std::cout << "One-sided symbols (sum):  " << total_one_sided
              << " (" << (total_with_fills > 0 ? 100.0 * total_one_sided / total_with_fills : 0.0) << "%)\n";
    std::cout << "EOD liquidated (sum):     " << total_eod << '\n';
    std::cout << "Blacklisted (sum):        " << total_blacklisted << '\n';

    // Output hypothesis testing metrics (per-group)
    double avg_baseline_inv_var = (groups_with_results > 0) ? total_baseline_inv_var / groups_with_results : 0.0;
    double avg_toxicity_inv_var = (groups_with_results > 0) ? total_toxicity_inv_var / groups_with_results : 0.0;
    std::cout << "\n=== HYPOTHESIS TESTING METRICS ===\n";
    std::cout << "Average Baseline Inventory Variance: " << std::fixed << std::setprecision(2)
              << avg_baseline_inv_var << '\n';
    std::cout << "Average Toxicity Inventory Variance: " << std::fixed << std::setprecision(2)
              << avg_toxicity_inv_var << '\n';
    std::cout << "Inventory Variance Reduction: " << std::fixed << std::setprecision(2)
              << ((avg_baseline_inv_var > 0) ? (1.0 - avg_toxicity_inv_var / avg_baseline_inv_var) * 100.0 : 0.0)
              << "%\n";

    // Fill pipeline diagnostics
    uint64_t d_exec_total = 0, d_exec_no_oi = 0, d_exec_not_elig = 0;
    uint64_t d_try_fill = 0, d_halted = 0, d_not_live = 0, d_latency = 0;
    uint64_t d_price = 0, d_queue = 0, d_fill = 0, d_resets = 0;
    for (size_t i = 0; i < actual_groups; ++i) {
      if (shared_results[i].completed) {
        d_exec_total += shared_results[i].diag_exec_total;
        d_exec_no_oi += shared_results[i].diag_exec_no_order_info;
        d_exec_not_elig += shared_results[i].diag_exec_not_eligible;
        d_try_fill += shared_results[i].diag_try_fill_calls;
        d_halted += shared_results[i].diag_rejected_halted;
        d_not_live += shared_results[i].diag_rejected_not_live;
        d_latency += shared_results[i].diag_rejected_latency;
        d_price += shared_results[i].diag_rejected_price;
        d_queue += shared_results[i].diag_rejected_queue;
        d_fill += shared_results[i].diag_fill_succeeded;
        d_resets += shared_results[i].diag_quote_resets;
      }
    }
    std::cout << "\n=== FILL PIPELINE DIAGNOSTICS (toxicity strategy) ===\n";
    std::cout << "Execution messages total: " << d_exec_total << '\n';
    std::cout << "  - Order ID not found (cleaned up): " << d_exec_no_oi
              << " (" << std::fixed << std::setprecision(1)
              << (d_exec_total > 0 ? 100.0 * d_exec_no_oi / d_exec_total : 0.0) << "%)\n";
    std::cout << "  - Symbol not eligible: " << d_exec_not_elig
              << " (" << (d_exec_total > 0 ? 100.0 * d_exec_not_elig / d_exec_total : 0.0) << "%)\n";
    std::cout << "try_fill_one calls: " << d_try_fill << '\n';
    std::cout << "  - Rejected (halted): " << d_halted
              << " (" << (d_try_fill > 0 ? 100.0 * d_halted / d_try_fill : 0.0) << "%)\n";
    std::cout << "  - Rejected (not live/no remaining): " << d_not_live
              << " (" << (d_try_fill > 0 ? 100.0 * d_not_live / d_try_fill : 0.0) << "%)\n";
    std::cout << "  - Rejected (latency gate): " << d_latency
              << " (" << (d_try_fill > 0 ? 100.0 * d_latency / d_try_fill : 0.0) << "%)\n";
    std::cout << "  - Rejected (price ineligible): " << d_price
              << " (" << (d_try_fill > 0 ? 100.0 * d_price / d_try_fill : 0.0) << "%)\n";
    std::cout << "  - Rejected (queue not consumed): " << d_queue
              << " (" << (d_try_fill > 0 ? 100.0 * d_queue / d_try_fill : 0.0) << "%)\n";
    std::cout << "  - FILLED: " << d_fill
              << " (" << (d_try_fill > 0 ? 100.0 * d_fill / d_try_fill : 0.0) << "%)\n";
    std::cout << "Quote/queue resets: " << d_resets << '\n';

    // Output per-group data for hypothesis testing script (enhanced with decomposition)
    std::cout << "\n=== PER-GROUP RESULTS (FOR HYPOTHESIS TESTING) ===\n";
    for (size_t i = 0; i < actual_groups; ++i) {
      if (shared_results[i].completed) {
        const auto& r = shared_results[i];
        std::cout << "Group " << (i+1) << ": "
                  << "baseline_pnl=" << std::fixed << std::setprecision(4) << r.baseline_pnl
                  << ", toxicity_pnl=" << r.toxicity_pnl
                  << ", tox_real=" << r.toxicity_realized_pnl
                  << ", tox_unreal=" << r.toxicity_unrealized_pnl
                  << ", tox_adv=" << r.adverse_pnl
                  << ", base_real=" << r.baseline_realized_pnl
                  << ", base_unreal=" << r.baseline_unrealized_pnl
                  << ", base_adv=" << r.baseline_adverse_pnl
                  << ", tox_fills=" << r.toxicity_fills
                  << ", tox_buys=" << r.toxicity_buy_fills
                  << ", tox_sells=" << r.toxicity_sell_fills
                  << ", unwinds=" << r.toxicity_unwind_crosses
                  << ", unwind_cost=" << r.toxicity_unwind_cost
                  << ", eod=" << r.symbols_eod_liquidated
                  << ", blacklisted=" << r.symbols_blacklisted
                  << ", one_sided=" << r.symbols_one_sided
                  << ", w_fills=" << r.symbols_with_fills
                  << ", avg_inv=" << std::fixed << std::setprecision(1) << r.avg_final_abs_inventory
                  << '\n';
      }
    }

    if (g_config.walk_forward) {
      std::cout << "\n=== WALK-FORWARD ANALYSIS ===\n";
      std::cout << "Window size: " << g_config.wf_window_minutes << " minutes\n";
      std::cout << "Mode: learn in window N, apply frozen weights in window N+1\n";
      std::cout << "(Per-window detail in walk_forward_group_*.csv when --output-dir set)\n";
    }

    // Cleanup shared memory
    munmap(shared_results, shm_size);

    return 0;
  }

  // ==========================================================================
  // NON-HYBRID MODES (threaded or sequential)
  // ==========================================================================
  std::cout << "=== HFT Market Maker Simulation (" << mode_str << ") ===\n";
  std::cout << "PCAP files to process: " << pcap_files.size() << '\n';
  std::cout << "Parallel units: " << num_procs << '\n';
  if (!g_filter_ticker.empty()) {
    std::cout << "Filtering for ticker: " << g_filter_ticker << '\n';
  }
  std::cout << "Running baseline and toxicity-aware strategies...\n\n";

  (void)xdp::load_symbol_map(symbol_file);
  init_symbol_storage();

  if (g_use_parallel && pcap_files.size() > 1) {
    // =====================================================================
    // PARALLEL PROCESSING MODE
    // Process multiple files concurrently using thread pool
    // =====================================================================
    std::cout << "Starting parallel processing with " << num_procs << " threads...\n";

    xdp::ThreadPool pool(num_procs);
    std::mutex progress_mutex;
    std::vector<std::future<size_t>> futures;
    futures.reserve(pcap_files.size());

    // Submit all files to thread pool
    for (size_t file_idx = 0; file_idx < pcap_files.size(); file_idx++) {
      const std::string& pcap_file = pcap_files[file_idx];

      futures.push_back(pool.enqueue([&pcap_file, &progress_mutex,
                                       total_files = pcap_files.size()]() -> size_t {
        // Use memory-mapped reader for maximum throughput
        xdp::MmapPcapReader reader;
        if (!reader.open(pcap_file)) {
          std::lock_guard<std::mutex> lock(progress_mutex);
          std::cerr << "Warning: Error opening PCAP file " << pcap_file
                    << ": " << reader.error() << " - skipping\n";
          return 0;
        }

        // Pre-load file into memory
        reader.preload();

        size_t file_packets = reader.process_all(process_packet_callback);

        // Report progress
        size_t completed = ++g_files_completed;
        {
          std::lock_guard<std::mutex> lock(progress_mutex);
          size_t last_slash = pcap_file.find_last_of("/\\");
          std::string filename = (last_slash != std::string::npos)
                                     ? pcap_file.substr(last_slash + 1)
                                     : pcap_file;
          std::cout << "[" << completed << "/" << total_files << "] "
                    << filename << " - " << file_packets << " packets"
                    << " (total: " << g_total_packets.load() << " packets, "
                    << g_total_messages.load() << " msgs)\n" << std::flush;
        }

        return file_packets;
      }));
    }

    // Wait for all files to complete
    for (auto& f : futures) {
      f.wait();
    }

    std::cout << "\nAll files processed.\n";
  } else {
    // =====================================================================
    // SEQUENTIAL PROCESSING MODE (single file or --sequential flag)
    // =====================================================================
    std::cout << "Starting sequential processing...\n";

    for (size_t file_idx = 0; file_idx < pcap_files.size(); file_idx++) {
      const std::string& pcap_file = pcap_files[file_idx];

      // Use memory-mapped reader for faster I/O
      xdp::MmapPcapReader reader;
      if (!reader.open(pcap_file)) {
        std::cerr << "Warning: Error opening PCAP file " << pcap_file
                  << ": " << reader.error() << " - skipping\n";
        continue;
      }

      // Extract filename for progress display
      size_t last_slash = pcap_file.find_last_of("/\\");
      std::string filename = (last_slash != std::string::npos)
                                 ? pcap_file.substr(last_slash + 1)
                                 : pcap_file;

      std::cout << "[" << (file_idx + 1) << "/" << pcap_files.size() << "] "
                << filename << "..." << std::flush;

      // Pre-load for better performance
      reader.preload();

      uint64_t packets_before = g_total_packets.load();
      reader.process_all(process_packet_callback);
      uint64_t file_packets = g_total_packets.load() - packets_before;

      std::cout << " " << file_packets << " packets";
      report_memory_stats();
      std::cout << "\n";
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
  double seconds = duration.count() / 1000.0;
  double packets_per_sec = g_total_packets.load() / seconds;
  double msgs_per_sec = g_total_messages.load() / seconds;

  std::cout << "\n=== PERFORMANCE STATISTICS ===\n";
  std::cout << "Total processing time: " << std::fixed << std::setprecision(2)
            << seconds << " seconds\n";
  std::cout << "Total packets: " << g_total_packets.load() << '\n';
  std::cout << "Total messages: " << g_total_messages.load() << '\n';
  std::cout << "Throughput: " << std::fixed << std::setprecision(0)
            << packets_per_sec << " packets/sec, "
            << msgs_per_sec << " msgs/sec\n";
  std::cout << "Files processed: " << pcap_files.size() << '\n';

  print_results();

  cleanup_symbol_storage();

  return 0;
}
