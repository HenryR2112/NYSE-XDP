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

# Option to skip visualization targets (useful for headless builds)
option(BUILD_VISUALIZERS "Build visualization targets (requires SDL2 + OpenGL)" ON)

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
    ${SOURCE_DIR}/per_symbol_sim.cpp
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
    pthread
)

# Compiler flags for non-visualization targets
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

# ---- Visualization targets (optional) ----

if(BUILD_VISUALIZERS)
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
      endif()
  endif()

  if(NOT SDL2_FOUND)
      message(WARNING "SDL2 not found. Visualization targets will not be built.\n"
                      "  Install SDL2 to enable: brew install sdl2 (macOS) or apt install libsdl2-dev (Linux)\n"
                      "  Or pass -DBUILD_VISUALIZERS=OFF to suppress this warning.")
      set(BUILD_VISUALIZERS OFF)
  endif()
endif()

if(BUILD_VISUALIZERS)
  # Find OpenGL
  if(APPLE)
      find_library(OPENGL_LIBRARY OpenGL)
  else()
      find_package(OpenGL REQUIRED)
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

  # Visualizer with PCAP support
  add_executable(visualizer_pcap
      ${SOURCE_DIR}/visualizer_pcap.cpp
      ${IMGUI_SOURCES}
  )

  # Include/link for visualizer
  foreach(VIS_TARGET visualizer_pcap)
      target_include_directories(${VIS_TARGET} PRIVATE
          ${CMAKE_SOURCE_DIR}
          ${SOURCE_DIR}
          ${IMGUI_DIR}
          ${IMGUI_DIR}/backends
          ${SDL2_INCLUDE_DIRS}
      )
      target_link_libraries(${VIS_TARGET} PRIVATE ${SDL2_LIBRARIES})
      target_compile_options(${VIS_TARGET} PRIVATE -Wall -Wextra)

      if(APPLE)
          target_link_libraries(${VIS_TARGET} PRIVATE ${OPENGL_LIBRARY})
      else()
          target_link_libraries(${VIS_TARGET} PRIVATE OpenGL::GL)
      endif()
  endforeach()

  # visualizer_pcap needs pcap and symbol_map
  target_include_directories(visualizer_pcap PRIVATE ${LIBPCAP_INCLUDE_DIRS})
  target_link_libraries(visualizer_pcap PRIVATE xdp_common ${LIBPCAP_LIBRARIES})

  # Install visualization targets
  install(TARGETS visualizer_pcap RUNTIME DESTINATION bin)
endif()

# Debug flags
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")

# Release flags - aggressive optimization for maximum throughput
# Note: -ffast-math is intentionally excluded to preserve IEEE 754 semantics
# and ensure cross-compiler reproducibility of floating-point results.
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native")      # Use all CPU features (AVX2, etc.)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mtune=native")      # Tune for current CPU
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -flto")              # Link-time optimization
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -funroll-loops")     # Loop unrolling
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fomit-frame-pointer") # Extra register
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-signed-zeros")  # Safe FP relaxation: -0 == +0
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-trapping-math") # Safe FP relaxation: no FP exceptions

# Generate compile_commands.json for clangd/clang-tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Install targets
install(TARGETS reader market_maker_sim RUNTIME DESTINATION bin)
