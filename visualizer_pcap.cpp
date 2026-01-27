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
#include "order_book.hpp"
#include <pcap.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <cstring>
#include <arpa/inet.h>
#include <chrono>
#include <vector>
#include <algorithm>
#include <iostream>

// Forward declarations
uint16_t read_le16(const uint8_t *data);
uint32_t read_le32(const uint8_t *data);
uint64_t read_le64(const uint8_t *data);
double parse_price(uint32_t price_raw);
std::string get_symbol(uint32_t symbol_index);
void load_symbol_map(const char *filename);

// Global order book and synchronization
OrderBook order_book;
std::atomic<bool> should_stop(false);
std::atomic<uint64_t> packets_processed(0);
std::atomic<uint64_t> messages_processed(0);
std::string filter_ticker = "";

// Message entry for live feed
struct MessageEntry
{
  std::string text;
  bool is_buy;
  bool is_exec = false;
  double price;
  uint32_t volume;
  std::chrono::steady_clock::time_point timestamp;
};

// Trade execution marker for visualization
struct TradeMarker
{
  double price;
  uint32_t volume;
  std::chrono::steady_clock::time_point timestamp;
};

// Order book update queue
enum class UpdateType
{
  ADD,
  MODIFY,
  DELETE,
  EXECUTE,
  REPLACE
};

struct OrderBookUpdate
{
  UpdateType type;
  uint64_t order_id;
  uint64_t new_order_id; // For REPLACE
  double price;
  uint32_t volume;
  char side;
};

std::queue<OrderBookUpdate> update_queue;
std::mutex queue_mutex;
const size_t BATCH_SIZE = 500; // Process updates in batches (increased for better throughput)

// Playback storage (for replay after stream finishes)
std::vector<OrderBookUpdate> playback_updates;
std::mutex playback_mutex;
size_t playback_index = 0;

// Forward declare visualizer for message feed
class OrderBookVisualizer;
OrderBookVisualizer *g_visualizer = nullptr;

// OrderBookVisualizer class
class OrderBookVisualizer
{
private:
  OrderBook &order_book;
  SDL_Window *window;
  SDL_GLContext gl_context;
  bool auto_scale = true;
  float price_range = 5.0f;
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

public:
  OrderBookVisualizer(OrderBook &ob) : order_book(ob), window(nullptr) {}

  bool init();
  void render();
  void cleanup();
  bool should_close();
  void render_controls();
  void render_order_book_graph();
  void render_message_feed();
  void add_message(const std::string &text, bool is_buy, double price, uint32_t volume, bool is_exec = false);
  void add_trade_marker(double price, uint32_t volume);
  void apply_playback_to_index(size_t idx);
  void set_stream_finished(bool finished) { stream_finished = finished; }
  void process_playback();
};

// Process XDP message and queue update (non-blocking)
void process_xdp_message(const uint8_t *data, size_t max_len, uint16_t msg_type)
{
  // Need at least 4 bytes for message header
  if (max_len < 4)
    return;

  uint32_t symbol_index = 0;
  std::string ticker;

  // Handle messages with non-standard header structure (106, 223)
  if (msg_type == 106 || msg_type == 223)
  {
    // Messages 106 and 223: SourceTime@4, SourceTimeNS@8, SymbolIndex@12
    if (max_len < 16)
      return;
    symbol_index = read_le32(data + 12);
    ticker = get_symbol(symbol_index);
  }
  else
  {
    // Standard messages: SourceTimeNS@4, SymbolIndex@8
    if (max_len < 12)
      return;
    symbol_index = read_le32(data + 8);
    ticker = get_symbol(symbol_index);
  }

  // Debug: Count all messages before filtering
  static std::atomic<uint64_t> total_messages_seen(0);
  total_messages_seen++;

  // Filter by ticker if specified
  if (!filter_ticker.empty() && ticker != filter_ticker)
  {
    return;
  }

  messages_processed++;

  // Queue update instead of applying immediately
  OrderBookUpdate update;

  switch (msg_type)
  {
  case 100:
  { // Add Order
    if (max_len >= 39)
    {
      update.type = UpdateType::ADD;
      update.order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 24);
      update.volume = read_le32(data + 28);
      update.side = data[32];
      update.price = parse_price(price_raw);

      // Add to message feed
      if (g_visualizer)
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "ADD %s $%.2f x %u",
                 update.side == 'B' ? "BUY" : "SELL", update.price, update.volume);
        g_visualizer->add_message(msg, update.side == 'B', update.price, update.volume);
      }

      std::lock_guard<std::mutex> lock(queue_mutex);
      update_queue.push(update);
    }
    break;
  }

  case 101:
  { // Modify Order
    if (max_len >= 35)
    {
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

  case 102:
  { // Delete Order
    if (max_len >= 25)
    {
      update.type = UpdateType::DELETE;
      update.order_id = read_le64(data + 16);

      std::lock_guard<std::mutex> lock(queue_mutex);
      update_queue.push(update);
    }
    break;
  }

  case 103:
  { // Execute Order
    if (max_len >= 42)
    {
      update.type = UpdateType::EXECUTE;
      update.order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 28);
      update.volume = read_le32(data + 32);
      update.price = parse_price(price_raw);

      // Add to message feed (executions are important)
      if (g_visualizer)
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "EXEC $%.2f x %u", update.price, update.volume);
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

  case 104:
  { // Replace Order
    if (max_len >= 42)
    {
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

// Apply batched updates to order book (optimized for high throughput)
void apply_batched_updates()
{
  std::vector<OrderBookUpdate> batch;
  batch.reserve(BATCH_SIZE); // Pre-allocate for better performance

  // Collect a batch of updates (quick lock/unlock)
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    size_t count = std::min(update_queue.size(), BATCH_SIZE);
    batch.reserve(count);
    for (size_t i = 0; i < count; i++)
    {
      if (!update_queue.empty())
      {
        batch.push_back(update_queue.front());
        update_queue.pop();
      }
    }
  }

  // Apply all updates in batch (OrderBook methods are thread-safe with internal mutex)
  if (!batch.empty())
  {
    for (const auto &update : batch)
    {
      // Always store updates for playback so we can replay later
      {
        std::lock_guard<std::mutex> lock(playback_mutex);
        playback_updates.push_back(update);
      }

      // Apply update
      switch (update.type)
      {
      case UpdateType::ADD:
        order_book.add_order(update.order_id, update.price, update.volume, update.side);
        break;
      case UpdateType::MODIFY:
        order_book.modify_order(update.order_id, update.price, update.volume);
        break;
      case UpdateType::DELETE:
        order_book.delete_order(update.order_id);
        break;
      case UpdateType::EXECUTE:
        order_book.execute_order(update.order_id, update.volume, update.price);
        break;
      case UpdateType::REPLACE:
        order_book.delete_order(update.order_id);
        order_book.add_order(update.new_order_id, update.price, update.volume, update.side);
        break;
      }
    }
  }
}

// Parse XDP packet
void parse_xdp_packet(const uint8_t *data, size_t length)
{
  if (length < 16)
    return;

  uint8_t num_messages = data[3];
  size_t offset = 16; // Skip packet header

  // Debug: Check if we're getting packets
  extern std::atomic<uint64_t> packets_parsed;
  extern std::atomic<uint64_t> messages_parsed;
  packets_parsed++;

  for (uint8_t i = 0; i < num_messages && offset < length; i++)
  {
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
  if (packets_parsed.load() % 10000 == 0)
  {
    std::cout << "Debug: Parsed " << packets_parsed.load() << " XDP packets, "
              << messages_parsed.load() << " messages, "
              << messages_processed.load() << " matched filter" << std::endl;
  }
}

// PCAP packet handler
void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr, const u_char *packet)
{
  (void)user_data;
  packets_processed++;

  if (should_stop.load())
  {
    return;
  }

  if (pkthdr->caplen < 14)
    return;

  // Parse Ethernet header
  uint16_t eth_type = ntohs(*(uint16_t *)(packet + 12));
  size_t eth_header_len = 14;

  // Skip VLAN tag if present
  if (eth_type == 0x8100 || eth_type == 0x88A8)
  {
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

  if (payload_len > pkthdr->caplen - udp_offset - 8)
  {
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
void pcap_thread_func(const std::string &pcap_file)
{
  char errbuf[PCAP_ERRBUF_SIZE];
  pcap_t *handle = pcap_open_offline(pcap_file.c_str(), errbuf);

  if (!handle)
  {
    std::cerr << "Error opening PCAP file: " << errbuf << std::endl;
    should_stop.store(true);
    return;
  }

  std::cout << "Reading PCAP file: " << pcap_file << std::endl;

  // Use pcap_loop for better performance (it's optimized internally)
  // Process in batches to allow UI updates
  pcap_loop(handle, -1, packet_handler, nullptr);

  pcap_close(handle);

  std::cout << "Finished reading PCAP file. Processed " << packets_processed.load()
            << " packets, " << messages_processed.load() << " messages matched filter" << std::endl;

  // Print debug stats
  std::cout << "Debug: Parsed " << packets_parsed.load() << " XDP packets, "
            << messages_parsed.load() << " total messages" << std::endl;

  should_stop.store(true);
  if (g_visualizer)
  {
    g_visualizer->set_stream_finished(true);
  }
}

// OrderBookVisualizer implementation
bool OrderBookVisualizer::init()
{
  if (SDL_Init(SDL_INIT_VIDEO) != 0)
  {
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

  if (!window)
  {
    std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
    return false;
  }

  gl_context = SDL_GL_CreateContext(window);
  if (!gl_context)
  {
    std::cerr << "OpenGL context creation failed: " << SDL_GetError() << std::endl;
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
  if (!ImGui_ImplOpenGL3_Init(glsl_version))
  {
    std::cerr << "Failed to initialize ImGui OpenGL3 renderer" << std::endl;
    return false;
  }

  return true;
}

void OrderBookVisualizer::render()
{
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

  // Graph below
  ImGui::BeginChild("Graph", ImVec2(0, 0), true);
  render_order_book_graph();
  ImGui::EndChild();

  ImGui::EndChild();

  ImGui::End();

  ImGui::Render();
  glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
  glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  SDL_GL_SwapWindow(window);
}

void OrderBookVisualizer::process_playback()
{
  if (!is_playing || !stream_finished)
    return;

  std::lock_guard<std::mutex> lock(playback_mutex);
  if (playback_updates.empty() || playback_index >= playback_updates.size())
  {
    is_playing = false; // Reached end
    playback_index = 0; // Reset for next play
    return;
  }

  auto now = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - last_playback_time)
                     .count();

  // Calculate delay based on playback speed (default: 1 update per 20ms at 0.5x speed)
  // At 0.5x speed, delay is 20ms, so at 1x it's 10ms
  int delay_ms = (int)(20.0f / playback_speed);

  if (elapsed >= delay_ms)
  {
    // Apply next update
    const auto &update = playback_updates[playback_index];
    switch (update.type)
    {
    case UpdateType::ADD:
      order_book.add_order(update.order_id, update.price, update.volume, update.side);
      // Add to message feed
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "ADD %s $%.2f x %u",
                 update.side == 'B' ? "BUY" : "SELL", update.price, update.volume);
        add_message(msg, update.side == 'B', update.price, update.volume);
      }
      break;
    case UpdateType::MODIFY:
      order_book.modify_order(update.order_id, update.price, update.volume);
      break;
    case UpdateType::DELETE:
      order_book.delete_order(update.order_id);
      break;
    case UpdateType::EXECUTE:
      order_book.execute_order(update.order_id, update.volume, update.price);
      // Add to message feed
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "EXEC $%.2f x %u", update.price, update.volume);
        add_message(msg, true, update.price, update.volume, true);
      }
      // Add visual trade marker
      add_trade_marker(update.price, update.volume);
      break;
    case UpdateType::REPLACE:
      order_book.delete_order(update.order_id);
      order_book.add_order(update.new_order_id, update.price, update.volume, update.side);
      break;
    }
    playback_index++;
    last_playback_time = now;
  }
}

void OrderBookVisualizer::apply_playback_to_index(size_t idx)
{
  // Apply stored updates from 0..idx-1 to rebuild order book state
  std::vector<OrderBookUpdate> snapshot;
  {
    std::lock_guard<std::mutex> lock(playback_mutex);
    if (idx > playback_updates.size())
      idx = playback_updates.size();
    snapshot.reserve(idx);
    for (size_t i = 0; i < idx; ++i)
      snapshot.push_back(playback_updates[i]);
  }

  // Clear current state
  order_book.clear();
  {
    std::lock_guard<std::mutex> lock(feed_mutex);
    message_feed.clear();
  }
  {
    std::lock_guard<std::mutex> lock(markers_mutex);
    trade_markers.clear();
  }

  // Replay updates up to idx (without re-storing them)
  for (const auto &update : snapshot)
  {
    switch (update.type)
    {
    case UpdateType::ADD:
      order_book.add_order(update.order_id, update.price, update.volume, update.side);
      // Add to message feed
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "ADD %s $%.2f x %u",
                 update.side == 'B' ? "BUY" : "SELL", update.price, update.volume);
        add_message(msg, update.side == 'B', update.price, update.volume);
      }
      break;
    case UpdateType::MODIFY:
      order_book.modify_order(update.order_id, update.price, update.volume);
      break;
    case UpdateType::DELETE:
      order_book.delete_order(update.order_id);
      break;
    case UpdateType::EXECUTE:
      order_book.execute_order(update.order_id, update.volume, update.price);
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "EXEC $%.2f x %u", update.price, update.volume);
        add_message(msg, true, update.price, update.volume, true);
      }
      add_trade_marker(update.price, update.volume);
      break;
    case UpdateType::REPLACE:
      order_book.delete_order(update.order_id);
      order_book.add_order(update.new_order_id, update.price, update.volume, update.side);
      break;
    }
  }

  // Update playback index to reflect new position
  {
    std::lock_guard<std::mutex> lock(playback_mutex);
    playback_index = idx;
  }
}

void OrderBookVisualizer::render_controls()
{
  ImGui::Text("Order Book Controls");
  ImGui::SameLine();
  ImGui::Checkbox("Auto Scale", &auto_scale);
  if (!auto_scale)
  {
    ImGui::SameLine();
    ImGui::SliderFloat("Price Range", &price_range, 0.1f, 20.0f);
  }

  ImGui::SameLine();
  if (stream_finished)
  {
    if (ImGui::Button(is_playing ? "Pause" : "Play"))
    {
      is_playing = !is_playing;
      if (is_playing)
      {
        last_playback_time = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(playback_mutex);
        if (playback_index >= playback_updates.size())
        {
          playback_index = 0; // Reset to beginning
        }
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset"))
    {
      is_playing = false;
      playback_speed = 0.5f; // Reset to half speed

      // Clear order book
      order_book.clear();

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
    // Timeline slider / seek
    size_t playback_size = 0;
    {
      std::lock_guard<std::mutex> lock(playback_mutex);
      playback_size = playback_updates.size();
    }

    if (playback_size > 0)
    {
      ImGui::SameLine();
      static int seek_idx = 0;
      int max_idx = (int)playback_size;
      if (ImGui::SliderInt("Position", &seek_idx, 0, max_idx))
      {
        // Clamp
        if (seek_idx < 0)
          seek_idx = 0;
        if (seek_idx > max_idx)
          seek_idx = max_idx;
        apply_playback_to_index((size_t)seek_idx);
      }
    }
    if (is_playing)
    {
      ImGui::SameLine();
      ImGui::SliderFloat("Speed", &playback_speed, 0.1f, 5.0f);
      std::lock_guard<std::mutex> lock(playback_mutex);
      ImGui::SameLine();
      ImGui::Text("(%zu/%zu)", playback_index, playback_updates.size());
    }
  }
  else
  {
    ImGui::Text("Streaming...");
  }

  ImGui::Separator();

  // Statistics
  auto stats = order_book.get_stats();
  ImGui::Text("Best Bid: $%.4f | Best Ask: $%.4f | Spread: $%.4f | Mid: $%.4f",
              stats.best_bid, stats.best_ask, stats.spread, stats.mid_price);
  ImGui::Text("Bid Qty: %u | Ask Qty: %u | Levels: %d/%d",
              stats.total_bid_qty, stats.total_ask_qty, stats.bid_levels, stats.ask_levels);
  ImGui::Text("Packets: %llu | Messages: %llu",
              (unsigned long long)packets_processed.load(),
              (unsigned long long)messages_processed.load());
}

void OrderBookVisualizer::render_message_feed()
{
  ImGui::Text("Live Message Feed");
  ImGui::Separator();

  ImGui::Checkbox("Auto-scroll", &auto_scroll_feed);
  ImGui::SameLine();
  if (ImGui::Button("Clear"))
  {
    std::lock_guard<std::mutex> lock(feed_mutex);
    message_feed.clear();
  }

  ImGui::Separator();

  // Message list
  ImGui::BeginChild("FeedList", ImVec2(0, 0), true);

  std::lock_guard<std::mutex> lock(feed_mutex);
  for (const auto &entry : message_feed)
  {
    ImVec4 color;
    if (entry.is_exec)
    {
      color = ImVec4(0.2f, 0.6f, 1.0f, 1.0f); // Blue for executions
    }
    else
    {
      color = entry.is_buy ? ImVec4(0.0f, 1.0f, 0.0f, 1.0f) : ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextWrapped("%s", entry.text.c_str());
    ImGui::PopStyleColor();
  }

  if (auto_scroll_feed && !message_feed.empty())
  {
    ImGui::SetScrollHereY(1.0f);
  }

  ImGui::EndChild();
}

void OrderBookVisualizer::add_message(const std::string &text, bool is_buy, double price, uint32_t volume, bool is_exec)
{
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
  if (message_feed.size() > max_feed_entries)
  {
    message_feed.erase(message_feed.begin());
  }
}

void OrderBookVisualizer::add_trade_marker(double price, uint32_t volume)
{
  std::lock_guard<std::mutex> lock(markers_mutex);
  TradeMarker marker;
  marker.price = price;
  marker.volume = volume;
  marker.timestamp = std::chrono::steady_clock::now();

  trade_markers.push_back(marker);

  // Limit marker count
  if (trade_markers.size() > max_markers)
  {
    trade_markers.erase(trade_markers.begin());
  }
}

void OrderBookVisualizer::render_order_book_graph()
{
  // Get snapshots of the data (thread-safe copies)
  auto stats = order_book.get_stats();
  auto bids = order_book.get_bids();
  auto asks = order_book.get_asks();

  ImDrawList *draw_list = ImGui::GetWindowDrawList();
  ImVec2 graph_pos = ImGui::GetCursorScreenPos();
  ImVec2 graph_size = ImGui::GetContentRegionAvail();

  // Handle empty order book
  if (stats.best_bid == 0.0 && stats.best_ask == 0.0)
  {
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
  double mid_price = stats.mid_price > 0 ? stats.mid_price : (best_bid > 0 && best_ask > 0 ? (best_bid + best_ask) / 2.0 : (best_bid > 0 ? best_bid : best_ask));

  if (auto_scale)
  {
    double price_span = 0.0;
    if (best_bid > 0 && best_ask > 0)
    {
      double spread = best_ask - best_bid;
      price_span = std::max(spread * 10.0, mid_price * 0.02);
    }
    else
    {
      price_span = mid_price * 0.05;
    }
    min_price = mid_price - price_span;
    max_price = mid_price + price_span;
  }
  else
  {
    min_price = mid_price - price_range;
    max_price = mid_price + price_range;
  }

  if (max_price <= min_price)
  {
    max_price = min_price + price_range * 2;
  }

  // Find max quantity for scaling
  uint32_t max_qty = 0;
  for (const auto &pair : bids)
  {
    if (pair.first >= min_price && pair.first <= max_price)
    {
      max_qty = std::max(max_qty, pair.second);
    }
  }
  for (const auto &pair : asks)
  {
    if (pair.first >= min_price && pair.first <= max_price)
    {
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
  if (best_bid > 0 && best_ask > 0)
  {
    bid_x = plot_pos.x + (float)((best_bid - min_price) / (max_price - min_price)) * plot_size.x;
    ask_x = plot_pos.x + (float)((best_ask - min_price) / (max_price - min_price)) * plot_size.x;
  }
  else
  {
    bid_x = plot_pos.x + plot_size.x * 0.4f;
    ask_x = plot_pos.x + plot_size.x * 0.6f;
  }

  // Draw simplified bid/ask lines (no gradient) -- build sorted point lists
  std::vector<ImVec2> bid_points_unsorted;
  for (const auto &pair : bids)
  {
    double price = pair.first;
    uint32_t qty = pair.second;
    if (price < min_price || price > best_bid)
      continue;
    float x = plot_pos.x + (float)((price - min_price) / (max_price - min_price)) * plot_size.x;
    float y = plot_pos.y + plot_size.y - (float)(qty / (double)max_qty) * plot_size.y;
    bid_points_unsorted.emplace_back(x, y);
  }

  // Sort by x ascending so polyline goes left->center without crossing
  std::sort(bid_points_unsorted.begin(), bid_points_unsorted.end(), [](const ImVec2 &a, const ImVec2 &b) { return a.x < b.x; });

  // Build final bid polyline starting at left baseline
  std::vector<ImVec2> bid_line_points;
  bid_line_points.emplace_back(plot_pos.x, plot_pos.y + plot_size.y);
  for (const auto &p : bid_points_unsorted) bid_line_points.push_back(p);
  // Ensure endpoint at best bid x (bottom)
  bid_line_points.emplace_back(bid_x, plot_pos.y + plot_size.y);

  if (bid_line_points.size() >= 2)
  {
    for (size_t i = 0; i + 1 < bid_line_points.size(); ++i)
      draw_list->AddLine(bid_line_points[i], bid_line_points[i + 1], IM_COL32(0, 200, 0, 255), 2.0f);
  }

  // Ask side: collect and sort by x descending so polyline goes right->center
  std::vector<ImVec2> ask_points_unsorted;
  for (const auto &pair : asks)
  {
    double price = pair.first;
    uint32_t qty = pair.second;
    if (price < best_ask || price > max_price)
      continue;
    float x = plot_pos.x + (float)((price - min_price) / (max_price - min_price)) * plot_size.x;
    float y = plot_pos.y + plot_size.y - (float)(qty / (double)max_qty) * plot_size.y;
    ask_points_unsorted.emplace_back(x, y);
  }

  std::sort(ask_points_unsorted.begin(), ask_points_unsorted.end(), [](const ImVec2 &a, const ImVec2 &b) { return a.x > b.x; });

  std::vector<ImVec2> ask_line_points;
  ask_line_points.emplace_back(plot_pos.x + plot_size.x, plot_pos.y + plot_size.y);
  for (const auto &p : ask_points_unsorted) ask_line_points.push_back(p);
  ask_line_points.emplace_back(ask_x, plot_pos.y + plot_size.y);

  if (ask_line_points.size() >= 2)
  {
    for (size_t i = 0; i + 1 < ask_line_points.size(); ++i)
      draw_list->AddLine(ask_line_points[i], ask_line_points[i + 1], IM_COL32(200, 0, 0, 255), 2.0f);
  }

  // Draw opaque vertical lines to show spread
  draw_list->AddLine(ImVec2(bid_x, plot_pos.y), ImVec2(bid_x, plot_pos.y + plot_size.y), IM_COL32(255, 255, 0, 255), 3.0f);
  draw_list->AddLine(ImVec2(ask_x, plot_pos.y), ImVec2(ask_x, plot_pos.y + plot_size.y), IM_COL32(255, 255, 0, 255), 3.0f);

  // Draw trade execution markers
  {
    std::lock_guard<std::mutex> lock(markers_mutex);
    auto now = std::chrono::steady_clock::now();

    // Remove old markers and draw visible ones
    auto it = trade_markers.begin();
    while (it != trade_markers.end())
    {
      auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - it->timestamp)
                        .count();

      if (age_ms > marker_fade_time_ms)
      {
        // Remove old markers
        it = trade_markers.erase(it);
        continue;
      }

      // Calculate marker position
      double trade_price = it->price;
      if (trade_price >= min_price && trade_price <= max_price)
      {
        float x = plot_pos.x + (float)((trade_price - min_price) / (max_price - min_price)) * plot_size.x;
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
        draw_list->AddCircle(ImVec2(x, y), marker_size, IM_COL32(255, 255, 255, alpha), 0, 2.0f);

        // Draw vertical line from trade price up to show where it executed
        float line_height = plot_size.y * 0.3f; // 30% of graph height
        draw_list->AddLine(ImVec2(x, y - line_height),
                           ImVec2(x, y),
                           marker_color, 2.0f);
      }

      ++it;
    }
  }

  // Draw price labels on x-axis
  int num_price_labels = 10;
  for (int i = 0; i <= num_price_labels; i++)
  {
    double price = min_price + (max_price - min_price) * (i / (double)num_price_labels);
    float x = plot_pos.x + (i / (float)num_price_labels) * plot_size.x;
    char label[32];
    snprintf(label, sizeof(label), "$%.2f", price);
    draw_list->AddText(ImVec2(x - 30, plot_pos.y + plot_size.y + 5),
                       IM_COL32(255, 255, 255, 255), label);
  }

  // Draw quantity labels on y-axis
  int num_qty_labels = 5;
  for (int i = 0; i <= num_qty_labels; i++)
  {
    uint32_t qty = max_qty - (max_qty * i / num_qty_labels);
    float y = plot_pos.y + (i / (float)num_qty_labels) * plot_size.y;
    char label[32];
    snprintf(label, sizeof(label), "%u", qty);
    draw_list->AddText(ImVec2(plot_pos.x - 50, y - 8),
                       IM_COL32(255, 255, 255, 255), label);
  }

  // Draw axis labels
  draw_list->AddText(ImVec2(plot_pos.x + plot_size.x / 2 - 30, plot_pos.y + plot_size.y + 25),
                     IM_COL32(255, 255, 255, 255), "Price");
  draw_list->AddText(ImVec2(plot_pos.x - 35, plot_pos.y + plot_size.y / 2 - 10),
                     IM_COL32(255, 255, 255, 255), "Qty");
}

void OrderBookVisualizer::cleanup()
{
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();
}

bool OrderBookVisualizer::should_close()
{
  SDL_Event event;
  while (SDL_PollEvent(&event))
  {
    ImGui_ImplSDL2_ProcessEvent(&event);
    if (event.type == SDL_QUIT)
    {
      return true;
    }
    if (event.type == SDL_WINDOWEVENT &&
        event.window.event == SDL_WINDOWEVENT_CLOSE &&
        event.window.windowID == SDL_GetWindowID(window))
    {
      return true;
    }
  }
  return false;
}

// Helper functions from reader.cpp
uint16_t read_le16(const uint8_t *data)
{
  return data[0] | (data[1] << 8);
}

uint32_t read_le32(const uint8_t *data)
{
  return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

uint64_t read_le64(const uint8_t *data)
{
  return (uint64_t)read_le32(data) | ((uint64_t)read_le32(data + 4) << 32);
}

double parse_price(uint32_t price_raw)
{
  return price_raw / 10000.0;
}

std::unordered_map<uint32_t, std::string> symbol_map;

std::string get_symbol(uint32_t symbol_index)
{
  auto it = symbol_map.find(symbol_index);
  if (it != symbol_map.end())
  {
    return it->second;
  }
  return "UNKNOWN";
}

void load_symbol_map(const char *filename)
{
  std::ifstream file(filename);
  if (!file.is_open())
  {
    std::cerr << "Warning: Could not open symbol file: " << filename << std::endl;
    return;
  }

  std::string line;

  while (std::getline(file, line))
  {
    // Skip empty lines
    if (line.empty())
      continue;

    // Remove Windows carriage return if present
    if (!line.empty() && line.back() == '\r')
    {
      line.pop_back();
    }

    // Trim trailing whitespace
    while (!line.empty() && std::isspace(line.back()))
    {
      line.pop_back();
    }

    // Skip if line is now empty
    if (line.empty())
      continue;

    // Split by pipe delimiter
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream iss(line);

    while (std::getline(iss, token, '|'))
    {
      tokens.push_back(token);
    }

    // We need at least 3 fields: Symbol|Symbol|Index|...
    if (tokens.size() >= 3)
    {
      std::string symbol = tokens[0];
      std::string index_str = tokens[2];

      // Convert index to uint32_t
      char *endptr;
      unsigned long index_val = strtoul(index_str.c_str(), &endptr, 10);
      if (endptr != index_str.c_str() && *endptr == '\0' && index_val > 0 && index_val <= UINT32_MAX)
      {
        uint32_t index = (uint32_t)index_val;
        if (!symbol.empty())
        {
          symbol_map[index] = symbol;
        }
      }
    }
  }

  std::cout << "Loaded " << symbol_map.size() << " symbols" << std::endl;
}

int main(int argc, char *argv[])
{
  std::string pcap_file;
  std::string symbol_file = "symbol_nyse.txt";

  // Parse command line arguments
  for (int i = 1; i < argc; i++)
  {
    if (strcmp(argv[i], "-t") == 0 && i + 1 < argc)
    {
      filter_ticker = argv[++i];
    }
    else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
    {
      symbol_file = argv[++i];
    }
    else if (pcap_file.empty())
    {
      pcap_file = argv[i];
    }
  }

  if (pcap_file.empty())
  {
    std::cerr << "Usage: " << argv[0] << " <pcap_file> [-t ticker] [-s symbol_file]" << std::endl;
    std::cerr << "Example: " << argv[0] << " data/ny4-xnys-pillar-a-20230822T133000.pcap -t AAPL" << std::endl;
    return 1;
  }

  // Load symbol mapping
  load_symbol_map(symbol_file.c_str());

  if (!filter_ticker.empty())
  {
    std::cout << "Filtering for ticker: " << filter_ticker << std::endl;
  }

  // Create visualizer
  OrderBookVisualizer visualizer(order_book);
  g_visualizer = &visualizer;

  if (!visualizer.init())
  {
    std::cerr << "Failed to initialize visualizer" << std::endl;
    return 1;
  }

  // Start PCAP reading in a separate thread
  std::thread pcap_thread(pcap_thread_func, pcap_file);

  // Main render loop - optimized for high FPS
  bool running = true;
  auto last_update_time = std::chrono::steady_clock::now();
  const auto update_interval = std::chrono::milliseconds(8); // Update order book every 8ms (~120 FPS for data)

  while (running)
  {
    if (visualizer.should_close())
    {
      running = false;
      should_stop.store(true);
    }

    // Apply batched updates at high frequency for smooth updates
    auto now = std::chrono::steady_clock::now();
    if (now - last_update_time >= update_interval)
    {
      apply_batched_updates();
      last_update_time = now;
    }
    else
    {
      // If we have time, apply more updates immediately (adaptive processing)
      if (!update_queue.empty())
      {
        apply_batched_updates();
      }
    }

    // Process playback if active
    visualizer.process_playback();

    visualizer.render();

    // Yield to other threads occasionally to prevent 100% CPU
    // This allows the PCAP thread to make progress
    static int frame_count = 0;
    if (++frame_count % 60 == 0)
    {
      std::this_thread::yield(); // Yield every 60 frames
    }
  }

  visualizer.cleanup();
  pcap_thread.join();

  return 0;
}
