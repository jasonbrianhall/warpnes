#ifndef GTK3MAINWINDOW_HPP
#define GTK3MAINWINDOW_HPP

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <SDL2/SDL.h>
#include <cairo.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <vector>
#include "Emulation/ControllerSDL.hpp"
#include "Emulation/PPU.hpp"

// Forward declarations
class WarpNES;
class Controller;

// Rendering backend enum
enum class RenderBackend {
    SDL_HARDWARE,    // Hardware-accelerated SDL
    CAIRO_SOFTWARE, // Software Cairo/2D rendering
    AUTO            // Auto-detect best option
};

class GTK3MainWindow {
public:
    GTK3MainWindow();
    ~GTK3MainWindow();
    
    // Main interface
    bool initialize();
    void run(const char* rom_filename);
    void shutdown();
    PPU* getPPU() { return engine ? engine->getPPU() : nullptr; }
    
    // Rendering backend control
    void setRenderBackend(RenderBackend backend);
    RenderBackend getRenderBackend() const;
    bool switchRenderBackend(RenderBackend new_backend);

private:
    // Rendering backend state
    RenderBackend current_backend;
    RenderBackend preferred_backend;
    bool backend_switching_enabled;
    
    // GTK widgets
    GtkWidget* window;
    GtkWidget* main_vbox;
    GtkWidget* drawing_area;  // Unified drawing area for both backends
    GtkWidget* menubar;
    GtkWidget* status_bar;
    bool force_texture_recreation;
    
    // SDL components (only used when SDL backend is active)
    SDL_Window* sdl_window;
    SDL_Renderer* sdl_renderer;
    SDL_Texture* sdl_texture;
    bool sdl_initialized;
    
    // Cairo components (only used when Cairo backend is active)
    cairo_surface_t* cairo_surface;
    cairo_t* cairo_context;
    uint32_t* cairo_buffer;  // RGBA buffer for Cairo
    bool cairo_initialized;
    
    // Shared framebuffer (converted from NES RGB565 to appropriate format)
    uint16_t* nes_framebuffer;    // Raw NES data (RGB565)
    uint32_t* rgba_framebuffer;   // Converted RGBA data
    int framebuffer_width;
    int framebuffer_height;
    
    // Resolution and scaling
    struct Resolution {
        int width;
        int height;
        const char* name;
    };
    
    static const Resolution PRESET_RESOLUTIONS[];
    static const int NUM_PRESET_RESOLUTIONS;

    int current_resolution_index;
    int custom_width;
    int custom_height;
    bool use_custom_resolution;
    bool maintain_aspect_ratio;
    bool integer_scaling;
    
    // Game state
    WarpNES* engine;
    bool game_running;
    bool game_paused;
    
    // Input handling
    std::unordered_map<guint, bool> key_states;
    
    // Timing
    guint frame_timer_id;
    
    // Status messages
    char status_message[256];
    guint status_message_id;
    
    // Key mappings
    struct KeyMappings {
        guint up = GDK_KEY_Up;
        guint down = GDK_KEY_Down;
        guint left = GDK_KEY_Left;
        guint right = GDK_KEY_Right;
        guint button_a = GDK_KEY_x;
        guint button_b = GDK_KEY_z;
        guint start = GDK_KEY_Return;
        guint select = GDK_KEY_space;
    } player1_keys, player2_keys;

    // Widget creation and setup
    void create_widgets();
    void create_menubar();
    void setup_drawing_area();
    
    // Backend initialization and cleanup
    bool init_sdl_backend();
    bool init_cairo_backend();
    void cleanup_sdl_backend();
    void cleanup_cairo_backend();
    bool detect_best_backend();
    
    // Resolution management
    void apply_resolution(int width, int height);
    void calculate_render_rect(int source_width, int source_height, 
                              int target_width, int target_height, 
                              SDL_Rect& dest_rect);
    
    // Audio callback
    static void audio_callback(void* userdata, uint8_t* buffer, int len);
    
    // Rendering - unified interface
    void render_frame();
    void render_frame_sdl();
    void render_frame_cairo();
    void convert_nes_to_rgba();  // Convert RGB565 to RGBA32
    
    // Cairo-specific rendering callbacks
    static gboolean on_cairo_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_cairo_configure(GtkWidget* widget, GdkEventConfigure* event, gpointer user_data);
    
    // Game loop and timing
    static gboolean frame_update_callback(gpointer user_data);
    
    // Input handling
    static gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
    static gboolean on_key_release(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
    void process_input();
    void update_controller_state();
    
    // Menu callbacks
    static void on_file_open(GtkMenuItem* item, gpointer user_data);
    static void on_file_quit(GtkMenuItem* item, gpointer user_data);
    static void on_game_reset(GtkMenuItem* item, gpointer user_data);
    static void on_game_pause(GtkMenuItem* item, gpointer user_data);
    static void on_options_controls(GtkMenuItem* item, gpointer user_data);
    static void on_options_video(GtkMenuItem* item, gpointer user_data);
    static void on_options_resolution(GtkMenuItem* item, gpointer user_data);
    static void on_options_rendering(GtkMenuItem* item, gpointer user_data);  // NEW
    static void on_help_about(GtkMenuItem* item, gpointer user_data);
    
    // Dialog functions
    void show_controls_dialog();
    void show_video_options_dialog();
    void show_resolution_dialog();
    void show_rendering_dialog();  // NEW
    void show_about_dialog();
    
    // Window callbacks
    static gboolean on_window_delete(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static void on_window_destroy(GtkWidget* widget, gpointer user_data);
    static gboolean on_window_configure(GtkWidget* widget, GdkEventConfigure* event, gpointer user_data);
    
    // Utility functions
    void set_status_message(const char* message);
    void load_key_mappings();
    void save_key_mappings();
    void load_video_settings();
    void save_video_settings();
    const char* backend_to_string(RenderBackend backend);
    RenderBackend string_to_backend(const char* str);
    void handle_sdl_events();
    void handle_sdl_key_event(SDL_Event* event);

};

#endif // GTK3MAINWINDOW_HPP
