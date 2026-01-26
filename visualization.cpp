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

class OrderBookVisualizer {
private:
  OrderBook &order_book;
  SDL_Window *window;
  SDL_GLContext gl_context;

  // Display settings
  int max_display_levels = 20;
  bool show_volume_bars = true;
  bool auto_scale = true;
  float price_range = 5.0f; // +/- from mid price

  // Color settings
  ImVec4 bid_color = ImVec4(0.0f, 1.0f, 0.0f, 0.7f);    // Green
  ImVec4 ask_color = ImVec4(1.0f, 0.0f, 0.0f, 0.7f);    // Red
  ImVec4 spread_color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // Yellow
  ImVec4 trade_color = ImVec4(0.0f, 0.5f, 1.0f, 1.0f);  // Blue

public:
  OrderBookVisualizer(OrderBook &ob) : order_book(ob), window(nullptr) {}

  bool init() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
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

    if (!window) {
      std::cerr << "Window creation failed: " << SDL_GetError() << std::endl;
      return false;
    }

    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
      std::cerr << "OpenGL context creation failed: " << SDL_GetError()
                << std::endl;
      std::cerr << "Trying with compatibility profile..." << std::endl;
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                          SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
      gl_context = SDL_GL_CreateContext(window);
      if (!gl_context) {
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
    if (gl_version) {
      int major = 0, minor = 0;
      sscanf(gl_version, "%d.%d", &major, &minor);
      if (major >= 3 && minor >= 3) {
        glsl_version = "#version 330";
      } else if (major >= 3 && minor >= 2) {
        glsl_version = "#version 150";
      } else {
        glsl_version = "#version 130";
      }
    }
#endif

    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
      std::cerr << "Failed to initialize ImGui OpenGL3 renderer with GLSL: "
                << glsl_version << std::endl;
      std::cerr << "OpenGL Version: " << (gl_version ? gl_version : "Unknown")
                << std::endl;
      return false;
    }

    std::cout << "ImGui initialized successfully with GLSL: " << glsl_version
              << std::endl;
    if (gl_version) {
      std::cout << "OpenGL Version: " << gl_version << std::endl;
    }

    return true;
  }

  void render() {
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

    // Control panel
    ImGui::BeginChild("Controls", ImVec2(300, 0), true);
    render_controls();
    ImGui::EndChild();

    ImGui::SameLine();

    // Order book visualization
    ImGui::BeginChild("Visualization", ImVec2(0, 0), true);
    render_order_book();
    ImGui::EndChild();

    ImGui::End();

    // Rendering
    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  void render_controls() {
    ImGui::Text("Order Book Controls");
    ImGui::Separator();

    ImGui::SliderInt("Max Levels", &max_display_levels, 5, 50);
    ImGui::Checkbox("Show Volume Bars", &show_volume_bars);
    ImGui::Checkbox("Auto Scale", &auto_scale);

    if (!auto_scale) {
      ImGui::SliderFloat("Price Range", &price_range, 0.1f, 20.0f);
    }

    ImGui::Separator();
    ImGui::Text("Colors:");
    ImGui::ColorEdit3("Bid Color", (float *)&bid_color);
    ImGui::ColorEdit3("Ask Color", (float *)&ask_color);
    ImGui::ColorEdit3("Spread Color", (float *)&spread_color);

    ImGui::Separator();

    // Show statistics
    auto stats = order_book.get_stats();
    ImGui::Text("Statistics:");
    ImGui::Text("Best Bid: $%.4f", stats.best_bid);
    ImGui::Text("Best Ask: $%.4f", stats.best_ask);
    ImGui::Text("Spread: $%.4f", stats.spread);
    ImGui::Text("Mid Price: $%.4f", stats.mid_price);
    ImGui::Text("Total Bid Qty: %u", stats.total_bid_qty);
    ImGui::Text("Total Ask Qty: %u", stats.total_ask_qty);
    ImGui::Text("Bid Levels: %d", stats.bid_levels);
    ImGui::Text("Ask Levels: %d", stats.ask_levels);
    ImGui::Text("Last Trade: $%.4f", order_book.get_last_trade());
  }

  void render_order_book() {
    auto stats = order_book.get_stats();
    auto bids = order_book.get_bids();
    auto asks = order_book.get_asks();

    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();

    // Determine price range
    double min_price, max_price;
    if (auto_scale) {
      min_price = stats.best_bid - stats.spread * 5;
      max_price = stats.best_ask + stats.spread * 5;
    } else {
      double mid = stats.mid_price;
      min_price = mid - price_range;
      max_price = mid + price_range;
    }

    // Price scale (left side)
    ImGui::BeginChild("PriceScale", ImVec2(80, canvas_size.y), true);
    for (double price = max_price; price >= min_price;
         price -= (max_price - min_price) / 20.0) {
      ImGui::Text("$%.2f", price);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Main visualization area
    ImGui::BeginChild("BookArea", ImVec2(canvas_size.x - 80, canvas_size.y),
                      true);

    ImVec2 book_pos = ImGui::GetCursorScreenPos();
    ImVec2 book_size = ImGui::GetContentRegionAvail();

    // Draw spread line
    float spread_y = book_pos.y + book_size.y * 0.5f;
    draw_list->AddLine(ImVec2(book_pos.x, spread_y),
                       ImVec2(book_pos.x + book_size.x, spread_y),
                       ImColor(spread_color), 2.0f);

    // Draw bids (below spread)
    int bid_count = 0;
    for (const auto &pair : bids) {
      if (bid_count++ >= max_display_levels)
        break;
      double price = pair.first;
      uint32_t qty = pair.second;
      if (price < min_price)
        continue;

      float price_height =
          (float)((price - min_price) / (max_price - min_price));
      float y = book_pos.y + book_size.y * (1.0f - price_height);
      float width = std::min(qty / 1000.0f, 1.0f) * book_size.x * 0.4f;

      draw_list->AddRectFilled(ImVec2(book_pos.x, y - 2),
                               ImVec2(book_pos.x + width, y + 2),
                               ImColor(bid_color));

      // Price label
      ImGui::SetCursorScreenPos(ImVec2(book_pos.x + width + 5, y - 8));
      ImGui::Text("$%.2f x %u", price, qty);
    }

    // Draw asks (above spread)
    int ask_count = 0;
    for (const auto &pair : asks) {
      if (ask_count++ >= max_display_levels)
        break;
      double price = pair.first;
      uint32_t qty = pair.second;
      if (price > max_price)
        continue;

      float price_height =
          (float)((price - min_price) / (max_price - min_price));
      float y = book_pos.y + book_size.y * (1.0f - price_height);
      float width = std::min(qty / 1000.0f, 1.0f) * book_size.x * 0.4f;

      draw_list->AddRectFilled(ImVec2(book_pos.x + book_size.x - width, y - 2),
                               ImVec2(book_pos.x + book_size.x, y + 2),
                               ImColor(ask_color));

      // Price label
      ImGui::SetCursorScreenPos(
          ImVec2(book_pos.x + book_size.x - width - 50, y - 8));
      ImGui::Text("%u x $%.2f", qty, price);
    }

    ImGui::EndChild();
  }

  void cleanup() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
  }

  bool should_close() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_QUIT) {
        return true;
      }
      if (event.type == SDL_WINDOWEVENT &&
          event.window.event == SDL_WINDOWEVENT_CLOSE &&
          event.window.windowID == SDL_GetWindowID(window)) {
        return true;
      }
    }
    return false;
  }
};

// Main function
int main(int /*argc*/, char * /*argv*/[]) {
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

  if (!visualizer.init()) {
    std::cerr << "Failed to initialize visualizer" << std::endl;
    return 1;
  }

  // Main loop
  bool running = true;
  while (running) {
    if (visualizer.should_close()) {
      running = false;
    }

    visualizer.render();
  }

  visualizer.cleanup();
  return 0;
}