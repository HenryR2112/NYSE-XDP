cmake_minimum_required(VERSION 3.10)
project(nyse_xdp_parser)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPCAP REQUIRED libpcap)

# Main parser executable
add_executable(xdp_parser
    reader.cpp
)

target_include_directories(xdp_parser PRIVATE
    ${LIBPCAP_INCLUDE_DIRS}
)

target_link_libraries(xdp_parser
    ${LIBPCAP_LIBRARIES}
)

# Test utilities
add_executable(test_symbol_lookup
    test_symbol_lookup.cpp
)

add_executable(extract_symbols
    extract_symbols.cpp
)

target_link_libraries(extract_symbols
    ${LIBPCAP_LIBRARIES}
)

# Install targets
install(TARGETS xdp_parser
    RUNTIME DESTINATION bin
)

# Add debug flags for debug builds
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -Wall -Wextra -pedantic")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# Generate compile_commands.json for clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)