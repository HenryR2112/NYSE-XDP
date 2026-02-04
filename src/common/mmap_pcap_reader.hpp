#pragma once

#include "pcap_reader.hpp"
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace xdp {

// PCAP file header structure
struct PcapFileHeader {
  uint32_t magic_number;   // 0xa1b2c3d4 or 0xa1b23c4d (nanoseconds)
  uint16_t version_major;
  uint16_t version_minor;
  int32_t thiszone;
  uint32_t sigfigs;
  uint32_t snaplen;
  uint32_t network;        // Link layer type
};

// PCAP packet header
struct PcapPacketHeader {
  uint32_t ts_sec;
  uint32_t ts_usec;        // or ts_nsec for nanosecond format
  uint32_t incl_len;       // Bytes captured
  uint32_t orig_len;       // Original packet length
};

// High-performance memory-mapped PCAP reader
// Loads entire file into memory for maximum throughput
class MmapPcapReader {
public:
  MmapPcapReader() = default;
  ~MmapPcapReader() { close(); }

  // Non-copyable
  MmapPcapReader(const MmapPcapReader&) = delete;
  MmapPcapReader& operator=(const MmapPcapReader&) = delete;

  // Movable
  MmapPcapReader(MmapPcapReader&& other) noexcept
      : data_(other.data_), size_(other.size_), fd_(other.fd_),
        is_nanosec_(other.is_nanosec_) {
    other.data_ = nullptr;
    other.size_ = 0;
    other.fd_ = -1;
  }

  MmapPcapReader& operator=(MmapPcapReader&& other) noexcept {
    if (this != &other) {
      close();
      data_ = other.data_;
      size_ = other.size_;
      fd_ = other.fd_;
      is_nanosec_ = other.is_nanosec_;
      other.data_ = nullptr;
      other.size_ = 0;
      other.fd_ = -1;
    }
    return *this;
  }

  [[nodiscard]] bool open(const std::string& filename) {
    close();

    fd_ = ::open(filename.c_str(), O_RDONLY);
    if (fd_ < 0) {
      error_ = "Failed to open file: " + filename;
      return false;
    }

    struct stat st;
    if (fstat(fd_, &st) < 0) {
      error_ = "Failed to stat file";
      ::close(fd_);
      fd_ = -1;
      return false;
    }

    size_ = static_cast<size_t>(st.st_size);
    if (size_ < sizeof(PcapFileHeader)) {
      error_ = "File too small for PCAP header";
      ::close(fd_);
      fd_ = -1;
      return false;
    }

    // Memory-map the file
    data_ = static_cast<uint8_t*>(
        mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
    if (data_ == MAP_FAILED) {
      error_ = "Failed to mmap file";
      data_ = nullptr;
      ::close(fd_);
      fd_ = -1;
      return false;
    }

    // Advise kernel for sequential access
    madvise(data_, size_, MADV_SEQUENTIAL);

    // Parse file header
    const auto* file_header = reinterpret_cast<const PcapFileHeader*>(data_);
    if (file_header->magic_number == 0xa1b2c3d4) {
      is_nanosec_ = false;
    } else if (file_header->magic_number == 0xa1b23c4d) {
      is_nanosec_ = true;
    } else {
      error_ = "Invalid PCAP magic number";
      close();
      return false;
    }

    return true;
  }

  void close() {
    if (data_) {
      munmap(data_, size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    size_ = 0;
  }

  [[nodiscard]] bool is_open() const noexcept { return data_ != nullptr; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] size_t file_size() const noexcept { return size_; }

  // Process all packets with callback
  // Returns total number of packets processed
  template <typename Callback>
  size_t process_all(Callback&& callback) {
    if (!data_) return 0;

    size_t offset = sizeof(PcapFileHeader);
    size_t packet_count = 0;

    while (offset + sizeof(PcapPacketHeader) <= size_) {
      const auto* pkt_header =
          reinterpret_cast<const PcapPacketHeader*>(data_ + offset);

      size_t pkt_data_offset = offset + sizeof(PcapPacketHeader);
      if (pkt_data_offset + pkt_header->incl_len > size_) {
        break;  // Truncated packet
      }

      // Calculate timestamp in nanoseconds
      uint64_t timestamp_ns;
      if (is_nanosec_) {
        timestamp_ns = static_cast<uint64_t>(pkt_header->ts_sec) * 1000000000ULL +
                       static_cast<uint64_t>(pkt_header->ts_usec);
      } else {
        timestamp_ns = static_cast<uint64_t>(pkt_header->ts_sec) * 1000000000ULL +
                       static_cast<uint64_t>(pkt_header->ts_usec) * 1000ULL;
      }

      // Parse network headers
      NetworkPacketInfo info{};
      info.timestamp_ns = timestamp_ns;

      const uint8_t* pkt_data = data_ + pkt_data_offset;
      if (parse_network_headers(pkt_data, pkt_header->incl_len, info)) {
        packet_count++;
        callback(info.payload, info.payload_len, packet_count, info);
      }

      offset = pkt_data_offset + pkt_header->incl_len;
    }

    return packet_count;
  }

  // Get raw packet data for a specific range (for parallel processing)
  struct PacketRange {
    size_t start_offset;
    size_t end_offset;
    size_t packet_count;
  };

  // Split file into N ranges for parallel processing
  [[nodiscard]] std::vector<PacketRange> split_into_ranges(size_t num_ranges) const {
    std::vector<PacketRange> ranges;
    if (!data_ || num_ranges == 0) return ranges;

    // First pass: count packets and find split points
    std::vector<size_t> packet_offsets;
    packet_offsets.reserve(100000);  // Pre-allocate for performance

    size_t offset = sizeof(PcapFileHeader);
    while (offset + sizeof(PcapPacketHeader) <= size_) {
      packet_offsets.push_back(offset);
      const auto* pkt_header =
          reinterpret_cast<const PcapPacketHeader*>(data_ + offset);
      offset += sizeof(PcapPacketHeader) + pkt_header->incl_len;
    }

    if (packet_offsets.empty()) return ranges;

    // Create ranges
    size_t packets_per_range = (packet_offsets.size() + num_ranges - 1) / num_ranges;
    ranges.reserve(num_ranges);

    for (size_t i = 0; i < num_ranges && i * packets_per_range < packet_offsets.size(); ++i) {
      size_t start_idx = i * packets_per_range;
      size_t end_idx = std::min((i + 1) * packets_per_range, packet_offsets.size());

      PacketRange range;
      range.start_offset = packet_offsets[start_idx];
      if (end_idx < packet_offsets.size()) {
        range.end_offset = packet_offsets[end_idx];
      } else {
        range.end_offset = size_;
      }
      range.packet_count = end_idx - start_idx;
      ranges.push_back(range);
    }

    return ranges;
  }

  // Process a specific range of packets
  template <typename Callback>
  size_t process_range(const PacketRange& range, Callback&& callback) {
    if (!data_) return 0;

    size_t offset = range.start_offset;
    size_t packet_count = 0;

    while (offset + sizeof(PcapPacketHeader) <= range.end_offset) {
      const auto* pkt_header =
          reinterpret_cast<const PcapPacketHeader*>(data_ + offset);

      size_t pkt_data_offset = offset + sizeof(PcapPacketHeader);
      if (pkt_data_offset + pkt_header->incl_len > size_) {
        break;
      }

      uint64_t timestamp_ns;
      if (is_nanosec_) {
        timestamp_ns = static_cast<uint64_t>(pkt_header->ts_sec) * 1000000000ULL +
                       static_cast<uint64_t>(pkt_header->ts_usec);
      } else {
        timestamp_ns = static_cast<uint64_t>(pkt_header->ts_sec) * 1000000000ULL +
                       static_cast<uint64_t>(pkt_header->ts_usec) * 1000ULL;
      }

      NetworkPacketInfo info{};
      info.timestamp_ns = timestamp_ns;

      const uint8_t* pkt_data = data_ + pkt_data_offset;
      if (parse_network_headers(pkt_data, pkt_header->incl_len, info)) {
        packet_count++;
        callback(info.payload, info.payload_len, packet_count, info);
      }

      offset = pkt_data_offset + pkt_header->incl_len;
    }

    return packet_count;
  }

  // Pre-load entire file into memory (useful before parallel processing)
  void preload() {
    if (!data_) return;
    // Touch every page to ensure it's in memory
    volatile uint8_t sum = 0;
    for (size_t i = 0; i < size_; i += 4096) {
      sum += data_[i];
    }
    (void)sum;
  }

private:
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  int fd_ = -1;
  bool is_nanosec_ = false;
  std::string error_;
};

// Batch packet structure for collecting packets before parallel processing
struct BatchedPacket {
  const uint8_t* payload;
  size_t payload_len;
  uint64_t timestamp_ns;
  uint32_t symbol_index;  // Pre-extracted for sorting
};

} // namespace xdp
