#include <SDL2/SDL.h>
#include <algorithm>
#include <cmath>
#include "GTKMainWindow.hpp"
#include "Emulation/WarpNES.hpp"
#include "Emulation/ControllerSDL.hpp"
#include "Emulation/GameGenie.hpp"
#include "Configuration.hpp"
#include "Constants.hpp"
#include "XBRZScaler.hpp"

// NES runs at ~60.0988 frames per second (NTSC)
static const gint64 NES_FRAME_US = 16639;

// ---------------------------------------------------------------------------
// NesView: minimal GtkWidget that draws the current frame texture. Drawing
// goes through GtkSnapshot, so GSK scales the texture on the GPU.
// ---------------------------------------------------------------------------
struct NesView {
    GtkWidget parent_instance;
    GTKMainWindow* owner;
};
struct NesViewClass {
    GtkWidgetClass parent_class;
};
G_DEFINE_TYPE(NesView, nes_view, GTK_TYPE_WIDGET)

static void nes_view_snapshot(GtkWidget* widget, GtkSnapshot* snapshot) {
    NesView* self = reinterpret_cast<NesView*>(widget);
    if (self->owner) {
        self->owner->snapshot_view(widget, snapshot);
    }
}

static void nes_view_class_init(NesViewClass* klass) {
    GTK_WIDGET_CLASS(klass)->snapshot = nes_view_snapshot;
}

static void nes_view_init(NesView* self) {
    self->owner = nullptr;
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
}

static GtkWidget* nes_view_new(GTKMainWindow* owner) {
    NesView* view = static_cast<NesView*>(g_object_new(nes_view_get_type(), nullptr));
    view->owner = owner;
    return GTK_WIDGET(view);
}

// ---------------------------------------------------------------------------
// Small dialog helper: modal window with a content box and a button row.
// ---------------------------------------------------------------------------
static GtkWidget* make_dialog(GtkWindow* parent, const char* title,
                              GtkWidget** content, GtkWidget** buttons) {
    GtkWidget* dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), parent);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);

    GtkWidget* vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), vbox);

    *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(*content, TRUE);
    gtk_box_append(GTK_BOX(vbox), *content);

    *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(*buttons, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(vbox), *buttons);
    return dialog;
}

static GtkWidget* add_button(GtkWidget* button_box, const char* label, GCallback cb, gpointer data) {
    GtkWidget* button = gtk_button_new_with_mnemonic(label);
    g_signal_connect(button, "clicked", cb, data);
    gtk_box_append(GTK_BOX(button_box), button);
    return button;
}

static GtkWidget* framed(const char* title, GtkWidget** inner) {
    GtkWidget* frame = gtk_frame_new(title);
    *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(*inner, 8);
    gtk_widget_set_margin_bottom(*inner, 8);
    gtk_widget_set_margin_start(*inner, 8);
    gtk_widget_set_margin_end(*inner, 8);
    gtk_frame_set_child(GTK_FRAME(frame), *inner);
    return frame;
}

static void close_dialog_cb(GtkButton* button, gpointer) {
    gtk_window_destroy(GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button))));
}

template <typename T>
static void attach_data(GtkWidget* dialog, T* data) {
    g_object_set_data_full(G_OBJECT(dialog), "dialog-data", data,
                           [](gpointer p) { delete static_cast<T*>(p); });
}

// ---------------------------------------------------------------------------

const GTKMainWindow::Resolution GTKMainWindow::PRESET_RESOLUTIONS[] = {
    {256, 240, "1x Native (256x240)"},
    {512, 480, "2x Native (512x480)"},
    {768, 720, "3x Native (768x720)"},
    {1024, 960, "4x Native (1024x960)"},
    {1280, 1200, "5x Native (1280x1200)"},
    {640, 480, "VGA (640x480)"},
    {800, 600, "SVGA (800x600)"},
    {1024, 768, "XGA (1024x768)"},
    {1280, 720, "HD 720p (1280x720)"},
    {1366, 768, "WXGA (1366x768)"},
    {1920, 1080, "Full HD 1080p (1920x1080)"},
    {2560, 1440, "QHD 1440p (2560x1440)"},
    {3840, 2160, "4K UHD (3840x2160)"}
};

const int GTKMainWindow::NUM_PRESET_RESOLUTIONS = sizeof(PRESET_RESOLUTIONS) / sizeof(PRESET_RESOLUTIONS[0]);

GTKMainWindow::GTKMainWindow()
    : app(nullptr), gameGenie(nullptr),
      window(nullptr), main_vbox(nullptr), view(nullptr), menubar(nullptr), status_label(nullptr),
      nes_framebuffer(nullptr), rgba_framebuffer(nullptr), frame_texture(nullptr),
      texture_width(256), texture_height(240),
      current_resolution_index(0), custom_width(800), custom_height(600),
      use_custom_resolution(false), scale_mode(ScaleMode::FIT), smoothing(Smoothing::SHARP_BILINEAR), crop_overscan(true),
      prescaled_texture(nullptr), prescaled_source(nullptr), prescaled_factor(0),
      engine(nullptr), game_running(false), game_paused(false),
      tick_id(0), last_frame_time(0), time_accumulator(0),
      joystick_initialized(false),
      current_filter(FilterType::NONE), filter_buffer(nullptr), filter_scale(1)
{
    nes_framebuffer = new uint16_t[256 * 240]();
    rgba_framebuffer = new uint32_t[256 * 240]();

    player1_keys.up = GDK_KEY_Up;
    player1_keys.down = GDK_KEY_Down;
    player1_keys.left = GDK_KEY_Left;
    player1_keys.right = GDK_KEY_Right;
    player1_keys.button_a = GDK_KEY_x;
    player1_keys.button_b = GDK_KEY_z;
    player1_keys.start = GDK_KEY_bracketright;
    player1_keys.select = GDK_KEY_bracketleft;

    player2_keys.up = GDK_KEY_w;
    player2_keys.down = GDK_KEY_s;
    player2_keys.left = GDK_KEY_a;
    player2_keys.right = GDK_KEY_d;
    player2_keys.button_a = GDK_KEY_k;
    player2_keys.button_b = GDK_KEY_j;
    player2_keys.start = GDK_KEY_i;
    player2_keys.select = GDK_KEY_u;
}

GTKMainWindow::~GTKMainWindow() {
    shutdown();
    if (frame_texture) g_object_unref(frame_texture);
    if (prescaled_texture) g_object_unref(prescaled_texture);
    if (app) g_object_unref(app);
    delete[] nes_framebuffer;
    delete[] rgba_framebuffer;
    delete[] filter_buffer;
}

// ---------------------------------------------------------------------------
// Application lifecycle
// ---------------------------------------------------------------------------
int GTKMainWindow::run(int argc, char* argv[], const char* rom_filename) {
    if (rom_filename) pending_rom = rom_filename;

    app = gtk_application_new("io.github.jasonbrianhall.warpnes", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), this);

    // Arguments are parsed by main(); don't let GApplication try to "open" the ROM.
    char* app_argv[] = { argc > 0 ? argv[0] : (char*)"warpnes-gtk", nullptr };
    return g_application_run(G_APPLICATION(app), 1, app_argv);
}

void GTKMainWindow::on_activate(GtkApplication*, gpointer user_data) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);
    if (self->window) {
        gtk_window_present(GTK_WINDOW(self->window));
        return;
    }

    self->load_key_mappings();
    self->load_video_settings();
    self->create_widgets();
    self->init_audio();

    gtk_window_maximize(GTK_WINDOW(self->window));
    gtk_window_present(GTK_WINDOW(self->window));
    gtk_widget_grab_focus(self->view);

    self->set_status_message("WarpNES - Load a ROM to begin (File > Open ROM)");

    if (!self->pending_rom.empty()) {
        self->load_rom(self->pending_rom.c_str());
    }
}

void GTKMainWindow::init_audio() {
    if (!Configuration::getAudioEnabled()) return;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        printf("WARNING: Could not init SDL audio: %s\n", SDL_GetError());
        return;
    }
    SDL_AudioSpec desiredSpec;
    SDL_zero(desiredSpec);
    desiredSpec.freq = Configuration::getAudioFrequency();
    desiredSpec.format = AUDIO_S8;
    desiredSpec.channels = 1;
    desiredSpec.samples = 2048;
    desiredSpec.callback = audio_callback;
    desiredSpec.userdata = this;

    SDL_AudioSpec obtainedSpec;
    if (SDL_OpenAudio(&desiredSpec, &obtainedSpec) < 0) {
        printf("WARNING: Could not open audio: %s\n", SDL_GetError());
    } else {
        SDL_PauseAudio(0);
        printf("Audio initialized successfully\n");
    }
}

void GTKMainWindow::audio_callback(void* userdata, uint8_t* buffer, int len) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(userdata);
    if (self && self->engine && self->game_running && !self->game_paused) {
        self->engine->audioCallback(buffer, len);
    } else {
        memset(buffer, 0, len);
    }
}

void GTKMainWindow::init_gamepads() {
    joystick_initialized = false;
    if (!engine) return;
    joystick_initialized = engine->getController1().initJoystick();
    if (joystick_initialized) {
        printf("Gamepad initialized\n");
    }
}

bool GTKMainWindow::load_rom(const char* filename) {
    // Keep the audio thread out of the engine while it is replaced
    SDL_LockAudio();
    game_running = false;
    delete gameGenie;
    gameGenie = nullptr;
    delete engine;
    engine = new WarpNES();
    bool ok = engine->loadROM(filename);
    if (ok) {
        engine->reset();
        // The frame clock runs the emulator at real NES speed
        engine->setAudioFrameRate(1000000.0 / NES_FRAME_US);
    } else {
        delete engine;
        engine = nullptr;
    }
    SDL_UnlockAudio();

    if (!ok) {
        set_status_message("Failed to load ROM file");
        show_message("Failed to load ROM", filename);
        return false;
    }

    key_states.clear();
    key_taps.clear();
    init_gamepads();

    // Remember where ROMs live for the next Open dialog
    char* abs_path = g_canonicalize_filename(filename, nullptr);
    char* dir = g_path_get_dirname(abs_path);
    last_rom_dir = dir;
    g_free(dir);
    g_free(abs_path);
    save_video_settings();

    gameGenie = new GameGenie(engine);
    size_t codesLoaded = 0;
    if (gameGenie->loadCodesFromFile(engine->getROMBaseName() + ".gg")) {
        codesLoaded = gameGenie->getEnabledCodeCount();
    }

    game_paused = false;
    last_frame_time = 0;
    time_accumulator = 0;
    game_running = true;

    char status_msg[512];
    if (codesLoaded > 0) {
        snprintf(status_msg, sizeof(status_msg), "ROM loaded: %s - Auto-loaded %zu Game Genie codes",
                 filename, codesLoaded);
    } else {
        snprintf(status_msg, sizeof(status_msg), "ROM loaded: %s", filename);
    }
    set_status_message(status_msg);
    start_frame_loop();
    return true;
}

void GTKMainWindow::start_frame_loop() {
    if (!tick_id && view) {
        tick_id = gtk_widget_add_tick_callback(view, tick_callback, this, nullptr);
    }
}

void GTKMainWindow::stop_frame_loop() {
    if (tick_id && view) {
        gtk_widget_remove_tick_callback(view, tick_id);
    }
    tick_id = 0;
}

void GTKMainWindow::shutdown() {
    stop_frame_loop();
    if (window) {
        save_key_mappings();
        save_video_settings();
    }

    SDL_LockAudio();
    game_running = false;
    delete gameGenie;
    gameGenie = nullptr;
    delete engine;
    engine = nullptr;
    SDL_UnlockAudio();

    if (SDL_WasInit(SDL_INIT_AUDIO)) {
        SDL_CloseAudio();
    }
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------
void GTKMainWindow::create_widgets() {
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "WarpNES");
    gtk_window_set_default_size(GTK_WINDOW(window), 1024, 1000);
    g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), this);
    g_signal_connect(window, "notify::is-active", G_CALLBACK(on_window_active_changed), this);
    g_signal_connect(window, "notify::fullscreened", G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
        GTKMainWindow* self = static_cast<GTKMainWindow*>(data);
        bool fs = gtk_window_is_fullscreen(GTK_WINDOW(obj));
        gtk_widget_set_visible(self->menubar, !fs);
        gtk_widget_set_visible(self->status_label, !fs);
    }), this);

    // Keyboard: capture phase so game keys work regardless of focused child
    GtkEventController* keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), this);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), this);
    gtk_widget_add_controller(window, keys);

    main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(window), main_vbox);

    create_menubar();
    gtk_box_append(GTK_BOX(main_vbox), menubar);

    view = nes_view_new(this);
    gtk_widget_set_size_request(view, 256, 240);
    gtk_widget_set_hexpand(view, TRUE);
    gtk_widget_set_vexpand(view, TRUE);
    gtk_box_append(GTK_BOX(main_vbox), view);

    status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(status_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_start(status_label, 6);
    gtk_widget_set_margin_end(status_label, 6);
    gtk_widget_set_margin_top(status_label, 2);
    gtk_widget_set_margin_bottom(status_label, 2);
    gtk_box_append(GTK_BOX(main_vbox), status_label);
}

void GTKMainWindow::create_menubar() {
    static const GActionEntry entries[] = {
        {"open", action_open, nullptr, nullptr, nullptr, {0}},
        {"quit", action_quit, nullptr, nullptr, nullptr, {0}},
        {"reset", action_reset, nullptr, nullptr, nullptr, {0}},
        {"pause", action_pause, nullptr, nullptr, nullptr, {0}},
        {"game-genie", action_game_genie, nullptr, nullptr, nullptr, {0}},
        {"controls", action_controls, nullptr, nullptr, nullptr, {0}},
        {"resolution", action_resolution, nullptr, nullptr, nullptr, {0}},
        {"scaling", action_scaling, nullptr, nullptr, nullptr, {0}},
        {"filters", action_filters, nullptr, nullptr, nullptr, {0}},
        {"fullscreen", action_fullscreen, nullptr, nullptr, nullptr, {0}},
        {"about", action_about, nullptr, nullptr, nullptr, {0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(window), entries, G_N_ELEMENTS(entries), this);

    const char* open_accels[] = {"<Control>o", nullptr};
    const char* quit_accels[] = {"<Control>q", "<Alt>x", nullptr};
    const char* reset_accels[] = {"<Control>r", nullptr};
    const char* pause_accels[] = {"Pause", "<Control>p", nullptr};
    const char* fullscreen_accels[] = {"F11", nullptr};
    gtk_application_set_accels_for_action(app, "win.open", open_accels);
    gtk_application_set_accels_for_action(app, "win.quit", quit_accels);
    gtk_application_set_accels_for_action(app, "win.reset", reset_accels);
    gtk_application_set_accels_for_action(app, "win.pause", pause_accels);
    gtk_application_set_accels_for_action(app, "win.fullscreen", fullscreen_accels);

    GMenu* bar = g_menu_new();

    GMenu* file_menu = g_menu_new();
    g_menu_append(file_menu, "_Open ROM…", "win.open");
    g_menu_append(file_menu, "_Quit", "win.quit");
    g_menu_append_submenu(bar, "_File", G_MENU_MODEL(file_menu));

    GMenu* game_menu = g_menu_new();
    g_menu_append(game_menu, "_Reset", "win.reset");
    g_menu_append(game_menu, "_Pause", "win.pause");
    GMenu* game_section = g_menu_new();
    g_menu_append(game_section, "_Game Genie Codes…", "win.game-genie");
    g_menu_append_section(game_menu, nullptr, G_MENU_MODEL(game_section));
    g_menu_append_submenu(bar, "_Game", G_MENU_MODEL(game_menu));

    GMenu* options_menu = g_menu_new();
    g_menu_append(options_menu, "_Controls…", "win.controls");
    g_menu_append(options_menu, "_Scaling…", "win.scaling");
    g_menu_append(options_menu, "Window _Size…", "win.resolution");
    g_menu_append(options_menu, "_Filters…", "win.filters");
    g_menu_append(options_menu, "F_ullscreen", "win.fullscreen");
    g_menu_append_submenu(bar, "_Options", G_MENU_MODEL(options_menu));

    GMenu* help_menu = g_menu_new();
    g_menu_append(help_menu, "_About", "win.about");
    g_menu_append_submenu(bar, "_Help", G_MENU_MODEL(help_menu));

    menubar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(bar));

    g_object_unref(file_menu);
    g_object_unref(game_section);
    g_object_unref(game_menu);
    g_object_unref(options_menu);
    g_object_unref(help_menu);
    g_object_unref(bar);
}

void GTKMainWindow::set_status_message(const char* message) {
    if (status_label) {
        gtk_label_set_text(GTK_LABEL(status_label), message);
    }
}

void GTKMainWindow::show_message(const char* heading, const char* detail) {
    GtkAlertDialog* alert = gtk_alert_dialog_new("%s", heading);
    if (detail) gtk_alert_dialog_set_detail(alert, detail);
    gtk_alert_dialog_show(alert, window ? GTK_WINDOW(window) : nullptr);
    g_object_unref(alert);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
gboolean GTKMainWindow::tick_callback(GtkWidget*, GdkFrameClock* clock, gpointer user_data) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);
    gint64 now = gdk_frame_clock_get_frame_time(clock);

    if (!self->game_running || !self->engine) {
        self->last_frame_time = now;
        return G_SOURCE_CONTINUE;
    }

    self->poll_gamepads();

    if (self->game_paused || self->last_frame_time == 0) {
        self->last_frame_time = now;
        self->time_accumulator = 0;
        return G_SOURCE_CONTINUE;
    }

    // Run the emulator at NES speed independent of the display refresh rate
    self->time_accumulator += now - self->last_frame_time;
    self->last_frame_time = now;
    if (self->time_accumulator > 250000) {
        // Stalled (window hidden, debugger, ...): don't try to catch up
        self->time_accumulator = NES_FRAME_US;
    }

    int frames = 0;
    while (self->time_accumulator >= NES_FRAME_US && frames < 4) {
        self->update_controller_state();
        self->key_taps.clear();
        self->engine->update();
        self->time_accumulator -= NES_FRAME_US;
        frames++;
    }

    if (frames > 0) {
        self->present_frame();
    }
    return G_SOURCE_CONTINUE;
}

void GTKMainWindow::present_frame() {
    if (!engine) return;
    engine->render16(nes_framebuffer);

    uint32_t* pixels = rgba_framebuffer;
    int width = 256;
    int height = 240;

    bool filtered = false;
    if (current_filter != FilterType::NONE && filter_buffer) {
        filtered = true;
        switch (current_filter) {
            case FilterType::SCALE2X:       apply_scale2x_filter(nes_framebuffer, filter_buffer, 256, 240); break;
            case FilterType::HQ2X:          apply_hq2x_filter(nes_framebuffer, filter_buffer, 256, 240); break;
            case FilterType::HQ3X:          apply_hq3x_filter(nes_framebuffer, filter_buffer, 256, 240); break;
            case FilterType::CRT_SCANLINES: apply_crt_scanlines_filter(nes_framebuffer, filter_buffer, 256, 240); break;
            case FilterType::NTSC:          apply_ntsc_filter(nes_framebuffer, filter_buffer, 256, 240); break;
            case FilterType::SUPER_4XSAI:   apply_4xsai_filter(nes_framebuffer, filter_buffer, 256, 240); break;
            case FilterType::XBRZ2X:
            case FilterType::XBRZ3X:
            case FilterType::XBRZ4X:
                convert_nes_to_rgba();
                xbrzStyleScale(filter_scale, rgba_framebuffer, filter_buffer, 256, 240);
                break;
            default:                        filtered = false; break;
        }
    }

    if (filtered) {
        pixels = filter_buffer;
        width = 256 * filter_scale;
        height = 240 * filter_scale;
        // Filters don't all guarantee an opaque alpha channel
        for (int i = 0; i < width * height; i++) pixels[i] |= 0xFF000000u;
    } else {
        convert_nes_to_rgba();
    }

    // NTSC TVs hide roughly the top and bottom 8 lines, and many games
    // (e.g. Final Fantasy while scrolling vertically) draw garbage there.
    if (crop_overscan) {
        int scale = height / 240;
        pixels += (size_t)8 * scale * width;
        height -= 16 * scale;
    }

    // 0xAARRGGBB in native endianness == GDK_MEMORY_DEFAULT
    GBytes* bytes = g_bytes_new(pixels, (gsize)width * height * 4);
    GdkTexture* texture = gdk_memory_texture_new(width, height, GDK_MEMORY_DEFAULT, bytes, (gsize)width * 4);
    g_bytes_unref(bytes);

    if (frame_texture) g_object_unref(frame_texture);
    frame_texture = texture;
    texture_width = width;
    texture_height = height;
    gtk_widget_queue_draw(view);
}

void GTKMainWindow::convert_nes_to_rgba() {
    for (int i = 0; i < 256 * 240; i++) {
        rgba_framebuffer[i] = rgb565_to_rgb888(nes_framebuffer[i]);
    }
}

void GTKMainWindow::snapshot_view(GtkWidget* widget, GtkSnapshot* snapshot) {
    int width = gtk_widget_get_width(widget);
    int height = gtk_widget_get_height(widget);

    GdkRGBA black = {0.0f, 0.0f, 0.0f, 1.0f};
    graphene_rect_t area = GRAPHENE_RECT_INIT(0.0f, 0.0f, (float)width, (float)height);
    gtk_snapshot_append_color(snapshot, &black, &area);

    if (!frame_texture || !game_running) return;

    // Lay out by NES resolution so filters (2x/3x/4x textures) keep the same size
    Rect dest;
    calculate_render_rect(256, crop_overscan ? 224 : 240, width, height, dest);
    graphene_rect_t bounds = GRAPHENE_RECT_INIT((float)dest.x, (float)dest.y, (float)dest.w, (float)dest.h);

    if (smoothing == Smoothing::NEAREST) {
        gtk_snapshot_append_scaled_texture(snapshot, frame_texture, GSK_SCALING_FILTER_NEAREST, &bounds);
        return;
    }
    if (smoothing == Smoothing::BILINEAR) {
        gtk_snapshot_append_scaled_texture(snapshot, frame_texture, GSK_SCALING_FILTER_LINEAR, &bounds);
        return;
    }

    // Sharp bilinear: scale up by the largest whole factor with nearest
    // neighbour (on the GPU), then bilinear for the small remainder. Pixels
    // stay crisp but evenly sized at any window size.
    int device_scale = gtk_widget_get_scale_factor(widget);
    int device_w = dest.w * device_scale;
    int device_h = dest.h * device_scale;
    if (device_w % texture_width == 0 && device_h % texture_height == 0) {
        // Exact multiple: nearest is already perfect
        gtk_snapshot_append_scaled_texture(snapshot, frame_texture, GSK_SCALING_FILTER_NEAREST, &bounds);
        return;
    }

    int factor = std::min(device_w / texture_width, device_h / texture_height);
    GdkTexture* source = frame_texture;
    if (factor >= 2) {
        if (prescaled_source != frame_texture || prescaled_factor != factor || !prescaled_texture) {
            GskRenderer* renderer = gtk_native_get_renderer(gtk_widget_get_native(widget));
            if (renderer) {
                graphene_rect_t big = GRAPHENE_RECT_INIT(0.0f, 0.0f,
                                                         (float)(texture_width * factor),
                                                         (float)(texture_height * factor));
                GskRenderNode* node = gsk_texture_scale_node_new(frame_texture, &big, GSK_SCALING_FILTER_NEAREST);
                GdkTexture* scaled = gsk_renderer_render_texture(renderer, node, &big);
                gsk_render_node_unref(node);
                if (prescaled_texture) g_object_unref(prescaled_texture);
                prescaled_texture = scaled;
                prescaled_source = frame_texture;
                prescaled_factor = factor;
            }
        }
        if (prescaled_texture && prescaled_source == frame_texture) source = prescaled_texture;
    }
    gtk_snapshot_append_scaled_texture(snapshot, source, GSK_SCALING_FILTER_LINEAR, &bounds);
}

void GTKMainWindow::calculate_render_rect(int source_width, int source_height,
                                          int target_width, int target_height, Rect& dest_rect) {
    if (scale_mode == ScaleMode::STRETCH) {
        dest_rect = {0, 0, target_width, target_height};
        return;
    }

    if (scale_mode == ScaleMode::INTEGER) {
        int scale = std::max(1, std::min(target_width / source_width, target_height / source_height));
        dest_rect.w = source_width * scale;
        dest_rect.h = source_height * scale;
    } else {
        // Fit: as large as possible with the source aspect ratio
        double scale = std::min((double)target_width / source_width, (double)target_height / source_height);
        dest_rect.w = (int)std::lround(source_width * scale);
        dest_rect.h = (int)std::lround(source_height * scale);
    }
    dest_rect.x = (target_width - dest_rect.w) / 2;
    dest_rect.y = (target_height - dest_rect.h) / 2;
}

void GTKMainWindow::apply_resolution(int width, int height) {
    if (gtk_window_is_fullscreen(GTK_WINDOW(window))) {
        gtk_window_unfullscreen(GTK_WINDOW(window));
    }
    gtk_window_unmaximize(GTK_WINDOW(window));

    // Size the window so the game area (not the whole window) gets width x height
    int menu_h = 0, status_h = 0, unused = 0;
    gtk_widget_measure(menubar, GTK_ORIENTATION_VERTICAL, -1, &unused, &menu_h, nullptr, nullptr);
    gtk_widget_measure(status_label, GTK_ORIENTATION_VERTICAL, -1, &unused, &status_h, nullptr, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(window), width, height + menu_h + status_h);

    char status_msg[256];
    snprintf(status_msg, sizeof(status_msg), "Resolution changed to %dx%d", width, height);
    set_status_message(status_msg);
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
gboolean GTKMainWindow::on_key_pressed(GtkEventControllerKey*, guint keyval, guint,
                                       GdkModifierType state, gpointer user_data) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);
    guint key = gdk_keyval_to_lower(keyval);

    if (key == GDK_KEY_Escape && gtk_window_is_fullscreen(GTK_WINDOW(self->window))) {
        gtk_window_unfullscreen(GTK_WINDOW(self->window));
        return TRUE;
    }

    if (key >= GDK_KEY_F5 && key <= GDK_KEY_F9) {
        self->handle_save_state_key(key, (state & GDK_SHIFT_MASK) != 0);
        return TRUE;
    }

    // Leave Ctrl/Alt shortcuts to the menu accelerators
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) {
        return FALSE;
    }

    const KeyMappings* maps[] = {&self->player1_keys, &self->player2_keys};
    for (const KeyMappings* m : maps) {
        if (key == m->up || key == m->down || key == m->left || key == m->right ||
            key == m->button_a || key == m->button_b || key == m->start || key == m->select) {
            self->key_states[key] = true;
            self->key_taps[key] = true;
            return TRUE;  // Don't let arrow keys etc. move focus around
        }
    }
    return FALSE;
}

void GTKMainWindow::on_key_released(GtkEventControllerKey*, guint keyval, guint,
                                    GdkModifierType, gpointer user_data) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);
    self->key_states[gdk_keyval_to_lower(keyval)] = false;
}

void GTKMainWindow::on_window_active_changed(GObject* object, GParamSpec*, gpointer user_data) {
    // Key releases aren't delivered while another window has focus; drop all
    // held keys so nothing stays stuck after alt-tab or a dialog.
    if (!gtk_window_is_active(GTK_WINDOW(object))) {
        GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);
        self->key_states.clear();
        self->key_taps.clear();
    }
}

void GTKMainWindow::poll_gamepads() {
    if (!joystick_initialized || !engine) return;

    Controller& controller1 = engine->getController1();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_JOYAXISMOTION:
            case SDL_JOYHATMOTION:
            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
            case SDL_CONTROLLERAXISMOTION:
                controller1.processJoystickEvent(event);
                break;
            default:
                break;
        }
    }
    controller1.updateJoystickState();
}

void GTKMainWindow::update_controller_state() {
    if (!engine) return;

    Controller& controller1 = engine->getController1();
    Controller& controller2 = engine->getController2();

    auto key = [this](guint k) {
        auto it = key_states.find(k);
        if (it != key_states.end() && it->second) return true;
        auto tap = key_taps.find(k);
        return tap != key_taps.end() && tap->second;
    };
    // Gamepads are tracked separately from the keyboard so neither latches the other
    auto pad = [&](Player p, ControllerButton b) {
        return joystick_initialized && controller1.getJoystickButtonState(p, b);
    };

    controller1.setButtonState(PLAYER_1, BUTTON_UP,     key(player1_keys.up)       || pad(PLAYER_1, BUTTON_UP));
    controller1.setButtonState(PLAYER_1, BUTTON_DOWN,   key(player1_keys.down)     || pad(PLAYER_1, BUTTON_DOWN));
    controller1.setButtonState(PLAYER_1, BUTTON_LEFT,   key(player1_keys.left)     || pad(PLAYER_1, BUTTON_LEFT));
    controller1.setButtonState(PLAYER_1, BUTTON_RIGHT,  key(player1_keys.right)    || pad(PLAYER_1, BUTTON_RIGHT));
    controller1.setButtonState(PLAYER_1, BUTTON_A,      key(player1_keys.button_a) || pad(PLAYER_1, BUTTON_A));
    controller1.setButtonState(PLAYER_1, BUTTON_B,      key(player1_keys.button_b) || pad(PLAYER_1, BUTTON_B));
    controller1.setButtonState(PLAYER_1, BUTTON_START,  key(player1_keys.start)    || pad(PLAYER_1, BUTTON_START));
    controller1.setButtonState(PLAYER_1, BUTTON_SELECT, key(player1_keys.select)   || pad(PLAYER_1, BUTTON_SELECT));

    controller2.setButtonState(PLAYER_2, BUTTON_UP,     key(player2_keys.up)       || pad(PLAYER_2, BUTTON_UP));
    controller2.setButtonState(PLAYER_2, BUTTON_DOWN,   key(player2_keys.down)     || pad(PLAYER_2, BUTTON_DOWN));
    controller2.setButtonState(PLAYER_2, BUTTON_LEFT,   key(player2_keys.left)     || pad(PLAYER_2, BUTTON_LEFT));
    controller2.setButtonState(PLAYER_2, BUTTON_RIGHT,  key(player2_keys.right)    || pad(PLAYER_2, BUTTON_RIGHT));
    controller2.setButtonState(PLAYER_2, BUTTON_A,      key(player2_keys.button_a) || pad(PLAYER_2, BUTTON_A));
    controller2.setButtonState(PLAYER_2, BUTTON_B,      key(player2_keys.button_b) || pad(PLAYER_2, BUTTON_B));
    controller2.setButtonState(PLAYER_2, BUTTON_START,  key(player2_keys.start)    || pad(PLAYER_2, BUTTON_START));
    controller2.setButtonState(PLAYER_2, BUTTON_SELECT, key(player2_keys.select)   || pad(PLAYER_2, BUTTON_SELECT));
}

void GTKMainWindow::toggle_fullscreen() {
    if (gtk_window_is_fullscreen(GTK_WINDOW(window))) {
        gtk_window_unfullscreen(GTK_WINDOW(window));
    } else {
        gtk_window_fullscreen(GTK_WINDOW(window));
    }
}

void GTKMainWindow::handle_save_state_key(guint keyval, bool shift) {
    if (!engine || engine->getROMBaseName().empty()) {
        set_status_message("No ROM loaded");
        return;
    }
    int slot = (int)(keyval - GDK_KEY_F5) + 1;  // F5..F9 -> 1..5
    std::string filename = engine->getROMBaseName() + ".slot" + std::to_string(slot) + ".sav";

    char msg[64];
    if (shift) {
        bool ok = engine->loadState(filename);
        snprintf(msg, sizeof(msg), ok ? "State %d loaded" : "Failed to load state %d", slot);
    } else {
        engine->saveState(filename);
        snprintf(msg, sizeof(msg), "State %d saved", slot);
    }
    set_status_message(msg);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void GTKMainWindow::action_open(GSimpleAction*, GVariant*, gpointer user_data) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);

    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Open ROM File");

    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "NES ROM Files");
    gtk_file_filter_add_pattern(filter, "*.nes");
    gtk_file_filter_add_pattern(filter, "*.NES");
    GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filter);
    g_object_unref(filters);

    if (!self->last_rom_dir.empty() && g_file_test(self->last_rom_dir.c_str(), G_FILE_TEST_IS_DIR)) {
        GFile* folder = g_file_new_for_path(self->last_rom_dir.c_str());
        gtk_file_dialog_set_initial_folder(dialog, folder);
        g_object_unref(folder);
    }

    gtk_file_dialog_open(dialog, GTK_WINDOW(self->window), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
            GTKMainWindow* self = static_cast<GTKMainWindow*>(data);
            GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (file) {
                char* path = g_file_get_path(file);
                if (path) {
                    self->load_rom(path);
                    g_free(path);
                }
                g_object_unref(file);
            }
        }, self);
    g_object_unref(dialog);
}

void GTKMainWindow::action_quit(GSimpleAction*, GVariant*, gpointer user_data) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);
    gtk_window_close(GTK_WINDOW(self->window));
}

gboolean GTKMainWindow::on_close_request(GtkWindow*, gpointer user_data) {
    static_cast<GTKMainWindow*>(user_data)->shutdown();
    return FALSE;  // Let the window close; the app exits with its last window
}

void GTKMainWindow::action_reset(GSimpleAction*, GVariant*, gpointer user_data) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);
    if (self->engine) {
        self->engine->reset();
        self->set_status_message("Game reset");
    }
}

void GTKMainWindow::action_pause(GSimpleAction*, GVariant*, gpointer user_data) {
    GTKMainWindow* self = static_cast<GTKMainWindow*>(user_data);
    if (self->game_running) {
        self->game_paused = !self->game_paused;
        self->set_status_message(self->game_paused ? "Game paused" : "Game resumed");
    }
}

void GTKMainWindow::action_game_genie(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GTKMainWindow*>(user_data)->show_game_genie_dialog();
}

void GTKMainWindow::action_controls(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GTKMainWindow*>(user_data)->show_controls_dialog();
}

void GTKMainWindow::action_resolution(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GTKMainWindow*>(user_data)->show_resolution_dialog();
}

void GTKMainWindow::action_scaling(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GTKMainWindow*>(user_data)->show_scaling_dialog();
}

void GTKMainWindow::action_filters(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GTKMainWindow*>(user_data)->show_filters_dialog();
}

void GTKMainWindow::action_fullscreen(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GTKMainWindow*>(user_data)->toggle_fullscreen();
}

void GTKMainWindow::action_about(GSimpleAction*, GVariant*, gpointer user_data) {
    static_cast<GTKMainWindow*>(user_data)->show_about_dialog();
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------
void GTKMainWindow::show_controls_dialog() {
    show_message("Controls",
                 "Player 1: Arrow keys, X (A), Z (B), ] (Start), [ (Select)\n"
                 "Player 2: WASD, K (A), J (B), I (Start), U (Select)\n"
                 "Gamepads are detected when a ROM is loaded.\n\n"
                 "F5-F9: Save state 1-5    Shift+F5-F9: Load state 1-5\n"
                 "F11: Fullscreen (Esc to leave)\n"
                 "Ctrl+O: Open ROM    Ctrl+R: Reset    Ctrl+P / Pause: Pause");
}

void GTKMainWindow::show_about_dialog() {
    const char* authors[] = {"Jason Hall", nullptr};
    gtk_show_about_dialog(GTK_WINDOW(window),
                          "program-name", "WarpNES",
                          "version", "1.0",
                          "comments", "A Beta NES emulator that emulates some commercial games.",
                          "copyright", "Original Nintendo games © Nintendo\nWarpNES Emulator © 2025 Jason Hall",
                          "authors", authors,
                          nullptr);
}

void GTKMainWindow::show_filters_dialog() {
    struct FiltersData {
        GTKMainWindow* self;
        std::vector<std::pair<GtkWidget*, FilterType>> options;
    };

    GtkWidget *content, *buttons;
    GtkWidget* dialog = make_dialog(GTK_WINDOW(window), "Video Filters", &content, &buttons);
    FiltersData* data = new FiltersData{this, {}};
    attach_data(dialog, data);

    static const struct { FilterType type; const char* label; } choices[] = {
        {FilterType::NONE, "None (fastest)"},
        {FilterType::SCALE2X, "Scale2x (pixel-perfect)"},
        {FilterType::HQ2X, "hq2x (smooth)"},
        {FilterType::HQ3X, "hq3x (3x smooth)"},
        {FilterType::XBRZ2X, "xBRZ 2x (smooth edges)"},
        {FilterType::XBRZ3X, "xBRZ 3x (smooth edges)"},
        {FilterType::XBRZ4X, "xBRZ 4x (smooth edges)"},
        {FilterType::SUPER_4XSAI, "Super 4xSaI (premium 4x smooth)"},
        {FilterType::CRT_SCANLINES, "CRT Scanlines (retro CRT effect)"},
        {FilterType::NTSC, "NTSC Composite (authentic TV look)"},
    };

    GtkWidget* group = nullptr;
    for (const auto& c : choices) {
        GtkWidget* radio = gtk_check_button_new_with_label(c.label);
        if (group) gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(group));
        else group = radio;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), c.type == current_filter);
        gtk_box_append(GTK_BOX(content), radio);
        data->options.push_back({radio, c.type});
    }

    add_button(buttons, "_Cancel", G_CALLBACK(close_dialog_cb), nullptr);
    GtkWidget* ok = add_button(buttons, "_OK", G_CALLBACK(+[](GtkButton* b, gpointer d) {
        FiltersData* data = static_cast<FiltersData*>(d);
        for (const auto& opt : data->options) {
            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(opt.first))) {
                data->self->setFilter(opt.second);
                break;
            }
        }
        close_dialog_cb(b, nullptr);
    }), data);
    gtk_window_set_default_widget(GTK_WINDOW(dialog), ok);
    gtk_window_present(GTK_WINDOW(dialog));
}

void GTKMainWindow::show_resolution_dialog() {
    struct ResolutionData {
        GTKMainWindow* self;
        std::vector<GtkWidget*> presets;
        GtkWidget* custom_radio;
        GtkWidget* width_spin;
        GtkWidget* height_spin;
    };

    GtkWidget *content, *buttons;
    GtkWidget* dialog = make_dialog(GTK_WINDOW(window), "Window Size", &content, &buttons);
    ResolutionData* data = new ResolutionData{this, {}, nullptr, nullptr, nullptr};
    attach_data(dialog, data);

    GtkWidget* preset_box;
    gtk_box_append(GTK_BOX(content), framed("Preset Resolutions", &preset_box));
    GtkWidget* group = nullptr;
    for (int i = 0; i < NUM_PRESET_RESOLUTIONS; i++) {
        GtkWidget* radio = gtk_check_button_new_with_label(PRESET_RESOLUTIONS[i].name);
        if (group) gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(group));
        else group = radio;
        gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), i == current_resolution_index && !use_custom_resolution);
        gtk_box_append(GTK_BOX(preset_box), radio);
        data->presets.push_back(radio);
    }

    GtkWidget* custom_box;
    gtk_box_append(GTK_BOX(content), framed("Custom Resolution", &custom_box));
    data->custom_radio = gtk_check_button_new_with_label("Custom:");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(data->custom_radio), GTK_CHECK_BUTTON(group));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(data->custom_radio), use_custom_resolution);
    gtk_box_append(GTK_BOX(custom_box), data->custom_radio);

    GtkWidget* size_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(custom_box), size_row);
    gtk_box_append(GTK_BOX(size_row), gtk_label_new("Width:"));
    data->width_spin = gtk_spin_button_new_with_range(256, 7680, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(data->width_spin), custom_width);
    gtk_box_append(GTK_BOX(size_row), data->width_spin);
    gtk_box_append(GTK_BOX(size_row), gtk_label_new("Height:"));
    data->height_spin = gtk_spin_button_new_with_range(240, 4320, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(data->height_spin), custom_height);
    gtk_box_append(GTK_BOX(size_row), data->height_spin);

    auto apply = +[](ResolutionData* d) {
        GTKMainWindow* self = d->self;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(d->custom_radio))) {
            self->use_custom_resolution = true;
            self->custom_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->width_spin));
            self->custom_height = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->height_spin));
            self->apply_resolution(self->custom_width, self->custom_height);
        } else {
            self->use_custom_resolution = false;
            for (int i = 0; i < (int)d->presets.size(); i++) {
                if (gtk_check_button_get_active(GTK_CHECK_BUTTON(d->presets[i]))) {
                    self->current_resolution_index = i;
                    self->apply_resolution(PRESET_RESOLUTIONS[i].width, PRESET_RESOLUTIONS[i].height);
                    break;
                }
            }
        }
        self->save_video_settings();
        gtk_widget_queue_draw(self->view);
    };
    static void (*apply_fn)(ResolutionData*) = apply;

    add_button(buttons, "_Cancel", G_CALLBACK(close_dialog_cb), nullptr);
    add_button(buttons, "_Apply", G_CALLBACK(+[](GtkButton*, gpointer d) {
        apply_fn(static_cast<ResolutionData*>(d));
    }), data);
    GtkWidget* ok = add_button(buttons, "_OK", G_CALLBACK(+[](GtkButton* b, gpointer d) {
        apply_fn(static_cast<ResolutionData*>(d));
        close_dialog_cb(b, nullptr);
    }), data);
    gtk_window_set_default_widget(GTK_WINDOW(dialog), ok);
    gtk_window_present(GTK_WINDOW(dialog));
}

void GTKMainWindow::show_scaling_dialog() {
    struct Choice { GTKMainWindow* self; bool is_mode; int value; };

    GtkWidget *content, *buttons;
    GtkWidget* dialog = make_dialog(GTK_WINDOW(window), "Scaling", &content, &buttons);
    auto* choices = new std::vector<Choice*>();
    g_object_set_data_full(G_OBJECT(dialog), "choices", choices, [](gpointer p) {
        auto* v = static_cast<std::vector<Choice*>*>(p);
        for (Choice* c : *v) delete c;
        delete v;
    });

    auto on_toggled = +[](GtkCheckButton* button, gpointer d) {
        if (!gtk_check_button_get_active(button)) return;
        Choice* c = static_cast<Choice*>(d);
        if (c->is_mode) c->self->scale_mode = static_cast<ScaleMode>(c->value);
        else c->self->smoothing = static_cast<Smoothing>(c->value);
        c->self->save_video_settings();
        gtk_widget_queue_draw(c->self->view);
    };

    auto add_group = [&](const char* title, bool is_mode, int current,
                         std::initializer_list<std::pair<int, const char*>> items) {
        GtkWidget* box;
        gtk_box_append(GTK_BOX(content), framed(title, &box));
        GtkWidget* group = nullptr;
        for (const auto& item : items) {
            GtkWidget* radio = gtk_check_button_new_with_label(item.second);
            if (group) gtk_check_button_set_group(GTK_CHECK_BUTTON(radio), GTK_CHECK_BUTTON(group));
            else group = radio;
            gtk_check_button_set_active(GTK_CHECK_BUTTON(radio), item.first == current);
            Choice* c = new Choice{this, is_mode, item.first};
            choices->push_back(c);
            g_signal_connect(radio, "toggled", G_CALLBACK(on_toggled), c);
            gtk_box_append(GTK_BOX(box), radio);
        }
    };

    add_group("Size", true, (int)scale_mode, {
        {(int)ScaleMode::FIT, "Fit to window (keep aspect ratio)"},
        {(int)ScaleMode::INTEGER, "Integer multiples only (pixel-perfect, may leave borders)"},
        {(int)ScaleMode::STRETCH, "Stretch to fill"},
    });
    add_group("Smoothing", false, (int)smoothing, {
        {(int)Smoothing::SHARP_BILINEAR, "Sharp (crisp, evenly sized pixels)"},
        {(int)Smoothing::NEAREST, "Nearest neighbour (crisp, uneven at non-integer sizes)"},
        {(int)Smoothing::BILINEAR, "Smooth (bilinear)"},
    });
    GtkWidget* crop = gtk_check_button_new_with_label("Hide overscan (top and bottom 8 lines, like a TV)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(crop), crop_overscan);
    g_signal_connect(crop, "toggled", G_CALLBACK(+[](GtkCheckButton* b, gpointer d) {
        GTKMainWindow* self = static_cast<GTKMainWindow*>(d);
        self->crop_overscan = gtk_check_button_get_active(b);
        self->save_video_settings();
        if (self->engine && self->game_running) self->present_frame();
        gtk_widget_queue_draw(self->view);
    }), this);
    gtk_box_append(GTK_BOX(content), crop);

    GtkWidget* note = gtk_label_new("Changes apply immediately. For edge-smoothing upscalers\n"
                                    "such as xBRZ, see Options > Filters.");
    gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
    gtk_widget_add_css_class(note, "dim-label");
    gtk_box_append(GTK_BOX(content), note);

    GtkWidget* close = add_button(buttons, "_Close", G_CALLBACK(close_dialog_cb), nullptr);
    gtk_window_set_default_widget(GTK_WINDOW(dialog), close);
    gtk_window_present(GTK_WINDOW(dialog));
}

// ---------------------------------------------------------------------------
// Game Genie
// ---------------------------------------------------------------------------
void GTKMainWindow::show_game_genie_dialog() {
    if (!engine || !engine->isROMLoaded()) {
        show_message("No ROM loaded", "Please load a ROM first.");
        return;
    }
    if (!gameGenie) {
        gameGenie = new GameGenie(engine);
        gameGenie->loadCodesFromFile(engine->getROMBaseName() + ".gg");
    }

    GtkWidget *content, *buttons;
    GtkWidget* dialog = make_dialog(GTK_WINDOW(window), "Game Genie Codes", &content, &buttons);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 650, 450);

    GameGenieDialogData* data = new GameGenieDialogData{};
    data->window = this;
    data->dialog = dialog;
    data->game_genie = gameGenie;
    attach_data(dialog, data);

    std::string title = "Game Genie Codes for: " + engine->getROMBaseName();
    gtk_box_append(GTK_BOX(content), gtk_label_new(title.c_str()));
    data->info_label = gtk_label_new("");
    gtk_box_append(GTK_BOX(content), data->info_label);

    GtkWidget* entry_box;
    gtk_box_append(GTK_BOX(content), framed("Add New Code", &entry_box));

    GtkWidget* code_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(entry_box), code_row);
    gtk_box_append(GTK_BOX(code_row), gtk_label_new("Code:"));
    data->code_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(data->code_entry), 8);
    gtk_entry_set_placeholder_text(GTK_ENTRY(data->code_entry), "SXIOPO (6 or 8 letters)");
    gtk_widget_set_hexpand(data->code_entry, TRUE);
    gtk_box_append(GTK_BOX(code_row), data->code_entry);

    GtkWidget* desc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(entry_box), desc_row);
    gtk_box_append(GTK_BOX(desc_row), gtk_label_new("Description:"));
    data->desc_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(data->desc_entry), "What does this code do?");
    gtk_widget_set_hexpand(data->desc_entry, TRUE);
    gtk_box_append(GTK_BOX(desc_row), data->desc_entry);

    GtkWidget* add = gtk_button_new_with_label("Add Code");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_game_genie_code), data);
    gtk_box_append(GTK_BOX(entry_box), add);
    // Enter in either entry adds the code
    g_signal_connect_swapped(data->code_entry, "activate", G_CALLBACK(gtk_widget_activate), add);
    g_signal_connect_swapped(data->desc_entry, "activate", G_CALLBACK(gtk_widget_activate), add);

    GtkWidget* list_frame_box;
    GtkWidget* list_frame = framed("Codes", &list_frame_box);
    gtk_widget_set_vexpand(list_frame, TRUE);
    gtk_box_append(GTK_BOX(content), list_frame);
    GtkWidget* scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_append(GTK_BOX(list_frame_box), scrolled);
    data->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(data->list_box), GTK_SELECTION_SINGLE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), data->list_box);

    refresh_codes_list(data);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(content), actions);
    add_button(actions, "Load from File", G_CALLBACK(on_load_game_genie_file), data);
    add_button(actions, "Save to File", G_CALLBACK(on_save_game_genie_file), data);
    add_button(actions, "Remove Selected", G_CALLBACK(on_remove_game_genie_code), data);
    add_button(actions, "Clear All", G_CALLBACK(on_clear_game_genie_codes), data);

    add_button(buttons, "_Close", G_CALLBACK(close_dialog_cb), nullptr);
    gtk_window_present(GTK_WINDOW(dialog));
}

void GTKMainWindow::refresh_codes_list(GameGenieDialogData* data) {
    GtkWidget* child;
    while ((child = gtk_widget_get_first_child(data->list_box)) != nullptr) {
        gtk_list_box_remove(GTK_LIST_BOX(data->list_box), child);
    }

    GameGenie* gg = data->game_genie;
    for (size_t i = 0; i < gg->getCodeCount(); i++) {
        GameGenie::CodeInfo info = gg->getCodeInfo(i);

        GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_set_margin_top(row, 2);
        gtk_widget_set_margin_bottom(row, 2);

        GtkWidget* check = gtk_check_button_new();
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), info.enabled);
        g_object_set_data(G_OBJECT(check), "code-index", GINT_TO_POINTER((int)i));
        g_signal_connect(check, "toggled", G_CALLBACK(on_toggle_game_genie_code), data);
        gtk_box_append(GTK_BOX(row), check);

        GtkWidget* code = gtk_label_new(info.code.c_str());
        gtk_label_set_width_chars(GTK_LABEL(code), 10);
        gtk_label_set_xalign(GTK_LABEL(code), 0.0f);
        gtk_box_append(GTK_BOX(row), code);

        GtkWidget* desc = gtk_label_new(info.description.c_str());
        gtk_label_set_xalign(GTK_LABEL(desc), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(desc), PANGO_ELLIPSIZE_END);
        gtk_widget_set_hexpand(desc, TRUE);
        gtk_box_append(GTK_BOX(row), desc);

        char addr_str[16];
        snprintf(addr_str, sizeof(addr_str), "$%04X", info.address);
        gtk_box_append(GTK_BOX(row), gtk_label_new(addr_str));

        gtk_list_box_append(GTK_LIST_BOX(data->list_box), row);
        g_object_set_data(G_OBJECT(gtk_widget_get_parent(row)), "code-index", GINT_TO_POINTER((int)i));
    }

    char info_text[128];
    snprintf(info_text, sizeof(info_text), "Currently loaded: %zu codes (%zu active)",
             gg->getCodeCount(), gg->getEnabledCodeCount());
    gtk_label_set_text(GTK_LABEL(data->info_label), info_text);
}

void GTKMainWindow::on_add_game_genie_code(GtkButton*, gpointer user_data) {
    GameGenieDialogData* data = static_cast<GameGenieDialogData*>(user_data);
    std::string code = gtk_editable_get_text(GTK_EDITABLE(data->code_entry));
    std::string desc = gtk_editable_get_text(GTK_EDITABLE(data->desc_entry));

    GtkWindow* parent = GTK_WINDOW(data->dialog);
    if (code.empty()) {
        GtkAlertDialog* alert = gtk_alert_dialog_new("Please enter a Game Genie code.");
        gtk_alert_dialog_show(alert, parent);
        g_object_unref(alert);
        return;
    }

    if (data->game_genie->addCode(code, desc.empty() ? "User entered code" : desc)) {
        gtk_editable_set_text(GTK_EDITABLE(data->code_entry), "");
        gtk_editable_set_text(GTK_EDITABLE(data->desc_entry), "");
        refresh_codes_list(data);
        data->window->set_status_message(("Added Game Genie code: " + code).c_str());
    } else {
        GtkAlertDialog* alert = gtk_alert_dialog_new("Invalid Game Genie code format.");
        gtk_alert_dialog_set_detail(alert, "Codes must be 6 or 8 letters using only: "
                                           "A P Z L G I T Y E O X U K S V N");
        gtk_alert_dialog_show(alert, parent);
        g_object_unref(alert);
    }
}

void GTKMainWindow::on_toggle_game_genie_code(GtkCheckButton* check, gpointer user_data) {
    GameGenieDialogData* data = static_cast<GameGenieDialogData*>(user_data);
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "code-index"));
    bool want = gtk_check_button_get_active(check);
    if (data->game_genie->isCodeEnabled(index) != want) {
        data->game_genie->toggleCode(index);
    }

    char info_text[128];
    snprintf(info_text, sizeof(info_text), "Currently loaded: %zu codes (%zu active)",
             data->game_genie->getCodeCount(), data->game_genie->getEnabledCodeCount());
    gtk_label_set_text(GTK_LABEL(data->info_label), info_text);

    GameGenie::CodeInfo info = data->game_genie->getCodeInfo(index);
    std::string msg = "Code " + info.code + (want ? " enabled" : " disabled");
    data->window->set_status_message(msg.c_str());
}

// Ask a yes/no question on top of `parent`, then run `on_yes`.
static void confirm(GtkWindow* parent, const std::string& heading, const char* confirm_label,
                    std::function<void()> on_yes) {
    GtkAlertDialog* alert = gtk_alert_dialog_new("%s", heading.c_str());
    const char* labels[] = {"_Cancel", confirm_label, nullptr};
    gtk_alert_dialog_set_buttons(alert, labels);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 1);
    auto* fn = new std::function<void()>(std::move(on_yes));
    gtk_alert_dialog_choose(alert, parent, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer d) {
            auto* fn = static_cast<std::function<void()>*>(d);
            int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, nullptr);
            if (choice == 1) (*fn)();
            delete fn;
        }, fn);
    g_object_unref(alert);
}

void GTKMainWindow::on_remove_game_genie_code(GtkButton*, gpointer user_data) {
    GameGenieDialogData* data = static_cast<GameGenieDialogData*>(user_data);
    GtkListBoxRow* row = gtk_list_box_get_selected_row(GTK_LIST_BOX(data->list_box));
    if (!row) {
        GtkAlertDialog* alert = gtk_alert_dialog_new("Please select a code to remove.");
        gtk_alert_dialog_show(alert, GTK_WINDOW(data->dialog));
        g_object_unref(alert);
        return;
    }

    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "code-index"));
    GameGenie::CodeInfo info = data->game_genie->getCodeInfo(index);
    confirm(GTK_WINDOW(data->dialog), "Remove Game Genie code " + info.code + " - " + info.description + "?",
            "_Remove", [data, index]() {
        data->game_genie->removeCodeByIndex(index);
        refresh_codes_list(data);
        data->window->set_status_message("Game Genie code removed");
    });
}

void GTKMainWindow::on_clear_game_genie_codes(GtkButton*, gpointer user_data) {
    GameGenieDialogData* data = static_cast<GameGenieDialogData*>(user_data);
    if (data->game_genie->getCodeCount() == 0) {
        GtkAlertDialog* alert = gtk_alert_dialog_new("No codes to clear.");
        gtk_alert_dialog_show(alert, GTK_WINDOW(data->dialog));
        g_object_unref(alert);
        return;
    }

    confirm(GTK_WINDOW(data->dialog),
            "Clear all " + std::to_string(data->game_genie->getCodeCount()) +
            " Game Genie codes? This cannot be undone.",
            "C_lear All", [data]() {
        data->game_genie->clearAllCodes();
        refresh_codes_list(data);
        data->window->set_status_message("All Game Genie codes cleared");
    });
}

void GTKMainWindow::on_load_game_genie_file(GtkButton*, gpointer user_data) {
    GameGenieDialogData* data = static_cast<GameGenieDialogData*>(user_data);
    data->window->load_game_genie_codes_file([data]() { refresh_codes_list(data); });
}

void GTKMainWindow::on_save_game_genie_file(GtkButton*, gpointer user_data) {
    static_cast<GameGenieDialogData*>(user_data)->window->save_game_genie_codes_file();
}

static GtkFileDialog* make_gg_file_dialog(const char* title) {
    GtkFileDialog* dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, title);
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Game Genie Files (*.gg)");
    gtk_file_filter_add_pattern(filter, "*.gg");
    GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filter);
    g_object_unref(filters);
    return dialog;
}

void GTKMainWindow::load_game_genie_codes_file(std::function<void()> on_done) {
    if (!gameGenie) return;

    struct LoadRequest { GTKMainWindow* self; std::function<void()> on_done; };
    GtkFileDialog* dialog = make_gg_file_dialog("Load Game Genie Codes");
    gtk_file_dialog_open(dialog, GTK_WINDOW(window), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer d) {
            LoadRequest* req = static_cast<LoadRequest*>(d);
            GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (file && req->self->gameGenie) {
                char* path = g_file_get_path(file);
                if (path && req->self->gameGenie->loadCodesFromFile(path)) {
                    req->self->set_status_message((std::string("Loaded Game Genie codes from ") + path).c_str());
                    if (req->on_done) req->on_done();
                } else {
                    req->self->show_message("Failed to load Game Genie codes file.");
                }
                g_free(path);
            }
            if (file) g_object_unref(file);
            delete req;
        }, new LoadRequest{this, std::move(on_done)});
    g_object_unref(dialog);
}

void GTKMainWindow::save_game_genie_codes_file() {
    if (!gameGenie) return;

    GtkFileDialog* dialog = make_gg_file_dialog("Save Game Genie Codes");
    if (engine && engine->isROMLoaded()) {
        std::string defaultName = engine->getROMBaseName() + ".gg";
        gtk_file_dialog_set_initial_name(dialog, defaultName.c_str());
    }
    gtk_file_dialog_save(dialog, GTK_WINDOW(window), nullptr,
        [](GObject* source, GAsyncResult* result, gpointer d) {
            GTKMainWindow* self = static_cast<GTKMainWindow*>(d);
            GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, nullptr);
            if (!file) return;
            char* path = g_file_get_path(file);
            if (path && self->gameGenie && self->gameGenie->saveCodesToFile(path)) {
                self->set_status_message((std::string("Saved Game Genie codes to ") + path).c_str());
            } else {
                self->show_message("Failed to save Game Genie codes file.");
            }
            g_free(path);
            g_object_unref(file);
        }, this);
    g_object_unref(dialog);
}

// ---------------------------------------------------------------------------
// Configuration files
// ---------------------------------------------------------------------------
void GTKMainWindow::load_key_mappings() {
    // gtk3_controls.cfg is read for compatibility with the old GTK3 frontend
    FILE* file = fopen("gtk_controls.cfg", "r");
    if (!file) file = fopen("gtk3_controls.cfg", "r");
    if (!file) file = fopen("controls.cfg", "r");
    if (!file) return;

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char key_name[64];
        guint keyval;
        if (sscanf(line, "%63[^=]=%u", key_name, &keyval) == 2) {
            keyval = gdk_keyval_to_lower(keyval);
            if (strcmp(key_name, "P1_UP") == 0) player1_keys.up = keyval;
            else if (strcmp(key_name, "P1_DOWN") == 0) player1_keys.down = keyval;
            else if (strcmp(key_name, "P1_LEFT") == 0) player1_keys.left = keyval;
            else if (strcmp(key_name, "P1_RIGHT") == 0) player1_keys.right = keyval;
            else if (strcmp(key_name, "P1_A") == 0) player1_keys.button_a = keyval;
            else if (strcmp(key_name, "P1_B") == 0) player1_keys.button_b = keyval;
            else if (strcmp(key_name, "P1_START") == 0) player1_keys.start = keyval;
            else if (strcmp(key_name, "P1_SELECT") == 0) player1_keys.select = keyval;
            else if (strcmp(key_name, "P2_UP") == 0) player2_keys.up = keyval;
            else if (strcmp(key_name, "P2_DOWN") == 0) player2_keys.down = keyval;
            else if (strcmp(key_name, "P2_LEFT") == 0) player2_keys.left = keyval;
            else if (strcmp(key_name, "P2_RIGHT") == 0) player2_keys.right = keyval;
            else if (strcmp(key_name, "P2_A") == 0) player2_keys.button_a = keyval;
            else if (strcmp(key_name, "P2_B") == 0) player2_keys.button_b = keyval;
            else if (strcmp(key_name, "P2_START") == 0) player2_keys.start = keyval;
            else if (strcmp(key_name, "P2_SELECT") == 0) player2_keys.select = keyval;
        }
    }
    fclose(file);
}

void GTKMainWindow::save_key_mappings() {
    FILE* file = fopen("gtk_controls.cfg", "w");
    if (!file) return;

    fprintf(file, "P1_UP=%u\n", player1_keys.up);
    fprintf(file, "P1_DOWN=%u\n", player1_keys.down);
    fprintf(file, "P1_LEFT=%u\n", player1_keys.left);
    fprintf(file, "P1_RIGHT=%u\n", player1_keys.right);
    fprintf(file, "P1_A=%u\n", player1_keys.button_a);
    fprintf(file, "P1_B=%u\n", player1_keys.button_b);
    fprintf(file, "P1_START=%u\n", player1_keys.start);
    fprintf(file, "P1_SELECT=%u\n", player1_keys.select);
    fprintf(file, "P2_UP=%u\n", player2_keys.up);
    fprintf(file, "P2_DOWN=%u\n", player2_keys.down);
    fprintf(file, "P2_LEFT=%u\n", player2_keys.left);
    fprintf(file, "P2_RIGHT=%u\n", player2_keys.right);
    fprintf(file, "P2_A=%u\n", player2_keys.button_a);
    fprintf(file, "P2_B=%u\n", player2_keys.button_b);
    fprintf(file, "P2_START=%u\n", player2_keys.start);
    fprintf(file, "P2_SELECT=%u\n", player2_keys.select);
    fclose(file);
}

void GTKMainWindow::load_video_settings() {
    FILE* file = fopen("video_settings.cfg", "r");
    if (!file) {
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') continue;

        if (strncmp(line, "last_rom_dir=", 13) == 0) {
            last_rom_dir = line + 13;
            while (!last_rom_dir.empty() && (last_rom_dir.back() == '\n' || last_rom_dir.back() == '\r'))
                last_rom_dir.pop_back();
            continue;
        }

        char key[64];
        char value[64];
        if (sscanf(line, "%63[^=]=%63s", key, value) == 2) {
            if (strcmp(key, "current_resolution_index") == 0) {
                current_resolution_index = atoi(value);
                if (current_resolution_index < 0 || current_resolution_index >= NUM_PRESET_RESOLUTIONS) {
                    current_resolution_index = 0;
                }
            } else if (strcmp(key, "custom_width") == 0) {
                custom_width = atoi(value);
            } else if (strcmp(key, "custom_height") == 0) {
                custom_height = atoi(value);
            } else if (strcmp(key, "use_custom_resolution") == 0) {
                use_custom_resolution = (strcmp(value, "true") == 0);
            } else if (strcmp(key, "scale_mode") == 0) {
                if (strcmp(value, "integer") == 0) scale_mode = ScaleMode::INTEGER;
                else if (strcmp(value, "stretch") == 0) scale_mode = ScaleMode::STRETCH;
                else scale_mode = ScaleMode::FIT;
            } else if (strcmp(key, "crop_overscan") == 0) {
                crop_overscan = (strcmp(value, "false") != 0);
            } else if (strcmp(key, "smoothing") == 0) {
                if (strcmp(value, "nearest") == 0) smoothing = Smoothing::NEAREST;
                else if (strcmp(value, "bilinear") == 0) smoothing = Smoothing::BILINEAR;
                else smoothing = Smoothing::SHARP_BILINEAR;
            } else if (strcmp(key, "filter") == 0) {
                int f = atoi(value);
                if (f >= (int)FilterType::NONE && f <= (int)FilterType::XBRZ4X) setFilter(static_cast<FilterType>(f));
            }
            // maintain_aspect_ratio / integer_scaling from older versions are
            // ignored: they defaulted to integer scaling, which left borders.
            // preferred_backend / backend_switching_enabled from the GTK3
            // frontend are ignored: there is only one renderer now.
        }
    }
    fclose(file);
}

void GTKMainWindow::save_video_settings() {
    FILE* file = fopen("video_settings.cfg", "w");
    if (!file) return;

    fprintf(file, "# WarpNES GTK Video Settings\n");
    fprintf(file, "current_resolution_index=%d\n", current_resolution_index);
    fprintf(file, "custom_width=%d\n", custom_width);
    fprintf(file, "custom_height=%d\n", custom_height);
    fprintf(file, "use_custom_resolution=%s\n", use_custom_resolution ? "true" : "false");
    fprintf(file, "scale_mode=%s\n", scale_mode == ScaleMode::INTEGER ? "integer" :
                                       scale_mode == ScaleMode::STRETCH ? "stretch" : "fit");
    fprintf(file, "crop_overscan=%s\n", crop_overscan ? "true" : "false");
    fprintf(file, "smoothing=%s\n", smoothing == Smoothing::NEAREST ? "nearest" :
                                      smoothing == Smoothing::BILINEAR ? "bilinear" : "sharp");
    fprintf(file, "filter=%d\n", (int)current_filter);
    if (!last_rom_dir.empty()) fprintf(file, "last_rom_dir=%s\n", last_rom_dir.c_str());
    fclose(file);
}

// ---------------------------------------------------------------------------
// Filters
// ---------------------------------------------------------------------------
void GTKMainWindow::setFilter(FilterType filter) {
    current_filter = filter;

    delete[] filter_buffer;
    filter_buffer = nullptr;

    switch (filter) {
        case FilterType::NONE:
        case FilterType::BILINEAR:
            filter_scale = 1;
            break;
        case FilterType::HQ2X:
        case FilterType::SCALE2X:
        case FilterType::CRT_SCANLINES:
            filter_scale = 2;
            filter_buffer = new uint32_t[256 * 240 * 4];
            break;
        case FilterType::HQ3X:
        case FilterType::SCALE3X:
            filter_scale = 3;
            filter_buffer = new uint32_t[256 * 240 * 9];
            break;
        case FilterType::SUPER_4XSAI:
        case FilterType::XBRZ4X:
            filter_scale = 4;
            filter_buffer = new uint32_t[256 * 240 * 16];
            break;
        case FilterType::XBRZ2X:
            filter_scale = 2;
            filter_buffer = new uint32_t[256 * 240 * 4];
            break;
        case FilterType::XBRZ3X:
            filter_scale = 3;
            filter_buffer = new uint32_t[256 * 240 * 9];
            break;
        case FilterType::NTSC:
            filter_scale = 1;
            filter_buffer = new uint32_t[256 * 240];
            init_ntsc_filter();
            break;
    }

    char status_msg[128];
    snprintf(status_msg, sizeof(status_msg), "Filter changed to %s",
             filter == FilterType::NONE ? "None" :
             filter == FilterType::HQ2X ? "hq2x" :
             filter == FilterType::HQ3X ? "hq3x" :
             filter == FilterType::SCALE2X ? "Scale2x" :
             filter == FilterType::SCALE3X ? "Scale3x" :
             filter == FilterType::CRT_SCANLINES ? "CRT Scanlines" :
             filter == FilterType::NTSC ? "NTSC Composite" :
             filter == FilterType::SUPER_4XSAI ? "Super 4xSaI" :
             filter == FilterType::XBRZ2X ? "xBRZ 2x" :
             filter == FilterType::XBRZ3X ? "xBRZ 3x" :
             filter == FilterType::XBRZ4X ? "xBRZ 4x" : "Bilinear");
    set_status_message(status_msg);
    if (window) save_video_settings();
}

uint32_t GTKMainWindow::rgb565_to_rgb888(uint16_t color) {
    uint8_t r = (color >> 11) & 0x1F;
    uint8_t g = (color >> 5) & 0x3F;
    uint8_t b = color & 0x1F;
    
    r = (r * 255 + 15) / 31;
    g = (g * 255 + 31) / 63;
    b = (b * 255 + 15) / 31;
    
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

void GTKMainWindow::apply_scale2x_filter(uint16_t* input, uint32_t* output, int width, int height) {
    // Simple Scale2x algorithm
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint16_t C = input[y * width + x];  // Current pixel
            
            // Get neighbors (with bounds checking)
            uint16_t A = (y > 0) ? input[(y-1) * width + x] : C;           // Above
            uint16_t B = (x < width-1) ? input[y * width + (x+1)] : C;     // Right
            uint16_t D = (x > 0) ? input[y * width + (x-1)] : C;           // Left
            uint16_t E = (y < height-1) ? input[(y+1) * width + x] : C;    // Below
            
            uint32_t color = rgb565_to_rgb888(C);
            
            // Scale2x logic
            int out_y = y * 2;
            int out_x = x * 2;
            int out_width = width * 2;
            
            if (A != E && D != B) {
                output[out_y * out_width + out_x] = (D == A) ? rgb565_to_rgb888(A) : color;
                output[out_y * out_width + (out_x + 1)] = (A == B) ? rgb565_to_rgb888(A) : color;
                output[(out_y + 1) * out_width + out_x] = (D == E) ? rgb565_to_rgb888(E) : color;
                output[(out_y + 1) * out_width + (out_x + 1)] = (E == B) ? rgb565_to_rgb888(E) : color;
            } else {
                // No scaling pattern, just duplicate
                output[out_y * out_width + out_x] = color;
                output[out_y * out_width + (out_x + 1)] = color;
                output[(out_y + 1) * out_width + out_x] = color;
                output[(out_y + 1) * out_width + (out_x + 1)] = color;
            }
        }
    }
}

void GTKMainWindow::apply_hq2x_filter(uint16_t* input, uint32_t* output, int width, int height) {
    // Simplified hq2x-style filter
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Get 3x3 neighborhood
            uint16_t w[9];
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int ny = y + dy;
                    int nx = x + dx;
                    
                    // Clamp to bounds
                    if (ny < 0) ny = 0;
                    if (ny >= height) ny = height - 1;
                    if (nx < 0) nx = 0;
                    if (nx >= width) nx = width - 1;
                    
                    w[(dy + 1) * 3 + (dx + 1)] = input[ny * width + nx];
                }
            }
            
            uint32_t colors[9];
            for (int i = 0; i < 9; i++) {
                colors[i] = rgb565_to_rgb888(w[i]);
            }
            
            // hq2x pattern matching (simplified)
            uint32_t c = colors[4]; // Center pixel
            int out_y = y * 2;
            int out_x = x * 2;
            int out_width = width * 2;
            
            // Check for diagonal patterns and smooth accordingly
            bool diag1 = (w[0] == w[8]) && (w[0] != w[4]);
            bool diag2 = (w[2] == w[6]) && (w[2] != w[4]);
            
            if (diag1 || diag2) {
                // Apply smoothing
                uint32_t blend1 = colors[1]; // Top
                uint32_t blend2 = colors[3]; // Left
                uint32_t blend3 = colors[5]; // Right
                uint32_t blend4 = colors[7]; // Bottom
                
                output[out_y * out_width + out_x] = c;
                output[out_y * out_width + (out_x + 1)] = blend1;
                output[(out_y + 1) * out_width + out_x] = blend2;
                output[(out_y + 1) * out_width + (out_x + 1)] = c;
            } else {
                // Simple duplication
                output[out_y * out_width + out_x] = c;
                output[out_y * out_width + (out_x + 1)] = c;
                output[(out_y + 1) * out_width + out_x] = c;
                output[(out_y + 1) * out_width + (out_x + 1)] = c;
            }
        }
    }
}

void GTKMainWindow::apply_hq3x_filter(uint16_t* input, uint32_t* output, int width, int height) {
    // Simplified hq3x implementation
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t color = rgb565_to_rgb888(input[y * width + x]);
            
            // For now, just do a simple 3x scale
            // A full hq3x would require extensive pattern matching
            int out_y = y * 3;
            int out_x = x * 3;
            int out_width = width * 3;
            
            for (int dy = 0; dy < 3; dy++) {
                for (int dx = 0; dx < 3; dx++) {
                    output[(out_y + dy) * out_width + (out_x + dx)] = color;
                }
            }
        }
    }
}



void GTKMainWindow::apply_crt_scanlines_filter(uint16_t* input, uint32_t* output, int width, int height) {
    const float scanline_intensity = 0.25f; // How dark the scanlines are (0.0 = invisible, 1.0 = black)
    const float phosphor_decay = 0.95f;     // Simulates phosphor persistence
    const float beam_width = 1.2f;          // Width of the electron beam
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t color = rgb565_to_rgb888(input[y * width + x]);
            
            // Extract RGB components
            uint8_t r = (color >> 16) & 0xFF;
            uint8_t g = (color >> 8) & 0xFF;
            uint8_t b = color & 0xFF;
            
            // Apply phosphor glow effect
            float glow_factor = 1.0f + 0.1f * sinf(x * 0.5f);
            r = (uint8_t)(std::min(255.0f, r * glow_factor));
            g = (uint8_t)(std::min(255.0f, g * glow_factor));
            b = (uint8_t)(std::min(255.0f, b * glow_factor));
            
            // 2x scaling with scanlines
            int out_y = y * 2;
            int out_x = x * 2;
            int out_width = width * 2;
            
            // Top row - normal brightness
            uint32_t top_color = (0xFF << 24) | (r << 16) | (g << 8) | b;
            output[out_y * out_width + out_x] = top_color;
            output[out_y * out_width + (out_x + 1)] = top_color;
            
            // Bottom row - darkened for scanline effect
            uint8_t dark_r = (uint8_t)(r * (1.0f - scanline_intensity));
            uint8_t dark_g = (uint8_t)(g * (1.0f - scanline_intensity));
            uint8_t dark_b = (uint8_t)(b * (1.0f - scanline_intensity));
            uint32_t bottom_color = (0xFF << 24) | (dark_r << 16) | (dark_g << 8) | dark_b;
            
            output[(out_y + 1) * out_width + out_x] = bottom_color;
            output[(out_y + 1) * out_width + (out_x + 1)] = bottom_color;
        }
    }
}

// NTSC Composite filter implementation
void GTKMainWindow::apply_ntsc_filter(uint16_t* input, uint32_t* output, int width, int height) {
    // NTSC composite video simulation
    const float color_bleeding = 0.15f;     // How much colors bleed into adjacent pixels
    const float dot_crawl = 0.08f;          // Simulates dot crawl artifact
    const float rainbow_banding = 0.12f;    // Rainbow banding on high contrast edges
    const float chroma_resolution = 0.6f;   // Chroma resolution vs luma
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint32_t center_color = rgb565_to_rgb888(input[y * width + x]);
            
            // Get neighboring pixels for blending
            uint32_t left_color = (x > 0) ? 
                rgb565_to_rgb888(input[y * width + (x - 1)]) : center_color;
            uint32_t right_color = (x < width - 1) ? 
                rgb565_to_rgb888(input[y * width + (x + 1)]) : center_color;
            
            // Extract RGB components
            uint8_t r = (center_color >> 16) & 0xFF;
            uint8_t g = (center_color >> 8) & 0xFF;
            uint8_t b = center_color & 0xFF;
            
            // Convert to YIQ color space (simplified)
            float y_luma = 0.299f * r + 0.587f * g + 0.114f * b;
            float i_chroma = 0.596f * r - 0.274f * g - 0.322f * b;
            float q_chroma = 0.211f * r - 0.523f * g + 0.312f * b;
            
            // Apply chroma resolution reduction
            i_chroma *= chroma_resolution;
            q_chroma *= chroma_resolution;
            
            // Add NTSC artifacts
            float phase = (x + y * 3) * 0.5f; // NTSC color carrier phase
            
            // Dot crawl effect
            float dot_crawl_offset = dot_crawl * sinf(phase * 2.0f);
            i_chroma += dot_crawl_offset;
            
            // Color bleeding from adjacent pixels
            uint8_t left_r = (left_color >> 16) & 0xFF;
            uint8_t left_g = (left_color >> 8) & 0xFF;
            uint8_t left_b = left_color & 0xFF;
            
            uint8_t right_r = (right_color >> 16) & 0xFF;
            uint8_t right_g = (right_color >> 8) & 0xFF;
            uint8_t right_b = right_color & 0xFF;
            
            r = (uint8_t)(r * (1.0f - color_bleeding) + 
                         (left_r + right_r) * 0.5f * color_bleeding);
            g = (uint8_t)(g * (1.0f - color_bleeding) + 
                         (left_g + right_g) * 0.5f * color_bleeding);
            b = (uint8_t)(b * (1.0f - color_bleeding) + 
                         (left_b + right_b) * 0.5f * color_bleeding);
            
            // Rainbow banding on high contrast edges
            if (abs(r - left_r) > 50 || abs(r - right_r) > 50) {
                float rainbow_phase = phase + x * 0.3f;
                r = (uint8_t)std::min(255.0f, r + rainbow_banding * 50.0f * sinf(rainbow_phase));
                g = (uint8_t)std::min(255.0f, g + rainbow_banding * 50.0f * sinf(rainbow_phase + 2.0f));
                b = (uint8_t)std::min(255.0f, b + rainbow_banding * 50.0f * sinf(rainbow_phase + 4.0f));
            }
            
            // Apply gamma correction for NTSC
            apply_gamma_correction(&center_color, ntsc_settings.gamma);
            
            // Add some random noise to simulate analog signal
            float noise = ((rand() % 100) / 100.0f - 0.5f) * 0.02f;
            r = (uint8_t)std::max(0.0f, std::min(255.0f, r + noise * 255.0f));
            g = (uint8_t)std::max(0.0f, std::min(255.0f, g + noise * 255.0f));
            b = (uint8_t)std::max(0.0f, std::min(255.0f, b + noise * 255.0f));
            
            output[y * width + x] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
}

// Helper functions for NTSC filter
void GTKMainWindow::init_ntsc_filter() {
    // Initialize NTSC settings with default values for authentic CRT TV look
    ntsc_settings.hue = 0.0f;
    ntsc_settings.saturation = 0.0f;
    ntsc_settings.contrast = 0.0f;
    ntsc_settings.brightness = 0.0f;
    ntsc_settings.gamma = 1.8f;          // Typical CRT gamma
    ntsc_settings.sharpness = 0.2f;
    ntsc_settings.resolution = 0.7f;
    ntsc_settings.artifacts = 0.0f;
    ntsc_settings.fringing = 0.0f;
    ntsc_settings.bleed = 0.0f;
}

uint32_t GTKMainWindow::apply_ntsc_artifacts(uint32_t color, int x, int y) {
    // Apply NTSC-specific color artifacts
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;
    
    // Simulate NTSC color carrier interference
    float phase = (x * 3.579545f + y * 0.1f); // NTSC color subcarrier frequency
    
    // Add color fringing
    float fringe_offset = ntsc_settings.fringing * sinf(phase);
    r = (uint8_t)std::max(0.0f, std::min(255.0f, r + fringe_offset * 20.0f));
    g = (uint8_t)std::max(0.0f, std::min(255.0f, g + fringe_offset * 15.0f));
    b = (uint8_t)std::max(0.0f, std::min(255.0f, b + fringe_offset * 10.0f));
    
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

uint32_t GTKMainWindow::blend_colors(uint32_t color1, uint32_t color2, float ratio) {
    // Blend two RGBA colors
    uint8_t r1 = (color1 >> 16) & 0xFF;
    uint8_t g1 = (color1 >> 8) & 0xFF;
    uint8_t b1 = color1 & 0xFF;
    
    uint8_t r2 = (color2 >> 16) & 0xFF;
    uint8_t g2 = (color2 >> 8) & 0xFF;
    uint8_t b2 = color2 & 0xFF;
    
    uint8_t r = (uint8_t)(r1 * (1.0f - ratio) + r2 * ratio);
    uint8_t g = (uint8_t)(g1 * (1.0f - ratio) + g2 * ratio);
    uint8_t b = (uint8_t)(b1 * (1.0f - ratio) + b2 * ratio);
    
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

void GTKMainWindow::apply_gamma_correction(uint32_t* color, float gamma) {
    // Apply gamma correction to simulate CRT monitor characteristics
    uint8_t r = (*color >> 16) & 0xFF;
    uint8_t g = (*color >> 8) & 0xFF;
    uint8_t b = *color & 0xFF;
    
    // Normalize to 0-1 range
    float rf = r / 255.0f;
    float gf = g / 255.0f;
    float bf = b / 255.0f;
    
    // Apply gamma correction
    rf = powf(rf, 1.0f / gamma);
    gf = powf(gf, 1.0f / gamma);
    bf = powf(bf, 1.0f / gamma);
    
    // Convert back to 0-255 range
    r = (uint8_t)(rf * 255.0f);
    g = (uint8_t)(gf * 255.0f);
    b = (uint8_t)(bf * 255.0f);
    
    *color = (0xFF << 24) | (r << 16) | (g << 8) | b;
}

void GTKMainWindow::apply_4xsai_filter(uint16_t* input, uint32_t* output, int width, int height) {
    // 4xSaI (4x Scale and Interpolate) filter
    // Creates 4x4 output for each input pixel using sophisticated interpolation
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Get 3x3 neighborhood with bounds checking
            uint32_t colors[9];
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int ny = y + dy;
                    int nx = x + dx;
                    
                    // Clamp to bounds
                    if (ny < 0) ny = 0;
                    if (ny >= height) ny = height - 1;
                    if (nx < 0) nx = 0;
                    if (nx >= width) nx = width - 1;
                    
                    colors[(dy + 1) * 3 + (dx + 1)] = rgb565_to_rgb888(input[ny * width + nx]);
                }
            }
            
            // Name the colors for clarity:
            //  0 | 1 | 2
            // -----------
            //  3 | 4 | 5  
            // -----------
            //  6 | 7 | 8
            
            uint32_t c0 = colors[0], c1 = colors[1], c2 = colors[2];
            uint32_t c3 = colors[3], c4 = colors[4], c5 = colors[5];
            uint32_t c6 = colors[6], c7 = colors[7], c8 = colors[8];
            
            // Output 4x4 block
            int out_y = y * 4;
            int out_x = x * 4;
            int out_width = width * 4;
            
            // 4xSaI algorithm - determine output pixels based on edge patterns
            uint32_t product1a, product1b, product2a, product2b;
            
            // Top-left quadrant
            if (colors_equal(c3, c1) && !colors_equal(c3, c5)) {
                product1a = interpolate_4xsai(c3, c3, c4, c1);
            } else if (colors_equal(c5, c1) && !colors_equal(c3, c5)) {
                product1a = interpolate_4xsai(c5, c1, c4, c5);
            } else if (colors_equal(c3, c5) && !colors_equal(c3, c1)) {
                product1a = c4;
            } else {
                product1a = interpolate_4xsai(c1, c3, c4, c5);
            }
            
            // Top-right quadrant  
            if (colors_equal(c1, c5) && !colors_equal(c1, c7)) {
                product1b = interpolate_4xsai(c1, c1, c4, c5);
            } else if (colors_equal(c7, c5) && !colors_equal(c1, c7)) {
                product1b = interpolate_4xsai(c7, c5, c4, c7);
            } else if (colors_equal(c1, c7) && !colors_equal(c1, c5)) {
                product1b = c4;
            } else {
                product1b = interpolate_4xsai(c5, c1, c4, c7);
            }
            
            // Bottom-left quadrant
            if (colors_equal(c3, c7) && !colors_equal(c3, c1)) {
                product2a = interpolate_4xsai(c3, c7, c4, c3);
            } else if (colors_equal(c1, c7) && !colors_equal(c3, c1)) {
                product2a = interpolate_4xsai(c1, c7, c4, c1);
            } else if (colors_equal(c3, c1) && !colors_equal(c3, c7)) {
                product2a = c4;
            } else {
                product2a = interpolate_4xsai(c7, c3, c4, c1);
            }
            
            // Bottom-right quadrant
            if (colors_equal(c5, c7) && !colors_equal(c5, c3)) {
                product2b = interpolate_4xsai(c5, c7, c4, c5);
            } else if (colors_equal(c3, c7) && !colors_equal(c5, c3)) {
                product2b = interpolate_4xsai(c3, c7, c4, c3);
            } else if (colors_equal(c5, c3) && !colors_equal(c5, c7)) {
                product2b = c4;
            } else {
                product2b = interpolate_4xsai(c7, c5, c4, c3);
            }
            
            // Additional refinement for smoother results
            uint32_t product1a_refined = product1a;
            uint32_t product1b_refined = product1b;
            uint32_t product2a_refined = product2a;
            uint32_t product2b_refined = product2b;
            
            // Apply edge-directed interpolation
            if (!colors_equal(c1, c5) && !colors_equal(c3, c7)) {
                if (colors_equal(c3, c1)) {
                    product1a_refined = mix_colors(product1a, c3, 0.75f);
                }
                if (colors_equal(c1, c5)) {
                    product1b_refined = mix_colors(product1b, c1, 0.75f);
                }
                if (colors_equal(c3, c7)) {
                    product2a_refined = mix_colors(product2a, c3, 0.75f);
                }
                if (colors_equal(c5, c7)) {
                    product2b_refined = mix_colors(product2b, c5, 0.75f);
                }
            }
            
            // Output the 4x4 block
            // Top half
            output[out_y * out_width + out_x] = product1a_refined;
            output[out_y * out_width + (out_x + 1)] = mix_colors(product1a_refined, product1b_refined, 0.5f);
            output[out_y * out_width + (out_x + 2)] = mix_colors(product1b_refined, product1a_refined, 0.5f);
            output[out_y * out_width + (out_x + 3)] = product1b_refined;
            
            output[(out_y + 1) * out_width + out_x] = mix_colors(product1a_refined, product2a_refined, 0.5f);
            output[(out_y + 1) * out_width + (out_x + 1)] = mix_colors(c4, product1a_refined, 0.7f);
            output[(out_y + 1) * out_width + (out_x + 2)] = mix_colors(c4, product1b_refined, 0.7f);
            output[(out_y + 1) * out_width + (out_x + 3)] = mix_colors(product1b_refined, product2b_refined, 0.5f);
            
            // Bottom half
            output[(out_y + 2) * out_width + out_x] = mix_colors(product2a_refined, product1a_refined, 0.5f);
            output[(out_y + 2) * out_width + (out_x + 1)] = mix_colors(c4, product2a_refined, 0.7f);
            output[(out_y + 2) * out_width + (out_x + 2)] = mix_colors(c4, product2b_refined, 0.7f);
            output[(out_y + 2) * out_width + (out_x + 3)] = mix_colors(product2b_refined, product1b_refined, 0.5f);
            
            output[(out_y + 3) * out_width + out_x] = product2a_refined;
            output[(out_y + 3) * out_width + (out_x + 1)] = mix_colors(product2a_refined, product2b_refined, 0.5f);
            output[(out_y + 3) * out_width + (out_x + 2)] = mix_colors(product2b_refined, product2a_refined, 0.5f);
            output[(out_y + 3) * out_width + (out_x + 3)] = product2b_refined;
        }
    }
}

uint32_t GTKMainWindow::interpolate_4xsai(uint32_t c1, uint32_t c2, uint32_t c3, uint32_t c4) {
    // Advanced 4-point interpolation for smoother results
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    uint8_t r3 = (c3 >> 16) & 0xFF, g3 = (c3 >> 8) & 0xFF, b3 = c3 & 0xFF;
    uint8_t r4 = (c4 >> 16) & 0xFF, g4 = (c4 >> 8) & 0xFF, b4 = c4 & 0xFF;
    
    // Weighted average with emphasis on the center pixel
    uint8_t r = (r1 + r2 + (r3 * 2) + r4) / 5;
    uint8_t g = (g1 + g2 + (g3 * 2) + g4) / 5;
    uint8_t b = (b1 + b2 + (b3 * 2) + b4) / 5;
    
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

uint32_t GTKMainWindow::mix_colors(uint32_t c1, uint32_t c2, float ratio) {
    // Linear interpolation between two colors
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    
    uint8_t r = (uint8_t)(r1 * (1.0f - ratio) + r2 * ratio);
    uint8_t g = (uint8_t)(g1 * (1.0f - ratio) + g2 * ratio);
    uint8_t b = (uint8_t)(b1 * (1.0f - ratio) + b2 * ratio);
    
    return (0xFF << 24) | (r << 16) | (g << 8) | b;
}

bool GTKMainWindow::colors_equal(uint32_t c1, uint32_t c2) {
    // Check if colors are similar enough to be considered equal
    // Allow small differences to account for compression artifacts
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
    
    return (abs(r1 - r2) < 8) && (abs(g1 - g2) < 8) && (abs(b1 - b2) < 8);
}


// Main function
int main(int argc, char* argv[]) {
    Configuration::initialize("config.cfg");

    printf("WarpNES GTK4 NES Emulator\n");

    const char* rom_filename = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--neszapper") == 0) {
            printf("NES Zapper support noted (not yet implemented)\n");
        } else if (argv[i][0] != '-') {
            rom_filename = argv[i];
        }
    }

    if (!rom_filename) {
        printf("No ROM specified - use File > Open ROM to load a game\n");
    }

    GTKMainWindow main_window;
    return main_window.run(argc, argv, rom_filename);
}
