#ifndef GTK3MAINWINDOW_HPP
#define GTK3MAINWINDOW_HPP

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <SDL2/SDL.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <vector>
#include "Emulation/ControllerSDL.hpp"

// Forward declarations
class WarpNES;
class Controller;

class GTK3MainWindow {
public:
    GTK3MainWindow();
    ~GTK3MainWindow();
    
    // Main interface
    bool initialize();
    void run(const char* rom_filename);
    void shutdown();

private:
    // GTK widgets
    GtkWidget* window;
    GtkWidget* main_vbox;
    GtkWidget* sdl_socket;
    GtkWidget* menubar;
    GtkWidget* status_bar;
    
    // SDL components
    SDL_Window* sdl_window;
    SDL_Renderer* sdl_renderer;
    SDL_Texture* sdl_texture;
    
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
    void setup_sdl_area();
    
    // SDL setup and management
    bool init_sdl();
    void cleanup_sdl();
    
    // Rendering
    void render_frame();
    
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
    static void on_help_about(GtkMenuItem* item, gpointer user_data);
    
    // Dialog functions
    void show_controls_dialog();
    void show_video_options_dialog();
    void show_about_dialog();
    
    // Window callbacks
    static gboolean on_window_delete(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static void on_window_destroy(GtkWidget* widget, gpointer user_data);
    
    // Utility functions
    void set_status_message(const char* message);
    void load_key_mappings();
    void save_key_mappings();
};

#endif // GTK3MAINWINDOW_HPP
