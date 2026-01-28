// visualizer_main.cpp - Main entry point for visualizer with PCAP support
#include "order_book.hpp"
#include "visualization.cpp"
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
#include <vector>
#include <chrono>

// Forward declarations from reader.cpp
extern std::unordered_map<uint32_t, std::string> symbol_map;
extern uint16_t read_le16(const uint8_t *p);
extern uint32_t read_le32(const uint8_t *p);
extern uint64_t read_le64(const uint8_t *p);
extern double parse_price(uint32_t price_raw);
extern std::string get_symbol(uint32_t index);

// Packet event structure
struct PacketEvent
{
  std::vector<uint8_t> data;
  std::chrono::system_clock::time_point timestamp;
};

// Global order book and synchronization
OrderBook order_book;
std::mutex order_book_mutex;
std::atomic<bool> should_stop{false};
std::string filter_ticker = "";
std::vector<PacketEvent> packet_events;
std::mutex packet_events_mutex;
std::atomic<size_t> current_packet_index{0};
std::atomic<bool> is_paused{false};
std::atomic<bool> is_playing{false};

// Load symbol mapping
bool load_symbol_file(const std::string &filename)
{
  std::ifstream file(filename);
  if (!file.is_open())
  {
    std::cerr << "Warning: Could not open symbol file: " << filename << std::endl;
    return false;
  }

  std::string line;
  while (std::getline(file, line))
  {
    std::istringstream iss(line);
    uint32_t index;
    std::string symbol;
    if (iss >> index >> symbol)
    {
      symbol_map[index] = symbol;
    }
  }
  std::cout << "Loaded " << symbol_map.size() << " symbols" << std::endl;
  return true;
}

// Parse XDP message and update order book
void process_xdp_message(const uint8_t *data, size_t max_len, uint16_t msg_type)
{
  if (max_len < 16)
    return;

  uint32_t symbol_index = read_le32(data + 8);
  std::string ticker = get_symbol(symbol_index);

  // Filter by ticker if specified
  if (!filter_ticker.empty() && ticker != filter_ticker)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(order_book_mutex);

  switch (msg_type)
  {
  case 100:
  { // Add Order
    if (max_len >= 39)
    {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      uint8_t side = data[32];
      double price = parse_price(price_raw);
      order_book.add_order(order_id, price, volume, side);
    }
    break;
  }

  case 101:
  { // Modify Order
    if (max_len >= 35)
    {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 24);
      uint32_t volume = read_le32(data + 28);
      double price = parse_price(price_raw);
      order_book.modify_order(order_id, price, volume);
    }
    break;
  }

  case 102:
  { // Delete Order
    if (max_len >= 25)
    {
      uint64_t order_id = read_le64(data + 16);
      order_book.delete_order(order_id);
    }
    break;
  }

  case 103:
  { // Execute Order
    if (max_len >= 42)
    {
      uint64_t order_id = read_le64(data + 16);
      uint32_t price_raw = read_le32(data + 28);
      uint32_t volume = read_le32(data + 32);
      double price = parse_price(price_raw);
      order_book.execute_order(order_id, volume, price);
    }
    break;
  }

  case 104:
  { // Replace Order
    if (max_len >= 42)
    {
      uint64_t old_order_id = read_le64(data + 16);
      uint64_t new_order_id = read_le64(data + 24);
      uint32_t price_raw = read_le32(data + 32);
      uint32_t volume = read_le32(data + 36);
      double price = parse_price(price_raw);
      // Replace = delete old + add new
      order_book.delete_order(old_order_id);
      // Get side from the order (we need to track this, but for now assume 'B')
      // In a full implementation, you'd track the side in the order book
      order_book.add_order(new_order_id, price, volume, 'B');
    }
    break;
  }
  }
}

// Parse XDP packet
void parse_xdp_packet(const uint8_t *data, size_t length)
{
  if (length < 16)
    return;

  uint16_t packet_size = read_le16(data);
  uint8_t num_messages = data[3];

  size_t offset = 16;
  int msg_count = 0;

  while (offset + 4 <= length && msg_count < num_messages)
  {
    size_t remaining = length - offset;
    uint16_t msg_size = read_le16(data + offset);
    uint16_t msg_type = read_le16(data + offset + 2);

    if (msg_size < 4 || msg_size > remaining)
      break;

    process_xdp_message(data + offset, remaining, msg_type);

    offset += msg_size;
    msg_count++;
  }
}

// PCAP packet handler - now stores packets instead of processing directly
void packet_handler(u_char *user_data, const struct pcap_pkthdr *pkthdr,
                    const uint8_t *packet_data)
{
  // Validate packet and extract XDP data
  if (pkthdr->caplen < 14)
    return;

  // Parse Ethernet header
  uint16_t eth_type = ntohs(*(uint16_t *)(packet_data + 12));
  size_t eth_header_len = 14;

  // Skip VLAN tag if present
  if (eth_type == 0x8100 || eth_type == 0x88A8)
  {
    if (pkthdr->caplen < 18)
      return;
    eth_type = ntohs(*(uint16_t *)(packet_data + 16));
    eth_header_len = 18;
  }

  // Only process IPv4
  if (eth_type != 0x0800)
    return;

  // Parse IP header
  if (pkthdr->caplen < eth_header_len + 20)
    return;

  const uint8_t *ip_header = packet_data + eth_header_len;
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

  const uint8_t *udp_header = packet_data + udp_offset;
  uint16_t udp_len = ntohs(*(uint16_t *)(udp_header + 4));

  // Extract UDP payload (XDP data)
  const uint8_t *udp_payload = udp_header + 8;
  size_t payload_len = udp_len - 8;

  if (payload_len > pkthdr->caplen - udp_offset - 8)
  {
    payload_len = pkthdr->caplen - udp_offset - 8;
  }

  // Store packet for playback
  PacketEvent event;
  event.data.assign(udp_payload, udp_payload + payload_len);
  event.timestamp = std::chrono::system_clock::now();

  {
    std::lock_guard<std::mutex> lock(packet_events_mutex);
    packet_events.push_back(event);
  }
}

// Process a single XDP packet
void process_packet_event(const PacketEvent &event)
{
  if (event.data.empty())
    return;
  parse_xdp_packet(event.data.data(), event.data.size());
}

// Playback thread - replays packets from the queue
void playback_thread_func()
{
  while (!should_stop.load())
  {
    if (!is_playing.load() || is_paused.load())
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      continue;
    }

    size_t idx = current_packet_index.load();

    {
      std::lock_guard<std::mutex> lock(packet_events_mutex);
      if (idx < packet_events.size())
      {
        process_packet_event(packet_events[idx]);
        current_packet_index.store(idx + 1);
      }
      else if (idx > 0)
      {
        // End of playback
        is_playing.store(false);
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

// PCAP reading thread - loads all packets into memory
void pcap_reader_thread(const std::string &pcap_file)
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
  pcap_loop(handle, -1, packet_handler, nullptr);
  pcap_close(handle);

  std::cout << "Loaded " << packet_events.size() << " packets" << std::endl;
  std::cout << "Ready to play - press play button to begin" << std::endl;
}

int main(int argc, char *argv[])
{
  std::string pcap_file;
  std::string symbol_file = "data/symbol_nyse.txt";

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
    std::cerr << "Default symbol file: data/symbol_nyse.txt" << std::endl;
    return 1;
  }

  // Load symbol mapping
  load_symbol_file(symbol_file);

  if (!filter_ticker.empty())
  {
    std::cout << "Filtering for ticker: " << filter_ticker << std::endl;
  }

  // Create visualizer
  OrderBookVisualizer visualizer(order_book);

  if (!visualizer.init())
  {
    std::cerr << "Failed to initialize visualizer" << std::endl;
    return 1;
  }

  // Start PCAP reading in a separate thread
  std::thread pcap_thread(pcap_reader_thread, pcap_file);

  // Start playback thread
  std::thread playback_thread(playback_thread_func);

  // Main render loop
  bool running = true;
  while (running)
  {
    if (visualizer.should_close())
    {
      running = false;
      should_stop.store(true);
    }

    // Get playback controls from visualizer
    auto controls = visualizer.get_playback_controls();
    if (controls.play_pressed)
    {
      is_playing.store(true);
      is_paused.store(false);
    }
    if (controls.pause_pressed)
    {
      is_paused.store(true);
    }
    if (controls.reset_pressed)
    {
      // Reset: stop, clear, and reload
      is_playing.store(false);
      is_paused.store(false);
      current_packet_index.store(0);
      order_book.clear();
    }
    if (controls.seek_requested)
    {
      // Seek to requested index
      current_packet_index.store(controls.seek_index);
      // Need to replay from start to the seek point
      order_book.clear();
      for (size_t i = 0; i < controls.seek_index && i < packet_events.size(); ++i)
      {
        process_packet_event(packet_events[i]);
      }
    }

    visualizer.render(current_packet_index.load(), packet_events.size());

    // Small delay to prevent 100% CPU usage
    std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
  }

  // Wait for threads to finish
  pcap_thread.join();
  playback_thread.join();

  visualizer.cleanup();
  return 0;
}
