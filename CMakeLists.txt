cmake_minimum_required(VERSION 3.14)
project(NYSE_XDP_Parser VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build configuration
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Debug or Release" FORCE)
endif()
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

# Source directory
set(SOURCE_DIR ${CMAKE_SOURCE_DIR}/src)
set(COMMON_DIR ${SOURCE_DIR}/common)

# Common library sources
set(COMMON_SOURCES
    ${COMMON_DIR}/symbol_map.cpp
)

# Common library (header-only + symbol_map implementation)
add_library(xdp_common STATIC ${COMMON_SOURCES})
target_include_directories(xdp_common PUBLIC ${SOURCE_DIR})
target_compile_options(xdp_common PRIVATE -Wall -Wextra -Wpedantic)

# Main parser executable
add_executable(reader
    ${SOURCE_DIR}/reader.cpp
)

# Market maker simulator executable
add_executable(market_maker_sim
    ${SOURCE_DIR}/market_maker_sim.cpp
    ${SOURCE_DIR}/market_maker.cpp
)

target_include_directories(reader PRIVATE
    ${SOURCE_DIR}
    ${LIBPCAP_INCLUDE_DIRS}
)

target_link_libraries(reader PRIVATE
    xdp_common
    ${LIBPCAP_LIBRARIES}
)

target_include_directories(market_maker_sim PRIVATE
    ${SOURCE_DIR}
    ${LIBPCAP_INCLUDE_DIRS}
)

target_link_libraries(market_maker_sim PRIVATE
    xdp_common
    ${LIBPCAP_LIBRARIES}
)

# Compiler flags
target_compile_options(reader PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_compile_options(market_maker_sim PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Find SDL2 for visualization
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    # Try to find SDL2 manually (common on macOS with Homebrew)
    find_path(SDL2_INCLUDE_DIR SDL.h
        PATHS
            /usr/local/include/SDL2
            /opt/homebrew/include/SDL2
            /usr/include/SDL2
    )
    find_library(SDL2_LIBRARY
        NAMES SDL2
        PATHS
            /usr/local/lib
            /opt/homebrew/lib
            /usr/lib
    )
    if(SDL2_INCLUDE_DIR AND SDL2_LIBRARY)
        set(SDL2_FOUND TRUE)
        set(SDL2_INCLUDE_DIRS ${SDL2_INCLUDE_DIR})
        set(SDL2_LIBRARIES ${SDL2_LIBRARY})
    else()
        message(FATAL_ERROR "SDL2 not found. Please install SDL2:\n  macOS: brew install sdl2\n  Ubuntu/Debian: sudo apt-get install libsdl2-dev\n  Fedora: sudo dnf install SDL2-devel")
    endif()
endif()

# ImGui source files
set(IMGUI_DIR ${SOURCE_DIR}/thirdparty/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

# Visualization executable (standalone with sample data)
add_executable(visualizer
    ${SOURCE_DIR}/visualization.cpp
    ${IMGUI_SOURCES}
)

# Visualizer with PCAP support
add_executable(visualizer_pcap
    ${SOURCE_DIR}/visualizer_pcap.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(visualizer PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(visualizer PRIVATE
    ${SDL2_LIBRARIES}
)

# Visualizer with PCAP support
target_include_directories(visualizer_pcap PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${SDL2_INCLUDE_DIRS}
    ${LIBPCAP_INCLUDE_DIRS}
)

target_link_libraries(visualizer_pcap PRIVATE
    xdp_common
    ${SDL2_LIBRARIES}
    ${LIBPCAP_LIBRARIES}
)

# OpenGL linking for visualizer_pcap
if(APPLE)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(visualizer_pcap PRIVATE ${OPENGL_LIBRARY})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(visualizer_pcap PRIVATE OpenGL::GL)
endif()

# Compiler flags for visualizer_pcap
target_compile_options(visualizer_pcap PRIVATE
    -Wall
    -Wextra
)

# OpenGL linking
if(APPLE)
    find_library(OPENGL_LIBRARY OpenGL)
    target_link_libraries(visualizer PRIVATE ${OPENGL_LIBRARY})
else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(visualizer PRIVATE OpenGL::GL)
endif()

# Compiler flags for visualizer
target_compile_options(visualizer PRIVATE
    -Wall
    -Wextra
)

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
# Release flags
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")

# Generate compile_commands.json for clangd/clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Install targets
install(TARGETS reader visualizer visualizer_pcap market_maker_sim
    RUNTIME DESTINATION bin
)