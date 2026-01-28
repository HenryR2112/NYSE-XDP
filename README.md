# NYSE XDP Integrated Feed Parser

A C++ parser for NYSE XDP (Exchange Data Protocol) Integrated Feed market data from PCAP files. This parser extracts and displays real-time order book events, trades, and market data messages from NYSE Pillar feeds.

## Overview

This parser processes PCAP files containing NYSE XDP Integrated Feed data and displays:
- **Order Book Events**: Add, Modify, Replace, Delete orders
- **Trade Messages**: Execute orders, Non-displayed trades, Cross trades
- **Market Data**: Imbalance messages, Retail Price Improvement indicators, Stock summaries
- **Real-time Sequencing**: Messages are displayed in chronological order with microsecond timestamps

## Technical Details

### Protocol Specification
- **XDP Integrated Feed Client Specification v2.3a** (October 25, 2019)
- **XDP Common Client Specification v2.3c**
- Supports NYSE Tape A, B, C, Arca, American, National, and Chicago markets

### Message Types Supported

#### Order Book Messages
- **Msg 100**: Add Order (39 bytes) - New visible order added to book
- **Msg 101**: Modify Order (35 bytes) - Price/volume change with position tracking
- **Msg 102**: Delete Order (25 bytes) - Order removed from book
- **Msg 104**: Replace Order (42 bytes) - Cancel/replace operation

#### Trade Messages
- **Msg 103**: Execute Order (32 bytes) - Order execution with price and quantity
- **Msg 110**: Non-Displayed Trade (32 bytes) - Hidden trade execution
- **Msg 111**: Cross Trade (40 bytes) - Cross trade with type indicator
- **Msg 112**: Trade Cancel (32 bytes) - Trade cancellation
- **Msg 113**: Cross Correction (40 bytes) - Cross trade correction

#### Market Data Messages
- **Msg 105**: Imbalance Message (60 bytes) - Auction imbalance data
- **Msg 106**: Add Order Refresh (39 bytes) - Order refresh during recovery
- **Msg 114**: Retail Price Improvement (17 bytes) - RPI indicator (None/Bid/Offer/Both)
- **Msg 223**: Stock Summary (40 bytes) - OHLC and volume data

### Data Structures

#### XDP Packet Header (16 bytes)
```
Offset  Size  Field
0       2     Packet Size (little-endian)
2       1     Delivery Flag
3       1     Number of Messages
4       4     Sequence Number (little-endian)
8       4     Send Time Seconds (little-endian)
12      4     Send Time Nanoseconds (little-endian)
```

#### XDP Message Header (4 bytes)
```
Offset  Size  Field
0       2     Message Size (little-endian)
2       2     Message Type (little-endian)
```

#### Common Message Fields (16 bytes)
```
Offset  Size  Field
4       4     SourceTimeNS - Nanosecond offset from Time Reference
8       4     SymbolIndex - Symbol ID from mapping
12      4     SymbolSeqNum - Sequence number for this symbol
```

### Price Format
- Prices are stored as 32-bit integers
- Divide by 10,000 to get dollar value (e.g., 176540000 = $17,654.0000)
- Price scale may vary by symbol (check Symbol Index Mapping)

### Byte Order
- All multi-byte fields use **little-endian** byte order
- Network byte order conversion is handled automatically

## Building

### Prerequisites
- C++11 compatible compiler (GCC, Clang)
- CMake 3.10 or higher
- libpcap development libraries

### macOS
```bash
brew install libpcap cmake
```

### Linux (Ubuntu/Debian)
```bash
sudo apt-get install libpcap-dev cmake build-essential
```

### Build with CMake
```bash
mkdir -p build
cd build
cmake ..
make
```

### Build with g++ (direct)
```bash
g++ -std=c++11 reader.cpp -lpcap -o reader
```

## Usage

### Basic Usage
```bash
./reader <pcap_file> [verbose] [symbol_file] [-t ticker]
```

### Arguments
- **pcap_file**: Path to PCAP file containing XDP data (required)
- **verbose**: `0` = simplified output, `1` = detailed verbose output (optional, default: 0)
- **symbol_file**: Path to symbol mapping file (optional, default: data/symbol_nyse.txt for visualizers)
- **-t ticker**: Filter messages for specific ticker symbol (e.g., `-t AAPL`)

### Examples

#### Parse all messages (simplified output)
```bash
./reader data/ny4-xnys-pillar-a-20230822T133000.pcap
```

#### Parse with verbose output
```bash
./reader data/ny4-xnys-pillar-a-20230822T133000.pcap 1
```

#### Filter for specific ticker
```bash
./reader data/ny4-xnys-pillar-a-20230822T133000.pcap 0 data/symbol_nyse.txt -t AAPL
```

#### Verbose output for specific ticker
```bash
./reader data/ny4-xnys-pillar-a-20230822T133000.pcap 1 data/symbol_nyse.txt -t AAPL
```

### Output Format

#### Simplified Mode
```
HH:MM:SS.microseconds MESSAGE_TYPE ticker msg_num [fields]
```

Example:
```
06:39:44.676578 ADD_ORDER AAPL 49119 OrderID=18015498024234362 $17658.0000 100 B
06:39:44.676589 RETAIL_PRICE_IMPROVEMENT AAPL 49120 RPI=Offer
06:39:44.703325 DELETE_ORDER AAPL 49123 OrderID=18015498024228476
```

#### Verbose Mode
```
=== Packet N ===
Source: IP -> Multicast: IP:PORT
Total length: N bytes

XDP Packet Header:
  Packet Size: N bytes
  Delivery Flag: N
  Message Count: N
  Sequence Number: N
  Send Time: HH:MM:SS.microseconds

Messages (N expected):
  [1] Type: 100 (ADD_ORDER)
      Size: 39 bytes
      SourceTimeNS: N
      SymbolIndex: N (AAPL)
      SymbolSeqNum: N
      OrderID: N
      Price: $N
      Volume: N
      Side: BUY/SELL
      FirmID: 'XXXXX'
```

## Symbol Mapping File

The symbol mapping file should be in the format:
```
<symbol_index> <ticker_symbol>
```

Example:
```
1 AAPL
2 MSFT
3 GOOGL
```

If no symbol file is provided or a symbol index is not found, the parser will display the numeric index.

## Sample Data

Sample PCAP files are available from [Databento PCAP Samples](https://databento.com/pcaps#samples).

The included sample file:
- `data/ny4-xnys-pillar-a-20230822T133000.pcap` - NYSE Tape A data from August 22, 2023

## Debugging

### VS Code Debug Configuration
The project includes VS Code launch and tasks configurations for debugging:
- **Build Task**: Compiles the project using CMake
- **Debug Launch**: Attaches debugger to the reader executable

### Manual Debugging
```bash
# Build with debug symbols
cmake -DCMAKE_BUILD_TYPE=Debug ..
make

# Run with gdb
gdb ./reader
(gdb) run data/ny4-xnys-pillar-a-20230822T133000.pcap 0 data/symbol_nyse.txt -t AAPL
```

## Project Structure

```
NYSE-XDP/
├── CMakeLists.txt          # CMake build configuration
├── reader.cpp              # Command-line parser implementation
├── order_book.hpp          # Order book data structure
├── visualization.cpp       # ImGui visualization components
├── visualizer_main.cpp     # PCAP visualizer with playback controls
├── visualizer_pcap.cpp     # Alternative PCAP visualizer implementation
├── README.md               # This file
├── data/                   # Data files
│   ├── ny4-xnys-pillar-a-20230822T133000.pcap
│   └── symbol_nyse.txt     # Symbol index mapping
├── data_sheets/            # Protocol specifications
│   ├── XDP_Integrated_Feed_Client_Specification_v2.3a.pdf
│   └── XDP_Common_Client_Specification_v2.3c.pdf
├── documentation/          # Architecture and design documentation
│   └── ARCHITECTURE.md     # System architecture documentation
└── thirdparty/             # Third-party dependencies
    └── imgui/              # Dear ImGui library
```

## Visualization

The project includes multiple visualization tools for real-time order book analysis:

### 1. Command-Line Reader (`reader`)
Basic parser that outputs XDP messages to console in simplified or verbose format.

### 2. Standalone Visualizer (`visualizer`)
Simple visualizer with sample data for testing the UI components.

### 3. PCAP Visualizer (`visualizer_pcap`)
Full-featured visualizer with PCAP file playback support, featuring:
- **Terminal-Style Order Book**: Traditional bid/ask ladder display
- **Playback Controls**: Play, pause, reset, and timeline seeking
- **Real-time Updates**: Order book updates as packets are processed
- **Symbol Filtering**: Filter by specific ticker symbols
- **Statistics Display**: Best bid/ask, spread, mid price, volume totals

### Building the Visualizers

The visualizers require SDL2 and OpenGL:

**macOS:**
```bash
brew install sdl2
cd build
cmake ..
make
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install libsdl2-dev
cd build
cmake ..
make
```

**Linux (Fedora):**
```bash
sudo dnf install SDL2-devel
cd build
cmake ..
make
```

### Running the Visualizers

**Command-line reader:**
```bash
./build/reader data/ny4-xnys-pillar-a-20230822T133000.pcap 0 data/symbol_nyse.txt -t AAPL
```

**PCAP visualizer with playback:**
```bash
./build/visualizer_pcap data/ny4-xnys-pillar-a-20230822T133000.pcap -t AAPL
# Or specify custom symbol file:
./build/visualizer_pcap data/ny4-xnys-pillar-a-20230822T133000.pcap -t AAPL -s data/symbol_nyse.txt
```

**Standalone visualizer (sample data):**
```bash
./build/visualizer
```

### Visualizer Features

- **Terminal-Style Order Book**: Traditional market data terminal layout showing bid/ask levels
- **Playback Controls**: Interactive timeline with play, pause, reset, and seek functionality
- **Real-time Statistics**: Best bid/ask, spread, mid price, total quantities, level counts
- **Symbol Filtering**: Filter messages by ticker symbol via command-line
- **Thread-Safe Updates**: Multi-threaded packet processing with safe order book updates
- **Customizable Display**: Adjustable number of levels, colors, and display options

## Implementation Notes

- Uses `#pragma pack(push, 1)` to ensure structure packing matches binary format
- Little-endian byte order conversion for all multi-byte fields
- Symbol mapping loaded into `std::unordered_map` for O(1) lookup
- Message counters tracked per symbol for sequencing
- Filtering happens after symbol lookup to minimize processing

## License

This project is for educational and research purposes. NYSE XDP protocol specifications are copyright Intercontinental Exchange, Inc.

## Documentation

- **[Architecture Documentation](documentation/ARCHITECTURE.md)**: Comprehensive system architecture, component design, data flow, and threading model documentation

## References

- [NYSE XDP Integrated Feed Client Specification v2.3a](data_sheets/XDP_Integrated_Feed_Client_Specification_v2.3a.pdf)
- [NYSE XDP Common Client Specification v2.3c](data_sheets/XDP_Common_Client_Specification_v2.3c.pdf)
- [Databento PCAP Samples](https://databento.com/pcaps#samples)
