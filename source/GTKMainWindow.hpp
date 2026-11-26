#ifndef GTKMAINWINDOW_HPP
#define GTKMAINWINDOW_HPP

// GTK4 frontend for WarpNES.
//
// Frames are uploaded as a GdkTexture and drawn by the GSK renderer
// (OpenGL/Vulkan when available), so scaling is GPU-accelerated without a
// separate SDL window or Cairo path. SDL is used only for audio and gamepads.

#include <gtk/gtk.h>
#include <SDL2/SDL.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include "Emulation/ControllerSDL.hpp"
#include "Emulation/PPU.hpp"
#include "Emulation/GameGenie.hpp"

#if !GTK_CHECK_VERSION(4, 10, 0)
#error "WarpNES GTK frontend requires GTK 4.10 or newer"
#endif

class WarpNES;

enum class FilterType {
    NONE,
    HQ2X,
    HQ3X,
    SCALE2X,
    SCALE3X,
    BILINEAR,
    CRT_SCANLINES,
    NTSC,
    SUPER_4XSAI,
    XBRZ2X,
    XBRZ3X,
    XBRZ4X
};

// How the picture is fitted into the window
enum class ScaleMode {
    FIT,      // As large as possible, keep aspect ratio (default)
    INTEGER,  // Largest whole-number multiple, keep aspect ratio
    STRETCH   // Fill the whole area
};

// How pixels are filtered when scaled
enum class Smoothing {
    NEAREST,         // Hard pixels (uneven at non-integer sizes)
    SHARP_BILINEAR,  // Integer nearest prescale + bilinear remainder (default)
    BILINEAR         // Smooth
};

class GTKMainWindow {
public:
    GTKMainWindow();
    ~GTKMainWindow();

    // Runs the application; returns the process exit status.
    int run(int argc, char* argv[], const char* rom_filename);

    PPU* getPPU() { return engine ? engine->getPPU() : nullptr; }
    void setFilter(FilterType filter);
    FilterType getFilter() const { return current_filter; }

    // Called by the NES view widget
    void snapshot_view(GtkWidget* widget, GtkSnapshot* snapshot);

    struct Rect { int x, y, w, h; };

private:
    GtkApplication* app;
    GameGenie* gameGenie;

    // Widgets
    GtkWidget* window;
    GtkWidget* main_vbox;
    GtkWidget* view;          // NesView: draws the current frame texture
    GtkWidget* menubar;
    GtkWidget* status_label;

    // Frame data
    uint16_t* nes_framebuffer;    // Raw NES data (RGB565)
    uint32_t* rgba_framebuffer;   // Converted 0xAARRGGBB
    GdkTexture* frame_texture;
    int texture_width;
    int texture_height;

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
    ScaleMode scale_mode;
    Smoothing smoothing;
    std::string last_rom_dir;

    // Sharp-bilinear: GPU-side integer prescale of the current frame
    GdkTexture* prescaled_texture;
    GdkTexture* prescaled_source;
    int prescaled_factor;

    // Game state
    WarpNES* engine;
    bool game_running;
    bool game_paused;
    std::string pending_rom;

    // Timing (driven by the GTK frame clock)
    guint tick_id;
    gint64 last_frame_time;
    gint64 time_accumulator;

    // Input
    std::unordered_map<guint, bool> key_states;
    // Keys pressed since the last emulated frame. A tap shorter than one
    // frame (press+release between frames) still reaches the game this way.
    std::unordered_map<guint, bool> key_taps;
    bool joystick_initialized;

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

    // Setup
    static void on_activate(GtkApplication* app, gpointer user_data);
    void create_widgets();
    void create_menubar();
    void init_audio();
    void init_gamepads();

    // ROM handling
    bool load_rom(const char* filename);
    void start_frame_loop();
    void stop_frame_loop();
    void shutdown();

    // Resolution management
    void apply_resolution(int width, int height);
    void calculate_render_rect(int source_width, int source_height,
                               int target_width, int target_height, Rect& dest_rect);

    // Audio
    static void audio_callback(void* userdata, uint8_t* buffer, int len);

    // Rendering
    static gboolean tick_callback(GtkWidget* widget, GdkFrameClock* clock, gpointer user_data);
    void present_frame();
    void convert_nes_to_rgba();

    // Input
    static gboolean on_key_pressed(GtkEventControllerKey* controller, guint keyval,
                                   guint keycode, GdkModifierType state, gpointer user_data);
    static void on_key_released(GtkEventControllerKey* controller, guint keyval,
                                guint keycode, GdkModifierType state, gpointer user_data);
    static void on_window_active_changed(GObject* object, GParamSpec* pspec, gpointer user_data);
    void poll_gamepads();
    void update_controller_state();
    void toggle_fullscreen();
    void handle_save_state_key(guint keyval, bool shift);

    // Actions (menu items)
    static void action_open(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_quit(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_reset(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_pause(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_game_genie(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_controls(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_resolution(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_scaling(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_filters(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_fullscreen(GSimpleAction*, GVariant*, gpointer user_data);
    static void action_about(GSimpleAction*, GVariant*, gpointer user_data);
    static gboolean on_close_request(GtkWindow* window, gpointer user_data);

    // Dialogs
    void show_message(const char* heading, const char* detail = nullptr);
    void show_controls_dialog();
    void show_resolution_dialog();
    void show_scaling_dialog();
    void show_filters_dialog();
    void show_about_dialog();
    void show_game_genie_dialog();
    void load_game_genie_codes_file(std::function<void()> on_done);
    void save_game_genie_codes_file();

    // Config
    void set_status_message(const char* message);
    void load_key_mappings();
    void save_key_mappings();
    void load_video_settings();
    void save_video_settings();

    // Filters
    FilterType current_filter;
    uint32_t* filter_buffer;
    int filter_scale;

    void apply_hq2x_filter(uint16_t* input, uint32_t* output, int width, int height);
    void apply_hq3x_filter(uint16_t* input, uint32_t* output, int width, int height);
    void apply_scale2x_filter(uint16_t* input, uint32_t* output, int width, int height);
    void apply_crt_scanlines_filter(uint16_t* input, uint32_t* output, int width, int height);
    void apply_ntsc_filter(uint16_t* input, uint32_t* output, int width, int height);
    uint32_t rgb565_to_rgb888(uint16_t color);

    struct NTSCSettings {
        float hue = 0.0f;
        float saturation = 0.0f;
        float contrast = 0.0f;
        float brightness = 0.0f;
        float gamma = 1.8f;
        float sharpness = 0.2f;
        float resolution = 0.7f;
        float artifacts = 0.0f;
        float fringing = 0.0f;
        float bleed = 0.0f;
    } ntsc_settings;

    void init_ntsc_filter();
    uint32_t apply_ntsc_artifacts(uint32_t color, int x, int y);
    uint32_t blend_colors(uint32_t color1, uint32_t color2, float ratio);
    void apply_gamma_correction(uint32_t* color, float gamma);
    void apply_4xsai_filter(uint16_t* input, uint32_t* output, int width, int height);
    uint32_t interpolate_4xsai(uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4);
    uint32_t mix_colors(uint32_t c1, uint32_t c2, float ratio);
    bool colors_equal(uint32_t c1, uint32_t c2);

    // Game Genie dialog
    struct GameGenieDialogData {
        GTKMainWindow* window;
        GtkWidget* dialog;
        GtkWidget* code_entry;
        GtkWidget* desc_entry;
        GtkWidget* list_box;
        GtkWidget* info_label;
        GameGenie* game_genie;
    };
    static void refresh_codes_list(GameGenieDialogData* data);
    static void on_add_game_genie_code(GtkButton* button, gpointer user_data);
    static void on_load_game_genie_file(GtkButton* button, gpointer user_data);
    static void on_save_game_genie_file(GtkButton* button, gpointer user_data);
    static void on_remove_game_genie_code(GtkButton* button, gpointer user_data);
    static void on_clear_game_genie_codes(GtkButton* button, gpointer user_data);
    static void on_toggle_game_genie_code(GtkCheckButton* check, gpointer user_data);
};

#endif // GTKMAINWINDOW_HPP
