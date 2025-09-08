#include <cstdio>
#include <iostream>
#include <string>
#include <vector>
#include <signal.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <cstring>

#include <SDL2/SDL.h>

#include "Emulation/WarpNES.hpp"
#include "Configuration.hpp"
#include "Constants.hpp"

// NSF file structure
struct NSFHeader {
    char magic[5];          // "NESM" + 0x1A
    uint8_t version;        // Version number
    uint8_t total_songs;    // Total number of songs
    uint8_t starting_song;  // Starting song (1-based)
    uint16_t load_addr;     // Load address
    uint16_t init_addr;     // Init address
    uint16_t play_addr;     // Play address
    char title[32];         // Song title
    char artist[32];        // Artist name
    char copyright[32];     // Copyright info
    uint16_t ntsc_speed;    // NTSC speed (1/1000000 sec ticks)
    uint8_t bankswitch[8];  // Bankswitch init values
    uint16_t pal_speed;     // PAL speed
    uint8_t pal_ntsc_bits;  // PAL/NTSC bits
    uint8_t extra_sound;    // Extra sound chip support
    uint8_t expansion[4];   // Expansion (reserved)
    // ROM data follows...
};

class NSFPlayer {
private:
    WarpNES* engine;
    NSFHeader header;
    bool is_loaded;
    bool is_playing;
    bool is_paused;
    int current_song;
    std::vector<uint8_t> nsf_data;
    std::thread emulation_thread;
    bool emulation_running;
    
    static void audioCallback(void* userdata, uint8_t* buffer, int len) {
        NSFPlayer* player = static_cast<NSFPlayer*>(userdata);
        if (player->engine && player->is_playing && !player->is_paused) {
            player->engine->audioCallback(buffer, len);
        } else {
            // Silence when paused or stopped
            memset(buffer, 0, len);
        }
    }

public:
    NSFPlayer() : engine(nullptr), is_loaded(false), is_playing(false), 
                  is_paused(false), current_song(1), emulation_running(false) {
        memset(&header, 0, sizeof(header));
    }
    
    ~NSFPlayer() {
        cleanup();
    }
    
    bool loadNSF(const std::string& filename) {
        // Read NSF file first to get header info
        FILE* file = fopen(filename.c_str(), "rb");
        if (!file) {
            std::cerr << "Error: Could not open NSF file" << std::endl;
            return false;
        }
        
        // Read and validate NSF header
        if (fread(&header, sizeof(header), 1, file) != 1) {
            std::cerr << "Error: Could not read NSF header" << std::endl;
            fclose(file);
            return false;
        }
        
        // Verify NSF magic
        if (strncmp(header.magic, "NESM\x1A", 5) != 0) {
            std::cerr << "Error: Invalid NSF file format" << std::endl;
            fclose(file);
            return false;
        }
        
        fclose(file);
        
        // Now initialize WarpNES engine
        engine = new WarpNES();
        
        // Load NSF file using WarpNES NSF support
        if (!engine->loadNSF(filename.c_str())) {
            std::cerr << "Error: Could not load NSF file into engine" << std::endl;
            delete engine;
            engine = nullptr;
            return false;
        }
        
        current_song = header.starting_song;
        if (current_song == 0) current_song = 1; // Ensure valid song number
        is_loaded = true;
        
        std::cout << "NSF file loaded successfully" << std::endl;
        return true;
    }
    
    void initializeSong(int song_number) {
        if (!engine || song_number < 1 || song_number > header.total_songs) return;
        
        engine->initNSFSong(song_number);
        current_song = song_number;
        std::cout << "Initialized song " << song_number << std::endl;
    }
    
    bool initializeAudio() {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
            std::cerr << "Error: Could not initialize SDL audio: " << SDL_GetError() << std::endl;
            return false;
        }
        
        SDL_AudioSpec desiredSpec;
        desiredSpec.freq = 48000;
        desiredSpec.format = AUDIO_S8;
        desiredSpec.channels = 1;
        desiredSpec.samples = 2048;
        desiredSpec.callback = audioCallback;
        desiredSpec.userdata = this;

        SDL_AudioSpec obtainedSpec;
        if (SDL_OpenAudio(&desiredSpec, &obtainedSpec) < 0) {
            std::cerr << "Error: Could not open audio: " << SDL_GetError() << std::endl;
            return false;
        }
        
        return true;
    }
    
    void play() {
        if (!is_loaded || !engine) {
            std::cerr << "Error: No NSF file loaded" << std::endl;
            return;
        }
        
        is_playing = true;
        is_paused = false;
        SDL_PauseAudio(0);
        std::cout << "Playing song " << current_song << "/" << static_cast<int>(header.total_songs) << std::endl;
        
        // Start the emulation loop
        startEmulationLoop();
    }
    
    void startEmulationLoop() {
        if (emulation_running) {
            return; // Already running
        }
        
        emulation_running = true;
        emulation_thread = std::thread([this]() {
            while (emulation_running && is_playing) {
                if (engine && !is_paused) {
                    engine->update();
                }
                
                // Sleep for about 1/60th of a second (16.67ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
    }
    
    void pause() {
        is_paused = !is_paused;
        if (is_paused) {
            SDL_PauseAudio(1);
        } else {
            SDL_PauseAudio(0);
        }
        std::cout << (is_paused ? "Paused" : "Resumed") << std::endl;
    }
    
    void stop() {
        is_playing = false;
        is_paused = false;
        SDL_PauseAudio(1);
        
        // Stop emulation thread
        if (emulation_running) {
            emulation_running = false;
            if (emulation_thread.joinable()) {
                emulation_thread.join();
            }
        }
        
        std::cout << "Stopped" << std::endl;
    }
    
    void nextSong() {
        if (current_song < header.total_songs) {
            current_song++;
            initializeSong(current_song);
            std::cout << "Switched to song " << current_song << "/" << static_cast<int>(header.total_songs) << std::endl;
        }
    }
    
    void prevSong() {
        if (current_song > 1) {
            current_song--;
            initializeSong(current_song);
            std::cout << "Switched to song " << current_song << "/" << static_cast<int>(header.total_songs) << std::endl;
        }
    }
    
    void selectSong(int song_num) {
        if (song_num >= 1 && song_num <= header.total_songs) {
            current_song = song_num;
            initializeSong(current_song);
            std::cout << "Selected song " << current_song << "/" << static_cast<int>(header.total_songs) << std::endl;
        } else {
            std::cout << "Invalid song number. Valid range: 1-" << static_cast<int>(header.total_songs) << std::endl;
        }
    }
    
    void printInfo() {
        if (!is_loaded) {
            std::cout << "No NSF file loaded" << std::endl;
            return;
        }
        
        std::cout << "\n=== NSF File Information ===" << std::endl;
        std::cout << "Title: " << std::string(header.title, strnlen(header.title, 32)) << std::endl;
        std::cout << "Artist: " << std::string(header.artist, strnlen(header.artist, 32)) << std::endl;
        std::cout << "Copyright: " << std::string(header.copyright, strnlen(header.copyright, 32)) << std::endl;
        std::cout << "Version: " << static_cast<int>(header.version) << std::endl;
        std::cout << "Total Songs: " << static_cast<int>(header.total_songs) << std::endl;
        std::cout << "Starting Song: " << static_cast<int>(header.starting_song) << std::endl;
        std::cout << "Current Song: " << current_song << std::endl;
        std::cout << "Load Address: $" << std::hex << header.load_addr << std::endl;
        std::cout << "Init Address: $" << std::hex << header.init_addr << std::endl;
        std::cout << "Play Address: $" << std::hex << header.play_addr << std::endl;
        std::cout << "NTSC Speed: " << header.ntsc_speed << " μs" << std::endl;
        std::cout << std::dec << "============================\n" << std::endl;
    }
    
    void printHelp() {
        std::cout << "\n=== NSF Player Commands ===" << std::endl;
        std::cout << "p/space - Play/Pause" << std::endl;
        std::cout << "s - Stop" << std::endl;
        std::cout << "n/+ - Next song" << std::endl;
        std::cout << "b/- - Previous song" << std::endl;
        std::cout << "1-9 - Select song number" << std::endl;
        std::cout << "i - Show file information" << std::endl;
        std::cout << "h/? - Show this help" << std::endl;
        std::cout << "q - Quit" << std::endl;
        std::cout << "===========================\n" << std::endl;
    }
    
    bool isLoaded() const { return is_loaded; }
    bool isPlaying() const { return is_playing; }
    bool isPaused() const { return is_paused; }
    
    void cleanup() {
        if (is_playing) {
            stop();
        }
        
        if (engine) {
            delete engine;
            engine = nullptr;
        }
        
        SDL_CloseAudio();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
};

// Global player instance for signal handling
NSFPlayer* g_player = nullptr;

void signalHandler(int signal) {
    if (g_player) {
        std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
        g_player->stop();
        g_player->cleanup();
    }
    exit(0);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <nsf_file>" << std::endl;
        std::cout << "NSF Player - A command-line Nintendo Sound Format player using WarpNES" << std::endl;
        return -1;
    }
    
    // Initialize configuration (needed by WarpNES)
    Configuration::initialize("config.ini");
    
    // Set up signal handling
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    NSFPlayer player;
    g_player = &player;
    
    // Load NSF file
    if (!player.loadNSF(argv[1])) {
        std::cerr << "Failed to load NSF file: " << argv[1] << std::endl;
        return -1;
    }
    
    // Initialize audio
    if (!player.initializeAudio()) {
        std::cerr << "Failed to initialize audio system" << std::endl;
        return -1;
    }
    
    // Print initial information
    std::cout << "NSF Player - Loaded: " << argv[1] << std::endl;
    player.printInfo();
    player.printHelp();
    
    // Auto-start playback
    player.play();
    
    // Main command loop
    std::cout << "NSF> ";
    std::cout.flush();
    
    char command;
    while (std::cin >> command) {
        switch (command) {
            case 'p':
            case ' ':
                if (player.isPlaying() && !player.isPaused()) {
                    player.pause();
                } else {
                    player.play();
                }
                break;
                
            case 's':
                player.stop();
                break;
                
            case 'n':
            case '+':
                player.nextSong();
                break;
                
            case 'b':
            case '-':
                player.prevSong();
                break;
                
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9':
                player.selectSong(command - '0');
                break;
                
            case 'i':
                player.printInfo();
                break;
                
            case 'h':
            case '?':
                player.printHelp();
                break;
                
            case 'q':
                std::cout << "Quitting..." << std::endl;
                player.stop();
                player.cleanup();
                return 0;
                
            default:
                std::cout << "Unknown command. Type 'h' for help." << std::endl;
                break;
        }
        
        std::cout << "NSF> ";
        std::cout.flush();
    }
    
    player.cleanup();
    return 0;
}
