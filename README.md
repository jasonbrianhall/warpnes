# WarpNES Emulator

A 6502 CPU emulator that can run some classic NES games, built with Allegro 4 or SDL for both DOS and Linux platforms (DOS version is very slow, almost unusable as of now because I removed sprite cacheing [plan to readd later])

This was more of "Let's understand how a NES works" then a full emulator.

## ⚠️ Important Disclaimer

**This emulator is NOT cycle-accurate and has significant compatibility issues, but it does work for some commercial game.**

This is a hobby project that implements basic NES functionality. Don't expect commercial-level compatibility or accuracy.  I try to fix bugs as I find and understand them so the compatibility should increase over time unless I get bored of the project.

## What Works

- **Super Mario Bros** - 100% playable but XScroll Glitch with the scoring
- **Mario Bros** - 100% playable  
- **Popeye** - 100% playable
- **Duck Tales** - 100% playab
- **Many NROM games** - Excellent compatibility; most work without flaws
- **Basic MMC1, MM2, MMC3, UxROM support** - Limited but functional
- **Audio output** - Excellent APU emulation (~95% accurate)
- **Controllers** - Keyboard and joystick input
- **Save states** - F5-F8 for save/load (Some reload issues on certain games); needs implemented per game 
- **Smooth scrolling** - VSync implementation provides fluid gameplay


## What's Broken

- **Timing is completely wrong** - Not cycle-accurate at all
- **UxROM sprite banking** - Sprites often render incorrectly
- **MMC3 IRQ timing** - Many games won't work properly  
- **PPU timing** - VBlank, sprite 0 hit, and scrolling issues
- **APU accuracy** - Very good audio quality (~95% accurate)
- **Many mappers** - Limited mapper support
- **MS Pacman** - For whatever reason;  think it's looking for VBlank Timing but not sure (gives title screen)
- **Final Fantasy** - Game works but it has some rendering issues (I had it working better at one point but broke it)
- **Super Mario Brothers 2/3** - Stuck in an infinite loop after hitting start and sprites are wrong (pretty much all mapper 4 game are broken)
- **Punch out** - Mapper 9 game; title screen has some tile/sprite priority issues and ring is messed up.

## Controls

### Default Player 1 (Joystick support only for SDL version; haven't figured out the joystick and keyboard fighting for the same freaking keys)
- **Arrow Keys** - D-pad
- **Z** - A button  
- **X** - B button
- **[** - Select
- **]** - Start



### Default Player 2  
- **WASD** - D-pad
- **F** - A button
- **G** - B button  
- **O** - Select
- **P** - Start

### Hotkeys
- **ESC** - Menu
- **F5-F8** - Save states (Shift+F5-F8 to load)
- **Ctrl+R** - Reset game
- **Ctrl+P** - Pause
- **F11** - Fullscreen toggle (Linux only)

## Building

### DOS (DJGPP)
**Requires Docker**
```bash
./build_dos.sh
```

### Linux
```bash
make
```

Their is also a make help for building other versions.

## Requirements

- **Allegro 4.x** development libraries (for DOS and Linux versions)
- **SDL** Linux/Windows version (I actually built a SDL to Allegro 4 wrapper so it can compile in DOS (very minimal) but it's very slow and didn't implement sound [SDL to Allegro 4 Wrapper](https://github.com/jasonbrianhall/super_mario_brothers/tree/main/SDL_Allegro_Wrapper))
- **Docker** (required for DOS builds)
- **GCC** for Linux builds

## Usage

```bash
./warpnes-sdl <rom_file.nes>
```

Example:
```bash
./warpnes-sdl smb.nes
```

## Supported Mappers

- **Mapper 0 (NROM)** - Basic support
- **Mapper 1 (MMC1)** - Buggy banking
- **Mapper 2 (UxROM)** - Sprite issues but games like Final Fantasy and Duckhunt boot fine and are playable.
- **Mapper 3 (CNROM)** - Basic CHR banking
- **Mapper 4 (MMC3)** - Very buggy IRQ timing
- **Mapper 9 (MMC2)** - It will boot Punch out but the graphics are wrong
- **Mapper 66 (GxROM)** - Seems fully supported (Super Mario Bros + Duck Hunt work even though the zapper doesn't work so you can't actually kill the ducks)

## Known Issues

1. **UxROM games** (Contra, DuckTales) have sprite rendering problems
2. **MMC3 games** often hang or crash due to IRQ timing
3. **CPU timing** is completely inaccurate
4. **PPU rendering** happens once per frame instead of scanline-by-scanline

## Configuration Files

- `controls.cfg` - Controller mappings
- `video.cfg` - Video settings  
- `audio.cfg` - Audio configuration
- `config.cfg` - General settings

## Why Is This So Buggy?

This emulator was built as a learning project without proper cycle-accurate timing. Real NES emulation requires:

- Cycle-by-cycle CPU/PPU synchronization
- Accurate mapper implementations  
- Proper PPU scanline rendering
- Precise IRQ timing
- Complex sprite evaluation logic

This emulator does none of that correctly. For accurate NES emulation, use:
- **Mesen** 
- **FCEUX**
- **Nestopia**
- **puNES**

## License

MIT License

Copyright (c) 2025

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

The original games remain property of their respective copyright holders.

## Contributing

Feel free to submit pull requests to fix issues, but be aware that the architecture has fundamental limitations for accuracy. A complete rewrite would be needed for proper cycle-accurate NES emulation.

---

*"Emulation is hard. Accurate emulation is harder."*
