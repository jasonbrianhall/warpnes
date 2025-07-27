#include "GTKMainWindow.hpp"
#include "Emulation/WarpNES.hpp"
#include "Emulation/ControllerSDL.hpp"
#include "Configuration.hpp"
#include "Constants.hpp"

GTK3MainWindow::GTK3MainWindow() 
    : window(nullptr), gl_area(nullptr), engine(nullptr), 
      game_running(false), game_paused(false),
      nes_texture(0), shader_program(0), vertex_buffer(0), vertex_array(0), element_buffer(0),
      frame_timer_id(0), status_message_id(0)
{
    strcpy(status_message, "Ready");
    
    // Initialize Player 2 default keys
    player2_keys.up = GDK_KEY_w;
    player2_keys.down = GDK_KEY_s;
    player2_keys.left = GDK_KEY_a;
    player2_keys.right = GDK_KEY_d;
    player2_keys.button_a = GDK_KEY_g;
    player2_keys.button_b = GDK_KEY_f;
    player2_keys.start = GDK_KEY_p;
    player2_keys.select = GDK_KEY_o;
}

GTK3MainWindow::~GTK3MainWindow() {
    shutdown();
}

bool GTK3MainWindow::initialize() {
    // Initialize GTK
    gtk_init(nullptr, nullptr);
    
    create_widgets();
    load_key_mappings();
    
    // Show the window
    gtk_widget_show_all(window);
    
    set_status_message("WarpNES GTK3 - Load a ROM file to begin");
    
    return true;
}

void GTK3MainWindow::create_widgets() {
    // Create main window
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "WarpNES GTK3");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    
    // Connect window signals
    g_signal_connect(window, "delete-event", G_CALLBACK(on_window_delete), this);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), this);
    g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_press), this);
    g_signal_connect(window, "key-release-event", G_CALLBACK(on_key_release), this);
    
    // Set up keyboard focus
    gtk_widget_set_can_focus(window, TRUE);
    
    // Create main vertical box
    main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(window), main_vbox);
    
    // Create menubar
    create_menubar();
    gtk_box_pack_start(GTK_BOX(main_vbox), menubar, FALSE, FALSE, 0);
    
    // Create OpenGL area
    setup_gl_area();
    gtk_box_pack_start(GTK_BOX(main_vbox), gl_area, TRUE, TRUE, 0);
    
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

void GTK3MainWindow::setup_gl_area() {
    // Create GTK3 GtkGLArea widget
    gl_area = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(gl_area), 2, 1);  // Use OpenGL 2.1 for compatibility
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(gl_area), FALSE);
    
    // Set minimum size to NES resolution
    gtk_widget_set_size_request(gl_area, 256, 240);
    
    // Connect OpenGL signals
    g_signal_connect(gl_area, "realize", G_CALLBACK(on_gl_realize), this);
    g_signal_connect(gl_area, "unrealize", G_CALLBACK(on_gl_unrealize), this);
    g_signal_connect(gl_area, "render", G_CALLBACK(on_gl_draw), this);
    g_signal_connect(gl_area, "resize", G_CALLBACK(on_gl_resize), this);
}

bool GTK3MainWindow::init_opengl() {
    // Clear any existing OpenGL errors
    while (glGetError() != GL_NO_ERROR);
    
    print_gl_info();
    
    // Setup OpenGL state
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LIGHTING);
    
    // Create texture for NES framebuffer
    glGenTextures(1, &nes_texture);
    glBindTexture(GL_TEXTURE_2D, nes_texture);
    
    // Use nearest neighbor for authentic pixel art look
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Initialize with black texture
    std::vector<uint8_t> black_texture(256 * 240 * 3, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 256, 240, 0, GL_RGB, GL_UNSIGNED_BYTE, black_texture.data());
    
    // Check for OpenGL errors
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        fprintf(stderr, "OpenGL error during initialization: %d\n", error);
        return false;
    }
    
    printf("GTK3 OpenGL initialization successful\n");
    return true;
}

void GTK3MainWindow::render_frame() {
    // Clear the screen
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    
    if (game_running && engine) {
        // Update texture with latest NES frame
        update_texture();
        
        // Set up orthographic projection
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, 1, 0, 1, -1, 1);
        
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        
        // Draw textured quad
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, nes_texture);
        
        glBegin(GL_QUADS);
        glTexCoord2f(0.0f, 1.0f); glVertex2f(0.0f, 0.0f);
        glTexCoord2f(1.0f, 1.0f); glVertex2f(1.0f, 0.0f);
        glTexCoord2f(1.0f, 0.0f); glVertex2f(1.0f, 1.0f);
        glTexCoord2f(0.0f, 0.0f); glVertex2f(0.0f, 1.0f);
        glEnd();
        
        glDisable(GL_TEXTURE_2D);
    }
    
    glFlush();
}

gboolean GTK3MainWindow::on_key_release(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->key_states[event->keyval] = false;
    return FALSE;
}

gboolean GTK3MainWindow::on_gl_draw(GtkGLArea* area, GdkGLContext* context, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    window->render_frame();
    return TRUE;
}

void GTK3MainWindow::on_gl_realize(GtkGLArea* area, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    
    gtk_gl_area_make_current(area);
    
    if (gtk_gl_area_get_error(area) != nullptr) {
        fprintf(stderr, "Failed to realize GL area\n");
        return;
    }
    
    printf("OpenGL context realized\n");
    window->init_opengl();
}

void GTK3MainWindow::on_gl_unrealize(GtkGLArea* area, gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    
    gtk_gl_area_make_current(area);
    window->cleanup_opengl();
}

void GTK3MainWindow::on_gl_resize(GtkGLArea* area, gint width, gint height, gpointer user_data) {
    glViewport(0, 0, width, height);
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
            
            gtk_widget_queue_draw(window->gl_area);
            
        }
        
        g_free(filename);
    }
    
    gtk_widget_destroy(dialog);
    
    // If we loaded a ROM, start the game loop after dialog is closed
    if (window->game_running && window->engine) {
        printf("Starting game loop from menu\n");
        window->run(nullptr);  // Don't load ROM again, just run the loop
    }
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
                                              "Controls:\nPlayer 1: Arrow keys, X/Z, Enter/Space\nPlayer 2: WASD, F/G, P/O\nF11: Fullscreen");
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void GTK3MainWindow::show_video_options_dialog() {
    GtkWidget* dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                              GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_INFO,
                                              GTK_BUTTONS_OK,
                                              "Video Options:\nF11 - Toggle fullscreen\nNearest neighbor scaling for authentic pixels");
    
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

void GTK3MainWindow::show_about_dialog() {
    GtkWidget* dialog = gtk_about_dialog_new();
    
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), "WarpNES GTK3");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), "1.0");
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), 
                                 "A high-performance NES emulator with GTK3 interface and OpenGL acceleration");
    gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), 
                                  "Original Nintendo games © Nintendo\nWarpNES Emulator © 2024");
    
    const gchar* authors[] = {"WarpNES Development Team", nullptr};
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
    
    fprintf(file, "# GTK3 WarpNES Control Configuration\n");
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

void GTK3MainWindow::update_game() {
    // Implementation for update_game if needed
}


void GTK3MainWindow::update_texture() {
    if (!engine) {
        printf("Engine is null in update_texture!\n");
        return;
    }
    
    // Convert RGB565 to RGB888 for OpenGL
    static uint8_t rgb_buffer[256 * 240 * 3];
    
    // Make sure engine is actually rendering
    engine->render16(frame_buffer);
    
    for (int i = 0; i < 256 * 240; i++) {
        uint16_t pixel = frame_buffer[i];
        
        uint8_t r = (pixel >> 11) & 0x1F;
        uint8_t g = (pixel >> 5) & 0x3F;
        uint8_t b = pixel & 0x1F;
        
        rgb_buffer[i * 3 + 0] = (r << 3) | (r >> 2);
        rgb_buffer[i * 3 + 1] = (g << 2) | (g >> 4);
        rgb_buffer[i * 3 + 2] = (b << 3) | (b >> 2);
    }
    
    glBindTexture(GL_TEXTURE_2D, nes_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 240, GL_RGB, GL_UNSIGNED_BYTE, rgb_buffer);
}

gboolean GTK3MainWindow::frame_update_callback(gpointer user_data) {
    GTK3MainWindow* window = static_cast<GTK3MainWindow*>(user_data);
    
    if (window->game_running && !window->game_paused && window->engine) {
        window->process_input();
        window->engine->update();
        gtk_widget_queue_draw(window->gl_area);  // Change this line
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
    // Only load ROM if one isn't already loaded
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
        
        gtk_widget_queue_draw(gl_area);
    }
    
    // Start the game loop if we have a ROM loaded
    if (game_running && engine) {
        printf("Starting main game loop\n");
        
        while (game_running) {
            while (gtk_events_pending()) {
                gtk_main_iteration();
            }
            
            if (!game_paused && engine) {
                process_input();
                engine->update();
                gtk_widget_queue_draw(gl_area);
            }
            
            usleep(16667);
        }
    } else {
        gtk_main();
    }
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
    
    cleanup_opengl();
    save_key_mappings();
}

void GTK3MainWindow::cleanup_opengl() {
    if (nes_texture) {
        glDeleteTextures(1, &nes_texture);
        nes_texture = 0;
    }
}

void GTK3MainWindow::set_status_message(const char* message) {
    if (status_message_id) {
        gtk_statusbar_remove(GTK_STATUSBAR(status_bar), 0, status_message_id);
    }
    
    strncpy(status_message, message, sizeof(status_message) - 1);
    status_message[sizeof(status_message) - 1] = '\0';
    
    status_message_id = gtk_statusbar_push(GTK_STATUSBAR(status_bar), 0, status_message);
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

// OpenGL helper functions
void GTK3MainWindow::print_gl_info() {
    printf("OpenGL Version: %s\n", glGetString(GL_VERSION));
    printf("OpenGL Renderer: %s\n", glGetString(GL_RENDERER));
    printf("OpenGL Vendor: %s\n", glGetString(GL_VENDOR));
}

bool GTK3MainWindow::check_gl_extension(const char* extension) {
    const char* extensions = (const char*)glGetString(GL_EXTENSIONS);
    return (extensions && strstr(extensions, extension) != nullptr);
}

void GTK3MainWindow::setup_shaders() {
    // Not implemented - using immediate mode OpenGL for compatibility
}

void GTK3MainWindow::setup_vertex_buffer() {
    // Not implemented - using immediate mode OpenGL for compatibility
}

// Main function
int main(int argc, char* argv[]) {
    Configuration::initialize("config.cfg");
    
    printf("WarpNES GTK3 - Hardware Accelerated NES Emulator\n");
    printf("Using OpenGL for rendering with GTK3 interface\n");
    
    const char* rom_filename = nullptr;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--neszapper") == 0) {
            printf("NES Zapper support noted (not yet implemented in GTK3 version)\n");
        } else if (argv[i][0] != '-') {
            rom_filename = argv[i];
        }
    }
    
    GTK3MainWindow main_window;
    
    if (!main_window.initialize()) {
        fprintf(stderr, "Failed to initialize GTK3 window\n");
        return 1;
    }
    
    if (rom_filename) {
        printf("Loading ROM: %s\n", rom_filename);
        main_window.run(rom_filename);
    } else {
        printf("No ROM specified - use File > Open ROM to load a game\n");
        gtk_main();
    }
    
    return 0;
}
