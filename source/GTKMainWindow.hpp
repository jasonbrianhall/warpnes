#ifndef GTK3MAINWINDOW_HPP
#define GTK3MAINWINDOW_HPP

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <vector>

// Forward declarations for your existing classes
class WarpNES;
class Controller;

/**
 * GTK3MainWindow - Modern GTK3 interface for WarpNES
 * 
 * This class provides a native Linux desktop experience using GTK3
 * with hardware-accelerated OpenGL rendering for the NES emulation.
 * It integrates with the existing WarpNES engine and Controller system.
 * 
 * Uses GTK3's built-in GtkGLArea widget for OpenGL support.
 */
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
    GtkWidget* gl_area;
    GtkWidget* menubar;
    GtkWidget* status_bar;
    
    // OpenGL resources
    GLuint nes_texture;
    GLuint shader_program;
    GLuint vertex_buffer;
    GLuint vertex_array;
    GLuint element_buffer;
    
    // Game state - uses your existing WarpNES engine
    WarpNES* engine;
    bool game_running;
    bool game_paused;
    uint16_t frame_buffer[256 * 240];  // NES resolution
    
    // Input handling
    std::unordered_map<guint, bool> key_states;
    
    // Timing
    std::chrono::high_resolution_clock::time_point last_frame_time;
    guint frame_timer_id;
    
    // Status messages
    char status_message[256];
    guint status_message_id;
    
    // Key mappings (compatible with your existing control system)
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

private:
    // Widget creation and setup
    void create_widgets();
    void create_menubar();
    void setup_gl_area();
    
    // OpenGL setup and management
    bool init_opengl();
    void setup_shaders();
    void setup_vertex_buffer();
    void cleanup_opengl();
    
    // Rendering pipeline
    void render_frame();
    void update_texture();
    
    // Game loop and timing
    static gboolean frame_update_callback(gpointer user_data);
    void update_game();
    
    // Input handling
    static gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
    static gboolean on_key_release(GtkWidget* widget, GdkEventKey* event, gpointer user_data);
    void process_input();
    void update_controller_state();
    
    // GTK3 GtkGLArea callbacks
    static gboolean on_gl_draw(GtkGLArea* area, GdkGLContext* context, gpointer user_data);
    static void on_gl_realize(GtkGLArea* area, gpointer user_data);
    static void on_gl_unrealize(GtkGLArea* area, gpointer user_data);
    static void on_gl_resize(GtkGLArea* area, gint width, gint height, gpointer user_data);
    
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
    
    // OpenGL helper functions
    bool check_gl_extension(const char* extension);
    void print_gl_info();
};

#endif // GTK3MAINWINDOW_HPP
