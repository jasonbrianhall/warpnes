#include <SDL2/SDL.h>
#include "GTKMainWindow.hpp"
#include "Emulation/WarpNES.hpp"
#include "Emulation/ControllerSDL.hpp"
#include "Configuration.hpp"
#include "Constants.hpp"

GTK3MainWindow::GTK3MainWindow() 
    : window(nullptr), sdl_socket(nullptr), engine(nullptr), 
      game_running(false), game_paused(false),
      frame_timer_id(0), status_message_id(0),
      sdl_window(nullptr), sdl_renderer(nullptr), sdl_texture(nullptr)
{
    strcpy(status_message, "Ready");
    
    // Initialize Player 1 default keys (FIXED MAPPINGS)
    player1_keys.up = GDK_KEY_Up;
    player1_keys.down = GDK_KEY_Down;
    player1_keys.left = GDK_KEY_Left;
    player1_keys.right = GDK_KEY_Right;
    player1_keys.button_a = GDK_KEY_x;
    player1_keys.button_b = GDK_KEY_z;
    player1_keys.start = GDK_KEY_bracketright;    // FIXED: ] for start (matches SDL)
    player1_keys.select = GDK_KEY_bracketleft;    // FIXED: [ for select (matches SDL)
    
    // Initialize Player 2 default keys (FIXED MAPPINGS)
    player2_keys.up = GDK_KEY_w;
    player2_keys.down = GDK_KEY_s;
    player2_keys.left = GDK_KEY_a;
    player2_keys.right = GDK_KEY_d;
    player2_keys.button_a = GDK_KEY_k;           // FIXED: k for A (matches SDL)
    player2_keys.button_b = GDK_KEY_j;           // FIXED: j for B (matches SDL)
    player2_keys.start = GDK_KEY_i;              // FIXED: i for start (matches SDL)
    player2_keys.select = GDK_KEY_u;             // FIXED: u for select (matches SDL)
}

GTK3MainWindow::~GTK3MainWindow() {
    shutdown();
}

bool GTK3MainWindow::initialize() {
    // Initialize GTK
    gtk_init(nullptr, nullptr);
    
    // Initialize SDL video and audio subsystem (FIXED: Added audio)
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        printf("ERROR: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    
    create_widgets();
    load_key_mappings();
    
    // Show the window maximized (FIXED: Start maximized)
    gtk_widget_show_all(window);
    gtk_window_maximize(GTK_WINDOW(window));
    
    // Initialize SDL after GTK window is shown
    if (!init_sdl()) {
        printf("ERROR: SDL initialization failed\n");
        return false;
    }
    
    // Initialize audio (FIXED: Added audio support)
    if (Configuration::getAudioEnabled()) {
        SDL_AudioSpec desiredSpec;
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
            // Start playing audio
            SDL_PauseAudio(0);
            printf("Audio initialized successfully\n");
        }
    }
    
    // Grab focus for key events
    gtk_widget_grab_focus(window);
    
    set_status_message("WarpNES GTK3+SDL - Load a ROM file to begin");
    
    return true;
}

// FIXED: Audio callback function
void GTK3MainWindow::audio_callback(void* userdata, uint8_t* buffer, int len) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(userdata);
    if (window && window->engine) {
        window->engine->audioCallback(buffer, len);
    } else {
        // Fill with silence if no engine
        memset(buffer, 0, len);
    }
}

void GTK3MainWindow::create_widgets() {
    // Create main window
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "WarpNES GTK3+SDL");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    
    // Set focus and connect key events
    gtk_widget_set_can_focus(window, TRUE);
    gtk_widget_set_focus_on_click(window, TRUE);
    
    // Connect window signals
    g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete), this);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), this);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), this);
    g_signal_connect(window, "key-release-event", G_CALLBACK(on_key_release), this);
    
    // Create main vertical box
    main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);
    
    // Create menubar
    create_menubar();
    gtk_box_pack_start(GTK_BOX(main_vbox), menubar, FALSE, FALSE, 0);
    
    // Create SDL area
    setup_sdl_area();
    gtk_box_pack_start(GTK_BOX(main_vbox), sdl_socket, TRUE, TRUE, 0);
    
    // Create status bar
    status_bar = gtk_statusbar_new();
    gtk_box_pack_start(GTK_BOX(main_vbox), status_bar, FALSE, FALSE, 0);
}

void GTK3MainWindow::create_menubar() {
    menubar = gtk_menu_bar_new();
    
    // File menu
    GtkWidget* file_menu = gtk_menu_new();
    GtkWidget* file_item = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);
    
    GtkWidget* open_item = gtk_menu_item_new_with_label("Open ROM...");
    g_signal_connect(open_item, "activate", G_CALLBACK(on_file_open), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), open_item);
    
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), gtk_separator_menu_item_new());
    
    GtkWidget* quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(on_file_quit), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu), quit_item);
    
    // Game menu
    GtkWidget* game_menu = gtk_menu_new();
    GtkWidget* game_item = gtk_menu_item_new_with_label("Game");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(game_item), game_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), game_item);
    
    GtkWidget* reset_item = gtk_menu_item_new_with_label("Reset");
    g_signal_connect(reset_item, "activate", G_CALLBACK(on_game_reset), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(game_menu), reset_item);
    
    GtkWidget* pause_item = gtk_menu_item_new_with_label("Pause");
    g_signal_connect(pause_item, "activate", G_CALLBACK(on_game_pause), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(game_menu), pause_item);
    
    // Options menu
    GtkWidget* options_menu = gtk_menu_new();
    GtkWidget* options_item = gtk_menu_item_new_with_label("Options");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(options_item), options_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), options_item);
    
    GtkWidget* controls_item = gtk_menu_item_new_with_label("Controls...");
    g_signal_connect(controls_item, "activate", G_CALLBACK(on_options_controls), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(options_menu), controls_item);
    
    GtkWidget* video_item = gtk_menu_item_new_with_label("Video...");
    g_signal_connect(video_item, "activate", G_CALLBACK(on_options_video), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(options_menu), video_item);
    
    // Help menu
    GtkWidget* help_menu = gtk_menu_new();
    GtkWidget* help_item = gtk_menu_item_new_with_label("Help");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_item);
    
    GtkWidget* about_item = gtk_menu_item_new_with_label("About");
    g_signal_connect(about_item, "activate", G_CALLBACK(on_help_about), this);
    gtk_menu_shell_append(GTK_MENU_SHELL(help_menu), about_item);
}

void GTK3MainWindow::setup_sdl_area() {
    // Create a GTK drawing area for SDL to render into
    sdl_socket = gtk_drawing_area_new();
    gtk_widget_set_size_request(sdl_socket, 256, 240);
    gtk_widget_set_can_focus(sdl_socket, TRUE);
    
    // Set the drawing area to black background to prevent bleedthrough
    GdkRGBA black = {0.0, 0.0, 0.0, 1.0};
    gtk_widget_override_background_color(sdl_socket, GTK_STATE_FLAG_NORMAL, &black);
}

bool GTK3MainWindow::init_sdl() {
    printf("=== Initializing SDL ===\n");
    
    // Wait for the widget to be realized
    gtk_widget_realize(sdl_socket);
    
    // Get the native window handle
    GdkWindow* gdk_window = gtk_widget_get_window(sdl_socket);
    if (!gdk_window) {
        printf("ERROR: Could not get GDK window\n");
        return false;
    }
    
    // Get X11 window ID
    unsigned long window_id = gdk_x11_window_get_xid(gdk_window);
    
    // Set SDL to use this window
    char window_id_str[32];
    snprintf(window_id_str, sizeof(window_id_str), "%lu", window_id);
    SDL_SetHint(SDL_HINT_VIDEO_WINDOW_SHARE_PIXEL_FORMAT, "1");
    
    // Create SDL window from existing window
    sdl_window = SDL_CreateWindowFrom((void*)(uintptr_t)window_id);
    if (!sdl_window) {
        printf("ERROR: SDL_CreateWindowFrom failed: %s\n", SDL_GetError());
        return false;
    }
    
    // Create SDL renderer with VSync
    sdl_renderer = SDL_CreateRenderer(sdl_window, -1, 
                                     SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!sdl_renderer) {
        printf("ERROR: SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    
    // Create texture for NES framebuffer (RGB565 format)
    sdl_texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_RGB565, 
                                   SDL_TEXTUREACCESS_STREAMING, 256, 240);
    if (!sdl_texture) {
        printf("ERROR: SDL_CreateTexture failed: %s\n", SDL_GetError());
        return false;
    }
    
    printf("SUCCESS: SDL initialized and embedded in GTK\n");
    return true;
}

void GTK3MainWindow::render_frame() {
    if (!sdl_renderer || !sdl_texture) return;
    
    // Always clear the entire renderer to black first
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(sdl_renderer);
    
    if (game_running && engine) {
        // Get pixels from game engine
        static uint16_t frame_buffer[256 * 240];
        engine->render16(frame_buffer);
        
        // Update SDL texture
        void* pixels;
        int pitch;
        if (SDL_LockTexture(sdl_texture, NULL, &pixels, &pitch) == 0) {
            memcpy(pixels, frame_buffer, sizeof(frame_buffer));
            SDL_UnlockTexture(sdl_texture);
        }
        
        // Calculate aspect-correct scaling
        int widget_width = gtk_widget_get_allocated_width(sdl_socket);
        int widget_height = gtk_widget_get_allocated_height(sdl_socket);
        
        float aspect = 256.0f / 240.0f;
        float widget_aspect = (float)widget_width / (float)widget_height;
        
        SDL_Rect dest_rect;
        if (widget_aspect > aspect) {
            // Widget is wider - fit to height
            dest_rect.h = widget_height;
            dest_rect.w = (int)(widget_height * aspect);
            dest_rect.x = (widget_width - dest_rect.w) / 2;
            dest_rect.y = 0;
        } else {
            // Widget is taller - fit to width
            dest_rect.w = widget_width;
            dest_rect.h = (int)(widget_width / aspect);
            dest_rect.x = 0;
            dest_rect.y = (widget_height - dest_rect.h) / 2;
        }
        
        SDL_RenderCopy(sdl_renderer, sdl_texture, NULL, &dest_rect);
    }
    
    // Always present, whether we have a game or not
    SDL_RenderPresent(sdl_renderer);
}

gboolean GTK3MainWindow::frame_update_callback(gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    
    if (!window->game_running || !window->engine) {
        window->frame_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    if (!window->game_paused) {
        // Process input
        window->process_input();
        
        // Update game engine
        window->engine->update();
        
        // Render frame
        window->render_frame();
    }
    
    return G_SOURCE_CONTINUE;
}

void GTK3MainWindow::process_input() {
    if (!engine) return;
    update_controller_state();
}

void GTK3MainWindow::update_controller_state() {
    if (!engine) return;
    
    // Get references to controllers
    Controller& controller1 = engine->getController1();
    Controller& controller2 = engine->getController2();
    
    // Update Player 1 controller
    controller1.setButtonState(PLAYER_1, BUTTON_UP, key_states[player1_keys.up]);
    controller1.setButtonState(PLAYER_1, BUTTON_DOWN, key_states[player1_keys.down]);
    controller1.setButtonState(PLAYER_1, BUTTON_LEFT, key_states[player1_keys.left]);
    controller1.setButtonState(PLAYER_1, BUTTON_RIGHT, key_states[player1_keys.right]);
    controller1.setButtonState(PLAYER_1, BUTTON_A, key_states[player1_keys.button_a]);
    controller1.setButtonState(PLAYER_1, BUTTON_B, key_states[player1_keys.button_b]);
    controller1.setButtonState(PLAYER_1, BUTTON_START, key_states[player1_keys.start]);
    controller1.setButtonState(PLAYER_1, BUTTON_SELECT, key_states[player1_keys.select]);
    
    // Update Player 2 controller
    controller2.setButtonState(PLAYER_2, BUTTON_UP, key_states[player2_keys.up]);
    controller2.setButtonState(PLAYER_2, BUTTON_DOWN, key_states[player2_keys.down]);
    controller2.setButtonState(PLAYER_2, BUTTON_LEFT, key_states[player2_keys.left]);
    controller2.setButtonState(PLAYER_2, BUTTON_RIGHT, key_states[player2_keys.right]);
    controller2.setButtonState(PLAYER_2, BUTTON_A, key_states[player2_keys.button_a]);
    controller2.setButtonState(PLAYER_2, BUTTON_B, key_states[player2_keys.button_b]);
    controller2.setButtonState(PLAYER_2, BUTTON_START, key_states[player2_keys.start]);
    controller2.setButtonState(PLAYER_2, BUTTON_SELECT, key_states[player2_keys.select]);
}

void GTK3MainWindow::run(const char* rom_filename) {
    // Load ROM if specified
    if (rom_filename && !engine) {
        engine = new WarpNES();
        
        if (!engine->loadROM(rom_filename)) {
            set_status_message("Failed to load ROM file");
            delete engine;
            engine = nullptr;
            gtk_main();
            return;
        }
        
        engine->reset();
        game_running = true;
        game_paused = false;
        
        char status_msg[512];
        snprintf(status_msg, sizeof(status_msg), "ROM loaded: %s", rom_filename);
        set_status_message(status_msg);
        
        // Start the game loop timer
        if (!frame_timer_id) {
            printf("Starting game timer\n");
            frame_timer_id = g_timeout_add(17, frame_update_callback, this);
        }
    }
    
    // Run GTK main loop
    gtk_main();
}

void GTK3MainWindow::shutdown() {
    game_running = false;
    
    if (frame_timer_id) {
        g_source_remove(frame_timer_id);
        frame_timer_id = 0;
    }
    
    if (engine) {
        delete engine;
        engine = nullptr;
    }
    
    // Close audio (FIXED: Added audio cleanup)
    SDL_CloseAudio();
    
    cleanup_sdl();
    save_key_mappings();
}

void GTK3MainWindow::cleanup_sdl() {
    if (sdl_texture) {
        SDL_DestroyTexture(sdl_texture);
        sdl_texture = nullptr;
    }
    
    if (sdl_renderer) {
        SDL_DestroyRenderer(sdl_renderer);
        sdl_renderer = nullptr;
    }
    
    if (sdl_window) {
        SDL_DestroyWindow(sdl_window);
        sdl_window = nullptr;
    }
    
    SDL_Quit();
}

// Static callback implementations
gboolean GTK3MainWindow::on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->key_states[event->keyval] = true;
    
    if (event->keyval == GDK_KEY_F11) {
        static bool is_fullscreen = false;
        if (is_fullscreen) {
            gtk_window_unfullscreen(GTK_WINDOW(window->window));
            is_fullscreen = false;
        } else {
            gtk_window_fullscreen(GTK_WINDOW(window->window));
            is_fullscreen = true;
        }
    }
    
    return FALSE;
}

gboolean GTK3MainWindow::on_key_release(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->key_states[event->keyval] = false;
    return FALSE;
}

gboolean GTK3MainWindow::on_window_delete(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->shutdown();
    return FALSE;
}

void GTK3MainWindow::on_window_destroy(GtkWidget* widget, gpointer user_data) {
    gtk_main_quit();
}

// Menu callbacks
void GTK3MainWindow::on_file_open(GtkMenuItem* item, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    
    GtkWidget* dialog = gtk_file_chooser_dialog_new("Open ROM File",
                                                   GTK_WINDOW(window->window),
                                                   GTK_FILE_CHOOSER_ACTION_OPEN,
                                                   "_Cancel", GTK_RESPONSE_CANCEL,
                                                   "_Open", GTK_RESPONSE_ACCEPT,
                                                   nullptr);
    
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "NES ROM Files");
    gtk_file_filter_add_pattern(filter, "*.nes");
    gtk_file_filter_add_pattern(filter, "*.NES");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        
        // Stop any existing game
        if (window->game_running) {
            window->game_running = false;
            if (window->frame_timer_id) {
                g_source_remove(window->frame_timer_id);
                window->frame_timer_id = 0;
            }
        }
        
        if (window->engine) {
            delete window->engine;
            window->engine = nullptr;
        }
        
        // Load new ROM
        window->engine = new WarpNES();
        
        if (!window->engine->loadROM(filename)) {
            window->set_status_message("Failed to load ROM file");
            delete window->engine;
            window->engine = nullptr;
        } else {
            window->engine->reset();
            window->game_running = true;
            window->game_paused = false;
            
            char status_msg[512];
            snprintf(status_msg, sizeof(status_msg), "ROM loaded: %s", filename);
            window->set_status_message(status_msg);
            
            // Start the game loop timer
            if (!window->frame_timer_id) {
                printf("Starting game timer from file open\n");
                window->frame_timer_id = g_timeout_add(17, frame_update_callback, window);
            }
        }
        
        g_free(filename);
    }
    
    gtk_widget_destroy(dialog);
}

void GTK3MainWindow::on_file_quit(GtkMenuItem* item, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->shutdown();
    gtk_main_quit();
}

void GTK3MainWindow::on_game_reset(GtkMenuItem* item, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    
    if (window->engine) {
        window->engine->reset();
        window->set_status_message("Game reset");
    }
}

void GTK3MainWindow::on_game_pause(GtkMenuItem* item, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    
    if (window->game_running) {
        window->game_paused = !window->game_paused;
        window->set_status_message(window->game_paused ? "Game paused" : "Game resumed");
    }
}

void GTK3MainWindow::on_options_controls(GtkMenuItem* item, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->show_controls_dialog();
}

void GTK3MainWindow::on_options_video(GtkMenuItem* item, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->show_video_options_dialog();
}

void GTK3MainWindow::on_help_about(GtkMenuItem* item, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->show_about_dialog();
}

// Dialog implementations
void GTK3MainWindow::show_controls_dialog() {
    GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                              GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_INFO,
                                              GTK_BUTTONS_OK,
                                              "Controls:\nPlayer 1: Arrow keys, X/Z, ]/[ (Start/Select)\nPlayer 2: WASD, J/K, I/U (Start/Select)\nF11: Fullscreen");
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void GTK3MainWindow::show_video_options_dialog() {
    GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                              GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_INFO,
                                              GTK_BUTTONS_OK,
                                              "Video Options:\nF11 - Toggle fullscreen\nSDL handles VSync automatically");
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void GTK3MainWindow::show_about_dialog() {
    GtkWidget* dialog = gtk_about_dialog_new();
    
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "WarpNES GTK3+SDL");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), "1.0");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), 
                                 "A high-performance NES emulator with GTK3 interface and SDL rendering");
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), 
                                  "Original Nintendo games © Nintendo\nWarpNES Emulator © 2025 Jason Hall");
    
    const gchar* authors[] = {"Jason Hall", nullptr};
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);
    
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window));
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

// Configuration file handling
void GTK3MainWindow::load_key_mappings() {
    FILE* file = fopen("gtk3_controls.cfg", "r");
    if (!file) {
        file = fopen("controls.cfg", "r");
        if (!file) return;
    }
    
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        
        char key_name[64];
        guint keyval;
        if (sscanf(line, "%63[^=]=%u", key_name, &keyval) == 2) {
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

void GTK3MainWindow::save_key_mappings() {
    FILE* file = fopen("gtk3_controls.cfg", "w");
    if (!file) return;
    
    fprintf(file, "# GTK3+SDL WarpNES Control Configuration\n");
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

void GTK3MainWindow::set_status_message(const char* message) {
    if (status_message_id) {
        gtk_statusbar_remove(GTK_STATUSBAR(status_bar), 0, status_message_id);
    }
    
    strncpy(status_message, message, sizeof(status_message) - 1);
    status_message[sizeof(status_message) - 1] = '\0';
    
    status_message_id = gtk_statusbar_push(GTK_STATUSBAR(status_bar), 0, status_message);
}

// Main function
int main(int argc, char* argv[]) {
    Configuration::initialize("config.cfg");
    
    printf("WarpNES GTK3+SDL - Hardware Accelerated NES Emulator\n");
    printf("Using SDL for rendering with GTK3 interface\n");
    
    const char* rom_filename = nullptr;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--neszapper") == 0) {
            printf("NES Zapper support noted (not yet implemented in GTK3+SDL version)\n");
        } else if (argv[i][0] != '-') {
            rom_filename = argv[i];
        }
    }
    
    GTK3MainWindow main_window;
    
    if (!main_window.initialize()) {
        fprintf(stderr, "Failed to initialize GTK3+SDL window\n");
        return 1;
    }
    
    printf("GTK3+SDL window initialized successfully\n");
    
    if (rom_filename) {
        printf("Loading ROM: %s\n", rom_filename);
        main_window.run(rom_filename);
    } else {
        printf("No ROM specified - use File > Open ROM to load a game\n");
        main_window.run(nullptr);
    }
    
    return 0;
}\
