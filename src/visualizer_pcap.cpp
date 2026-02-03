// visualizer_pcap.cpp - PCAP parser with ImGui visualization
#define GL_SILENCE_DEPRECATION
#include "thirdparty/imgui/backends/imgui_impl_opengl3.h"
#include "thirdparty/imgui/backends/imgui_impl_sdl2.h"
#include "thirdparty/imgui/imgui.h"
#include <SDL2/SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include "common/pcap_reader.hpp"
#include "common/symbol_map.hpp"
#include "common/xdp_types.hpp"
#include "common/xdp_utils.hpp"
#include "order_book.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <pcap.h>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Use functions from common modules
using xdp::read_le16;
using xdp::read_le32;
using xdp::read_le64;
using xdp::parse_price;
using xdp::get_symbol;

// Global order book and synchronization
OrderBook order_book;
std::atomic<bool> should_stop(false);
std::atomic<uint64_t> packets_processed(0);
std::atomic<uint64_t> messages_processed(0);
std::string filter_ticker = "";

// Message entry for live feed
struct MessageEntry {
  std::string text;
  bool is_buy;
  bool is_exec = false;
  double price;
  uint32_t volume;
  std::chrono::steady_clock::time_point timestamp;
};

// Trade execution marker for visualization
struct TradeMarker {
  double price;
  uint32_t volume;
  std::chrono::steady_clock::time_point timestamp;
};

// Order book update queue
enum class UpdateType { ADD, MODIFY, DELETE, EXECUTE, REPLACE };

struct OrderBookUpdate {
  UpdateType type;
  uint64_t order_id;
  uint64_t new_order_id; // For REPLACE
  double price;
  uint32_t volume;
  char side;
};

std::queue<OrderBookUpdate> update_queue;
std::mutex queue_mutex;
const size_t BATCH_SIZE =
    500; // Process updates in batches (increased for better throughput)

// Ring buffer for bounded playback storage
template <typename T> class RingBuffer {
public:
  explicit RingBuffer(size_t capacity) : capacity_(capacity), data_(capacity) {}

  void push_back(const T &item) {
    if (size_ < capacity_) {
      data_[write_pos_] = item;
      write_pos_ = (write_pos_ + 1) % capacity_;
      size_++;
    } else {
      // Overwrite oldest element
      data_[write_pos_] = item;
      write_pos_ = (write_pos_ + 1) % capacity_;
      read_pos_ = (read_pos_ + 1) % capacity_;
    }
  }

  [[nodiscard]] const T &operator[](size_t index) const {
    return data_[(read_pos_ + index) % capacity_];
  }

  [[nodiscard]] T &operator[](size_t index) {
    return data_[(read_pos_ + index) % capacity_];
  }

  [[nodiscard]] size_t size() const { return size_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }
  [[nodiscard]] size_t capacity() const { return capacity_; }

  void clear() {
    size_ = 0;
    read_pos_ = 0;
    write_pos_ = 0;
  }

  // Iterator support for range-based for loops
  class Iterator {
  public:
    Iterator(RingBuffer *buf, size_t pos) : buf_(buf), pos_(pos) {}
    T &operator*() { return (*buf_)[pos_]; }
    Iterator &operator++() {
      ++pos_;
      return *this;
    }
    bool operator!=(const Iterator &other) const { return pos_ != other.pos_; }

  private:
    RingBuffer *buf_;
    size_t pos_;
  };

  Iterator begin() { return Iterator(this, 0); }
  Iterator end() { return Iterator(this, size_); }

private:
  size_t capacity_;
  std::vector<T> data_;
  size_t read_pos_ = 0;
  size_t write_pos_ = 0;
  size_t size_ = 0;
};

// Checkpoint for fast seek operations
struct OrderBookCheckpoint {
  size_t update_index; // Index in playback buffer where this checkpoint was taken
  std::map<double, uint32_t, std::greater<double>> bids_snapshot;
  std::map<double, uint32_t, std::less<double>> asks_snapshot;
  std::unordered_map<uint64_t, Order> active_orders_snapshot;
};

// Playback storage with bounded capacity and checkpointing
constexpr size_t MAX_PLAYBACK_UPDATES = 500000; // ~500K updates max
constexpr size_t CHECKPOINT_INTERVAL = 10000;   // Checkpoint every 10K updates
RingBuffer<OrderBookUpdate> playback_buffer(MAX_PLAYBACK_UPDATES);
std::vector<OrderBookCheckpoint> checkpoints;
std::mutex playback_mutex;
size_t playback_index = 0;

// Forward declare visualizer for message feed
class OrderBookVisualizer;
OrderBookVisualizer *g_visualizer = nullptr;

// OrderBookVisualizer class
class OrderBookVisualizer {
private:
  OrderBook &order_book;
  SDL_Window *window;
  SDL_GLContext gl_context;
  bool auto_scale = true;
  bool lock_range = false;  // Lock the price range to prevent shifting
  float price_range = 5.0f;
  double locked_min_price = 0.0;
  double locked_max_price = 0.0;
  ImVec4 bid_color = ImVec4(0.0f, 1.0f, 0.0f, 0.5f);
  ImVec4 ask_color = ImVec4(1.0f, 0.0f, 0.0f, 0.5f);
  ImVec4 spread_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);

  // Message feed
  std::vector<MessageEntry> message_feed;
  std::mutex feed_mutex;
  size_t max_feed_entries = 200;
  bool auto_scroll_feed = true;

  // Playback controls
  bool is_playing = false;
  float playback_speed = 0.5f; // Baseline speed is now 0.5x (half speed)
  bool stream_finished = false;
  std::chrono::steady_clock::time_point last_playback_time;

  // Trade execution markers
  std::vector<TradeMarker> trade_markers;
  std::mutex markers_mutex;
  size_t max_markers = 50;             // Keep last 50 trades visible
  float marker_fade_time_ms = 2000.0f; // Fade out over 2 seconds

  // Toxicity over time tracking
  struct ToxicityTimePoint {
    std::chrono::steady_clock::time_point timestamp;
    double toxicity;
    double price;
    char side;
  };
  std::vector<ToxicityTimePoint> toxicity_history;
  std::mutex toxicity_history_mutex;
  size_t max_toxicity_history = 10000; // Keep last 10k points
  std::chrono::steady_clock::time_point start_time;
  bool start_time_set = false;
  std::chrono::steady_clock::time_point last_toxicity_sample;
  const int TOXICITY_SAMPLE_INTERVAL_MS =
      50; // Sample every 50ms for smooth graph

public:
  OrderBookVisualizer(OrderBook &ob) : order_book(ob), window(nullptr) {}

  bool init();
  void render();
  void cleanup();
  bool should_close();
  void render_controls();
  void render_order_book_graph();
  void render_toxicity_over_time();
  void render_message_feed();
  void add_message(const std::string &text, bool is_buy, double price,
                   uint32_t volume, bool is_exec = false);
  void add_trade_marker(double price, uint32_t volume);
  void record_toxicity_sample(double price, char side,
                              bool force_sample = false);
  void apply_playback_to_index(size_t idx);
  void set_stream_finished(bool finished) { stream_finished = finished; }
  void process_playback();
};

// Process XDP message and queue update (non-blocking)
void process_xdp_message(const uint8_t *data, size_t max_len,
                         uint16_t msg_type) {
  // Need at least 4 bytes for message header
  if (max_len < 4)
    return;

  uint32_t symbol_index = 0;
  std::string ticker;

  // Handle messages with non-standard header structure (106, 223)
  if (msg_type == 106 || msg_type == 223) {
    // Messages 106 and 223: SourceTime@4, SourceTimeNS@8, SymbolIndex@12
    if (max_len < 16)
      return;
    symbol_index = read_le32(data + 12);
    ticker = get_symbol(symbol_index);
  } else {
    // Standard messages: SourceTimeNS@4, SymbolIndex@8
    if (max_len < 12)
      return;
    symbol_index = read_le32(data + 8);
    ticker = get_symbol(symbol_index);
  }

  // Filter by ticker if specified
  if (!filter_ticker.empty()) {
    // Debug: Show sample symbols encountered (first 10 unique ones)
    static std::unordered_set<std::string> seen_symbols;
    static std::mutex seen_symbols_mutex;
    if (seen_symbols.size() < 10) {
      std::lock_guard<std::mutex> lock(seen_symbols_mutex);
      if (seen_symbols.find(ticker) == seen_symbols.end() &&
          seen_symbols.size() < 10) {
        seen_symbols.insert(ticker);
        if (seen_symbols.size() <= 5) {
          std::cout << "Sample symbol encountered: " << ticker
                    << " (index: " << symbol_index << ")" << std::endl;
        }
      }
    }

    if (ticker != filter_ticker) {
      return; // Skip this message - doesn't match filter
    }
    // Matched filter - process this message
    messages_processed++;
  } else {
    // No filter - process all messages
    messages_processed++;
  }

  // Queue update instead of applying immediately
  OrderBookUpdate update;

  switch (msg_type) {
  case 100: { // Add Order
    if (max_len >= 39) {
      update.type = UpdateType::ADD;
      update.order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 24);
      update.volume = read_le32(data + 28);
      update.side = data[32];
      update.price = parse_price(price_raw);

      // Add to message feed
      if (g_visualizer) {
        char msg[256];
        snprintf(msg, sizeof(msg), "ADD %s $%.2f x %u",
                 update.side == 'B' ? "BUY" : "SELL", update.price,
                 update.volume);
        g_visualizer->add_message(msg, update.side == 'B', update.price,
                                  update.volume);
      }

      std::lock_guard<std::mutex> lock(queue_mutex);
      update_queue.push(update);
    }
    break;
  }

  case 101: { // Modify Order
    if (max_len >= 35) {
      update.type = UpdateType::MODIFY;
      update.order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 24);
      update.volume = read_le32(data + 28);
      update.price = parse_price(price_raw);

      std::lock_guard<std::mutex> lock(queue_mutex);
      update_queue.push(update);
    }
    break;
  }

  case 102: { // Delete Order
    if (max_len >= 25) {
      update.type = UpdateType::DELETE;
      update.order_id = read_le64(data + 16);

      std::lock_guard<std::mutex> lock(queue_mutex);
      update_queue.push(update);
    }
    break;
  }

  case 103: { // Execute Order
    if (max_len >= 42) {
      update.type = UpdateType::EXECUTE;
      update.order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 28);
      update.volume = read_le32(data + 32);
      update.price = parse_price(price_raw);

      // Add to message feed (executions are important)
      if (g_visualizer) {
        char msg[256];
        snprintf(msg, sizeof(msg), "EXEC $%.2f x %u", update.price,
                 update.volume);
        // Mark execution messages specially (blue)
        g_visualizer->add_message(msg, true, update.price, update.volume, true);
        // Add visual trade marker
        g_visualizer->add_trade_marker(update.price, update.volume);
      }

      std::lock_guard<std::mutex> lock(queue_mutex);
      update_queue.push(update);
    }
    break;
  }

  case 104: { // Replace Order
    if (max_len >= 42) {
      update.type = UpdateType::REPLACE;
      update.order_id = read_le64(data + 16);
      update.new_order_id = read_le64(data + 24);
      uint32_t price_raw = read_le32(data + 32);
      update.volume = read_le32(data + 36);
      update.price = parse_price(price_raw);
      update.side = 'B'; // Default, would need to track from old order

      std::lock_guard<std::mutex> lock(queue_mutex);
      update_queue.push(update);
    }
    break;
  }
  }
}

// Create a checkpoint of current order book state
void create_checkpoint(size_t update_index) {
  auto snapshot = order_book.get_atomic_snapshot();

  OrderBookCheckpoint checkpoint;
  checkpoint.update_index = update_index;
  checkpoint.bids_snapshot = snapshot.bids;
  checkpoint.asks_snapshot = snapshot.asks;
  checkpoint.active_orders_snapshot = snapshot.active_orders;

  std::lock_guard<std::mutex> lock(playback_mutex);
  checkpoints.push_back(std::move(checkpoint));
}

// Apply batched updates to order book (optimized for high throughput)
void apply_batched_updates() {
  std::vector<OrderBookUpdate> batch;
  batch.reserve(BATCH_SIZE); // Pre-allocate for better performance

  // Collect a batch of updates (quick lock/unlock)
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    size_t count = std::min(update_queue.size(), BATCH_SIZE);
    batch.reserve(count);
    for (size_t i = 0; i < count; i++) {
      if (!update_queue.empty()) {
        batch.push_back(update_queue.front());
        update_queue.pop();
      }
    }
  }

  // Apply all updates in batch (OrderBook methods are thread-safe with internal
  // mutex)
  if (!batch.empty()) {
    for (const auto &update : batch) {
      size_t current_index = 0;
      // Store update in ring buffer for playback
      {
        std::lock_guard<std::mutex> lock(playback_mutex);
        playback_buffer.push_back(update);
        current_index = playback_buffer.size();

        // Create checkpoint at regular intervals
        if (current_index > 0 && current_index % CHECKPOINT_INTERVAL == 0) {
          // Release lock before creating checkpoint (checkpoint acquires its own lock)
        }
      }

      // Create checkpoint outside of playback_mutex to avoid deadlock
      if (current_index > 0 && current_index % CHECKPOINT_INTERVAL == 0) {
        create_checkpoint(current_index);
      }

      // Apply update
      switch (update.type) {
      case UpdateType::ADD:
        order_book.add_order(update.order_id, update.price, update.volume,
                             update.side);
        // Record toxicity sample
        if (g_visualizer) {
          g_visualizer->record_toxicity_sample(update.price, update.side);
        }
        break;
      case UpdateType::MODIFY:
        order_book.modify_order(update.order_id, update.price, update.volume);
        break;
      case UpdateType::DELETE:
        order_book.delete_order(update.order_id);
        // Record toxicity sample when order is cancelled
        if (g_visualizer) {
          g_visualizer->record_toxicity_sample(update.price, update.side);
        }
        break;
      case UpdateType::EXECUTE:
        order_book.execute_order(update.order_id, update.volume, update.price);
        break;
      case UpdateType::REPLACE:
        order_book.delete_order(update.order_id);
        order_book.add_order(update.new_order_id, update.price, update.volume,
                             update.side);
        // Record toxicity sample
        if (g_visualizer) {
          g_visualizer->record_toxicity_sample(update.price, update.side);
        }
        break;
      }
    }
  }
}

// Parse XDP packet
void parse_xdp_packet(const uint8_t *data, size_t length) {
  if (length < 16)
    return;

  uint8_t num_messages = data[3];
  size_t offset = 16; // Skip packet header

  // Debug: Check if we're getting packets
  extern std::atomic<uint64_t> packets_parsed;
  extern std::atomic<uint64_t> messages_parsed;
  packets_parsed++;

  for (uint8_t i = 0; i < num_messages && offset < length; i++) {
    if (offset + 2 > length)
      break;

    uint16_t msg_size = read_le16(data + offset);
    if (msg_size < 2 || offset + msg_size > length)
      break;

    uint16_t msg_type = read_le16(data + offset + 2);
    messages_parsed++;
    process_xdp_message(data + offset, msg_size, msg_type);

    offset += msg_size;
  }

  // Debug output every 10000 packets
  if (packets_parsed.load() % 10000 == 0) {
    std::cout << "Debug: Parsed " << packets_parsed.load() << " XDP packets, "
              << messages_parsed.load() << " messages, "
              << messages_processed.load() << " matched filter" << std::endl;
  }
}

// PCAP packet handler
void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr,
                    const u_char *packet) {
  (void)user_data;
  packets_processed++;

  if (should_stop.load()) {
    return;
  }

  if (pkthdr->caplen < 14)
    return;

  // Parse Ethernet header
  uint16_t eth_type = ntohs(*(uint16_t *)(packet + 12));
  size_t eth_header_len = 14;

  // Skip VLAN tag if present
  if (eth_type == 0x8100 || eth_type == 0x88A8) {
    if (pkthdr->caplen < 18)
      return;
    eth_type = ntohs(*(uint16_t *)(packet + 16));
    eth_header_len = 18;
  }

  // Only process IPv4
  if (eth_type != 0x0800)
    return;

  // Parse IP header
  if (pkthdr->caplen < eth_header_len + 20)
    return;
  const uint8_t *ip_header = packet + eth_header_len;
  uint8_t ip_ver_ihl = ip_header[0];
  uint8_t ip_header_len = (ip_ver_ihl & 0x0F) * 4;
  uint8_t protocol = ip_header[9];

  // Check for UDP
  if (protocol != 17)
    return;

  // Parse UDP header
  size_t udp_offset = eth_header_len + ip_header_len;
  if (pkthdr->caplen < udp_offset + 8)
    return;

  const uint8_t *udp_header = packet + udp_offset;
  uint16_t udp_len = ntohs(*(uint16_t *)(udp_header + 4));

  // Extract UDP payload (XDP data)
  const uint8_t *udp_payload = udp_header + 8;
  size_t payload_len = udp_len - 8;

  if (payload_len > pkthdr->caplen - udp_offset - 8) {
    payload_len = pkthdr->caplen - udp_offset - 8;
  }

  if (payload_len < 16)
    return;

  parse_xdp_packet(udp_payload, payload_len);
}

// Debug counters (declared here, used in parse_xdp_packet)
std::atomic<uint64_t> packets_parsed(0);
std::atomic<uint64_t> messages_parsed(0);

// PCAP reading thread function
void pcap_thread_func(const std::string &pcap_file) {
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t *handle = pcap_open_offline(pcap_file.c_str(), errbuf);

  if (!handle) {
    std::cerr << "Error opening PCAP file: " << errbuf << std::endl;
    should_stop.store(true);
    return;
  }

  std::cout << "Reading PCAP file: " << pcap_file << std::endl;

  // Use pcap_loop for better performance (it's optimized internally)
  // Process in batches to allow UI updates
  pcap_loop(handle, -1, packet_handler, nullptr);

  pcap_close(handle);

  std::cout << "Finished reading PCAP file. Processed "
            << packets_processed.load() << " packets, "
            << messages_processed.load() << " messages matched filter"
            << std::endl;

  // Print debug stats
  std::cout << "Debug: Parsed " << packets_parsed.load() << " XDP packets, "
            << messages_parsed.load() << " total messages" << std::endl;

  should_stop.store(true);
  if (g_visualizer) {
    g_visualizer->set_stream_finished(true);
  }
}

// OrderBookVisualizer implementation
bool OrderBookVisualizer::init() {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
    return false;
  }

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

  window = SDL_CreateWindow("Order Book Visualizer", SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, 1600, 900,
                            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);

  if (!window) {
    std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
    return false;
  }

  gl_context = SDL_GL_CreateContext(window);
  if (!gl_context) {
    std::cerr << "OpenGL context creation failed: " << SDL_GetError()
              << std::endl;
    return false;
  }

  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1);

  const char *glsl_version = "#version 150";
#ifdef __APPLE__
  glsl_version = "#version 150";
#endif

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
  if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
    std::cerr << "Failed to initialize ImGui OpenGL3 renderer" << std::endl;
    return false;
  }

  return true;
}

void OrderBookVisualizer::render() {
  ImGuiIO &io = ImGui::GetIO();
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplSDL2_NewFrame();
  ImGui::NewFrame();

  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("Order Book", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);

  // Left panel: Message feed
  ImGui::BeginChild("MessageFeed", ImVec2(300, 0), true);
  render_message_feed();
  ImGui::EndChild();

  ImGui::SameLine();

  // Right panel: Controls and graph
  ImGui::BeginChild("MainPanel", ImVec2(0, 0), true);

  // Controls at top
  ImGui::BeginChild("Controls", ImVec2(0, 100), true);
  render_controls();
  ImGui::EndChild();

  // Order book graph (takes 70% of remaining space)
  ImGui::BeginChild("OrderBookGraph",
                    ImVec2(0, -ImGui::GetContentRegionAvail().y * 0.3f), true);
  render_order_book_graph();
  ImGui::EndChild();

  // Toxicity over time graph (bottom 30%)
  ImGui::BeginChild("ToxicityGraph", ImVec2(0, 0), true);
  render_toxicity_over_time();
  ImGui::EndChild();

  // Periodically sample overall toxicity across all price levels
  static auto last_overall_sample = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - last_overall_sample)
                     .count();

  if (elapsed >= 100) { // Sample every 100ms for overall view
    // Sample average toxicity across all active price levels
    auto bids = order_book.get_bids();
    auto asks = order_book.get_asks();

    if (!bids.empty() || !asks.empty()) {
      // Sample a few representative price levels
      int samples_taken = 0;
      for (const auto &pair : bids) {
        if (samples_taken++ >= 5)
          break; // Sample top 5 bid levels
        record_toxicity_sample(pair.first, 'B');
      }
      samples_taken = 0;
      for (const auto &pair : asks) {
        if (samples_taken++ >= 5)
          break; // Sample top 5 ask levels
        record_toxicity_sample(pair.first, 'S');
      }
    }
    last_overall_sample = now;
  }

  ImGui::EndChild();

  ImGui::End();

  ImGui::Render();
  glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
  glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  SDL_GL_SwapWindow(window);
}

void OrderBookVisualizer::process_playback() {
  if (!is_playing || !stream_finished)
    return;

  size_t buffer_size = 0;
  {
    std::lock_guard<std::mutex> lock(playback_mutex);
    buffer_size = playback_buffer.size();
  }

  if (buffer_size == 0 || playback_index >= buffer_size) {
    is_playing = false; // Reached end
    playback_index = 0; // Reset for next play
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - last_playback_time)
                     .count();

  // Calculate delay based on playback speed (default: 1 update per 20ms at 0.5x
  // speed) At 0.5x speed, delay is 20ms, so at 1x it's 10ms
  int delay_ms = (int)(20.0f / playback_speed);

  if (elapsed >= delay_ms) {
    // Get the update to apply
    OrderBookUpdate update;
    {
      std::lock_guard<std::mutex> lock(playback_mutex);
      if (playback_index >= playback_buffer.size())
        return;
      update = playback_buffer[playback_index];
    }

    // Apply update
    switch (update.type) {
    case UpdateType::ADD:
      order_book.add_order(update.order_id, update.price, update.volume,
                           update.side);
      // Record toxicity sample during playback
      record_toxicity_sample(update.price, update.side);
      // Add to message feed
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "ADD %s $%.2f x %u",
                 update.side == 'B' ? "BUY" : "SELL", update.price,
                 update.volume);
        add_message(msg, update.side == 'B', update.price, update.volume);
      }
      break;
    case UpdateType::MODIFY:
      order_book.modify_order(update.order_id, update.price, update.volume);
      break;
    case UpdateType::DELETE:
      order_book.delete_order(update.order_id);
      // Record toxicity sample during playback
      record_toxicity_sample(update.price, update.side);
      break;
    case UpdateType::EXECUTE:
      order_book.execute_order(update.order_id, update.volume, update.price);
      // Add to message feed
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "EXEC $%.2f x %u", update.price,
                 update.volume);
        add_message(msg, true, update.price, update.volume, true);
      }
      // Add visual trade marker
      add_trade_marker(update.price, update.volume);
      break;
    case UpdateType::REPLACE:
      order_book.delete_order(update.order_id);
      order_book.add_order(update.new_order_id, update.price, update.volume,
                           update.side);
      // Record toxicity sample during playback
      record_toxicity_sample(update.price, update.side);
      break;
    }

    {
      std::lock_guard<std::mutex> lock(playback_mutex);
      playback_index++;
    }
    last_playback_time = now;
  }
}

void OrderBookVisualizer::apply_playback_to_index(size_t idx) {
  // Find the nearest checkpoint before the target index for fast seeking
  size_t start_from = 0;
  const OrderBookCheckpoint *nearest_checkpoint = nullptr;

  {
    std::lock_guard<std::mutex> lock(playback_mutex);
    if (idx > playback_buffer.size())
      idx = playback_buffer.size();

    // Find nearest checkpoint before target index
    for (auto it = checkpoints.rbegin(); it != checkpoints.rend(); ++it) {
      if (it->update_index <= idx) {
        nearest_checkpoint = &(*it);
        start_from = it->update_index;
        break;
      }
    }
  }

  // Clear UI state
  {
    std::lock_guard<std::mutex> lock(feed_mutex);
    message_feed.clear();
  }
  {
    std::lock_guard<std::mutex> lock(markers_mutex);
    trade_markers.clear();
  }
  {
    std::lock_guard<std::mutex> lock(toxicity_history_mutex);
    toxicity_history.clear();
    start_time_set = false;
  }

  // Restore from checkpoint or clear order book
  if (nearest_checkpoint) {
    order_book.restore_from_snapshot(nearest_checkpoint->bids_snapshot,
                                     nearest_checkpoint->asks_snapshot,
                                     nearest_checkpoint->active_orders_snapshot);
  } else {
    order_book.clear();
    start_from = 0;
  }

  // Collect updates to replay from checkpoint to target
  std::vector<OrderBookUpdate> updates_to_replay;
  {
    std::lock_guard<std::mutex> lock(playback_mutex);
    updates_to_replay.reserve(idx - start_from);
    for (size_t i = start_from; i < idx; ++i) {
      updates_to_replay.push_back(playback_buffer[i]);
    }
  }

  // Reset start time for toxicity tracking
  auto replay_start_time = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(toxicity_history_mutex);
    start_time = replay_start_time;
    start_time_set = true;
    last_toxicity_sample = replay_start_time;
  }

  // Replay updates from checkpoint to target (much faster than from beginning)
  int sample_count = 0;
  for (const auto &update : updates_to_replay) {
    switch (update.type) {
    case UpdateType::ADD:
      order_book.add_order(update.order_id, update.price, update.volume,
                           update.side);
      if (sample_count % 10 == 0) { // Sample every 10th update during replay
        record_toxicity_sample(update.price, update.side, true);
        {
          std::lock_guard<std::mutex> lock(toxicity_history_mutex);
          last_toxicity_sample +=
              std::chrono::milliseconds(TOXICITY_SAMPLE_INTERVAL_MS);
        }
      }
      sample_count++;
      break;
    case UpdateType::MODIFY:
      order_book.modify_order(update.order_id, update.price, update.volume);
      break;
    case UpdateType::DELETE:
      order_book.delete_order(update.order_id);
      if (sample_count % 10 == 0) {
        record_toxicity_sample(update.price, update.side, true);
        {
          std::lock_guard<std::mutex> lock(toxicity_history_mutex);
          last_toxicity_sample +=
              std::chrono::milliseconds(TOXICITY_SAMPLE_INTERVAL_MS);
        }
      }
      sample_count++;
      break;
    case UpdateType::EXECUTE:
      order_book.execute_order(update.order_id, update.volume, update.price);
      break;
    case UpdateType::REPLACE:
      order_book.delete_order(update.order_id);
      order_book.add_order(update.new_order_id, update.price, update.volume,
                           update.side);
      if (sample_count % 10 == 0) {
        record_toxicity_sample(update.price, update.side, true);
        {
          std::lock_guard<std::mutex> lock(toxicity_history_mutex);
          last_toxicity_sample +=
              std::chrono::milliseconds(TOXICITY_SAMPLE_INTERVAL_MS);
        }
      }
      sample_count++;
      break;
    }
  }

  // Update playback index
  {
    std::lock_guard<std::mutex> lock(playback_mutex);
    playback_index = idx;
  }
}

void OrderBookVisualizer::render_controls() {
  ImGui::Text("Order Book Controls");
  ImGui::SameLine();
  ImGui::Checkbox("Auto Scale", &auto_scale);
  if (!auto_scale) {
    ImGui::SameLine();
    ImGui::SliderFloat("Price Range", &price_range, 0.1f, 20.0f);
  }
  ImGui::SameLine();
  if (ImGui::Checkbox("Lock Range", &lock_range)) {
    if (lock_range) {
      // Capture current price range when locking
      auto stats = order_book.get_stats();
      double mid_price = stats.mid_price > 0 ? stats.mid_price :
        (stats.best_bid > 0 && stats.best_ask > 0 ? (stats.best_bid + stats.best_ask) / 2.0 :
         (stats.best_bid > 0 ? stats.best_bid : stats.best_ask));

      if (auto_scale) {
        double price_span = 0.0;
        if (stats.best_bid > 0 && stats.best_ask > 0) {
          double spread = stats.best_ask - stats.best_bid;
          price_span = std::max(spread * 10.0, mid_price * 0.02);
        } else {
          price_span = mid_price * 0.05;
        }
        locked_min_price = mid_price - price_span;
        locked_max_price = mid_price + price_span;
      } else {
        locked_min_price = mid_price - price_range;
        locked_max_price = mid_price + price_range;
      }
    }
  }

  ImGui::SameLine();
  if (stream_finished) {
    if (ImGui::Button(is_playing ? "Pause" : "Play")) {
      is_playing = !is_playing;
      if (is_playing) {
        last_playback_time = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(playback_mutex);
        if (playback_index >= playback_buffer.size()) {
          playback_index = 0; // Reset to beginning
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
      is_playing = false;
      playback_speed = 0.5f; // Reset to half speed

      // Clear order book
      order_book.clear();

      // Clear toxicity history
      {
        std::lock_guard<std::mutex> lock(toxicity_history_mutex);
        toxicity_history.clear();
        start_time_set = false;
      }

      // Clear message feed
      {
        std::lock_guard<std::mutex> lock(feed_mutex);
        message_feed.clear();
      }

      // Clear trade markers
      {
        std::lock_guard<std::mutex> lock(markers_mutex);
        trade_markers.clear();
      }

      // Reset playback to beginning
      {
        std::lock_guard<std::mutex> lock(playback_mutex);
        playback_index = 0;
      }

      // Start playback automatically
      is_playing = true;
      last_playback_time = std::chrono::steady_clock::now();
    }

    // Timeline slider / seek - only apply seek on mouse release
    size_t playback_size = 0;
    {
      std::lock_guard<std::mutex> lock(playback_mutex);
      playback_size = playback_buffer.size();
    }

    if (playback_size > 0) {
      ImGui::SameLine();
      static int seek_idx = 0;
      static int prev_seek_idx = 0;
      int max_idx = (int)playback_size;

      // Update current position if playing
      if (is_playing) {
        std::lock_guard<std::mutex> lock(playback_mutex);
        seek_idx = (int)playback_index;
        prev_seek_idx = seek_idx;
      }

      if (ImGui::SliderInt("Position", &seek_idx, 0, max_idx)) {
        // Clamp
        if (seek_idx < 0)
          seek_idx = 0;
        if (seek_idx > max_idx)
          seek_idx = max_idx;

        // Only apply seek if mouse was released (slider changed and no longer active)
        if (!ImGui::IsItemActive() && seek_idx != prev_seek_idx) {
          apply_playback_to_index((size_t)seek_idx);
          prev_seek_idx = seek_idx;
        }
      }
    }

    if (is_playing) {
      ImGui::SameLine();
      ImGui::SliderFloat("Speed", &playback_speed, 0.1f, 5.0f);
      std::lock_guard<std::mutex> lock(playback_mutex);
      ImGui::SameLine();
      ImGui::Text("(%zu/%zu)", playback_index, playback_buffer.size());
    }
  } else {
    ImGui::Text("Streaming...");
  }

  ImGui::Separator();

  // Statistics
  auto stats = order_book.get_stats();
  ImGui::Text("Best Bid: $%.4f | Best Ask: $%.4f | Spread: $%.4f | Mid: $%.4f",
              stats.best_bid, stats.best_ask, stats.spread, stats.mid_price);
  ImGui::Text("Bid Qty: %u | Ask Qty: %u | Levels: %d/%d", stats.total_bid_qty,
              stats.total_ask_qty, stats.bid_levels, stats.ask_levels);
  ImGui::Text("Packets: %llu | Messages: %llu",
              (unsigned long long)packets_processed.load(),
              (unsigned long long)messages_processed.load());
}

void OrderBookVisualizer::render_message_feed() {
  ImGui::Text("Live Message Feed");
  ImGui::Separator();

  ImGui::Checkbox("Auto-scroll", &auto_scroll_feed);
  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    std::lock_guard<std::mutex> lock(feed_mutex);
    message_feed.clear();
  }

  ImGui::Separator();

  // Message list
  ImGui::BeginChild("FeedList", ImVec2(0, 0), true);

  std::lock_guard<std::mutex> lock(feed_mutex);
  for (const auto &entry : message_feed) {
    ImVec4 color;
    if (entry.is_exec) {
      color = ImVec4(0.2f, 0.6f, 1.0f, 1.0f); // Blue for executions
    } else {
      color = entry.is_buy ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f)
                           : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", entry.text.c_str());
    ImGui::PopStyleColor();
  }

  if (auto_scroll_feed && !message_feed.empty()) {
    ImGui::SetScrollHereY(1.0f);
  }

  ImGui::EndChild();
}

void OrderBookVisualizer::add_message(const std::string &text, bool is_buy,
                                      double price, uint32_t volume,
                                      bool is_exec) {
  std::lock_guard<std::mutex> lock(feed_mutex);
  MessageEntry entry;
  entry.text = text;
  entry.is_buy = is_buy;
  entry.is_exec = is_exec;
  entry.price = price;
  entry.volume = volume;
  entry.timestamp = std::chrono::steady_clock::now();

  message_feed.push_back(entry);

  // Limit feed size
  if (message_feed.size() > max_feed_entries) {
    message_feed.erase(message_feed.begin());
  }
}

void OrderBookVisualizer::add_trade_marker(double price, uint32_t volume) {
  std::lock_guard<std::mutex> lock(markers_mutex);
  TradeMarker marker;
  marker.price = price;
  marker.volume = volume;
  marker.timestamp = std::chrono::steady_clock::now();

  trade_markers.push_back(marker);

  // Limit marker count
  if (trade_markers.size() > max_markers) {
    trade_markers.erase(trade_markers.begin());
  }
}

void OrderBookVisualizer::render_order_book_graph() {
  // Get snapshots of the data (thread-safe copies)
  auto stats = order_book.get_stats();
  auto bids = order_book.get_bids();
  auto asks = order_book.get_asks();

  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  ImVec2 graph_pos = ImGui::GetCursorScreenPos();
  ImVec2 graph_size = ImGui::GetContentRegionAvail();

  // Handle empty order book
  if (stats.best_bid == 0.0 && stats.best_ask == 0.0) {
    ImGui::Text("No order book data available");
    ImGui::Text("Waiting for PCAP data...");
    return;
  }

  // Apply zoom and pan transformations
  // The graph will be scaled and panned based on zoom_level, pan_x, pan_y

  // Calculate proper price range
  double min_price, max_price;
  double best_bid = stats.best_bid;
  double best_ask = stats.best_ask;
  double mid_price = stats.mid_price > 0
                         ? stats.mid_price
                         : (best_bid > 0 && best_ask > 0
                                ? (best_bid + best_ask) / 2.0
                                : (best_bid > 0 ? best_bid : best_ask));

  if (lock_range && locked_min_price != 0.0 && locked_max_price != 0.0) {
    // Use locked price range - prevents window from shifting
    min_price = locked_min_price;
    max_price = locked_max_price;
  } else if (auto_scale) {
    double price_span = 0.0;
    if (best_bid > 0 && best_ask > 0) {
      double spread = best_ask - best_bid;
      price_span = std::max(spread * 10.0, mid_price * 0.02);
    } else {
      price_span = mid_price * 0.05;
    }
    min_price = mid_price - price_span;
    max_price = mid_price + price_span;
  } else {
    min_price = mid_price - price_range;
    max_price = mid_price + price_range;
  }

  if (max_price <= min_price) {
    max_price = min_price + price_range * 2;
  }

  // Find max quantity for scaling
  uint32_t max_qty = 0;
  for (const auto &pair : bids) {
    if (pair.first >= min_price && pair.first <= max_price) {
      max_qty = std::max(max_qty, pair.second);
    }
  }
  for (const auto &pair : asks) {
    if (pair.first >= min_price && pair.first <= max_price) {
      max_qty = std::max(max_qty, pair.second);
    }
  }
  if (max_qty == 0)
    max_qty = 1000;

  // Add padding (fixed window, no zoom/pan)
  float padding = 40.0f;
  ImVec2 plot_pos(graph_pos.x + padding, graph_pos.y + padding);
  ImVec2 plot_size(graph_size.x - padding * 2, graph_size.y - padding * 2 - 60);

  // Draw axes
  // X-axis (price) at bottom
  draw_list->AddLine(ImVec2(plot_pos.x, plot_pos.y + plot_size.y),
                     ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y),
                     IM_COL32(255, 255, 255, 255), 2.0f);

  // Y-axis (quantity) on left
  draw_list->AddLine(ImVec2(plot_pos.x, plot_pos.y),
                     ImVec2(plot_pos.x, plot_pos.y + plot_size.y),
                     IM_COL32(255, 255, 255, 255), 2.0f);

  // Calculate spread boundaries
  float bid_x = 0.0f, ask_x = 0.0f;
  if (best_bid > 0 && best_ask > 0) {
    bid_x =
        plot_pos.x +
        (float)((best_bid - min_price) / (max_price - min_price)) * plot_size.x;
    ask_x =
        plot_pos.x +
        (float)((best_ask - min_price) / (max_price - min_price)) * plot_size.x;
  } else {
    bid_x = plot_pos.x + plot_size.x * 0.4f;
    ask_x = plot_pos.x + plot_size.x * 0.6f;
  }

  // Draw simplified bid/ask lines with toxicity coloring
  // Build sorted point lists with toxicity data and detailed metrics
  struct PointWithToxicity {
    ImVec2 pos;
    double toxicity;
    double price;
    OrderBook::ToxicityMetrics metrics;
  };

  std::vector<PointWithToxicity> bid_points_with_toxicity;
  for (const auto &pair : bids) {
    double price = pair.first;
    uint32_t qty = pair.second;
    if (price < min_price || price > best_bid)
      continue;
    float x =
        plot_pos.x +
        (float)((price - min_price) / (max_price - min_price)) * plot_size.x;
    float y =
        plot_pos.y + plot_size.y - (float)(qty / (double)max_qty) * plot_size.y;
    double toxicity = order_book.get_toxicity(price, 'B');
    OrderBook::ToxicityMetrics metrics =
        order_book.get_toxicity_metrics(price, 'B');
    bid_points_with_toxicity.push_back(
        {ImVec2(x, y), toxicity, price, metrics});
  }

  // Sort by x ascending so polyline goes left->center without crossing
  std::sort(bid_points_with_toxicity.begin(), bid_points_with_toxicity.end(),
            [](const PointWithToxicity &a, const PointWithToxicity &b) {
              return a.pos.x < b.pos.x;
            });

  // Build final bid polyline starting at left baseline
  std::vector<ImVec2> bid_line_points;
  bid_line_points.emplace_back(plot_pos.x, plot_pos.y + plot_size.y);
  for (const auto &p : bid_points_with_toxicity)
    bid_line_points.push_back(p.pos);
  // Ensure endpoint at best bid x (bottom)
  bid_line_points.emplace_back(bid_x, plot_pos.y + plot_size.y);

  // Draw bid lines with toxicity-based color intensity and tooltips
  if (bid_line_points.size() >= 2) {
    for (size_t i = 0; i + 1 < bid_line_points.size(); ++i) {
      // Get toxicity for this segment
      double toxicity = 0.0;
      PointWithToxicity *point_data = nullptr;
      if (i > 0 && i - 1 < bid_points_with_toxicity.size()) {
        point_data = &bid_points_with_toxicity[i - 1];
        toxicity = point_data->toxicity;
      }

      // Color: Green (0,255,0) for low toxicity, Yellow (255,255,0) for medium,
      // Red (255,0,0) for high Interpolate based on toxicity (0.0 = green, 1.0
      // = red)
      uint8_t r = (uint8_t)(toxicity * 255);
      uint8_t g = (uint8_t)((1.0 - toxicity) * 255);
      uint8_t b = 0;

      draw_list->AddLine(bid_line_points[i], bid_line_points[i + 1],
                         IM_COL32(r, g, b, 255),
                         3.0f); // Thicker line for visibility

      // Add hover tooltip with detailed toxicity explanation
      if (point_data && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Price: $%.2f", point_data->price);
        ImGui::Text("Toxicity: %.2f", point_data->toxicity);
        ImGui::Separator();
        ImGui::Text("%s", point_data->metrics.get_explanation().c_str());
        ImGui::EndTooltip();
      }
    }
  }

  // Ask side: collect and sort by x descending so polyline goes right->center
  std::vector<PointWithToxicity> ask_points_with_toxicity;
  for (const auto &pair : asks) {
    double price = pair.first;
    uint32_t qty = pair.second;
    if (price < best_ask || price > max_price)
      continue;
    float x =
        plot_pos.x +
        (float)((price - min_price) / (max_price - min_price)) * plot_size.x;
    float y =
        plot_pos.y + plot_size.y - (float)(qty / (double)max_qty) * plot_size.y;
    double toxicity = order_book.get_toxicity(price, 'S');
    OrderBook::ToxicityMetrics metrics =
        order_book.get_toxicity_metrics(price, 'S');
    ask_points_with_toxicity.push_back(
        {ImVec2(x, y), toxicity, price, metrics});
  }

  std::sort(ask_points_with_toxicity.begin(), ask_points_with_toxicity.end(),
            [](const PointWithToxicity &a, const PointWithToxicity &b) {
              return a.pos.x > b.pos.x;
            });

  std::vector<ImVec2> ask_line_points;
  ask_line_points.emplace_back(plot_pos.x + plot_size.x,
                               plot_pos.y + plot_size.y);
  for (const auto &p : ask_points_with_toxicity)
    ask_line_points.push_back(p.pos);
  ask_line_points.emplace_back(ask_x, plot_pos.y + plot_size.y);

  // Draw ask lines with toxicity-based color intensity and tooltips
  if (ask_line_points.size() >= 2) {
    for (size_t i = 0; i + 1 < ask_line_points.size(); ++i) {
      // Get toxicity for this segment
      double toxicity = 0.0;
      PointWithToxicity *point_data = nullptr;
      if (i > 0 && i - 1 < ask_points_with_toxicity.size()) {
        point_data = &ask_points_with_toxicity[i - 1];
        toxicity = point_data->toxicity;
      }

      // Color: Red (255,0,0) for low toxicity, Yellow (255,255,0) for medium,
      // Bright Red (255,100,100) for high Interpolate based on toxicity (0.0 =
      // dark red, 1.0 = bright red/yellow)
      uint8_t r = 255;
      uint8_t g = (uint8_t)((1.0 - toxicity) * 100);
      uint8_t b = (uint8_t)((1.0 - toxicity) * 100);

      draw_list->AddLine(ask_line_points[i], ask_line_points[i + 1],
                         IM_COL32(r, g, b, 255),
                         3.0f); // Thicker line for visibility

      // Add hover tooltip with detailed toxicity explanation
      if (point_data && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Price: $%.2f", point_data->price);
        ImGui::Text("Toxicity: %.2f", point_data->toxicity);
        ImGui::Separator();
        ImGui::Text("%s", point_data->metrics.get_explanation().c_str());
        ImGui::EndTooltip();
      }
    }
  }

  // Draw opaque vertical lines to show spread
  draw_list->AddLine(ImVec2(bid_x, plot_pos.y),
                     ImVec2(bid_x, plot_pos.y + plot_size.y),
                     IM_COL32(255, 255, 0, 255), 3.0f);
  draw_list->AddLine(ImVec2(ask_x, plot_pos.y),
                     ImVec2(ask_x, plot_pos.y + plot_size.y),
                     IM_COL32(255, 255, 0, 255), 3.0f);

  // Draw trade execution markers
  {
    std::lock_guard<std::mutex> lock(markers_mutex);
    auto now = std::chrono::steady_clock::now();

    // Remove old markers and draw visible ones
    auto it = trade_markers.begin();
    while (it != trade_markers.end()) {
      auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->timestamp)
                        .count();

      if (age_ms > marker_fade_time_ms) {
        // Remove old markers
        it = trade_markers.erase(it);
        continue;
      }

      // Calculate marker position
      double trade_price = it->price;
      if (trade_price >= min_price && trade_price <= max_price) {
        float x = plot_pos.x +
                  (float)((trade_price - min_price) / (max_price - min_price)) *
                      plot_size.x;
        float y = plot_pos.y + plot_size.y; // At the bottom (x-axis)

        // Calculate alpha based on age (fade out)
        float age_ratio = (float)age_ms / marker_fade_time_ms;
        int alpha = (int)(255 * (1.0f - age_ratio));
        alpha = std::max(0, std::min(255, alpha));

        // Draw a bright circle/point at the trade price
        // Use bright yellow/cyan color to stand out
        ImU32 marker_color = IM_COL32(0, 255, 255, alpha); // Cyan
        float marker_size = 8.0f;

        // Draw filled circle
        draw_list->AddCircleFilled(ImVec2(x, y), marker_size, marker_color);

        // Draw outline for visibility
        draw_list->AddCircle(ImVec2(x, y), marker_size,
                             IM_COL32(255, 255, 255, alpha), 0, 2.0f);

        // Draw vertical line from trade price up to show where it executed
        float line_height = plot_size.y * 0.3f; // 30% of graph height
        draw_list->AddLine(ImVec2(x, y - line_height), ImVec2(x, y),
                           marker_color, 2.0f);
      }

      ++it;
    }
  }

  // Draw price labels on x-axis
  int num_price_labels = 10;
  for (int i = 0; i <= num_price_labels; i++) {
    double price =
        min_price + (max_price - min_price) * (i / (double)num_price_labels);
    float x = plot_pos.x + (i / (float)num_price_labels) * plot_size.x;
    char label[32];
    snprintf(label, sizeof(label), "$%.2f", price);
    draw_list->AddText(ImVec2(x - 30, plot_pos.y + plot_size.y + 5),
                       IM_COL32(255, 255, 255, 255), label);
  }

  // Draw quantity labels on y-axis
  int num_qty_labels = 5;
  for (int i = 0; i <= num_qty_labels; i++) {
    uint32_t qty = max_qty - (max_qty * i / num_qty_labels);
    float y = plot_pos.y + (i / (float)num_qty_labels) * plot_size.y;
    char label[32];
    snprintf(label, sizeof(label), "%u", qty);
    draw_list->AddText(ImVec2(plot_pos.x - 50, y - 8),
                       IM_COL32(255, 255, 255, 255), label);
  }

  // Draw axis labels
  draw_list->AddText(
      ImVec2(plot_pos.x + plot_size.x / 2 - 30, plot_pos.y + plot_size.y + 25),
      IM_COL32(255, 255, 255, 255), "Price");
  draw_list->AddText(ImVec2(plot_pos.x - 35, plot_pos.y + plot_size.y / 2 - 10),
                     IM_COL32(255, 255, 255, 255), "Qty");

  // Draw toxicity legend with explanation
  ImGui::SetCursorScreenPos(
      ImVec2(plot_pos.x + plot_size.x - 200, plot_pos.y + 10));
  ImGui::BeginGroup();
  ImGui::Text("Toxicity Score:");
  ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Green = Low (0.0)");
  ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Yellow = Med (0.5)");
  ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Red = High (1.0)");
  ImGui::Separator();
  ImGui::Text("Factors:");
  ImGui::BulletText("Cancel/Add ratio (40%%)");
  ImGui::BulletText("Pings < 10 shares (20%%)");
  ImGui::BulletText("Odd lots (15%%)");
  ImGui::BulletText("High precision prices (15%%)");
  ImGui::BulletText("Resistance levels (10%%)");
  ImGui::Text("Hover over lines for details");
  ImGui::EndGroup();
}

void OrderBookVisualizer::record_toxicity_sample(double price, char side,
                                                 bool force_sample) {
  auto now = std::chrono::steady_clock::now();

  std::lock_guard<std::mutex> lock(toxicity_history_mutex);

  if (!start_time_set) {
    start_time = now;
    last_toxicity_sample = now;
    start_time_set = true;
  }

  // Only sample at regular intervals for smoother graph (skip during rapid
  // updates) Unless force_sample is true (used during seek/replay)
  if (!force_sample) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - last_toxicity_sample)
                       .count();

    if (elapsed < TOXICITY_SAMPLE_INTERVAL_MS) {
      return; // Skip this sample, too soon
    }
  }

  double toxicity = order_book.get_toxicity(price, side);

  ToxicityTimePoint point;
  point.timestamp = force_sample ? last_toxicity_sample : now;
  point.toxicity = toxicity;
  point.price = price;
  point.side = side;

  toxicity_history.push_back(point);

  if (!force_sample) {
    last_toxicity_sample = now;
  }

  // Limit history size
  if (toxicity_history.size() > max_toxicity_history) {
    toxicity_history.erase(toxicity_history.begin());
  }
}

void OrderBookVisualizer::render_toxicity_over_time() {
  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  ImVec2 graph_pos = ImGui::GetCursorScreenPos();
  ImVec2 graph_size = ImGui::GetContentRegionAvail();

  if (graph_size.y < 50)
    return; // Too small to render

  // Get toxicity history (thread-safe copy)
  std::vector<ToxicityTimePoint> history;
  {
    std::lock_guard<std::mutex> lock(toxicity_history_mutex);
    if (toxicity_history.empty()) {
      ImGui::Text("No toxicity data yet - waiting for order book updates...");
      return;
    }
    history = toxicity_history;
  }

  if (history.empty()) {
    ImGui::Text("No toxicity data");
    return;
  }

  // Calculate time range
  auto now = std::chrono::steady_clock::now();
  auto start = start_time_set ? start_time : history.front().timestamp;
  auto total_duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
          .count();

  if (total_duration == 0)
    total_duration = 1;

  // Add padding
  float padding = 40.0f;
  ImVec2 plot_pos(graph_pos.x + padding, graph_pos.y + padding);
  ImVec2 plot_size(graph_size.x - padding * 2, graph_size.y - padding * 2);

  // Draw axes
  draw_list->AddLine(ImVec2(plot_pos.x, plot_pos.y + plot_size.y),
                     ImVec2(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y),
                     IM_COL32(255, 255, 255, 255), 2.0f); // X-axis
  draw_list->AddLine(ImVec2(plot_pos.x, plot_pos.y),
                     ImVec2(plot_pos.x, plot_pos.y + plot_size.y),
                     IM_COL32(255, 255, 255, 255), 2.0f); // Y-axis

  // Draw title
  ImGui::SetCursorScreenPos(ImVec2(plot_pos.x, graph_pos.y + 5));
  ImGui::Text("Toxicity Over Time (Weighted Score)");

  // Separate bid and ask history, sort by time
  std::vector<std::pair<int64_t, double>> bid_history; // (time_ms, toxicity)
  std::vector<std::pair<int64_t, double>> ask_history;

  for (const auto &point : history) {
    auto time_offset = std::chrono::duration_cast<std::chrono::milliseconds>(
                           point.timestamp - start)
                           .count();

    if (point.side == 'B') {
      bid_history.emplace_back(time_offset, point.toxicity);
    } else {
      ask_history.emplace_back(time_offset, point.toxicity);
    }
  }

  // Sort by time
  std::sort(
      bid_history.begin(), bid_history.end(),
      [](const std::pair<int64_t, double> &a,
         const std::pair<int64_t, double> &b) { return a.first < b.first; });
  std::sort(
      ask_history.begin(), ask_history.end(),
      [](const std::pair<int64_t, double> &a,
         const std::pair<int64_t, double> &b) { return a.first < b.first; });

  // Create smoothed data by averaging nearby points (moving average)
  auto smooth_data = [](const std::vector<std::pair<int64_t, double>> &data,
                        int window_ms) {
    if (data.empty())
      return data;

    std::vector<std::pair<int64_t, double>> smoothed;
    for (size_t i = 0; i < data.size(); ++i) {
      double sum = 0.0;
      int count = 0;

      // Average points within window
      for (size_t j = 0; j < data.size(); ++j) {
        int64_t time_diff = std::abs(data[i].first - data[j].first);
        if (time_diff <= window_ms) {
          sum += data[j].second;
          count++;
        }
      }

      if (count > 0) {
        smoothed.emplace_back(data[i].first, sum / count);
      }
    }
    return smoothed;
  };

  // Smooth the data (100ms window for smooth transitions)
  auto bid_smoothed = smooth_data(bid_history, 100);
  auto ask_smoothed = smooth_data(ask_history, 100);

  // Draw bid line (green to red gradient)
  if (bid_smoothed.size() >= 2) {
    for (size_t i = 0; i + 1 < bid_smoothed.size(); ++i) {
      float x1 =
          plot_pos.x +
          (float)(bid_smoothed[i].first / (double)total_duration) * plot_size.x;
      float y1 = plot_pos.y + plot_size.y -
                 (float)(bid_smoothed[i].second) * plot_size.y;
      float x2 = plot_pos.x +
                 (float)(bid_smoothed[i + 1].first / (double)total_duration) *
                     plot_size.x;
      float y2 = plot_pos.y + plot_size.y -
                 (float)(bid_smoothed[i + 1].second) * plot_size.y;

      // Color based on average toxicity of segment
      double avg_toxicity =
          (bid_smoothed[i].second + bid_smoothed[i + 1].second) / 2.0;
      uint8_t r = (uint8_t)(avg_toxicity * 255);
      uint8_t g = (uint8_t)((1.0 - avg_toxicity) * 255);
      uint8_t b = 0;

      draw_list->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(r, g, b, 255),
                         2.5f);
    }
  }

  // Draw ask line (red gradient)
  if (ask_smoothed.size() >= 2) {
    for (size_t i = 0; i + 1 < ask_smoothed.size(); ++i) {
      float x1 =
          plot_pos.x +
          (float)(ask_smoothed[i].first / (double)total_duration) * plot_size.x;
      float y1 = plot_pos.y + plot_size.y -
                 (float)(ask_smoothed[i].second) * plot_size.y;
      float x2 = plot_pos.x +
                 (float)(ask_smoothed[i + 1].first / (double)total_duration) *
                     plot_size.x;
      float y2 = plot_pos.y + plot_size.y -
                 (float)(ask_smoothed[i + 1].second) * plot_size.y;

      // Color based on average toxicity of segment
      double avg_toxicity =
          (ask_smoothed[i].second + ask_smoothed[i + 1].second) / 2.0;
      uint8_t r = 255;
      uint8_t g = (uint8_t)((1.0 - avg_toxicity) * 100);
      uint8_t b = (uint8_t)((1.0 - avg_toxicity) * 100);

      draw_list->AddLine(ImVec2(x1, y1), ImVec2(x2, y2), IM_COL32(r, g, b, 255),
                         2.5f);
    }
  }

  // Draw Y-axis labels (toxicity 0.0 to 1.0)
  for (int i = 0; i <= 5; i++) {
    float y_val = i / 5.0f;
    float y = plot_pos.y + plot_size.y - (y_val * plot_size.y);
    char label[32];
    snprintf(label, sizeof(label), "%.1f", y_val);
    draw_list->AddText(ImVec2(plot_pos.x - 35, y - 8),
                       IM_COL32(255, 255, 255, 255), label);
  }

  // Draw X-axis label
  draw_list->AddText(
      ImVec2(plot_pos.x + plot_size.x / 2 - 30, plot_pos.y + plot_size.y + 20),
      IM_COL32(255, 255, 255, 255), "Time");

  // Draw legend
  ImGui::SetCursorScreenPos(
      ImVec2(plot_pos.x + plot_size.x - 150, plot_pos.y + 5));
  ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Bids");
  ImGui::SameLine();
  ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Asks");
}

void OrderBookVisualizer::cleanup() {
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
}

bool OrderBookVisualizer::should_close() {
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    ImGui_ImplSDL2_ProcessEvent(&event);
    if (event.type == SDL_QUIT) {
      return true;
    }
    if (event.type == SDL_WINDOWEVENT &&
        event.window.event == SDL_WINDOWEVENT_CLOSE &&
        event.window.windowID == SDL_GetWindowID(window)) {
      return true;
    }
  }
  return false;
}

int main(int argc, char *argv[]) {
  std::string pcap_file;
  std::string symbol_file = "data/symbol_nyse.txt";

  // Parse command line arguments
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      filter_ticker = argv[++i];
    } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      symbol_file = argv[++i];
    } else if (pcap_file.empty()) {
      pcap_file = argv[i];
    }
  }

  if (pcap_file.empty()) {
    std::cerr << "Usage: " << argv[0]
              << " <pcap_file> [-t ticker] [-s symbol_file]" << std::endl;
    std::cerr << "Example: " << argv[0]
              << " data/ny4-xnys-pillar-a-20230822T133000.pcap -t AAPL"
              << std::endl;
    std::cerr << "Default symbol file: data/symbol_nyse.txt" << std::endl;
    return 1;
  }

  // Load symbol mapping
  size_t symbols_loaded = xdp::load_symbol_map(symbol_file);

  std::cout << "Loaded " << symbols_loaded << " symbols from " << symbol_file
            << std::endl;

  if (!filter_ticker.empty()) {
    std::cout << "Filtering for ticker: " << filter_ticker << std::endl;
  } else {
    std::cout << "No ticker filter specified - processing all messages"
              << std::endl;
  }

  // Create visualizer
  OrderBookVisualizer visualizer(order_book);
  g_visualizer = &visualizer;

  if (!visualizer.init()) {
    std::cerr << "Failed to initialize visualizer" << std::endl;
    return 1;
  }

  // Start PCAP reading in a separate thread
  std::thread pcap_thread(pcap_thread_func, pcap_file);

  // Main render loop - optimized for high FPS
  bool running = true;
  auto last_update_time = std::chrono::steady_clock::now();
  const auto update_interval = std::chrono::milliseconds(
      8); // Update order book every 8ms (~120 FPS for data)

  while (running) {
    if (visualizer.should_close()) {
      running = false;
      should_stop.store(true);
    }

    // Apply batched updates at high frequency for smooth updates
    auto now = std::chrono::steady_clock::now();
    if (now - last_update_time >= update_interval) {
      apply_batched_updates();
      last_update_time = now;
    } else {
      // If we have time, apply more updates immediately (adaptive processing)
      if (!update_queue.empty()) {
        apply_batched_updates();
      }
    }

    // Process playback if active
    visualizer.process_playback();

    visualizer.render();

    // Yield to other threads occasionally to prevent 100% CPU
    // This allows the PCAP thread to make progress
    static int frame_count = 0;
    if (++frame_count % 60 == 0) {
      std::this_thread::yield(); // Yield every 60 frames
    }
  }

  visualizer.cleanup();
  pcap_thread.join();

  return 0;
}
