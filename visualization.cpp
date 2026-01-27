// visualization.cpp - Dear ImGui integration
#define GL_SILENCE_DEPRECATION // Silence OpenGL deprecation warnings on macOS
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
#include <iostream>
#include <string>
#include <vector>

struct PlaybackControls
{
  bool play_pressed = false;
  bool pause_pressed = false;
  bool reset_pressed = false;
  bool seek_requested = false;
  size_t seek_index = 0;
};

class OrderBookVisualizer
{
private:
  OrderBook &order_book;
  SDL_Window *window;
  SDL_GLContext gl_context;

  // Display settings
  int max_display_levels = 20;
  bool show_volume_bars = true;
  bool auto_scale = true;
  float price_range = 5.0f; // +/- from mid price

  // Playback controls
  PlaybackControls playback_controls;
  bool play_button_pressed = false;
  bool pause_button_pressed = false;
  bool reset_button_pressed = false;
  float timeline_pos = 0.0f;

  // Color settings
  ImVec4 bid_color = ImVec4(0.0f, 1.0f, 0.0f, 0.8f);     // Green
  ImVec4 ask_color = ImVec4(1.0f, 0.0f, 0.0f, 0.8f);     // Red
  ImVec4 spread_color = ImVec4(1.0f, 1.0f, 0.0f, 0.2f);  // Yellow
  ImVec4 text_color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);    // White
  ImVec4 background = ImVec4(0.05f, 0.05f, 0.05f, 1.0f); // Dark gray

public:
  OrderBookVisualizer(OrderBook &ob) : order_book(ob), window(nullptr) {}

  bool init()
  {
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
      std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
      return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
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
      std::cerr << "OpenGL context creation failed: " << SDL_GetError()
                << std::endl;
      std::cerr << "Trying with compatibility profile..." << std::endl;
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                          SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
      gl_context = SDL_GL_CreateContext(window);
      if (!gl_context)
      {
        std::cerr << "Failed to create OpenGL context" << std::endl;
        return false;
      }
    }

    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Detect OpenGL version and use appropriate GLSL version
    const char *gl_version = (const char *)glGetString(GL_VERSION);
    const char *glsl_version = "#version 150"; // Default for OpenGL 3.2+

// On macOS, OpenGL 3.2+ requires GLSL 150 or higher
#ifdef __APPLE__
    glsl_version = "#version 150";
#else
                                               // For other platforms, try to
                                               // detect from OpenGL version
    if (gl_version)
    {
      int major = 0, minor = 0;
      sscanf(gl_version, "%d.%d", &major, &minor);
      if (major >= 3 && minor >= 3)
      {
        glsl_version = "#version 330";
      }
      else if (major >= 3 && minor >= 2)
      {
        glsl_version = "#version 150";
      }
      else
      {
        glsl_version = "#version 130";
      }
    }
#endif

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    if (!ImGui_ImplOpenGL3_Init(glsl_version))
    {
      std::cerr << "Failed to initialize ImGui OpenGL3 renderer with GLSL: "
                << glsl_version << std::endl;
      std::cerr << "OpenGL Version: " << (gl_version ? gl_version : "Unknown")
                << std::endl;
      return false;
    }

    std::cout << "ImGui initialized successfully with GLSL: " << glsl_version
              << std::endl;
    if (gl_version)
    {
      std::cout << "OpenGL Version: " << gl_version << std::endl;
    }

    return true;
  }

  void render()
  {
    ImGuiIO &io = ImGui::GetIO();

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Main window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Order Book", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Control panel and playback
    ImGui::BeginChild("Controls", ImVec2(0, 120), true);
    render_playback_controls();
    ImGui::EndChild();

    // Main order book display
    ImGui::BeginChild("OrderBook", ImVec2(0, 0), true);
    render_order_book_terminal_style();
    ImGui::EndChild();

    ImGui::End();

    // Rendering
    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(background.x, background.y, background.z, background.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  void render(size_t current_packet_idx, size_t total_packets)
  {
    ImGuiIO &io = ImGui::GetIO();

    // Start the Dear ImGui frame
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    // Main window
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("Order Book", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Playback controls section
    ImGui::BeginChild("Controls", ImVec2(0, 130), true);
    render_playback_controls(current_packet_idx, total_packets);
    ImGui::EndChild();

    // Main order book display
    ImGui::BeginChild("OrderBook", ImVec2(0, 0), true);
    render_order_book_terminal_style();
    ImGui::EndChild();

    ImGui::End();

    // Rendering
    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(background.x, background.y, background.z, background.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  void render_playback_controls()
  {
    ImGui::Text("Playback Controls");
    ImGui::SameLine(200);

    if (ImGui::Button("Play##single", ImVec2(60, 0)))
    {
      playback_controls.play_pressed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause", ImVec2(60, 0)))
    {
      playback_controls.pause_pressed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(60, 0)))
    {
      playback_controls.reset_pressed = true;
    }
  }

  void render_playback_controls(size_t current_packet_idx, size_t total_packets)
  {
    ImGui::Text("Playback Controls");
    ImGui::SameLine(200);

    if (ImGui::Button("Play##single", ImVec2(60, 0)))
    {
      playback_controls.play_pressed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause", ImVec2(60, 0)))
    {
      playback_controls.pause_pressed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(60, 0)))
    {
      playback_controls.reset_pressed = true;
    }

    ImGui::Separator();
    ImGui::Text("Timeline: %zu / %zu packets", current_packet_idx, total_packets);

    // Timeline slider
    static float seek_pos = 0.0f;
    float normalized_pos = total_packets > 0 ? (float)current_packet_idx / total_packets : 0.0f;
    if (ImGui::SliderFloat("##timeline", &normalized_pos, 0.0f, 1.0f, "%.1f%%"))
    {
      size_t target_idx = (size_t)(normalized_pos * total_packets);
      playback_controls.seek_requested = true;
      playback_controls.seek_index = target_idx;
    }
  }

  void render_order_book_terminal_style()
  {
    auto stats = order_book.get_stats();
    auto bids = order_book.get_bids();
    auto asks = order_book.get_asks();

    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "ORDER BOOK - MID: $%.2f | SPREAD: $%.4f",
                       stats.mid_price, stats.spread);
    ImGui::Separator();

    // Header row
    ImGui::TextColored(text_color, "%-12s %-15s %-15s", "ASK SIZE", "ASK PRICE", "BID PRICE");
    ImGui::TextColored(text_color, "%-12s %-15s %-15s", "", "", "BID SIZE");
    ImGui::Separator();

    int max_levels = std::min(max_display_levels,
                              (int)std::max(bids.size(), asks.size()));

    // Display from best ask down, and best bid up
    std::vector<std::pair<double, uint32_t>> asks_vec(asks.begin(), asks.end());
    std::vector<std::pair<double, uint32_t>> bids_vec(bids.begin(), bids.end());

    // Reverse asks to show best ask at top
    std::reverse(asks_vec.begin(), asks_vec.end());

    int ask_idx = 0;
    int bid_idx = 0;

    for (int level = 0; level < max_levels; level++)
    {
      std::string ask_str = "                    ";
      std::string bid_str = "                    ";

      if (ask_idx < (int)asks_vec.size())
      {
        char buf[100];
        double price = asks_vec[ask_idx].first;
        uint32_t qty = asks_vec[ask_idx].second;
        snprintf(buf, sizeof(buf), "%u @ $%.2f", qty, price);
        ask_str = buf;
        ask_idx++;
      }

      if (bid_idx < (int)bids_vec.size())
      {
        char buf[100];
        double price = bids_vec[bid_idx].first;
        uint32_t qty = bids_vec[bid_idx].second;
        snprintf(buf, sizeof(buf), "%.2f @ %u", price, qty);
        bid_str = buf;
        bid_idx++;
      }

      // Color code: red for asks, green for bids
      if (ask_idx <= (int)asks_vec.size() - 1)
      {
        ImGui::TextColored(ask_color, "%-12s %-15s %-15s", "", ask_str.c_str(), "");
      }
      else
      {
        ImGui::TextColored(text_color, "%-12s %-15s %-15s", "", "", "");
      }

      ImGui::SameLine(300);

      if (bid_idx <= (int)bids_vec.size())
      {
        ImGui::TextColored(bid_color, "%-15s", bid_str.c_str());
      }
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                       "Bid Levels: %d | Ask Levels: %d | Bid Vol: %u | Ask Vol: %u",
                       stats.bid_levels, stats.ask_levels,
                       stats.total_bid_qty, stats.total_ask_qty);
  }

  PlaybackControls get_playback_controls()
  {
    auto controls = playback_controls;
    // Reset one-time events
    playback_controls.play_pressed = false;
    playback_controls.pause_pressed = false;
    playback_controls.reset_pressed = false;
    playback_controls.seek_requested = false;
    return controls;
  }

  void cleanup()
  {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
  }

  bool should_close()
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
};

// Main function
int main(int /*argc*/, char * /*argv*/[])
{
  // Create a test order book
  OrderBook order_book;

  // Add some sample data for testing
  order_book.add_order(1, 150.00, 100, 'B');
  order_book.add_order(2, 149.99, 200, 'B');
  order_book.add_order(3, 149.98, 150, 'B');
  order_book.add_order(4, 150.01, 100, 'S');
  order_book.add_order(5, 150.02, 250, 'S');
  order_book.add_order(6, 150.03, 300, 'S');

  // Create visualizer
  OrderBookVisualizer visualizer(order_book);

  if (!visualizer.init())
  {
    std::cerr << "Failed to initialize visualizer" << std::endl;
    return 1;
  }

  // Main loop
  bool running = true;
  while (running)
  {
    if (visualizer.should_close())
    {
      running = false;
    }

    visualizer.render();
  }

  visualizer.cleanup();
  return 0;
}