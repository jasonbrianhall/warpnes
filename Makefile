# Makefile for warpnes with SDL, Allegro, and GTK3 Support for Linux and Windows
# Supports triple builds: SDL (modern), Allegro (retro), and GTK3 (desktop) versions
# Last modification: $(shell date +%m/%d/%Y)

# Compiler settings
CXX_LINUX = g++
CXX_WIN = x86_64-w64-mingw32-g++
CXXFLAGS_COMMON = -s -fpermissive

# Debug flags
DEBUG_FLAGS = -g -DDEBUG

# SDL flags for Linux and Windows
SDL_CFLAGS_LINUX = $(shell pkg-config --cflags sdl2 2>/dev/null || echo "-I/usr/include/SDL2")
SDL_LIBS_LINUX = $(shell pkg-config --libs sdl2 2>/dev/null || echo "-lSDL2")

SDL_CFLAGS_WIN = -I/usr/x86_64-w64-mingw32/include/SDL2
SDL_LIBS_WIN = -lmingw32 -lSDL2main -lSDL2 -static-libgcc -static-libstdc++

# Allegro flags for Linux and Windows
ALLEGRO_CFLAGS_LINUX = -I/usr/include
ALLEGRO_LIBS_LINUX = -lalleg

ALLEGRO_CFLAGS_WIN = -I/usr/x86_64-w64-mingw32/include
ALLEGRO_LIBS_WIN = -lalleg -lwinmm -static-libgcc -static-libstdc++

# GTK3 flags for Linux only (uses GTK3's built-in OpenGL support)
GTK3_CFLAGS_LINUX = $(shell pkg-config --cflags gtk+-3.0 2>/dev/null || echo "-I/usr/include/gtk-3.0") $(shell pkg-config --cflags sdl2 2>/dev/null || echo "-I/usr/include/SDL2")
GTK3_LIBS_LINUX = $(shell pkg-config --libs gtk+-3.0 gl glu sdl2 2>/dev/null || echo "-lgtk-3 -lgdk-3 -lSDL2")

# SDL Platform-specific settings
CXXFLAGS_LINUX_SDL = $(CXXFLAGS_COMMON) $(SDL_CFLAGS_LINUX) -DLINUX -DSDL_BUILD -std=c++17
CXXFLAGS_WIN_SDL = $(CXXFLAGS_COMMON) $(SDL_CFLAGS_WIN)     -DWIN32 -DSDL_BUILD -std=c++17

# Allegro Platform-specific settings  
CXXFLAGS_LINUX_ALLEGRO = $(CXXFLAGS_COMMON) $(ALLEGRO_CFLAGS_LINUX) -DLINUX -DALLEGRO_BUILD
CXXFLAGS_WIN_ALLEGRO = $(CXXFLAGS_COMMON) $(ALLEGRO_CFLAGS_WIN)     -DWIN32 -DALLEGRO_BUILD

# GTK3 Platform-specific settings (Linux only)
CXXFLAGS_LINUX_GTK3 = $(CXXFLAGS_COMMON) $(GTK3_CFLAGS_LINUX) -DLINUX -DGTK3_BUILD -std=c++17

# Debug-specific flags
CXXFLAGS_LINUX_SDL_DEBUG = $(CXXFLAGS_LINUX_SDL) $(DEBUG_FLAGS)
CXXFLAGS_WIN_SDL_DEBUG = $(CXXFLAGS_WIN_SDL) $(DEBUG_FLAGS)
CXXFLAGS_LINUX_ALLEGRO_DEBUG = $(CXXFLAGS_LINUX_ALLEGRO) $(DEBUG_FLAGS)
CXXFLAGS_WIN_ALLEGRO_DEBUG = $(CXXFLAGS_WIN_ALLEGRO) $(DEBUG_FLAGS)
CXXFLAGS_LINUX_GTK3_DEBUG = $(CXXFLAGS_LINUX_GTK3) $(DEBUG_FLAGS)

# Linker flags
LDFLAGS_LINUX_SDL = $(SDL_LIBS_LINUX)
LDFLAGS_WIN_SDL = $(SDL_LIBS_WIN)
LDFLAGS_LINUX_ALLEGRO = $(ALLEGRO_LIBS_LINUX)
LDFLAGS_WIN_ALLEGRO = $(ALLEGRO_LIBS_WIN)
LDFLAGS_LINUX_GTK3 = $(GTK3_LIBS_LINUX)

# Windows DLL settings
DLL_SOURCE_DIR = /usr/x86_64-w64-mingw32/sys-root/mingw/bin

# Common source files (used by all platforms)
COMMON_SOURCE_FILES = \
    source/Configuration.cpp \
    source/Emulation/APU.cpp \
    source/Emulation/PPU.cpp \
    source/Zapper.cpp \
    source/helper.cpp \
    source/Emulation/WarpNES.cpp \
    source/Emulation/AllegroMidi.cpp \
    source/Emulation/Battery.cpp \
    source/Emulation/Instructions.cpp \
    source/Emulation/GxROM.cpp \
    source/Emulation/UxROM.cpp \
    source/Emulation/MMC1.cpp \
    source/Emulation/MMC2.cpp \
    source/Emulation/MMC3.cpp

# Platform-specific source files
SDL_SOURCE_FILES = $(COMMON_SOURCE_FILES) \
    source/SDLMainWindow.cpp \
    source/SDLCacheScaling.cpp \
    source/Emulation/ControllerSDL.cpp

ALLEGRO_SOURCE_FILES = $(COMMON_SOURCE_FILES) \
    source/dos_main.cpp \
    source/Emulation/Controller.cpp

GTK3_SOURCE_FILES = $(COMMON_SOURCE_FILES) \
    source/GTKMainWindow.cpp \
    source/Emulation/ControllerSDL.cpp

# Object files for SDL versions
OBJS_LINUX_SDL = $(patsubst %.cpp,%.sdl.o,$(SDL_SOURCE_FILES))
OBJS_WIN_SDL = $(patsubst %.cpp,%.win.sdl.o,$(SDL_SOURCE_FILES))
OBJS_LINUX_SDL_DEBUG = $(patsubst %.cpp,%.sdl.debug.o,$(SDL_SOURCE_FILES))
OBJS_WIN_SDL_DEBUG = $(patsubst %.cpp,%.win.sdl.debug.o,$(SDL_SOURCE_FILES))

# Object files for Allegro versions
OBJS_LINUX_ALLEGRO = $(patsubst %.cpp,%.allegro.o,$(ALLEGRO_SOURCE_FILES))
OBJS_WIN_ALLEGRO = $(patsubst %.cpp,%.win.allegro.o,$(ALLEGRO_SOURCE_FILES))
OBJS_LINUX_ALLEGRO_DEBUG = $(patsubst %.cpp,%.allegro.debug.o,$(ALLEGRO_SOURCE_FILES))
OBJS_WIN_ALLEGRO_DEBUG = $(patsubst %.cpp,%.win.allegro.debug.o,$(ALLEGRO_SOURCE_FILES))

# Object files for GTK3 versions (Linux only)
OBJS_LINUX_GTK3 = $(patsubst %.cpp,%.gtk3.o,$(GTK3_SOURCE_FILES))
OBJS_LINUX_GTK3_DEBUG = $(patsubst %.cpp,%.gtk3.debug.o,$(GTK3_SOURCE_FILES))

# Target executables - SDL versions
TARGET_LINUX_SDL = warpnes-sdl
TARGET_WIN_SDL = warpnes-sdl.exe
TARGET_LINUX_SDL_DEBUG = warpnes-sdl_debug
TARGET_WIN_SDL_DEBUG = warpnes-sdl_debug.exe

# Target executables - Allegro versions
TARGET_LINUX_ALLEGRO = warpnes-allegro
TARGET_WIN_ALLEGRO = warpnes-allegro.exe
TARGET_LINUX_ALLEGRO_DEBUG = warpnes-allegro_debug
TARGET_WIN_ALLEGRO_DEBUG = warpnes-allegro_debug.exe

# Target executables - GTK3 versions (Linux only)
TARGET_LINUX_GTK3 = warpnes-gtk3
TARGET_LINUX_GTK3_DEBUG = warpnes-gtk3_debug

# Build directories
BUILD_DIR = build
BUILD_DIR_LINUX_SDL = $(BUILD_DIR)/linux-sdl
BUILD_DIR_WIN_SDL = $(BUILD_DIR)/windows-sdl
BUILD_DIR_LINUX_ALLEGRO = $(BUILD_DIR)/linux-allegro
BUILD_DIR_WIN_ALLEGRO = $(BUILD_DIR)/windows-allegro
BUILD_DIR_LINUX_SDL_DEBUG = $(BUILD_DIR)/linux-sdl-debug
BUILD_DIR_WIN_SDL_DEBUG = $(BUILD_DIR)/windows-sdl-debug
BUILD_DIR_LINUX_ALLEGRO_DEBUG = $(BUILD_DIR)/linux-allegro-debug
BUILD_DIR_WIN_ALLEGRO_DEBUG = $(BUILD_DIR)/windows-allegro-debug
BUILD_DIR_LINUX_GTK3 = $(BUILD_DIR)/linux-gtk3
BUILD_DIR_LINUX_GTK3_DEBUG = $(BUILD_DIR)/linux-gtk3-debug

# Create necessary directories
$(shell mkdir -p $(BUILD_DIR_LINUX_SDL)/source/Emulation $(BUILD_DIR_LINUX_SDL)/source/SMB \
	$(BUILD_DIR_WIN_SDL)/source/Emulation $(BUILD_DIR_WIN_SDL)/source/SMB \
	$(BUILD_DIR_LINUX_ALLEGRO)/source/Emulation $(BUILD_DIR_LINUX_ALLEGRO)/source/SMB \
	$(BUILD_DIR_WIN_ALLEGRO)/source/Emulation $(BUILD_DIR_WIN_ALLEGRO)/source/SMB \
	$(BUILD_DIR_LINUX_SDL_DEBUG)/source/Emulation $(BUILD_DIR_LINUX_SDL_DEBUG)/source/SMB \
	$(BUILD_DIR_WIN_SDL_DEBUG)/source/Emulation $(BUILD_DIR_WIN_SDL_DEBUG)/source/SMB \
	$(BUILD_DIR_LINUX_ALLEGRO_DEBUG)/source/Emulation $(BUILD_DIR_LINUX_ALLEGRO_DEBUG)/source/SMB \
	$(BUILD_DIR_WIN_ALLEGRO_DEBUG)/source/Emulation $(BUILD_DIR_WIN_ALLEGRO_DEBUG)/source/SMB \
	$(BUILD_DIR_LINUX_GTK3)/source/Emulation $(BUILD_DIR_LINUX_GTK3)/source/SMB \
	$(BUILD_DIR_LINUX_GTK3_DEBUG)/source/Emulation $(BUILD_DIR_LINUX_GTK3_DEBUG)/source/SMB)

# Default target - build Allegro versions for Linux (unchanged for compatibility)
.PHONY: all
all: linux-sdl linux-allegro linux-gtk3

# Main build targets
.PHONY: linux
linux: linux-sdl linux-allegro linux-gtk3

.PHONY: windows  
windows: windows-sdl windows-allegro

# SDL-specific targets
.PHONY: linux-sdl
linux-sdl: $(BUILD_DIR_LINUX_SDL)/$(TARGET_LINUX_SDL)

.PHONY: windows-sdl
windows-sdl: $(BUILD_DIR_WIN_SDL)/$(TARGET_WIN_SDL) collect-dlls-win-sdl

# Allegro-specific targets
.PHONY: linux-allegro
linux-allegro: $(BUILD_DIR_LINUX_ALLEGRO)/$(TARGET_LINUX_ALLEGRO)

.PHONY: windows-allegro
windows-allegro: $(BUILD_DIR_WIN_ALLEGRO)/$(TARGET_WIN_ALLEGRO) collect-dlls-win-allegro

# GTK3-specific targets (Linux only)
.PHONY: linux-gtk3
linux-gtk3: $(BUILD_DIR_LINUX_GTK3)/$(TARGET_LINUX_GTK3)

# Debug targets
.PHONY: debug
debug: linux-sdl-debug windows-sdl-debug linux-allegro-debug windows-allegro-debug linux-gtk3-debug

.PHONY: linux-sdl-debug
linux-sdl-debug: $(BUILD_DIR_LINUX_SDL_DEBUG)/$(TARGET_LINUX_SDL_DEBUG)

.PHONY: windows-sdl-debug
windows-sdl-debug: $(BUILD_DIR_WIN_SDL_DEBUG)/$(TARGET_WIN_SDL_DEBUG) collect-dlls-win-sdl-debug

.PHONY: linux-allegro-debug
linux-allegro-debug: $(BUILD_DIR_LINUX_ALLEGRO_DEBUG)/$(TARGET_LINUX_ALLEGRO_DEBUG)

.PHONY: windows-allegro-debug
windows-allegro-debug: $(BUILD_DIR_WIN_ALLEGRO_DEBUG)/$(TARGET_WIN_ALLEGRO_DEBUG) collect-dlls-win-allegro-debug

.PHONY: linux-gtk3-debug
linux-gtk3-debug: $(BUILD_DIR_LINUX_GTK3_DEBUG)/$(TARGET_LINUX_GTK3_DEBUG)

#
# DLL collection targets for Windows builds
#
.PHONY: collect-dlls-win-sdl
collect-dlls-win-sdl: $(BUILD_DIR_WIN_SDL)/$(TARGET_WIN_SDL)
	@echo "Collecting DLLs for Windows SDL build..."
	@if [ -f build/windows-sdl/collect_dlls.sh ]; then \
		build/windows-sdl/collect_dlls.sh $(BUILD_DIR_WIN_SDL)/$(TARGET_WIN_SDL) $(DLL_SOURCE_DIR) $(BUILD_DIR_WIN_SDL); \
	else \
		echo "Warning: collect_dlls.sh not found. Please ensure it exists in build/windows-sdl/"; \
	fi

.PHONY: collect-dlls-win-sdl-debug
collect-dlls-win-sdl-debug: $(BUILD_DIR_WIN_SDL_DEBUG)/$(TARGET_WIN_SDL_DEBUG)
	@echo "Collecting DLLs for Windows SDL debug build..."
	@if [ -f build/windows-sdl/collect_dlls.sh ]; then \
		build/windows-sdl/collect_dlls.sh $(BUILD_DIR_WIN_SDL_DEBUG)/$(TARGET_WIN_SDL_DEBUG) $(DLL_SOURCE_DIR) $(BUILD_DIR_WIN_SDL_DEBUG); \
	else \
		echo "Warning: collect_dlls.sh not found. Please ensure it exists in build/windows-sdl/"; \
	fi

.PHONY: collect-dlls-win-allegro
collect-dlls-win-allegro: $(BUILD_DIR_WIN_ALLEGRO)/$(TARGET_WIN_ALLEGRO)
	@echo "Collecting DLLs for Windows Allegro build..."
	@if [ -f build/windows-allegro/collect_dlls.sh ]; then \
		build/windows-allegro/collect_dlls.sh $(BUILD_DIR_WIN_ALLEGRO)/$(TARGET_WIN_ALLEGRO) $(DLL_SOURCE_DIR) $(BUILD_DIR_WIN_ALLEGRO); \
	else \
		echo "Warning: collect_dlls.sh not found. Please ensure it exists in build/windows-allegro/"; \
	fi

.PHONY: collect-dlls-win-allegro-debug
collect-dlls-win-allegro-debug: $(BUILD_DIR_WIN_ALLEGRO_DEBUG)/$(TARGET_WIN_ALLEGRO_DEBUG)
	@echo "Collecting DLLs for Windows Allegro debug build..."
	@if [ -f build/windows-allegro/collect_dlls.sh ]; then \
		build/windows-allegro/collect_dlls.sh $(BUILD_DIR_WIN_ALLEGRO_DEBUG)/$(TARGET_WIN_ALLEGRO_DEBUG) $(DLL_SOURCE_DIR) $(BUILD_DIR_WIN_ALLEGRO_DEBUG); \
	else \
		echo "Warning: collect_dlls.sh not found. Please ensure it exists in build/windows-allegro/"; \
	fi

# Convenience target to collect all DLLs
.PHONY: collect-dlls-all
collect-dlls-all: collect-dlls-win-sdl collect-dlls-win-allegro collect-dlls-win-sdl-debug collect-dlls-win-allegro-debug

#
# Linux SDL build targets
#
$(BUILD_DIR_LINUX_SDL)/$(TARGET_LINUX_SDL): $(addprefix $(BUILD_DIR_LINUX_SDL)/,$(OBJS_LINUX_SDL))
	@echo "Linking Linux SDL executable..."
	$(CXX_LINUX) $^ -o $@ $(LDFLAGS_LINUX_SDL)
	@echo "Linux SDL build complete: $@"

$(BUILD_DIR_LINUX_SDL)/%.sdl.o: %.cpp
	@echo "Compiling $< for Linux SDL..."
	$(CXX_LINUX) $(CXXFLAGS_LINUX_SDL) -c $< -o $@

#
# Linux SDL debug build targets  
#
$(BUILD_DIR_LINUX_SDL_DEBUG)/$(TARGET_LINUX_SDL_DEBUG): $(addprefix $(BUILD_DIR_LINUX_SDL_DEBUG)/,$(OBJS_LINUX_SDL_DEBUG))
	@echo "Linking Linux SDL debug executable..."
	$(CXX_LINUX) $^ -o $@ $(LDFLAGS_LINUX_SDL)
	@echo "Linux SDL debug build complete: $@"

$(BUILD_DIR_LINUX_SDL_DEBUG)/%.sdl.debug.o: %.cpp
	@echo "Compiling $< for Linux SDL debug..."
	$(CXX_LINUX) $(CXXFLAGS_LINUX_SDL_DEBUG) -c $< -o $@

#
# Windows SDL build targets
#
$(BUILD_DIR_WIN_SDL)/$(TARGET_WIN_SDL): $(addprefix $(BUILD_DIR_WIN_SDL)/,$(OBJS_WIN_SDL))
	@echo "Linking Windows SDL executable..."
	$(CXX_WIN) $^ -o $@ $(LDFLAGS_WIN_SDL)
	@echo "Windows SDL build complete: $@"

$(BUILD_DIR_WIN_SDL)/%.win.sdl.o: %.cpp
	@echo "Compiling $< for Windows SDL..."
	$(CXX_WIN) $(CXXFLAGS_WIN_SDL) -c $< -o $@

#
# Windows SDL debug build targets
#
$(BUILD_DIR_WIN_SDL_DEBUG)/$(TARGET_WIN_SDL_DEBUG): $(addprefix $(BUILD_DIR_WIN_SDL_DEBUG)/,$(OBJS_WIN_SDL_DEBUG))
	@echo "Linking Windows SDL debug executable..."
	$(CXX_WIN) $(CXXFLAGS_WIN_SDL_DEBUG) $^ -o $@ $(LDFLAGS_WIN_SDL)
	@echo "Windows SDL debug build complete: $@"

$(BUILD_DIR_WIN_SDL_DEBUG)/%.win.sdl.debug.o: %.cpp
	@echo "Compiling $< for Windows SDL debug..."
	$(CXX_WIN) $(CXXFLAGS_WIN_SDL_DEBUG) -c $< -o $@

#
# Linux Allegro build targets
#
$(BUILD_DIR_LINUX_ALLEGRO)/$(TARGET_LINUX_ALLEGRO): $(addprefix $(BUILD_DIR_LINUX_ALLEGRO)/,$(OBJS_LINUX_ALLEGRO))
	@echo "Linking Linux Allegro executable..."
	$(CXX_LINUX) $^ -o $@ $(LDFLAGS_LINUX_ALLEGRO)
	@echo "Linux Allegro build complete: $@"

$(BUILD_DIR_LINUX_ALLEGRO)/%.allegro.o: %.cpp
	@echo "Compiling $< for Linux Allegro..."
	$(CXX_LINUX) $(CXXFLAGS_LINUX_ALLEGRO) -c $< -o $@

#
# Linux Allegro debug build targets
#
$(BUILD_DIR_LINUX_ALLEGRO_DEBUG)/$(TARGET_LINUX_ALLEGRO_DEBUG): $(addprefix $(BUILD_DIR_LINUX_ALLEGRO_DEBUG)/,$(OBJS_LINUX_ALLEGRO_DEBUG))
	@echo "Linking Linux Allegro debug executable..."
	$(CXX_LINUX) $^ -o $@ $(LDFLAGS_LINUX_ALLEGRO)
	@echo "Linux Allegro debug build complete: $@"

$(BUILD_DIR_LINUX_ALLEGRO_DEBUG)/%.allegro.debug.o: %.cpp
	@echo "Compiling $< for Linux Allegro debug..."
	$(CXX_LINUX) $(CXXFLAGS_LINUX_ALLEGRO_DEBUG) -c $< -o $@

#
# Windows Allegro build targets
#
$(BUILD_DIR_WIN_ALLEGRO)/$(TARGET_WIN_ALLEGRO): $(addprefix $(BUILD_DIR_WIN_ALLEGRO)/,$(OBJS_WIN_ALLEGRO))
	@echo "Linking Windows Allegro executable..."
	$(CXX_WIN) $^ -o $@ $(LDFLAGS_WIN_ALLEGRO)
	@echo "Windows Allegro build complete: $@"

$(BUILD_DIR_WIN_ALLEGRO)/%.win.allegro.o: %.cpp
	@echo "Compiling $< for Windows Allegro..."
	$(CXX_WIN) $(CXXFLAGS_WIN_ALLEGRO) -c $< -o $@

#
# Windows Allegro debug build targets
#
$(BUILD_DIR_WIN_ALLEGRO_DEBUG)/$(TARGET_WIN_ALLEGRO_DEBUG): $(addprefix $(BUILD_DIR_WIN_ALLEGRO_DEBUG)/,$(OBJS_WIN_ALLEGRO_DEBUG))
	@echo "Linking Windows Allegro debug executable..."
	$(CXX_WIN) $(CXXFLAGS_WIN_ALLEGRO_DEBUG) $^ -o $@ $(LDFLAGS_WIN_ALLEGRO)
	@echo "Windows Allegro debug build complete: $@"

$(BUILD_DIR_WIN_ALLEGRO_DEBUG)/%.win.allegro.debug.o: %.cpp
	@echo "Compiling $< for Windows Allegro debug..."
	$(CXX_WIN) $(CXXFLAGS_WIN_ALLEGRO_DEBUG) -c $< -o $@

#
# Linux GTK3 build targets
#
$(BUILD_DIR_LINUX_GTK3)/$(TARGET_LINUX_GTK3): $(addprefix $(BUILD_DIR_LINUX_GTK3)/,$(OBJS_LINUX_GTK3))
	@echo "Linking Linux GTK3 executable..."
	$(CXX_LINUX) $^ -o $@ $(LDFLAGS_LINUX_GTK3)
	@echo "Linux GTK3 build complete: $@"

$(BUILD_DIR_LINUX_GTK3)/%.gtk3.o: %.cpp
	@echo "Compiling $< for Linux GTK3..."
	$(CXX_LINUX) $(CXXFLAGS_LINUX_GTK3) -c $< -o $@

#
# Linux GTK3 debug build targets
#
$(BUILD_DIR_LINUX_GTK3_DEBUG)/$(TARGET_LINUX_GTK3_DEBUG): $(addprefix $(BUILD_DIR_LINUX_GTK3_DEBUG)/,$(OBJS_LINUX_GTK3_DEBUG))
	@echo "Linking Linux GTK3 debug executable..."
	$(CXX_LINUX) $^ -o $@ $(LDFLAGS_LINUX_GTK3)
	@echo "Linux GTK3 debug build complete: $@"

$(BUILD_DIR_LINUX_GTK3_DEBUG)/%.gtk3.debug.o: %.cpp
	@echo "Compiling $< for Linux GTK3 debug..."
	$(CXX_LINUX) $(CXXFLAGS_LINUX_GTK3_DEBUG) -c $< -o $@

# Check dependencies target
.PHONY: check-deps
check-deps:
	@echo "Checking dependencies..."
	@echo "Linux SDL2:"; pkg-config --exists sdl2 && echo "  ✓ SDL2 found" || echo "  ✗ SDL2 not found - install libsdl2-dev"
	@echo "Linux Allegro:"; test -f /usr/lib/liballeg.so && echo "  ✓ Allegro 4 found" || echo "  ✗ Allegro 4 not found - install liballegro4-dev"
	@echo "Linux GTK3:"; pkg-config --exists gtk+-3.0 gl glu && echo "  ✓ GTK3 with OpenGL found" || echo "  ✗ GTK3/OpenGL not found - install libgtk-3-dev libgl1-mesa-dev libglu1-mesa-dev"
	@echo "Windows SDL2:"; test -f /usr/x86_64-w64-mingw32/include/SDL2/SDL.h && echo "  ✓ MinGW SDL2 found" || echo "  ✗ MinGW SDL2 not found - install mingw64-SDL2-devel"
	@echo "Windows Allegro:"; test -f /usr/x86_64-w64-mingw32/lib/liballeg.a && echo "  ✓ MinGW Allegro 4 found" || echo "  ✗ MinGW Allegro 4 not found - install mingw64-allegro4"
	@echo "DLL collect scripts:"; \
	test -f build/windows-sdl/collect_dlls.sh && echo "  ✓ SDL collect_dlls.sh found" || echo "  ✗ SDL collect_dlls.sh not found - create in build/windows-sdl/"; \
	test -f build/windows-allegro/collect_dlls.sh && echo "  ✓ Allegro collect_dlls.sh found" || echo "  ✗ Allegro collect_dlls.sh not found - create in build/windows-allegro/"

# Install dependencies (Ubuntu/Debian)
.PHONY: install-deps
install-deps:
	@echo "Installing dependencies for Ubuntu/Debian..."
	sudo apt-get update
	sudo apt-get install -y libsdl2-dev liballegro4-dev libgtk-3-dev libgl1-mesa-dev libglu1-mesa-dev pkg-config
	sudo apt-get install -y mingw-w64 || echo "MinGW may need manual installation"
	@echo "Note: MinGW SDL2 and Allegro libraries may need manual installation"

# Clean target
.PHONY: clean
clean:
	@echo "Cleaning build files..."
	find $(BUILD_DIR) -type f -name "*.o" -delete 2>/dev/null || true
	find $(BUILD_DIR) -type f -name "*.exe" -delete 2>/dev/null || true
	find $(BUILD_DIR) -type f -name "*.dll" -delete 2>/dev/null || true
	rm -f $(BUILD_DIR_LINUX_SDL)/$(TARGET_LINUX_SDL) 2>/dev/null || true
	rm -f $(BUILD_DIR_LINUX_SDL_DEBUG)/$(TARGET_LINUX_SDL_DEBUG) 2>/dev/null || true
	rm -f $(BUILD_DIR_WIN_SDL)/$(TARGET_WIN_SDL) 2>/dev/null || true
	rm -f $(BUILD_DIR_WIN_SDL_DEBUG)/$(TARGET_WIN_SDL_DEBUG) 2>/dev/null || true
	rm -f $(BUILD_DIR_LINUX_ALLEGRO)/$(TARGET_LINUX_ALLEGRO) 2>/dev/null || true
	rm -f $(BUILD_DIR_LINUX_ALLEGRO_DEBUG)/$(TARGET_LINUX_ALLEGRO_DEBUG) 2>/dev/null || true
	rm -f $(BUILD_DIR_WIN_ALLEGRO)/$(TARGET_WIN_ALLEGRO) 2>/dev/null || true
	rm -f $(BUILD_DIR_WIN_ALLEGRO_DEBUG)/$(TARGET_WIN_ALLEGRO_DEBUG) 2>/dev/null || true
	rm -f $(BUILD_DIR_LINUX_GTK3)/$(TARGET_LINUX_GTK3) 2>/dev/null || true
	rm -f $(BUILD_DIR_LINUX_GTK3_DEBUG)/$(TARGET_LINUX_GTK3_DEBUG) 2>/dev/null || true

# Test builds (quick compilation test)
.PHONY: test-builds
test-builds:
	@echo "Testing compilation for all platforms..."
	@echo "Testing Linux SDL build..."
	@$(MAKE) linux-sdl >/dev/null 2>&1 && echo "  ✓ Linux SDL builds successfully" || echo "  ✗ Linux SDL build failed"
	@echo "Testing Linux Allegro build..."
	@$(MAKE) linux-allegro >/dev/null 2>&1 && echo "  ✓ Linux Allegro builds successfully" || echo "  ✗ Linux Allegro build failed"
	@echo "Testing Linux GTK3 build..."
	@$(MAKE) linux-gtk3 >/dev/null 2>&1 && echo "  ✓ Linux GTK3 builds successfully" || echo "  ✗ Linux GTK3 build failed"
	@echo "Testing Windows SDL build..."
	@$(MAKE) windows-sdl >/dev/null 2>&1 && echo "  ✓ Windows SDL builds successfully" || echo "  ✗ Windows SDL build failed"
	@echo "Testing Windows Allegro build..."
	@$(MAKE) windows-allegro >/dev/null 2>&1 && echo "  ✓ Windows Allegro builds successfully" || echo "  ✗ Windows Allegro build failed"

# Debug what files the makefile thinks it should build
.PHONY: debug-files
debug-files:
	@echo "SDL Source Files:"
	@for file in $(SDL_SOURCE_FILES); do echo "  $$file"; done
	@echo ""
	@echo "SDL Object Files (Linux):"
	@for file in $(OBJS_LINUX_SDL); do echo "  $$file"; done
	@echo ""
	@echo "Allegro Source Files:"
	@for file in $(ALLEGRO_SOURCE_FILES); do echo "  $$file"; done
	@echo ""
	@echo "Allegro Object Files (Linux):"
	@for file in $(OBJS_LINUX_ALLEGRO); do echo "  $$file"; done
	@echo ""
	@echo "GTK3 Source Files:"
	@for file in $(GTK3_SOURCE_FILES); do echo "  $$file"; done
	@echo ""
	@echo "GTK3 Object Files (Linux):"
	@for file in $(OBJS_LINUX_GTK3); do echo "  $$file"; done

# Help target
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  make               - Build warpnes for Linux with Allegro (default)"
	@echo "  make linux         - Build SDL, Allegro, and GTK3 versions for Linux"
	@echo "  make windows       - Build SDL and Allegro versions for Windows"
	@echo ""
	@echo "  make linux-sdl     - Build warpnes for Linux with SDL"
	@echo "  make linux-allegro - Build warpnes for Linux with Allegro"
	@echo "  make linux-gtk3    - Build warpnes for Linux with GTK3 + OpenGL"
	@echo "  make windows-sdl   - Build warpnes for Windows with SDL (requires MinGW + DLLs)"
	@echo "  make windows-allegro - Build warpnes for Windows with Allegro (requires MinGW + DLLs)"
	@echo ""
	@echo "  make debug         - Build debug versions for all platforms"
	@echo "  make linux-sdl-debug     - Build Linux SDL with debug symbols"
	@echo "  make linux-allegro-debug - Build Linux Allegro with debug symbols"
	@echo "  make linux-gtk3-debug    - Build Linux GTK3 with debug symbols"
	@echo "  make windows-sdl-debug
