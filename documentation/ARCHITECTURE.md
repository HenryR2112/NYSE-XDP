# NYSE XDP Parser Architecture Documentation

## Table of Contents
1. [System Overview](#system-overview)
2. [Architecture Components](#architecture-components)
3. [Data Flow](#data-flow)
4. [Threading Model](#threading-model)
5. [Data Structures](#data-structures)
6. [Message Processing Pipeline](#message-processing-pipeline)
7. [Visualization System](#visualization-system)
8. [Protocol Parsing](#protocol-parsing)
9. [Performance Considerations](#performance-considerations)

## System Overview

The NYSE XDP Parser is a C++ application designed to parse, process, and visualize market data from NYSE Exchange Data Protocol (XDP) Integrated Feed. The system consists of three main executables:

1. **`reader`**: Command-line parser for analyzing PCAP files and outputting message data
2. **`visualizer`**: Standalone GUI application with sample data for testing
3. **`visualizer_pcap`**: Full-featured GUI application with PCAP file playback and real-time order book visualization

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
├──────────────────┬──────────────────┬───────────────────────┤
│   reader.cpp     │  visualizer.cpp  │  visualizer_pcap.cpp  │
│  (CLI Parser)    │  (Standalone UI) │  (PCAP Playback UI)   │
└────────┬─────────┴────────┬─────────┴───────────┬───────────┘
         │                   │                     │
         └───────────────────┼─────────────────────┘
                             │
         ┌───────────────────▼───────────────────────┐
         │         Core Components                    │
         ├────────────────────────────────────────────┤
         │  • OrderBook (order_book.hpp)             │
         │  • XDP Message Parser                     │
         │  • PCAP Packet Handler                     │
         │  • Symbol Mapping                         │
         └────────────────────────────────────────────┘
                             │
         ┌───────────────────▼───────────────────────┐
         │      External Libraries                    │
         ├────────────────────────────────────────────┤
         │  • libpcap (PCAP parsing)                 │
         │  • SDL2 (Window management)                │
         │  • OpenGL (Rendering)                      │
         │  • Dear ImGui (UI framework)              │
         └────────────────────────────────────────────┘
```

## Architecture Components

### 1. Order Book Module (`order_book.hpp`)

The `OrderBook` class is the core data structure that maintains the current state of the market.

**Key Features:**
- Thread-safe operations using mutex locks
- Separate bid and ask price levels stored in ordered maps
- Active order tracking with order ID lookup
- Real-time statistics calculation (best bid/ask, spread, mid price)
- Support for all order book operations: add, modify, delete, execute, replace

**Data Structure:**
```cpp
class OrderBook {
  std::map<double, uint32_t, std::greater<double>> bids;  // Price descending
  std::map<double, uint32_t, std::less<double>> asks;     // Price ascending
  std::unordered_map<uint64_t, Order> active_orders;       // Order ID -> Order
  mutable std::mutex mtx;                                  // Thread safety
};
```

**Operations:**
- `add_order()`: Add new order to book
- `modify_order()`: Update price/volume of existing order
- `delete_order()`: Remove order from book
- `execute_order()`: Process trade execution (partial or full fill)
- `clear()`: Reset order book state
- `get_stats()`: Get current market statistics (thread-safe snapshot)

### 2. PCAP Processing Module

Handles reading and parsing PCAP files containing XDP market data.

**Components:**
- **Packet Handler**: Extracts UDP payloads from Ethernet/IP/UDP headers
- **XDP Parser**: Parses XDP packet headers and messages
- **Message Router**: Routes messages to appropriate handlers based on message type

**Packet Processing Flow:**
```
Ethernet Frame → IP Packet → UDP Datagram → XDP Packet → XDP Messages
```

### 3. Message Processing Module

Processes individual XDP message types and updates the order book accordingly.

**Supported Message Types:**
- **100 (Add Order)**: New visible order added to book
- **101 (Modify Order)**: Price/volume change with position tracking
- **102 (Delete Order)**: Order removed from book
- **103 (Execute Order)**: Order execution with price and quantity
- **104 (Replace Order)**: Cancel/replace operation

**Message Processing:**
Each message type has a dedicated handler that:
1. Validates message size and structure
2. Extracts relevant fields (order ID, price, volume, side)
3. Updates the order book state
4. Maintains thread safety through mutex locks

### 4. Symbol Mapping Module

Maps numeric symbol indices to ticker symbols for human-readable output.

**Implementation:**
- Loads symbol mappings from text file
- Uses `std::unordered_map<uint32_t, std::string>` for O(1) lookup
- Supports filtering by ticker symbol
- Fallback to numeric index if symbol not found

### 5. Visualization Module

Provides real-time graphical display of order book state using Dear ImGui.

**Components:**
- **OrderBookVisualizer Class**: Main visualization controller
- **Rendering Pipeline**: SDL2 window + OpenGL context + ImGui rendering
- **Playback Controls**: Interactive timeline and playback management
- **Terminal-Style Display**: Traditional market data terminal layout

## Data Flow

### PCAP Visualizer Data Flow

```
┌──────────────┐
│  PCAP File   │
└──────┬───────┘
       │
       ▼
┌─────────────────────┐
│  PCAP Reader Thread │
│  (pcap_reader_thread)│
└──────┬──────────────┘
       │
       │ Extract UDP payloads
       ▼
┌─────────────────────┐
│  Packet Events      │
│  (packet_events[])  │
└──────┬──────────────┘
       │
       │ Store for playback
       ▼
┌─────────────────────┐
│  Playback Thread    │
│  (playback_thread)  │
└──────┬──────────────┘
       │
       │ Process packets sequentially
       ▼
┌─────────────────────┐
│  XDP Message Parser │
│  (parse_xdp_packet) │
└──────┬──────────────┘
       │
       │ Extract messages
       ▼
┌─────────────────────┐
│  Message Processor  │
│  (process_xdp_msg)  │
└──────┬──────────────┘
       │
       │ Update order book
       ▼
┌─────────────────────┐
│  OrderBook          │
│  (order_book)       │
└──────┬──────────────┘
       │
       │ Read state
       ▼
┌─────────────────────┐
│  Visualizer         │
│  (render loop)      │
└─────────────────────┘
```

### Command-Line Reader Data Flow

```
PCAP File → Packet Handler → XDP Parser → Message Parser → Console Output
```

## Threading Model

### PCAP Visualizer Threading

The `visualizer_pcap` application uses a multi-threaded architecture:

1. **Main Thread (UI Thread)**
   - Handles SDL event processing
   - Renders ImGui UI at 60 FPS
   - Reads order book state (thread-safe snapshots)
   - Processes user input (playback controls)

2. **PCAP Reader Thread**
   - Reads PCAP file sequentially
   - Extracts XDP payloads from network packets
   - Stores packet events in memory
   - Runs until file is fully loaded

3. **Playback Thread**
   - Processes stored packet events sequentially
   - Respects play/pause state
   - Updates order book state
   - Handles seek operations

**Synchronization:**
- `order_book_mutex`: Protects order book updates
- `packet_events_mutex`: Protects packet event storage
- `std::atomic<bool>`: Play/pause/stop flags
- `std::atomic<size_t>`: Current packet index

**Thread Safety:**
- Order book operations are protected by mutex locks
- Getters return copies (snapshots) to avoid race conditions
- Atomic variables for state flags
- Mutex-protected packet event storage

## Data Structures

### Order Structure

```cpp
struct Order {
  uint64_t order_id;      // Unique order identifier
  double price;           // Order price
  uint32_t volume;        // Order quantity
  char side;              // 'B' (buy) or 'S' (sell)
  std::chrono::system_clock::time_point timestamp;
};
```

### Packet Event Structure

```cpp
struct PacketEvent {
  std::vector<uint8_t> data;                              // XDP payload
  std::chrono::system_clock::time_point timestamp;        // Capture time
};
```

### Book Statistics Structure

```cpp
struct BookStats {
  double best_bid;        // Highest bid price
  double best_ask;        // Lowest ask price
  double spread;          // best_ask - best_bid
  double mid_price;       // (best_bid + best_ask) / 2
  uint32_t total_bid_qty; // Sum of all bid quantities
  uint32_t total_ask_qty; // Sum of all ask quantities
  int bid_levels;         // Number of bid price levels
  int ask_levels;         // Number of ask price levels
};
```

## Message Processing Pipeline

### XDP Packet Structure

```
┌─────────────────────────────────────┐
│      XDP Packet Header (16 bytes)   │
├─────────────────────────────────────┤
│ Packet Size (2) | Delivery Flag (1) │
│ Message Count (1) | Sequence (4)     │
│ Send Time Seconds (4)               │
│ Send Time Nanoseconds (4)           │
└─────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────┐
│      XDP Message 1 (variable)      │
├─────────────────────────────────────┤
│ Message Size (2) | Message Type (2) │
│ Message-specific data...            │
└─────────────────────────────────────┘
         │
         ▼
┌─────────────────────────────────────┐
│      XDP Message 2 (variable)      │
│      ... (up to Message Count)      │
└─────────────────────────────────────┘
```

### Message Processing Steps

1. **Packet Validation**
   - Verify minimum packet size (16 bytes for header)
   - Check message count field
   - Validate sequence numbers

2. **Message Extraction**
   - Read message size and type from header
   - Extract message payload
   - Validate message size against remaining packet data

3. **Symbol Resolution**
   - Extract symbol index from message
   - Lookup ticker symbol in mapping
   - Apply symbol filter if specified

4. **Order Book Update**
   - Lock order book mutex
   - Parse message-specific fields
   - Execute appropriate order book operation
   - Update statistics
   - Release mutex

## Visualization System

### Rendering Architecture

```
┌─────────────────────────────────────────┐
│         SDL2 Window Manager             │
│  • Window creation                      │
│  • Event handling                       │
│  • OpenGL context                       │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│         OpenGL Renderer                │
│  • Context management                   │
│  • Buffer swapping                      │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│         Dear ImGui                      │
│  • Immediate mode GUI                   │
│  • Widget rendering                     │
│  • Input handling                       │
└──────────────┬──────────────────────────┘
               │
               ▼
┌─────────────────────────────────────────┐
│    OrderBookVisualizer                  │
│  • UI layout                            │
│  • Order book rendering                 │
│  • Playback controls                    │
│  • Statistics display                   │
└─────────────────────────────────────────┘
```

### UI Components

1. **Playback Controls Panel**
   - Play/Pause/Reset buttons
   - Timeline slider for seeking
   - Packet progress indicator

2. **Order Book Display**
   - Terminal-style bid/ask ladder
   - Color-coded price levels (green bids, red asks)
   - Real-time statistics header
   - Level-by-level quantity display

3. **Statistics Display**
   - Best bid/ask prices
   - Spread and mid price
   - Total bid/ask quantities
   - Level counts

### Rendering Loop

```cpp
while (running) {
  1. Process SDL events
  2. Start ImGui frame
  3. Render playback controls
  4. Get order book snapshot (thread-safe)
  5. Render order book display
  6. Render statistics
  7. End ImGui frame
  8. Swap OpenGL buffers
  9. Sleep for ~16ms (60 FPS)
}
```

## Protocol Parsing

### Network Stack Parsing

The system parses the complete network stack:

1. **Ethernet Header** (14 bytes)
   - Extract EtherType
   - Handle VLAN tags if present

2. **IP Header** (20+ bytes)
   - Extract protocol field
   - Extract source/destination IPs
   - Calculate header length

3. **UDP Header** (8 bytes)
   - Extract destination port
   - Extract payload length
   - Calculate payload offset

4. **XDP Payload**
   - Parse XDP packet header
   - Extract individual messages
   - Process each message

### Byte Order Handling

- **Network byte order**: Ethernet, IP, UDP headers (big-endian)
- **XDP byte order**: All XDP fields (little-endian)
- **Conversion**: Use `ntohs()`/`ntohl()` for network fields
- **Direct read**: Use helper functions for XDP fields (`read_le16()`, `read_le32()`, `read_le64()`)

### Price Format

- Prices stored as 32-bit integers
- Divide by 10,000 to get dollar value
- Example: `176540000` → `$17,654.0000`
- Price scale may vary by symbol

## Performance Considerations

### Memory Management

- **Packet Storage**: All packets loaded into memory for playback
- **Order Book**: Efficient map-based storage (O(log n) operations)
- **Symbol Mapping**: Hash map for O(1) lookups
- **Memory Footprint**: Scales with PCAP file size and order book depth

### Threading Performance

- **Lock Contention**: Minimized by using atomic variables for flags
- **Snapshot Pattern**: Order book getters return copies to avoid long-held locks
- **Batch Processing**: Consider batching updates for high-frequency scenarios

### Rendering Performance

- **60 FPS Target**: 16ms frame budget
- **ImGui Efficiency**: Immediate mode rendering is efficient for this use case
- **Minimal Redraws**: Only update when order book changes

### Optimization Opportunities

1. **Lazy Statistics**: Calculate statistics only when requested
2. **Update Batching**: Batch multiple order book updates per frame
3. **Memory Pooling**: Reuse packet event structures
4. **Compression**: Compress stored packet events for large files

## Future Enhancements

### Potential Improvements

1. **Multi-Symbol Support**: Track multiple symbols simultaneously
2. **Historical Playback**: Store order book snapshots at intervals
3. **Export Functionality**: Export order book state to CSV/JSON
4. **Advanced Filtering**: Filter by message type, price range, volume
5. **Performance Metrics**: Display processing statistics (packets/sec, messages/sec)
6. **Network Capture**: Support live network capture in addition to PCAP files
7. **Order Book Replay**: Replay order book state from stored snapshots

### Scalability Considerations

- **Large PCAP Files**: Implement streaming processing for very large files
- **High-Frequency Updates**: Optimize order book operations for high message rates
- **Multiple Symbols**: Support per-symbol order books with efficient lookup
- **Distributed Processing**: Consider parallel packet processing for large files
