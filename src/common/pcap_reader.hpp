#pragma once

#include "xdp_types.hpp"
#include "xdp_utils.hpp"
#include <arpa/inet.h>
#include <cstdint>
#include <functional>
#include <pcap.h>
#include <string>

namespace xdp {

// Ethernet type constants
constexpr uint16_t ETH_TYPE_IPV4 = 0x0800;
constexpr uint16_t ETH_TYPE_VLAN = 0x8100;
constexpr uint16_t ETH_TYPE_QINQ = 0x88A8;

// IP protocol constants
constexpr uint8_t IP_PROTOCOL_UDP = 17;

// Header sizes
constexpr size_t ETH_HEADER_SIZE = 14;
constexpr size_t ETH_VLAN_HEADER_SIZE = 18;
constexpr size_t MIN_IP_HEADER_SIZE = 20;
constexpr size_t UDP_HEADER_SIZE = 8;

// Network packet info extracted from Ethernet/IP/UDP headers
struct NetworkPacketInfo {
  char src_ip[INET_ADDRSTRLEN];
  char dst_ip[INET_ADDRSTRLEN];
  uint16_t src_port;
  uint16_t dst_port;
  const uint8_t *payload;
  size_t payload_len;
  uint64_t timestamp_ns; // Packet capture timestamp in nanoseconds
};

// Callback type for processing XDP messages
// Parameters: message data, message length, message type, packet timestamp (ns)
using MessageCallback =
    std::function<void(const uint8_t *, size_t, uint16_t, uint64_t)>;

// Callback type for processing raw XDP packets (after network header parsing)
// Parameters: packet data, packet length, packet number, network info
using PacketCallback =
    std::function<void(const uint8_t *, size_t, uint64_t, const NetworkPacketInfo &)>;

// Parse network headers and extract UDP payload
// Returns true if this is a valid UDP packet with payload
[[nodiscard]] inline bool parse_network_headers(const uint8_t *packet,
                                                size_t caplen,
                                                NetworkPacketInfo &info) {
  if (caplen < ETH_HEADER_SIZE)
    return false;

  // Parse Ethernet header
  uint16_t eth_type = ntohs(*reinterpret_cast<const uint16_t *>(packet + 12));
  size_t eth_header_len = ETH_HEADER_SIZE;

  // Handle VLAN tags
  if (eth_type == ETH_TYPE_VLAN || eth_type == ETH_TYPE_QINQ) {
    if (caplen < ETH_VLAN_HEADER_SIZE)
      return false;
    eth_type = ntohs(*reinterpret_cast<const uint16_t *>(packet + 16));
    eth_header_len = ETH_VLAN_HEADER_SIZE;
  }

  // Only process IPv4
  if (eth_type != ETH_TYPE_IPV4)
    return false;

  // Parse IP header
  if (caplen < eth_header_len + MIN_IP_HEADER_SIZE)
    return false;

  const uint8_t *ip_header = packet + eth_header_len;
  uint8_t ip_ver_ihl = ip_header[0];
  uint8_t ip_header_len = (ip_ver_ihl & 0x0F) * 4;
  uint8_t protocol = ip_header[9];

  // Extract IP addresses
  inet_ntop(AF_INET, ip_header + 12, info.src_ip, sizeof(info.src_ip));
  inet_ntop(AF_INET, ip_header + 16, info.dst_ip, sizeof(info.dst_ip));

  // Check for UDP
  if (protocol != IP_PROTOCOL_UDP)
    return false;

  // Parse UDP header
  size_t udp_offset = eth_header_len + ip_header_len;
  if (caplen < udp_offset + UDP_HEADER_SIZE)
    return false;

  const uint8_t *udp_header = packet + udp_offset;
  info.src_port = ntohs(*reinterpret_cast<const uint16_t *>(udp_header));
  info.dst_port = ntohs(*reinterpret_cast<const uint16_t *>(udp_header + 2));
  uint16_t udp_len = ntohs(*reinterpret_cast<const uint16_t *>(udp_header + 4));

  // Extract UDP payload
  info.payload = udp_header + UDP_HEADER_SIZE;
  info.payload_len = udp_len - UDP_HEADER_SIZE;

  // Validate payload length against captured data
  if (info.payload_len > caplen - udp_offset - UDP_HEADER_SIZE) {
    info.payload_len = caplen - udp_offset - UDP_HEADER_SIZE;
  }

  return info.payload_len > 0;
}

// Parse XDP packet and invoke callback for each message
inline void parse_xdp_packet(const uint8_t *data, size_t length,
                             uint64_t timestamp_ns,
                             const MessageCallback &callback) {
  if (length < PACKET_HEADER_SIZE)
    return;

  PacketHeader pkt_header;
  if (!parse_packet_header(data, length, pkt_header))
    return;

  size_t offset = PACKET_HEADER_SIZE;
  uint8_t msg_count = 0;

  while (offset + MESSAGE_HEADER_SIZE <= length &&
         msg_count < pkt_header.num_messages) {
    MessageHeader msg_header;
    if (!parse_message_header(data + offset, length - offset, msg_header))
      break;

    if (!validate_message_size(msg_header.msg_size, length - offset))
      break;

    // Invoke callback with message data
    callback(data + offset, msg_header.msg_size, msg_header.msg_type,
             timestamp_ns);

    offset += msg_header.msg_size;
    msg_count++;
  }
}

// Simple PCAP file reader class
class PcapReader {
public:
  PcapReader() = default;
  ~PcapReader() { close(); }

  // Non-copyable
  PcapReader(const PcapReader &) = delete;
  PcapReader &operator=(const PcapReader &) = delete;

  // Movable
  PcapReader(PcapReader &&other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  PcapReader &operator=(PcapReader &&other) noexcept {
    if (this != &other) {
      close();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  // Open a PCAP file
  [[nodiscard]] bool open(const std::string &filename) {
    close();
    char errbuf[PCAP_ERRBUF_SIZE];
    handle_ = pcap_open_offline(filename.c_str(), errbuf);
    if (!handle_) {
      error_ = errbuf;
      return false;
    }
    return true;
  }

  // Close the PCAP file
  void close() {
    if (handle_) {
      pcap_close(handle_);
      handle_ = nullptr;
    }
  }

  // Check if file is open
  [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }

  // Get last error message
  [[nodiscard]] const std::string &error() const noexcept { return error_; }

  // Process all packets using pcap_loop
  // Returns number of packets processed, or -1 on error
  int process_all(const PacketCallback &callback) {
    if (!handle_)
      return -1;

    struct CallbackData {
      const PacketCallback *callback;
      uint64_t packet_count;
    };

    CallbackData data{&callback, 0};

    auto pcap_callback = [](u_char *user, const struct pcap_pkthdr *header,
                            const u_char *packet) {
      auto *data = reinterpret_cast<CallbackData *>(user);
      data->packet_count++;

      NetworkPacketInfo info{};
      info.timestamp_ns = static_cast<uint64_t>(header->ts.tv_sec) * 1000000000ULL +
                          static_cast<uint64_t>(header->ts.tv_usec) * 1000ULL;

      if (parse_network_headers(packet, header->caplen, info)) {
        (*data->callback)(info.payload, info.payload_len, data->packet_count,
                          info);
      }
    };

    int result =
        pcap_loop(handle_, 0, pcap_callback, reinterpret_cast<u_char *>(&data));

    if (result == -1) {
      error_ = pcap_geterr(handle_);
      return -1;
    }

    return static_cast<int>(data.packet_count);
  }

  // Process packets one at a time
  // Returns true if a packet was read, false on EOF or error
  [[nodiscard]] bool next_packet(NetworkPacketInfo &info) {
    if (!handle_)
      return false;

    struct pcap_pkthdr header;
    const u_char *packet = pcap_next(handle_, &header);

    if (!packet)
      return false;

    info.timestamp_ns = static_cast<uint64_t>(header.ts.tv_sec) * 1000000000ULL +
                        static_cast<uint64_t>(header.ts.tv_usec) * 1000ULL;

    return parse_network_headers(packet, header.caplen, info);
  }

private:
  pcap_t *handle_ = nullptr;
  std::string error_;
};

} // namespace xdp
