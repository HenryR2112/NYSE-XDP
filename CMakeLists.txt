cmake_minimum_required(VERSION 3.10)
project(NYSE_XDP_Parser VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build configuration
set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type: Debug or Release")
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)

# Find required libraries
# Try pkg-config first, fall back to find_library
find_package(PkgConfig)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBPCAP libpcap)
endif()

# Fallback to find_library if pkg-config didn't work
if(NOT LIBPCAP_FOUND)
  find_library(PCAP_LIBRARY pcap)
  if(PCAP_LIBRARY)
    set(LIBPCAP_LIBRARIES ${PCAP_LIBRARY})
    set(LIBPCAP_FOUND TRUE)
    # Try to find include directory
    find_path(PCAP_INCLUDE_DIR pcap.h
      PATHS
        /usr/include
        /usr/local/include
        /opt/homebrew/include
    )
    if(PCAP_INCLUDE_DIR)
      set(LIBPCAP_INCLUDE_DIRS ${PCAP_INCLUDE_DIR})
    endif()
  endif()
endif()

if(NOT LIBPCAP_FOUND)
  message(FATAL_ERROR "libpcap library not found. Please install libpcap-dev (Linux) or libpcap (macOS)")
endif()

# Main parser executable
add_executable(reader
    reader.cpp
)

target_include_directories(reader PRIVATE
    ${LIBPCAP_INCLUDE_DIRS}
)

target_link_libraries(reader
    ${LIBPCAP_LIBRARIES}
)

# Compiler flags
target_compile_options(reader PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
# Release flags
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

# Generate compile_commands.json for clangd/clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Install targets
install(TARGETS reader
    RUNTIME DESTINATION bin
)