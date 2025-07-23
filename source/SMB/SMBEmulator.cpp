#include "SMBEmulator.hpp"
#include "../Configuration.hpp"
#include "../Emulation/APU.hpp"

#ifdef ALLEGRO_BUILD
#include "../Emulation/Controller.hpp"
#else
#include "../Emulation/ControllerSDL.hpp"
#endif

#include "../Emulation/PPU.hpp"
#include "../Zapper.hpp"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

// 6502 instruction cycle counts
const uint8_t SMBEmulator::instructionCycles[256] = {
    // 0x00-0x0F
    7, 6, 0, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6,
    // 0x10-0x1F
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    // 0x20-0x2F
    6, 6, 0, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6,
    // 0x30-0x3F
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    // 0x40-0x4F
    6, 6, 0, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6,
    // 0x50-0x5F
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    // 0x60-0x6F
    6, 6, 0, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6,
    // 0x70-0x7F
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    // 0x80-0x8F
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
    // 0x90-0x9F
    2, 6, 0, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    // 0xA0-0xAF
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4,
    // 0xB0-0xBF
    2, 5, 0, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    // 0xC0-0xCF
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
    // 0xD0-0xDF
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    // 0xE0-0xEF
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6,
    // 0xF0-0xFF
    2, 5, 0, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7};

SMBEmulator::SMBEmulator()
    : regA(0), regX(0), regY(0), regSP(0xFF), regPC(0), regP(0x24),
      totalCycles(0), frameCycles(0), prgROM(nullptr), chrROM(nullptr),
      prgSize(0), chrSize(0), romLoaded(false),
      masterCycles(0), ppuCycles(0), nmiPending(false),
      sram(nullptr), sramSize(0), sramEnabled(false), sramDirty(false) {
  // Initialize RAM
  memset(ram, 0, sizeof(ram));
  memset(&nesHeader, 0, sizeof(nesHeader));

  // Create components - they'll get CHR data when ROM is loaded
  apu = new APU();
  ppu = new PPU(*this);


  controller1 = new Controller();
  controller2 = new Controller();


  zapper = new Zapper();
  zapperEnabled = 0;
}

SMBEmulator::~SMBEmulator() {
  delete apu;
  delete ppu;

  delete controller1;
  delete controller2;

  delete zapper;
  unloadROM();
  cleanupSRAM();
}

void SMBEmulator::writeCNROMRegister(uint16_t address, uint8_t value) {
  uint8_t oldCHRBank = cnrom.chrBank;

  // Mapper 3: Write to $8000-$FFFF sets CHR bank
  cnrom.chrBank = value & 0x03; // Only 2 bits for CHR bank

  // Invalidate cache if CHR bank changed
  /*if (oldCHRBank != cnrom.chrBank) {
      ppu->invalidateTileCache();
  }*/
}

void SMBEmulator::writeCHRData(uint16_t address, uint8_t value) {
  if (address >= 0x2000)
    return;

  // DON'T call catchUpPPU here - it causes infinite loops!
  // The cycle-accurate loop already keeps everything in sync

  // Handle CHR-RAM writes based on mapper type
  switch (nesHeader.mapper) {
  case 0: // NROM
    // NROM can have CHR-RAM if chrROMPages == 0
    if (nesHeader.chrROMPages == 0) {
      if (address < chrSize) {
        chrROM[address] = value; // chrROM is actually CHR-RAM

        static int nromChrWriteCount = 0;
        if (nromChrWriteCount < 5) {
          printf("NROM CHR-RAM write: $%04X = $%02X\n", address, value);
          nromChrWriteCount++;
        }
      }
    }
    // NROM with CHR-ROM: writes are ignored (read-only)
    break;

  case 1: // MMC1
    if (nesHeader.chrROMPages == 0) {
      // MMC1 with CHR-RAM (like Metroid)
      if (address < chrSize) {
        chrROM[address] = value; // Direct write to CHR-RAM
      }
    }
    // MMC1 with CHR-ROM: writes are ignored (banking controlled by registers)
    break;

  case 2: // UxROM
    // UxROM always uses CHR-RAM
    if (address < chrSize) {
      chrROM[address] = value; // chrROM is actually CHR-RAM for UxROM

      static int uxromChrWriteCount = 0;
      if (uxromChrWriteCount < 5) {
        printf("UxROM CHR-RAM write: $%04X = $%02X\n", address, value);
        uxromChrWriteCount++;
      }
    }
    break;

  case 3: // CNROM
    // CNROM typically uses CHR-ROM (read-only), but some variants have CHR-RAM
    if (nesHeader.chrROMPages == 0) {
      if (address < chrSize) {
        chrROM[address] = value;

        static int cnromChrWriteCount = 0;
        if (cnromChrWriteCount < 5) {
          printf("CNROM CHR-RAM write: $%04X = $%02X\n", address, value);
          cnromChrWriteCount++;
        }
      }
    }
    // CNROM with CHR-ROM: writes ignored
    break;

  case 4: // MMC3
    if (nesHeader.chrROMPages == 0) {
      // MMC3 with CHR-RAM
      if (address < chrSize) {
        chrROM[address] = value;

        static int mmc3ChrWriteCount = 0;
        if (mmc3ChrWriteCount < 5) {
          printf("MMC3 CHR-RAM write: $%04X = $%02X\n", address, value);
          mmc3ChrWriteCount++;
        }
      }
    }
    // MMC3 with CHR-ROM: writes ignored (banking controlled)
    break;

  case 66: // GxROM
    if (nesHeader.chrROMPages == 0) {
      // GxROM with CHR-RAM
      if (address < chrSize) {
        chrROM[address] = value;

        static int gxromChrWriteCount = 0;
        if (gxromChrWriteCount < 5) {
          printf("GxROM CHR-RAM write: $%04X = $%02X\n", address, value);
          gxromChrWriteCount++;
        }
      }
    }
    // GxROM with CHR-ROM: writes ignored
    break;

  case 7: // AxROM
    // AxROM typically uses CHR-RAM
    if (address < chrSize) {
      chrROM[address] = value;

      static int axromChrWriteCount = 0;
      if (axromChrWriteCount < 5) {
        printf("AxROM CHR-RAM write: $%04X = $%02X\n", address, value);
        axromChrWriteCount++;
      }
    }
    break;

  case 9:  // MMC2
  case 10: // MMC4
    if (nesHeader.chrROMPages == 0) {
      if (address < chrSize) {
        chrROM[address] = value;

        static int mmc2_4ChrWriteCount = 0;
        if (mmc2_4ChrWriteCount < 5) {
          printf("MMC2/4 CHR-RAM write: $%04X = $%02X\n", address, value);
          mmc2_4ChrWriteCount++;
        }
      }
    }
    break;

  case 11: // Color Dreams
    if (nesHeader.chrROMPages == 0) {
      if (address < chrSize) {
        chrROM[address] = value;

        static int colorDreamsChrWriteCount = 0;
        if (colorDreamsChrWriteCount < 5) {
          printf("Color Dreams CHR-RAM write: $%04X = $%02X\n", address, value);
          colorDreamsChrWriteCount++;
        }
      }
    }
    break;

  case 13: // CPROM
    // CPROM uses CHR-RAM
    if (address < chrSize) {
      chrROM[address] = value;

      static int cpromChrWriteCount = 0;
      if (cpromChrWriteCount < 5) {
        printf("CPROM CHR-RAM write: $%04X = $%02X\n", address, value);
        cpromChrWriteCount++;
      }
    }
    break;

  case 28: // Action 53
  case 30: // UNROM 512
    // These modern homebrew mappers often use CHR-RAM
    if (address < chrSize) {
      chrROM[address] = value;

      static int homebrewChrWriteCount = 0;
      if (homebrewChrWriteCount < 5) {
        printf("Homebrew mapper %d CHR-RAM write: $%04X = $%02X\n",
               nesHeader.mapper, address, value);
        homebrewChrWriteCount++;
      }
    }
    break;

  default:
    // For unknown mappers, be safe and allow CHR-RAM writes if no CHR-ROM
    if (nesHeader.chrROMPages == 0) {
      if (address < chrSize) {
        chrROM[address] = value;

        static int unknownChrWriteCount = 0;
        if (unknownChrWriteCount < 3) {
          printf("Unknown mapper %d CHR-RAM write: $%04X = $%02X\n",
                 nesHeader.mapper, address, value);
          unknownChrWriteCount++;
        }
      }
    } else {
      // Unknown mapper with CHR-ROM - log the attempt but don't write
      static int unknownChrROMWriteCount = 0;
      if (unknownChrROMWriteCount < 3) {
        printf("Warning: Mapper %d attempted CHR-ROM write: $%04X = $%02X "
               "(ignored)\n",
               nesHeader.mapper, address, value);
        unknownChrROMWriteCount++;
      }
    }
    break;
  }
}

// Modify loadROM to initialize SRAM:
bool SMBEmulator::loadROM(const std::string &filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file.is_open()) {
    std::cerr << "Failed to open ROM file: " << filename << std::endl;
    return false;
  }

  // Extract base filename for save files
  size_t lastSlash = filename.find_last_of("/\\");
  size_t lastDot = filename.find_last_of('.');
  if (lastSlash == std::string::npos) lastSlash = 0; else lastSlash++;
  if (lastDot == std::string::npos || lastDot < lastSlash) lastDot = filename.length();
  
  romBaseName = filename.substr(lastSlash, lastDot - lastSlash);

  // Parse NES header
  if (!parseNESHeader(file)) {
    std::cerr << "Invalid NES ROM header" << std::endl;
    return false;
  }

  // Skip trainer if present
  if (nesHeader.trainer) {
    file.seekg(512, std::ios::cur);
  }

  // Load PRG ROM
  if (!loadPRGROM(file)) {
    std::cerr << "Failed to load PRG ROM" << std::endl;
    return false;
  }

  // Load CHR ROM
  if (!loadCHRROM(file)) {
    std::cerr << "Failed to load CHR ROM" << std::endl;
    return false;
  }

  file.close();
  romLoaded = true;

  std::cout << "ROM loaded successfully: " << filename << std::endl;
  std::cout << "PRG ROM: " << (prgSize / 1024)
            << "KB, CHR ROM: " << (chrSize / 1024) << "KB" << std::endl;
  std::cout << "Mapper: " << (int)nesHeader.mapper << ", Mirroring: "
            << (nesHeader.mirroring ? "Vertical" : "Horizontal") << std::endl;

  // Initialize SRAM for battery games
  initializeSRAM();

  // Reset emulator state
  reset();

  return true;
}

void SMBEmulator::unloadROM() {
  if (prgROM) {
    delete[] prgROM;
    prgROM = nullptr;
  }
  if (chrROM) {
    delete[] chrROM;
    chrROM = nullptr;
  }
  prgSize = chrSize = 0;
  romLoaded = false;
}

bool SMBEmulator::parseNESHeader(std::ifstream &file) {
  uint8_t header[16];
  file.read(reinterpret_cast<char *>(header), 16);

  if (!file.good())
    return false;

  // Check "NES\x1A" signature
  if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' ||
      header[3] != 0x1A) {
    std::cout << "Invalid NES signature" << std::endl;
    return false;
  }

  // Detect iNES format version
  bool isINES2 = false;
  if ((header[7] & 0x0C) == 0x08) {
    isINES2 = true;
    std::cout << "ROM Format: iNES 2.0" << std::endl;
  } else if ((header[7] & 0x0C) == 0x00) {
    // Check if bytes 12-15 are zero (archaic iNES vs iNES 1.0)
    bool hasTrailingZeros = (header[12] == 0 && header[13] == 0 &&
                             header[14] == 0 && header[15] == 0);
    if (hasTrailingZeros) {
      std::cout << "ROM Format: iNES 1.0" << std::endl;
    } else {
      std::cout << "ROM Format: Archaic iNES" << std::endl;
    }
  } else {
    std::cout << "ROM Format: Unknown/Invalid" << std::endl;
    return false;
  }

  // Parse basic header info
  nesHeader.prgROMPages = header[4];
  nesHeader.chrROMPages = header[5];

  // Parse mapper number
  if (isINES2) {
    // iNES 2.0 mapper parsing (12-bit mapper number)
    nesHeader.mapper =
        (header[6] >> 4) | (header[7] & 0xF0) | ((header[8] & 0x0F) << 8);
  } else {
    // iNES 1.0 mapper parsing (8-bit mapper number)
    nesHeader.mapper = (header[6] >> 4) | (header[7] & 0xF0);
  }

  nesHeader.mirroring = header[6] & 0x01;
  nesHeader.battery = (header[6] & 0x02) != 0;
  nesHeader.trainer = (header[6] & 0x04) != 0;

  // Print detailed header info
  std::cout << "=== ROM Header Info ===" << std::endl;
  std::cout << "PRG ROM Pages: " << (int)nesHeader.prgROMPages << " (16KB each)"
            << std::endl;
  std::cout << "CHR ROM Pages: " << (int)nesHeader.chrROMPages << " (8KB each)"
            << std::endl;
  std::cout << "Mapper: " << (int)nesHeader.mapper << std::endl;
  std::cout << "Mirroring: "
            << (nesHeader.mirroring ? "Vertical" : "Horizontal") << std::endl;
  std::cout << "Battery: " << (nesHeader.battery ? "Yes" : "No") << std::endl;
  std::cout << "Trainer: " << (nesHeader.trainer ? "Yes" : "No") << std::endl;

  // Check for unsupported features
  if (nesHeader.mapper != 0 && nesHeader.mapper != 1 && nesHeader.mapper != 2 &&
      nesHeader.mapper != 3 && nesHeader.mapper != 4 &&
      nesHeader.mapper != 66) {
    std::cout << "WARNING: Mapper " << (int)nesHeader.mapper << " not supported"
              << std::endl;
  }

  // Mapper-specific notes
  switch (nesHeader.mapper) {
  case 0:
    std::cout << "Mapper: NROM (simple)" << std::endl;
    break;
  case 1:
    std::cout << "Mapper: MMC1 (complex)" << std::endl;
    break;
  case 2:
    std::cout << "Mapper: UxROM (Contra, Mega Man, Duck Tales)" << std::endl;
    break;
  case 3:
    std::cout << "Mapper: CNROM (simple CHR banking)" << std::endl;
    break;
  case 4:
    std::cout << "Mapper: MMC3 (complex)" << std::endl;
    break;
  case 66:
    std::cout << "Mapper: GxROM" << std::endl;
    break;
  default:
    std::cout << "Mapper: Unknown/Unsupported (" << (int)nesHeader.mapper << ")"
              << std::endl;
    break;
  }

  if (isINES2) {
    std::cout << "WARNING: iNES 2.0 features not fully implemented"
              << std::endl;
  }

  return true;
}

bool SMBEmulator::loadPRGROM(std::ifstream &file) {
  prgSize = nesHeader.prgROMPages * 16384; // 16KB pages
  if (prgSize == 0)
    return false;

  prgROM = new uint8_t[prgSize];
  file.read(reinterpret_cast<char *>(prgROM), prgSize);

  return file.good();
}

bool SMBEmulator::loadCHRROM(std::ifstream &file) {
  chrSize = nesHeader.chrROMPages * 8192; // 8KB pages
  if (chrSize == 0) {
    chrSize = 8192;
    chrROM = new uint8_t[chrSize];
    memset(chrROM, 0, chrSize);
    printf("Using CHR RAM (8KB) for mapper %d\n", nesHeader.mapper);
    return true;
  }

  chrROM = new uint8_t[chrSize];
  file.read(reinterpret_cast<char *>(chrROM), chrSize);

  // Debug output
  printf("Loaded CHR ROM: %d bytes for mapper %d\n", chrSize, nesHeader.mapper);

  // CRITICAL DEBUG INFO
  uint32_t totalCHRBanks = chrSize / 0x400; // Number of 1KB banks
  printf("=== CHR ROM DEBUG ===\n");
  printf("CHR ROM Size: %d bytes (%d KB)\n", chrSize, chrSize / 1024);
  printf("Total 1KB CHR banks: %d (0x00 - 0x%02X)\n", totalCHRBanks,
         totalCHRBanks - 1);
  printf("CHR ROM Pages: %d\n", nesHeader.chrROMPages);

  // Check if common bank numbers are valid
  printf("Bank validity check:\n");
  for (int bank : {0x00, 0x08, 0x09, 0x0C, 0x10, 0x11, 0x18, 0x19}) {
    bool valid = (bank < totalCHRBanks);
    printf("  Bank 0x%02X: %s\n", bank, valid ? "VALID" : "OUT OF BOUNDS!");
  }
  printf("=== END CHR DEBUG ===\n");

  return file.good();
}

void SMBEmulator::handleNMI() {
  static int nmiCount = 0;

  nmiCount++;

  // Push PC and status to stack
  pushWord(regPC);
  pushByte(regP & ~FLAG_BREAK);
  setFlag(FLAG_INTERRUPT, true);

  // Jump to NMI vector
  regPC = readWord(0xFFFA);

  totalCycles += 7;
  frameCycles += 7;
}

/*void SMBEmulator::update()
{
    if (!romLoaded) return;

#ifdef __DJGPP__
    // BIOS timer tick (each tick ≈ 55ms)
    unsigned long ticksStart = (*(unsigned long*)MK_FP(0x40, 0x6C));
#else
    auto start = std::chrono::high_resolution_clock::now();
#endif

    if (needsCycleAccuracy()) {
        updateCycleAccurate();
    } else {
        updateFrameBased();
    }

#ifdef __DJGPP__
    unsigned long ticksEnd = (*(unsigned long*)MK_FP(0x40, 0x6C));
    unsigned long ticksElapsed = ticksEnd - ticksStart;

    std::ofstream logFile("timing.log", std::ios::app);
    if (logFile.is_open()) {
        logFile << "[DJGPP] SMBEmulator::update took approx " << (ticksElapsed *
55)
                << " ms (" << (needsCycleAccuracy() ? "Cycle Accurate" : "Frame
Based") << ")\n"; logFile.close();
    }
#else
    auto end = std::chrono::high_resolution_clock::now();
    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end
- start).count();

    std::cout << "[SMBEmulator::update] took " << duration_us / 1000.0 << " ms
("
              << (needsCycleAccuracy() ? "Cycle Accurate" : "Frame Based") <<
")\n"; #endif
}*/

void SMBEmulator::update() {
  if (!romLoaded)
    return;
    updateCycleAccurate();

}

bool SMBEmulator::needsCycleAccuracy() const {
  return false;
  if (zapperEnabled) {
    return true;
  };
  switch (nesHeader.mapper) {
  case 0: // NROM - no banking, use fast path
    return false;
  case 1: // MMC1 - banking but usually not mid-frame critical
    return false;
  case 2: // UxROM - CHR-RAM can be updated mid-frame!
    return true;
  case 3: // CNROM - banking but not usually mid-frame critical
    return false;
  case 4: // MMC3 - has IRQ timing that requires cycle accuracy
    return true;
  case 66: // GxROM - CHR banking can happen mid-frame!
    return true;
  default:
    return true; // Be safe for unknown mappers
  }
}

void SMBEmulator::updateFrameBased() {
  if (!romLoaded)
    return;
  static int frameCount = 0;
  frameCycles = 0;

  // More precise NTSC timing
  const int CYCLES_PER_SCANLINE = 113;
  const int VISIBLE_SCANLINES = 240;
  const int VBLANK_START_SCANLINE = 241;
  const int TOTAL_SCANLINES = 262;

  // Execute visible scanlines (0-240)
  for (int scanline = 0; scanline <= VISIBLE_SCANLINES; scanline++) {
    for (int cycle = 0; cycle < CYCLES_PER_SCANLINE; cycle++) {
      executeInstruction();
    }

    // Simulate sprite 0 hit around scanline 32 (status bar area)
    if (scanline == 32 && (ppu->getMask() & 0x18)) { // If rendering enabled
      ppu->setSprite0Hit(true);
    }
  }

  // CAPTURE SCROLL VALUE RIGHT BEFORE VBLANK
  ppu->captureFrameScroll();

  // Start VBlank on scanline 241
  ppu->setVBlankFlag(true);

  // Execute a few cycles to let the game see VBlank before NMI
  for (int i = 0; i < 3; i++) {
    executeInstruction();
  }

  // Trigger NMI if enabled
  if (ppu->getControl() & 0x80) {
    handleNMI();
  }

  // Execute remaining VBlank scanlines (241-261)
  for (int scanline = VBLANK_START_SCANLINE; scanline < TOTAL_SCANLINES;
       scanline++) {
    for (int cycle = 0; cycle < CYCLES_PER_SCANLINE; cycle++) {
      executeInstruction();
    }
  }

  // Clear VBlank flag and sprite 0 hit at start of next frame
  ppu->setVBlankFlag(false);
  ppu->setSprite0Hit(false);

  frameCount++;

  // Advance audio frame
  if (Configuration::getAudioEnabled()) {
    apu->stepFrame();
  }
}

void SMBEmulator::updateCycleAccurate() {
    if (!romLoaded) return;
    
    static int debugFrame = 0;    
    frameCycles = 0;
    ppuCycleState.scanline = 0;
    ppuCycleState.cycle = 0;
    ppuCycleState.inVBlank = false;
    ppuCycleState.renderingEnabled = false;
    
    // NTSC PPU timing constants
    const int CYCLES_PER_SCANLINE = 341;
    const int VISIBLE_SCANLINES = 240;
    const int VBLANK_START_SCANLINE = 241;
    const int TOTAL_SCANLINES = 262;
    const int CPU_DIVIDER = 3;  // CPU runs every 3 PPU cycles
    
    // Frame timing state
    bool frameEven = (totalCycles / (TOTAL_SCANLINES * CYCLES_PER_SCANLINE / CPU_DIVIDER)) % 2 == 0;
    int cpuCycleDebt = 0;  // Track how many CPU cycles we owe
    int totalCPUInstructions = 0;
    
    for (int scanline = 0; scanline < TOTAL_SCANLINES; scanline++) {
        ppuCycleState.scanline = scanline;
        // Determine scanline state
        if (scanline < VISIBLE_SCANLINES) {
            ppuCycleState.inVBlank = false;
            ppuCycleState.renderingEnabled = (ppu->getMask() & 0x18) != 0;
        } else if (scanline == VBLANK_START_SCANLINE) {
            ppuCycleState.inVBlank = true;
        } else if (scanline == 261) {
            ppuCycleState.inVBlank = false;
            ppuCycleState.renderingEnabled = (ppu->getMask() & 0x18) != 0;
        }
        
        // Calculate cycles for this scanline
        int cyclesThisScanline = CYCLES_PER_SCANLINE;
        if (scanline == 261 && frameEven && ppuCycleState.renderingEnabled) {
            cyclesThisScanline = 340;  // Skip cycle 340 on odd frames when rendering
        }
        
        for (int cycle = 0; cycle < cyclesThisScanline; cycle++) {
            ppuCycleState.cycle = cycle;
            
            // VBlank flag set on cycle 1 of scanline 241
            if (scanline == VBLANK_START_SCANLINE && cycle == 1) {
                ppu->setVBlankFlag(true);
                ppu->captureFrameScroll();
                
                // FIXED: Fire NMI immediately if enabled, don't use pending system
                if (ppu->getControl() & 0x80) {
                    handleNMI();
                    // Account for NMI cycles (7 CPU cycles = 21 PPU cycles)
                    cpuCycleDebt += 7;
                }
            }
            
            // VBlank flag cleared on cycle 1 of pre-render scanline (261)
            if (scanline == 261 && cycle == 1) {
                ppu->setVBlankFlag(false);
                // DON'T clear sprite0hit here - it persists until end of frame
            }
            
            // FIXED: Check Zapper during visible scanlines when rendering
            if (zapperEnabled && zapper && scanline >= 0 && scanline < VISIBLE_SCANLINES && ppuCycleState.renderingEnabled) {
                // Check if we're at the current mouse position
                int nesMouseX = zapper->getMouseX();
                int nesMouseY = zapper->getMouseY();
                
                // Only check during visible pixel output (cycles 1-256)
                if (cycle >= 1 && cycle <= 256) {
                    int pixelX = cycle - 1;  // Convert cycle to pixel position
                    int pixelY = scanline;
                    
                    // Check if zapper is aimed at this pixel and trigger is pressed
                    if (zapper->isTriggerPressed() && 
                        pixelX >= nesMouseX - 2 && pixelX <= nesMouseX + 2 &&
                        pixelY >= nesMouseY - 2 && pixelY <= nesMouseY + 2) {
                        
                        // Get the current pixel color from PPU
                        // This would need to be implemented in your PPU class
                        uint16_t pixelColor = ppu->getCurrentPixelColor(pixelX, pixelY);
                        printf("pixelcolor %i brigth %i\n", pixelColor, isPixelBright(pixelColor));
                        // Duck Hunt looks for bright colors (white/light colors)
                        // Check if pixel is bright enough to trigger light detection
                        if (isPixelBright(pixelColor)) {
                            zapper->setLightDetected(true);
                        }
                    }
                }
            }
            
            if (scanline >= 0 && scanline < VISIBLE_SCANLINES && ppuCycleState.renderingEnabled) {
                checkSprite0Hit(scanline, cycle);
            }
            
            // Step PPU cycle for other events
            ppu->stepCycle(scanline, cycle);
            
            // MMC3 IRQ handling
            if (nesHeader.mapper == 4) {
                checkMMC3IRQ(scanline, cycle);
            }
            
            // FIXED: CPU execution tracking with proper cycle accounting
            cpuCycleDebt++;  // Each PPU cycle = 1/3 CPU cycle
            
            if (cpuCycleDebt >= CPU_DIVIDER) {
                cpuCycleDebt -= CPU_DIVIDER;  // We owe 1 CPU cycle
                totalCPUInstructions++;
                
                // Execute CPU instruction and get actual cycles used
                uint64_t cyclesBefore = totalCycles;
                executeInstruction();
                uint64_t cyclesUsed = totalCycles - cyclesBefore;
                
                // FIXED: Account for multi-cycle instructions
                // If instruction took more than 1 cycle, we need to "pay back" the extra cycles
                if (cyclesUsed > 1) {
                    cpuCycleDebt += (cyclesUsed - 1) * CPU_DIVIDER;
                }
                
                // Update master cycles to match actual execution
                masterCycles = totalCycles;
            }
        }
    }
    
    // End of frame cleanup
    ppu->setVBlankFlag(false);
    ppu->setSprite0Hit(false);
    
    // Clear zapper light detection at end of frame if trigger not pressed
    if (zapperEnabled && zapper && !zapper->isTriggerPressed()) {
        zapper->setLightDetected(false);
    }
    
    // Audio frame advance
    if (Configuration::getAudioEnabled()) {
        apu->stepFrame();
    }
}

bool SMBEmulator::isPixelBright(uint16_t pixelColor) {
    // Convert 16-bit color to RGB components
    // Assuming 5-6-5 RGB format (adjust based on your actual format)
    uint8_t r = (pixelColor >> 11) & 0x1F;
    uint8_t g = (pixelColor >> 5) & 0x3F;
    uint8_t b = pixelColor & 0x1F;
    
    // Scale to 8-bit values
    r = (r * 255) / 31;
    g = (g * 255) / 63;
    b = (b * 255) / 31;
    
    // Calculate brightness (simple luminance formula)
    int brightness = (r * 299 + g * 587 + b * 114) / 1000;
    
    // Duck Hunt typically detects white/light colors (brightness > 200)
    return brightness > 200;
}

void SMBEmulator::checkSprite0Hit(int scanline, int cycle) {
   if (ppu->getStatus() & 0x40) return;  // Already hit
   if (!ppuCycleState.renderingEnabled) return;  // Rendering disabled
   if (scanline >= 240) return;  // Not visible scanline
   
   uint8_t sprite0_y = ppu->getOAM()[0];
   uint8_t sprite0_x = ppu->getOAM()[3];
   
   sprite0_y++;  // Sprite delay
   
   // Simple collision detection - if we're in sprite 0's area
   if (scanline >= sprite0_y && scanline < sprite0_y + 8) {
       if (cycle >= sprite0_x && cycle < sprite0_x + 8) {
           // WE GOT A HIT!
           ppu->setSprite0Hit(true);
           //printf("*** SPRITE 0 HIT DETECTED *** at scanline %d, cycle %d (sprite at x=%d, y=%d)\n",  scanline, cycle, sprite0_x, sprite0_y-1);
       }
   }
}

void SMBEmulator::checkMMC3IRQ(int scanline, int cycle) {
    if (nesHeader.mapper != 4) return;
    if (!ppuCycleState.renderingEnabled) return;
    if (scanline >= 240 && scanline < 261) return; // Skip VBlank
    
    // MMC3 IRQ is clocked by A12 rises during rendering
    // A12 goes high when accessing pattern tables ($1000-$1FFF)
    
    bool a12High = false;
    
    // Background pattern fetches
    if ((cycle >= 1 && cycle <= 256) || (cycle >= 321 && cycle <= 336)) {
        int fetchCycle = cycle % 8;
        if (fetchCycle == 5 || fetchCycle == 7) { // Pattern table fetch cycles
            // A12 depends on background pattern table selection (PPUCTRL bit 4)
            uint8_t ppuCtrl = ppu->getControl();
            a12High = (ppuCtrl & 0x10) != 0;
        }
    }
    
    // Sprite pattern fetches (during sprite evaluation/loading)
    if (cycle >= 257 && cycle <= 320) {
        // A12 depends on sprite pattern table selection (PPUCTRL bit 3)  
        uint8_t ppuCtrl = ppu->getControl();
        a12High = (ppuCtrl & 0x08) != 0;
    }
    
    // Call the A12 transition detector
    stepMMC3A12Transition(a12High);
}


void SMBEmulator::reset() {
    if (!romLoaded) return;

    // Reset CPU state
    regA = regX = regY = 0;
    regSP = 0xFF;
    regP = 0x24;
    totalCycles = frameCycles = 0;
    if (nesHeader.mapper == 1) {
        // Reset MMC1 state properly
        mmc1 = MMC1State();  // Reset to default constructor state
        
        // CRITICAL: Force proper reset state according to MMC1 spec
        mmc1.control = 0x0C;  // Mode 3: 16KB PRG mode, last bank fixed at $C000
        mmc1.shiftRegister = 0x10;
        mmc1.shiftCount = 0;
        mmc1.prgBank = 0;
        mmc1.chrBank0 = 0;
        mmc1.chrBank1 = 0;
        
        // CRITICAL: Call updateMMC1Banks to initialize CHR banking
        updateMMC1Banks();
    } else if (nesHeader.mapper == 66) {
        gxrom = GxROMState();
    } else if (nesHeader.mapper == 2) {
        uxrom = UxROMState();
    } else if (nesHeader.mapper == 3) {
        cnrom = CNROMState();
    } else if (nesHeader.mapper == 4) {
        mmc3 = MMC3State();
        mmc3.bankData[0] = 0;
        mmc3.bankData[1] = 2;
        mmc3.bankData[2] = 4;
        mmc3.bankData[3] = 5;
        mmc3.bankData[4] = 6;
        mmc3.bankData[5] = 7;
        mmc3.bankData[6] = 0;
        mmc3.bankData[7] = 1;
        mmc3.bankSelect = 0;
        mmc3.irqPending = false;
        mmc3.irqEnable = false;
        updateMMC3Banks();        
    } else if (nesHeader.mapper == 9) {
        mmc2 = MMC2State();
        updateMMC2Banks();
    }

    // NOW read reset vector from correct location
    uint8_t lowByte = readByte(0xFFFC);
    uint8_t highByte = readByte(0xFFFD);
    regPC = lowByte | (highByte << 8);

    // Clear RAM
    memset(ram, 0, sizeof(ram));
}

void SMBEmulator::checkCHRLatch(uint16_t address, uint8_t tileID) {
    if (nesHeader.mapper == 9) {
        checkMMC2CHRLatch(address, tileID);
    }
}

void SMBEmulator::step() {
  if (!romLoaded)
    return;
  executeInstruction();
}

void SMBEmulator::stepPPUCycle() {
  int scanline = ppuCycleState.scanline;
  int cycle = ppuCycleState.cycle;

  // Background fetching during visible scanlines and pre-render
  if ((scanline < 240 || scanline == 261) && ppuCycleState.renderingEnabled) {

    // Tile fetching happens on specific cycles
    int fetchCycle = cycle % 8;

    switch (fetchCycle) {
    case 1: // Fetch nametable byte
      if (cycle >= 1 && cycle <= 256) {
        // This is where CHR bank switches can affect fetching
        stepPPUFetchNametable();
      }
      break;

    case 3: // Fetch attribute byte
      if (cycle >= 1 && cycle <= 256) {
        stepPPUFetchAttribute();
      }
      break;

    case 5: // Fetch pattern table low byte
      if (cycle >= 1 && cycle <= 256) {
        stepPPUFetchPatternLow();
      }
      break;

    case 7: // Fetch pattern table high byte
      if (cycle >= 1 && cycle <= 256) {
        stepPPUFetchPatternHigh();
      }
      break;
    }

    // Sprite evaluation (cycles 65-256)
    if (cycle >= 65 && cycle <= 256) {
      stepPPUSpriteEvaluation();
    }
  }
}

void SMBEmulator::stepPPUFetchNametable() {
  // Nametable fetch - doesn't usually affect mappers
  // But MMC3 A12 line changes can happen here
  if (nesHeader.mapper == 4) {
    // Check for A12 transitions during nametable access
    stepMMC3A12Transition(false); // Nametable access
  }
}

void SMBEmulator::stepPPUFetchAttribute() {
  // Attribute fetch - similar to nametable
  if (nesHeader.mapper == 4) {
    stepMMC3A12Transition(false);
  }
}

void SMBEmulator::stepPPUFetchPatternLow() {
  // Pattern table fetch - this is where CHR banking matters!
  if (nesHeader.mapper == 4) {
    stepMMC3A12Transition(true); // Pattern table access (A12 high)
  }

  // For other mappers, CHR data is fetched here
  // This is where CHR-RAM updates would be visible
}

void SMBEmulator::stepPPUFetchPatternHigh() {
  // Second pattern table fetch
  if (nesHeader.mapper == 4) {
    stepMMC3A12Transition(true);
  }
}

void SMBEmulator::stepPPUSpriteEvaluation() {
  // Sprite evaluation logic
  // This affects sprite 0 hit detection
}

void SMBEmulator::stepPPUEndOfScanline(int scanline) {
  // End-of-scanline events
  // Scroll register updates, etc.
}

// Enhanced MMC3 IRQ handling with proper A12 detection
void SMBEmulator::stepMMC3A12Transition(bool a12High) {
    static bool lastA12 = false;
    static int filterCounter = 0;
    
    // Filter rapid A12 changes (MMC3 hardware characteristic)
    if (a12High != lastA12) {
        filterCounter++;
        if (filterCounter >= 2) { // Require stability for 2 cycles
            if (a12High && !lastA12) {
                // Rising edge detected - clock the IRQ counter
                stepMMC3IRQ();
            }
            lastA12 = a12High;
            filterCounter = 0;
        }
    } else {
        filterCounter = 0;
    }
}

void SMBEmulator::executeInstruction() {
    if (nesHeader.mapper == 4 && mmc3.irqPending && !getFlag(FLAG_INTERRUPT)) {
        // Handle MMC3 IRQ immediately
        mmc3.irqPending = false;
        
        // IRQ sequence (similar to NMI but uses different vector)
        pushWord(regPC);
        pushByte(regP & ~FLAG_BREAK);  // Clear break flag
        setFlag(FLAG_INTERRUPT, true); // Set interrupt disable
        
        // Jump to IRQ vector at $FFFE/$FFFF
        regPC = readWord(0xFFFE);
        
        totalCycles += 7;  // IRQ takes 7 cycles
        frameCycles += 7;
        
        printf("*** MMC3 IRQ TRIGGERED at PC=$%04X, jumping to $%04X ***\n", 
               regPC, readWord(0xFFFE));
        return; // Don't execute normal instruction this cycle
    }

  uint8_t opcode = fetchByte();
  uint8_t cycles = instructionCycles[opcode];

#ifdef PRINTOPCODE
  if (opcode == 0x02 || opcode == 0x03 || opcode == 0x04 || opcode == 0x07 ||
      opcode == 0x0B || opcode == 0x0C || opcode == 0x0F || opcode == 0x12 ||
      opcode == 0x13 || opcode == 0x14 || opcode == 0x17 || opcode == 0x1A ||
      opcode == 0x1B || opcode == 0x1C || opcode == 0x1F || opcode == 0x22 ||
      opcode == 0x23 || opcode == 0x27 || opcode == 0x2B || opcode == 0x2F ||
      opcode == 0x32 || opcode == 0x33 || opcode == 0x34 || opcode == 0x37 ||
      opcode == 0x3A || opcode == 0x3B || opcode == 0x3C || opcode == 0x3F ||
      opcode == 0x42 || opcode == 0x43 || opcode == 0x44 || opcode == 0x47 ||
      opcode == 0x4B || opcode == 0x4F || opcode == 0x52 || opcode == 0x53 ||
      opcode == 0x54 || opcode == 0x57 || opcode == 0x5A || opcode == 0x5B ||
      opcode == 0x5C || opcode == 0x5F || opcode == 0x62 || opcode == 0x63 ||
      opcode == 0x64 || opcode == 0x67 || opcode == 0x6B || opcode == 0x6F ||
      opcode == 0x72 || opcode == 0x73 || opcode == 0x74 || opcode == 0x77 ||
      opcode == 0x7A || opcode == 0x7B || opcode == 0x7C || opcode == 0x7F ||
      opcode == 0x80 || opcode == 0x82 || opcode == 0x83 || opcode == 0x87 ||
      opcode == 0x89 || opcode == 0x8B || opcode == 0x8F || opcode == 0x92 ||
      opcode == 0x93 || opcode == 0x97 || opcode == 0x9B || opcode == 0x9C ||
      opcode == 0x9E || opcode == 0x9F || opcode == 0xA3 || opcode == 0xA7 ||
      opcode == 0xAB || opcode == 0xAF || opcode == 0xB2 || opcode == 0xB3 ||
      opcode == 0xB7 || opcode == 0xBB || opcode == 0xBF || opcode == 0xC2 ||
      opcode == 0xC3 || opcode == 0xC7 || opcode == 0xCB || opcode == 0xCF ||
      opcode == 0xD2 || opcode == 0xD3 || opcode == 0xD4 || opcode == 0xD7 ||
      opcode == 0xDA || opcode == 0xDB || opcode == 0xDC || opcode == 0xDF ||
      opcode == 0xE2 || opcode == 0xE3 || opcode == 0xE7 || opcode == 0xEB ||
      opcode == 0xEF || opcode == 0xF2 || opcode == 0xF3 || opcode == 0xF4 ||
      opcode == 0xF7 || opcode == 0xFA || opcode == 0xFB || opcode == 0xFC ||
      opcode == 0xFF) {

    static int illegalCount = 0;
    illegalCount++;
    printf("ILLEGAL OPCODE[%d]: $%02X at PC=$%04X\n", illegalCount, opcode,
           regPC - 1);
  }

  printf("  LEGAL OPCODE[%d]: $%02X at PC=$%04X\n", illegalCount, opcode,
         regPC - 1);
#endif

  // Decode and execute instruction
  switch (opcode) {
  // ADC - Add with Carry
  case 0x69:
    ADC(addrImmediate());
    break;
  case 0x65:
    ADC(addrZeroPage());
    break;
  case 0x75:
    ADC(addrZeroPageX());
    break;
  case 0x6D:
    ADC(addrAbsolute());
    break;
  case 0x7D:
    ADC(addrAbsoluteX());
    break;
  case 0x79:
    ADC(addrAbsoluteY());
    break;
  case 0x61:
    ADC(addrIndirectX());
    break;
  case 0x71:
    ADC(addrIndirectY());
    break;

  // AND - Logical AND
  case 0x29:
    AND(addrImmediate());
    break;
  case 0x25:
    AND(addrZeroPage());
    break;
  case 0x35:
    AND(addrZeroPageX());
    break;
  case 0x2D:
    AND(addrAbsolute());
    break;
  case 0x3D:
    AND(addrAbsoluteX());
    break;
  case 0x39:
    AND(addrAbsoluteY());
    break;
  case 0x21:
    AND(addrIndirectX());
    break;
  case 0x31:
    AND(addrIndirectY());
    break;

  // ASL - Arithmetic Shift Left
  case 0x0A:
    ASL_ACC();
    break;
  case 0x06:
    ASL(addrZeroPage());
    break;
  case 0x16:
    ASL(addrZeroPageX());
    break;
  case 0x0E:
    ASL(addrAbsolute());
    break;
  case 0x1E:
    ASL(addrAbsoluteX());
    break;

  // Branch instructions
  case 0x90:
    BCC();
    break;
  case 0xB0:
    BCS();
    break;
  case 0xF0:
    BEQ();
    break;
  case 0x30:
    BMI();
    break;
  case 0xD0:
    BNE();
    break;
  case 0x10:
    BPL();
    break;
  case 0x50:
    BVC();
    break;
  case 0x70:
    BVS();
    break;

  // BIT - Bit Test
  case 0x24:
    BIT(addrZeroPage());
    break;
  case 0x2C:
    BIT(addrAbsolute());
    break;

  // BRK - Force Interrupt
  case 0x00:
    BRK();
    break;

  // Flag instructions
  case 0x18:
    CLC();
    break;
  case 0xD8:
    CLD();
    break;
  case 0x58:
    CLI();
    break;
  case 0xB8:
    CLV();
    break;

  // CMP - Compare
  case 0xC9:
    CMP(addrImmediate());
    break;
  case 0xC5:
    CMP(addrZeroPage());
    break;
  case 0xD5:
    CMP(addrZeroPageX());
    break;
  case 0xCD:
    CMP(addrAbsolute());
    break;
  case 0xDD:
    CMP(addrAbsoluteX());
    break;
  case 0xD9:
    CMP(addrAbsoluteY());
    break;
  case 0xC1:
    CMP(addrIndirectX());
    break;
  case 0xD1:
    CMP(addrIndirectY());
    break;

  // CPX - Compare X
  case 0xE0:
    CPX(addrImmediate());
    break;
  case 0xE4:
    CPX(addrZeroPage());
    break;
  case 0xEC:
    CPX(addrAbsolute());
    break;

  // CPY - Compare Y
  case 0xC0:
    CPY(addrImmediate());
    break;
  case 0xC4:
    CPY(addrZeroPage());
    break;
  case 0xCC:
    CPY(addrAbsolute());
    break;

  // DEC - Decrement
  case 0xC6:
    DEC(addrZeroPage());
    break;
  case 0xD6:
    DEC(addrZeroPageX());
    break;
  case 0xCE:
    DEC(addrAbsolute());
    break;
  case 0xDE:
    DEC(addrAbsoluteX());
    break;

  // DEX, DEY - Decrement X, Y
  case 0xCA:
    DEX();
    break;
  case 0x88:
    DEY();
    break;

  case 0x02:
  case 0x12:
  case 0x22:
  case 0x32:
  case 0x42:
  case 0x52:
  case 0x62:
  case 0x72:
  case 0x92:
  case 0xB2:
  case 0xD2:
  case 0xF2:
    KIL();
    break;

  // EOR - Exclusive OR
  case 0x49:
    EOR(addrImmediate());
    break;
  case 0x45:
    EOR(addrZeroPage());
    break;
  case 0x55:
    EOR(addrZeroPageX());
    break;
  case 0x4D:
    EOR(addrAbsolute());
    break;
  case 0x5D:
    EOR(addrAbsoluteX());
    break;
  case 0x59:
    EOR(addrAbsoluteY());
    break;
  case 0x41:
    EOR(addrIndirectX());
    break;
  case 0x51:
    EOR(addrIndirectY());
    break;

  // INC - Increment
  case 0xE6:
    INC(addrZeroPage());
    break;
  case 0xF6:
    INC(addrZeroPageX());
    break;
  case 0xEE:
    INC(addrAbsolute());
    break;
  case 0xFE:
    INC(addrAbsoluteX());
    break;

  // INX, INY - Increment X, Y
  case 0xE8:
    INX();
    break;
  case 0xC8:
    INY();
    break;

  // JMP - Jump
  case 0x4C:
    JMP(addrAbsolute());
    break;
  case 0x6C:
    JMP(addrIndirect());
    break;

  // JSR - Jump to Subroutine
  case 0x20:
    JSR(addrAbsolute());
    break;

  // LDA - Load A
  case 0xA9:
    LDA(addrImmediate());
    break;
  case 0xA5:
    LDA(addrZeroPage());
    break;
  case 0xB5:
    LDA(addrZeroPageX());
    break;
  case 0xAD:
    LDA(addrAbsolute());
    break;
  case 0xBD:
    LDA(addrAbsoluteX());
    break;
  case 0xB9:
    LDA(addrAbsoluteY());
    break;
  case 0xA1:
    LDA(addrIndirectX());
    break;
  case 0xB1:
    LDA(addrIndirectY());
    break;

  // LDX - Load X
  case 0xA2:
    LDX(addrImmediate());
    break;
  case 0xA6:
    LDX(addrZeroPage());
    break;
  case 0xB6:
    LDX(addrZeroPageY());
    break;
  case 0xAE:
    LDX(addrAbsolute());
    break;
  case 0xBE:
    LDX(addrAbsoluteY());
    break;

  // LDY - Load Y
  case 0xA0:
    LDY(addrImmediate());
    break;
  case 0xA4:
    LDY(addrZeroPage());
    break;
  case 0xB4:
    LDY(addrZeroPageX());
    break;
  case 0xAC:
    LDY(addrAbsolute());
    break;
  case 0xBC:
    LDY(addrAbsoluteX());
    break;

  // LSR - Logical Shift Right
  case 0x4A:
    LSR_ACC();
    break;
  case 0x46:
    LSR(addrZeroPage());
    break;
  case 0x56:
    LSR(addrZeroPageX());
    break;
  case 0x4E:
    LSR(addrAbsolute());
    break;
  case 0x5E:
    LSR(addrAbsoluteX());
    break;

  // NOP - No Operation
  case 0xEA:
    NOP();
    break;

  // ORA - Logical OR
  case 0x09:
    ORA(addrImmediate());
    break;
  case 0x05:
    ORA(addrZeroPage());
    break;
  case 0x15:
    ORA(addrZeroPageX());
    break;
  case 0x0D:
    ORA(addrAbsolute());
    break;
  case 0x1D:
    ORA(addrAbsoluteX());
    break;
  case 0x19:
    ORA(addrAbsoluteY());
    break;
  case 0x01:
    ORA(addrIndirectX());
    break;
  case 0x11:
    ORA(addrIndirectY());
    break;

  // Stack operations
  case 0x48:
    PHA();
    break;
  case 0x08:
    PHP();
    break;
  case 0x68:
    PLA();
    break;
  case 0x28:
    PLP();
    break;

  // ROL - Rotate Left
  case 0x2A:
    ROL_ACC();
    break;
  case 0x26:
    ROL(addrZeroPage());
    break;
  case 0x36:
    ROL(addrZeroPageX());
    break;
  case 0x2E:
    ROL(addrAbsolute());
    break;
  case 0x3E:
    ROL(addrAbsoluteX());
    break;

  // ROR - Rotate Right
  case 0x6A:
    ROR_ACC();
    break;
  case 0x66:
    ROR(addrZeroPage());
    break;
  case 0x76:
    ROR(addrZeroPageX());
    break;
  case 0x6E:
    ROR(addrAbsolute());
    break;
  case 0x7E:
    ROR(addrAbsoluteX());
    break;

  // RTI - Return from Interrupt
  case 0x40:
    RTI();
    break;

  // RTS - Return from Subroutine
  case 0x60:
    RTS();
    break;

  // SBC - Subtract with Carry
  case 0xE9:
    SBC(addrImmediate());
    break;
  case 0xE5:
    SBC(addrZeroPage());
    break;
  case 0xF5:
    SBC(addrZeroPageX());
    break;
  case 0xED:
    SBC(addrAbsolute());
    break;
  case 0xFD:
    SBC(addrAbsoluteX());
    break;
  case 0xF9:
    SBC(addrAbsoluteY());
    break;
  case 0xE1:
    SBC(addrIndirectX());
    break;
  case 0xF1:
    SBC(addrIndirectY());
    break;

  // Set flag instructions
  case 0x38:
    SEC();
    break;
  case 0xF8:
    SED();
    break;
  case 0x78:
    SEI();
    break;

  // STA - Store A
  case 0x85:
    STA(addrZeroPage());
    break;
  case 0x95:
    STA(addrZeroPageX());
    break;
  case 0x8D:
    STA(addrAbsolute());
    break;
  case 0x9D:
    STA(addrAbsoluteX());
    break;
  case 0x99:
    STA(addrAbsoluteY());
    break;
  case 0x81:
    STA(addrIndirectX());
    break;
  case 0x91:
    STA(addrIndirectY());
    break;

  // STX - Store X
  case 0x86:
    STX(addrZeroPage());
    break;
  case 0x96:
    STX(addrZeroPageY());
    break;
  case 0x8E:
    STX(addrAbsolute());
    break;

  // STY - Store Y
  case 0x84:
    STY(addrZeroPage());
    break;
  case 0x94:
    STY(addrZeroPageX());
    break;
  case 0x8C:
    STY(addrAbsolute());
    break;

  // Transfer instructions
  case 0xAA:
    TAX();
    break;
  case 0xA8:
    TAY();
    break;
  case 0xBA:
    TSX();
    break;
  case 0x8A:
    TXA();
    break;
  case 0x9A:
    TXS();
    break;
  case 0x98:
    TYA();
    break;
  // Illegal/Undocumented opcodes commonly used by NES games
  case 0x0B:
  case 0x2B:
    ANC(addrImmediate());
    break; // ANC (AND + set carry)
  case 0x4B:
    ALR(addrImmediate());
    break; // ALR (AND + LSR)
  case 0x6B:
    ARR(addrImmediate());
    break; // ARR (AND + ROR)
  case 0x8B:
    XAA(addrImmediate());
    break; // XAA (unstable)
  case 0xAB:
    LAX(addrImmediate());
    break; // LAX (LDA + LDX)
  case 0xCB:
    AXS(addrImmediate());
    break; // AXS (A&X - immediate)
  case 0xEB:
    SBC(addrImmediate());
    break; // SBC (same as legal SBC)

  // ISC (INC + SBC) - very common
  case 0xE3:
    ISC(addrIndirectX());
    break;
  case 0xE7:
    ISC(addrZeroPage());
    break;
  case 0xEF:
    ISC(addrAbsolute());
    break;
  case 0xF3:
    ISC(addrIndirectY());
    break;
  case 0xF7:
    ISC(addrZeroPageX());
    break;
  case 0xFB:
    ISC(addrAbsoluteY());
    break; // This is your $FB!
  case 0xFF:
    ISC(addrAbsoluteX());
    break;

  // DCP (DEC + CMP) - common
  case 0xC3:
    DCP(addrIndirectX());
    break;
  case 0xC7:
    DCP(addrZeroPage());
    break;
  case 0xCF:
    DCP(addrAbsolute());
    break;
  case 0xD3:
    DCP(addrIndirectY());
    break;
  case 0xD7:
    DCP(addrZeroPageX());
    break;
  case 0xDB:
    DCP(addrAbsoluteY());
    break;
  case 0xDF:
    DCP(addrAbsoluteX());
    break;

  // LAX (LDA + LDX) - common
  case 0xA3:
    LAX(addrIndirectX());
    break;
  case 0xA7:
    LAX(addrZeroPage());
    break;
  case 0xAF:
    LAX(addrAbsolute());
    break;
  case 0xB3:
    LAX(addrIndirectY());
    break;
  case 0xB7:
    LAX(addrZeroPageY());
    break;
  case 0xBF:
    LAX(addrAbsoluteY());
    break;

  // SAX (A & X)
  case 0x83:
    SAX(addrIndirectX());
    break;
  case 0x87:
    SAX(addrZeroPage());
    break;
  case 0x8F:
    SAX(addrAbsolute());
    break;
  case 0x97:
    SAX(addrZeroPageY());
    break;

  // SLO (ASL + ORA)
  case 0x03:
    SLO(addrIndirectX());
    break;
  case 0x07:
    SLO(addrZeroPage());
    break;
  case 0x0F:
    SLO(addrAbsolute());
    break;
  case 0x13:
    SLO(addrIndirectY());
    break;
  case 0x17:
    SLO(addrZeroPageX());
    break;
  case 0x1B:
    SLO(addrAbsoluteY());
    break;
  case 0x1F:
    SLO(addrAbsoluteX());
    break;

  // RLA (ROL + AND)
  case 0x23:
    RLA(addrIndirectX());
    break;
  case 0x27:
    RLA(addrZeroPage());
    break;
  case 0x2F:
    RLA(addrAbsolute());
    break;
  case 0x33:
    RLA(addrIndirectY());
    break;
  case 0x37:
    RLA(addrZeroPageX());
    break;
  case 0x3B:
    RLA(addrAbsoluteY());
    break;
  case 0x3F:
    RLA(addrAbsoluteX());
    break;

  // SRE (LSR + EOR)
  case 0x43:
    SRE(addrIndirectX());
    break;
  case 0x47:
    SRE(addrZeroPage());
    break;
  case 0x4F:
    SRE(addrAbsolute());
    break;
  case 0x53:
    SRE(addrIndirectY());
    break;
  case 0x57:
    SRE(addrZeroPageX());
    break;
  case 0x5B:
    SRE(addrAbsoluteY());
    break;
  case 0x5F:
    SRE(addrAbsoluteX());
    break;

  // RRA (ROR + ADC)
  case 0x63:
    RRA(addrIndirectX());
    break;
  case 0x67:
    RRA(addrZeroPage());
    break;
  case 0x6F:
    RRA(addrAbsolute());
    break;
  case 0x73:
    RRA(addrIndirectY());
    break;
  case 0x77:
    RRA(addrZeroPageX());
    break;
  case 0x7B:
    RRA(addrAbsoluteY());
    break;
  case 0x7F:
    RRA(addrAbsoluteX());
    break;

  case 0x93:
    SHA(addrIndirectY());
    break; // SHA (A&X&H) indirect,Y
  case 0x9F:
    SHA(addrAbsoluteY());
    break; // SHA (A&X&H) absolute,Y

  // SHX (X & high byte)
  case 0x9E:
    SHX(addrAbsoluteY());
    break; // SHX (X&H) absolute,Y

  // SHY (Y & high byte)
  case 0x9C:
    SHY(addrAbsoluteX());
    break; // SHY (Y&H) absolute,X

  // TAS (A & X -> SP, then A & X & H)
  case 0x9B:
    TAS(addrAbsoluteY());
    break; // TAS absolute,Y

  // LAS (load A,X,SP from memory & SP)
  case 0xBB:
    LAS(addrAbsoluteY());
    break; // LAS absolute,Y

  // NOPs (various forms)
  case 0x1A:
  case 0x3A:
  case 0x5A:
  case 0x7A:
  case 0xDA:
  case 0xFA:
    NOP();
    break;
  case 0x80:
  case 0x82:
  case 0x89:
  case 0xC2:
  case 0xE2:
    regPC++;
    break; // NOP immediate
  case 0x04:
  case 0x44:
  case 0x64:
    regPC++;
    break; // NOP zero page
  case 0x0C:
    regPC += 2;
    break; // NOP absolute
  case 0x14:
  case 0x34:
  case 0x54:
  case 0x74:
  case 0xD4:
  case 0xF4:
    regPC++;
    break; // NOP zero page,X
  case 0x1C:
  case 0x3C:
  case 0x5C:
  case 0x7C:
  case 0xDC:
  case 0xFC:
    regPC += 2;
    break; // NOP absolute,X

  default:
    std::cerr << "Unknown opcode: $" << std::hex << (int)opcode << " at PC=$"
              << (regPC - 1) << std::dec << std::endl;
    cycles = 2; // Default cycle count for unknown instructions
    break;
  }

  // Update cycle counters
  totalCycles += cycles;
  frameCycles += cycles;
  masterCycles += cycles;
}

void SMBEmulator::catchUpPPU() {
    // CRITICAL FIX: Don't do complex PPU catch-up during cycle-accurate mode
    // The cycle-accurate loop already keeps PPU in sync
    
    // Simple sync check - just ensure we're not too far off
    uint64_t targetPPUCycles = masterCycles * 3;
    uint64_t currentPPUCycles = ppu->getCurrentCycles();
    
    // Only do very minor adjustments
    if (targetPPUCycles > currentPPUCycles) {
        uint64_t difference = targetPPUCycles - currentPPUCycles;
        if (difference < 10) { // Very small adjustments only
            ppu->addCycles(difference);
        }
    }
    
    ppuCycles = ppu->getCurrentCycles();
}



void SMBEmulator::checkPendingInterrupts() {
    // Handle MMC3 IRQ
    if (nesHeader.mapper == 4 && mmc3.irqPending) {
        mmc3.irqPending = false;
        
        // Only trigger IRQ if not disabled
        if (!getFlag(FLAG_INTERRUPT)) {
            // Push PC and status to stack
            pushWord(regPC);
            pushByte(regP & ~FLAG_BREAK);
            setFlag(FLAG_INTERRUPT, true);
            
            // Jump to IRQ vector
            regPC = readWord(0xFFFE);
            
            totalCycles += 7; // IRQ takes 7 cycles
            frameCycles += 7;
            
            printf("MMC3 IRQ triggered at PC=$%04X\n", regPC);
        }
    }
    
    // Handle NMI
    if (nmiPending) {
        handleNMI();
        nmiPending = false;
    }
}


uint8_t SMBEmulator::readByte(uint16_t address) {
    if (address < 0x2000) {
        // RAM (mirrored every 2KB)
        return ram[address & 0x7FF];
    } else if (address < 0x4000) {
        // PPU registers (mirrored every 8 bytes)
        catchUpPPU();
        uint16_t ppuAddr = 0x2000 + (address & 0x7);
        return ppu->readRegister(ppuAddr);
    } else if (address < 0x4020) {
        // APU and I/O registers

        switch (address) {
        case 0x4016:
            return controller1->readByte(PLAYER_1);
        case 0x4017: {
            if (zapperEnabled && zapper) {
                uint8_t zapperValue = zapper->readByte();
                return zapperValue;
            } else {
                return controller2->readByte(PLAYER_2);
            }
        }
        }

    } else if (address >= 0x6000 && address < 0x8000) {
        // SRAM area ($6000-$7FFF)
        if (sramEnabled && sram && nesHeader.battery) {
            uint16_t sramAddr = address - 0x6000;
            if (sramAddr < sramSize) {
                return sram[sramAddr];
            }
        }
        return 0;  // Open bus if SRAM not available
    } else if (address >= 0x8000) {
        // PRG ROM with FIXED MMC1 mapping
        if (nesHeader.mapper == 1) {
            uint32_t romAddr;
            uint8_t totalBanks = prgSize / 0x4000;
            uint8_t prgMode = (mmc1.control >> 2) & 0x03;

            if (address < 0xC000) {
                // $8000-$BFFF
                switch (prgMode) {
                    case 0:
                    case 1:
                        // 32KB mode
                        romAddr = (mmc1.currentPRGBank * 0x8000) + (address - 0x8000);
                        break;
                    case 2:
                        // Fixed first bank at $8000
                        romAddr = address - 0x8000;
                        break;
                    case 3:
                    default:
                        // Switchable bank at $8000
                        romAddr = (mmc1.currentPRGBank * 0x4000) + (address - 0x8000);
                        break;
                }
            } else {
                // $C000-$FFFF
                switch (prgMode) {
                    case 0:
                    case 1:
                        // 32KB mode
                        romAddr = (mmc1.currentPRGBank * 0x8000) + (address - 0x8000);
                        break;
                    case 2:
                        // Switchable last bank
                        romAddr = (mmc1.prgBank * 0x4000) + (address - 0xC000);
                        break;
                    case 3:
                    default:
                        // FIXED last bank at $C000 - CRITICAL FOR RESET!
                        uint8_t lastBank = totalBanks - 1;
                        romAddr = (lastBank * 0x4000) + (address - 0xC000);
                        break;
                }
            }

            // Bounds checking
            if (romAddr >= prgSize) {
                printf("MMC1: ROM address $%08X out of bounds (size $%08X)\n", romAddr, prgSize);
                return 0;
            }

            return prgROM[romAddr];
        } else if (nesHeader.mapper == 0) {
            // NROM - simple mapping
            uint32_t romAddr = address - 0x8000;
            if (prgSize == 16384) {
                // 16KB ROM mirrored
                romAddr &= 0x3FFF;
            }
            if (romAddr < prgSize) {
                return prgROM[romAddr];
            }
        } else if (nesHeader.mapper == 2) {
            // UxROM mapping
            uint32_t romAddr;

            if (address < 0xC000) {
                // $8000-$BFFF: Switchable 16KB PRG bank
                romAddr = (uxrom.prgBank * 0x4000) + (address - 0x8000);
            } else {
                // $C000-$FFFF: Fixed to LAST 16KB PRG bank
                uint8_t totalBanks = prgSize / 0x4000;
                uint8_t lastBank = totalBanks - 1;
                romAddr = (lastBank * 0x4000) + (address - 0xC000);
            }

            if (romAddr < prgSize) {
                return prgROM[romAddr];
            }
        } else if (nesHeader.mapper == 4) {
            // MMC3 mapping - 8KB banks
            uint8_t bankIndex = (address - 0x8000) / 0x2000; // 0-3
            uint16_t bankOffset = (address - 0x8000) % 0x2000;
            uint32_t romAddr = (mmc3.currentPRGBanks[bankIndex] * 0x2000) + bankOffset;

            if (romAddr < prgSize) {
                return prgROM[romAddr];
            }
       } else if (nesHeader.mapper == 9) {
         // MMC2 mapping
         uint32_t romAddr;
    
         if (address < 0xA000) {
             // $8000-$9FFF: Switchable 8KB PRG bank
             romAddr = (mmc2.prgBank * 0x2000) + (address - 0x8000);
         } else if (address < 0xC000) {
             // $A000-$BFFF: Fixed to last 8KB of ROM - 3
             uint8_t totalBanks = prgSize / 0x2000;  // 8KB banks
             romAddr = ((totalBanks - 3) * 0x2000) + (address - 0xA000);
         } else if (address < 0xE000) {
             // $C000-$DFFF: Fixed to last 8KB of ROM - 2  
             uint8_t totalBanks = prgSize / 0x2000;
             romAddr = ((totalBanks - 2) * 0x2000) + (address - 0xC000);
         } else {
             // $E000-$FFFF: Fixed to last 8KB of ROM - 1
             uint8_t totalBanks = prgSize / 0x2000;
             romAddr = ((totalBanks - 1) * 0x2000) + (address - 0xE000);
         }
    
         if (romAddr < prgSize) {
             return prgROM[romAddr];
         }
     } else if (nesHeader.mapper == 66) {
            // GxROM mapping - 32KB PRG banks
            uint32_t romAddr = (gxrom.prgBank * 0x8000) + (address - 0x8000);

            if (romAddr < prgSize) {
                return prgROM[romAddr];
            }
        } else if (nesHeader.mapper == 3) {
            // CNROM - 32KB PRG ROM (no PRG banking)
            uint32_t romAddr = address - 0x8000;

            if (romAddr < prgSize) {
                return prgROM[romAddr];
            }
        }
    }

    return 0; // Open bus
}


void SMBEmulator::writeMMC3Register(uint16_t address, uint8_t value) {
    static int regDebugCount = 0;
    
    switch (address & 0xE001) {
        case 0x8000: // Bank select
            mmc3.bankSelect = value;
            updateMMC3Banks();
            break;
            
        case 0x8001: // Bank data
        {
            uint8_t bank = mmc3.bankSelect & 7;
            mmc3.bankData[bank] = value;
            updateMMC3Banks();
            break;
        }
        
        case 0xA000: // Mirroring
            mmc3.mirroring = value & 1;
            break;
            
        case 0xA001: // PRG RAM protect
            mmc3.prgRamProtect = value;
            break;
            
        case 0xC000: // IRQ latch
            mmc3.irqLatch = value;
            if (regDebugCount < 10) {
                printf("MMC3: Set IRQ latch to %d\n", value);
                regDebugCount++;
            }
            break;
            
        case 0xC001: // IRQ reload
            mmc3.irqReload = true;
            if (regDebugCount < 10) {
                printf("MMC3: IRQ reload requested\n");
            }
            break;
            
        case 0xE000: // IRQ disable
            mmc3.irqEnable = false;
            mmc3.irqPending = false; // Clear any pending IRQ
            if (regDebugCount < 10) {
                printf("MMC3: IRQ disabled\n");
            }
            break;
            
        case 0xE001: // IRQ enable
            mmc3.irqEnable = true;
            if (regDebugCount < 10) {
                printf("MMC3: IRQ enabled\n");
                regDebugCount++;
            }
            break;
    }
}

void SMBEmulator::updateMMC3Banks() {
    uint8_t totalPRGBanks = prgSize / 0x2000; // Number of 8KB banks
    uint8_t totalCHRBanks = chrSize / 0x400;  // Number of 1KB banks
    
    // PRG banking - ensure last two banks are properly fixed
    bool prgSwap = (mmc3.bankSelect & 0x40) != 0;
    
    if (prgSwap) {
        // PRG mode 1: $8000 swapped with $C000
        mmc3.currentPRGBanks[0] = (totalPRGBanks - 2) % totalPRGBanks; // Fixed second-to-last
        mmc3.currentPRGBanks[1] = mmc3.bankData[7] % totalPRGBanks;   // R7
        mmc3.currentPRGBanks[2] = mmc3.bankData[6] % totalPRGBanks;   // R6  
        mmc3.currentPRGBanks[3] = (totalPRGBanks - 1) % totalPRGBanks; // Fixed last
    } else {
        // PRG mode 0: Normal layout
        mmc3.currentPRGBanks[0] = mmc3.bankData[6] % totalPRGBanks;   // R6
        mmc3.currentPRGBanks[1] = mmc3.bankData[7] % totalPRGBanks;   // R7
        mmc3.currentPRGBanks[2] = (totalPRGBanks - 2) % totalPRGBanks; // Fixed second-to-last
        mmc3.currentPRGBanks[3] = (totalPRGBanks - 1) % totalPRGBanks; // Fixed last
    }
    
    // CHR banking - CRITICAL FIX for sprite issues
    bool chrA12Invert = (mmc3.bankSelect & 0x80) != 0;
    
    if (chrA12Invert) {
        // CHR inversion mode: Sprites use $0000-$0FFF, Background uses $1000-$1FFF
        // This is CRITICAL for SMB2 sprites to display correctly
        
        // $0000-$0FFF: 4×1KB banks (used for sprites in SMB2)
        mmc3.currentCHRBanks[0] = mmc3.bankData[2] % totalCHRBanks; // R2
        mmc3.currentCHRBanks[1] = mmc3.bankData[3] % totalCHRBanks; // R3  
        mmc3.currentCHRBanks[2] = mmc3.bankData[4] % totalCHRBanks; // R4
        mmc3.currentCHRBanks[3] = mmc3.bankData[5] % totalCHRBanks; // R5
        
        // $1000-$1FFF: 2×2KB banks (used for background in SMB2)
        uint8_t r0_base = mmc3.bankData[0] & 0xFE; // Force even for 2KB alignment
        uint8_t r1_base = mmc3.bankData[1] & 0xFE; // Force even for 2KB alignment
        
        mmc3.currentCHRBanks[4] = r0_base % totalCHRBanks;
        mmc3.currentCHRBanks[5] = (r0_base + 1) % totalCHRBanks;
        mmc3.currentCHRBanks[6] = r1_base % totalCHRBanks; 
        mmc3.currentCHRBanks[7] = (r1_base + 1) % totalCHRBanks;
    } else {
        // Normal CHR mode: Background uses $0000-$0FFF, Sprites use $1000-$1FFF
        
        // $0000-$0FFF: 2×2KB banks (background)
        uint8_t r0_base = mmc3.bankData[0] & 0xFE;
        uint8_t r1_base = mmc3.bankData[1] & 0xFE;
        
        mmc3.currentCHRBanks[0] = r0_base % totalCHRBanks;
        mmc3.currentCHRBanks[1] = (r0_base + 1) % totalCHRBanks;
        mmc3.currentCHRBanks[2] = r1_base % totalCHRBanks;
        mmc3.currentCHRBanks[3] = (r1_base + 1) % totalCHRBanks;
        
        // $1000-$1FFF: 4×1KB banks (sprites)  
        mmc3.currentCHRBanks[4] = mmc3.bankData[2] % totalCHRBanks; // R2
        mmc3.currentCHRBanks[5] = mmc3.bankData[3] % totalCHRBanks; // R3
        mmc3.currentCHRBanks[6] = mmc3.bankData[4] % totalCHRBanks; // R4
        mmc3.currentCHRBanks[7] = mmc3.bankData[5] % totalCHRBanks; // R5
    }
    
    // Debug output for CHR banking (remove after fixing)
    static int bankingDebugCount = 0;
    if (bankingDebugCount < 10) {
        printf("MMC3 Banking Update - CHR A12 Invert: %s\n", chrA12Invert ? "YES" : "NO");
        printf("  CHR Banks: [%02X %02X %02X %02X] [%02X %02X %02X %02X]\n",
               mmc3.currentCHRBanks[0], mmc3.currentCHRBanks[1], 
               mmc3.currentCHRBanks[2], mmc3.currentCHRBanks[3],
               mmc3.currentCHRBanks[4], mmc3.currentCHRBanks[5],
               mmc3.currentCHRBanks[6], mmc3.currentCHRBanks[7]);
        bankingDebugCount++;
    }
}

void SMBEmulator::stepMMC3IRQ() {
    static int irqDebugCount = 0;
    
    // Handle reload first
    if (mmc3.irqReload) {
        mmc3.irqCounter = mmc3.irqLatch;
        mmc3.irqReload = false;
        
        if (irqDebugCount < 10) {
            printf("MMC3 IRQ: Reloaded counter to %d\n", mmc3.irqCounter);
            irqDebugCount++;
        }
        
        // CRITICAL: If latch is 0, IRQ triggers immediately
        if (mmc3.irqCounter == 0 && mmc3.irqEnable) {
            mmc3.irqPending = true;
            if (irqDebugCount < 10) {
                printf("*** MMC3 IRQ: Immediate trigger (latch=0) ***\n");
            }
        }
        return; // Don't decrement on reload cycle
    }
    
    // Handle normal countdown
    if (mmc3.irqCounter > 0) {
        mmc3.irqCounter--;
        
        if (irqDebugCount < 10) {
            printf("MMC3 IRQ: Counter decremented to %d\n", mmc3.irqCounter);
        }
        
        // Trigger IRQ when counter reaches 0
        if (mmc3.irqCounter == 0 && mmc3.irqEnable) {
            mmc3.irqPending = true;
            if (irqDebugCount < 10) {
                printf("*** MMC3 IRQ: Counter reached 0, IRQ pending! ***\n");
                irqDebugCount++;
            }
        }
    }
}

void SMBEmulator::writeByte(uint16_t address, uint8_t value)
{
    if (address < 0x2000) {
        ram[address & 0x7FF] = value;
    }
    else if (address < 0x4000) {
        // PPU registers
        ppu->writeRegister(0x2000 + (address & 0x7), value);
    }
    else if (address < 0x4020) {
        switch (address) {
            case 0x4014:
                ppu->writeDMA(value);
                masterCycles += 513;  // DMA cycles
                break;

            case 0x4016:
                controller1->writeByte(value);
                controller2->writeByte(value);
                break;

            default:
                apu->writeRegister(address, value);
                break;
        }
    }
    else if (address >= 0x6000 && address < 0x8000) {
        // SRAM area ($6000-$7FFF)
        if (sramEnabled && sram && nesHeader.battery) {
            uint16_t sramAddr = address - 0x6000;
            if (sramAddr < sramSize) {
                sram[sramAddr] = value;
                sramDirty = true;  // Mark SRAM as needing to be saved
                // No auto-save - player controls when to save
            }
        }
    }
    else if (address >= 0x8000) {
        // Mapper registers
        if (nesHeader.mapper == 1) {
            writeMMC1Register(address, value);
        } else if (nesHeader.mapper == 66) {
            writeGxROMRegister(address, value);
        } else if (nesHeader.mapper == 3) {
            writeCNROMRegister(address, value);
        } else if (nesHeader.mapper == 4) {
            writeMMC3Register(address, value);
        } else if (nesHeader.mapper == 2) {
            writeUxROMRegister(address, value);  
        } else if (nesHeader.mapper == 9) {
            writeMMC2Register(address, value);
       }
   }  
}

uint16_t SMBEmulator::readWord(uint16_t address) {
  return readByte(address) | (readByte(address + 1) << 8);
}

void SMBEmulator::writeWord(uint16_t address, uint16_t value) {
  writeByte(address, value & 0xFF);
  writeByte(address + 1, value >> 8);
}

// Stack operations
void SMBEmulator::pushByte(uint8_t value) {
  writeByte(0x100 + regSP, value);
  regSP--;
}

uint8_t SMBEmulator::pullByte() {
  regSP++;
  return readByte(0x100 + regSP);
}

void SMBEmulator::pushWord(uint16_t value) {
  pushByte(value >> 8);
  pushByte(value & 0xFF);
}

uint16_t SMBEmulator::pullWord() {
  uint8_t lo = pullByte();
  uint8_t hi = pullByte();
  return lo | (hi << 8);
}

// Instruction fetch
uint8_t SMBEmulator::fetchByte() { return readByte(regPC++); }

uint16_t SMBEmulator::fetchWord() {
  uint16_t value = readWord(regPC);
  regPC += 2;
  return value;
}

// Status flag helpers
void SMBEmulator::setFlag(uint8_t flag, bool value) {
  if (value) {
    regP |= flag;
  } else {
    regP &= ~flag;
  }
}

bool SMBEmulator::getFlag(uint8_t flag) const { return (regP & flag) != 0; }

void SMBEmulator::updateZN(uint8_t value) {
  setFlag(FLAG_ZERO, value == 0);
  setFlag(FLAG_NEGATIVE, (value & 0x80) != 0);
}

// Addressing modes
uint16_t SMBEmulator::addrImmediate() { return regPC++; }

uint16_t SMBEmulator::addrZeroPage() { return fetchByte(); }

uint16_t SMBEmulator::addrZeroPageX() { return (fetchByte() + regX) & 0xFF; }

uint16_t SMBEmulator::addrZeroPageY() { return (fetchByte() + regY) & 0xFF; }

uint16_t SMBEmulator::addrAbsolute() { return fetchWord(); }

uint16_t SMBEmulator::addrAbsoluteX() { return fetchWord() + regX; }

uint16_t SMBEmulator::addrAbsoluteY() { return fetchWord() + regY; }

uint16_t SMBEmulator::addrIndirect() {
  uint16_t addr = fetchWord();
  // 6502 bug: if address is $xxFF, high byte is fetched from $xx00
  if ((addr & 0xFF) == 0xFF) {
    return readByte(addr) | (readByte(addr & 0xFF00) << 8);
  } else {
    return readWord(addr);
  }
}

uint16_t SMBEmulator::addrIndirectX() {
  uint8_t addr = (fetchByte() + regX) & 0xFF;
  return readByte(addr) | (readByte((addr + 1) & 0xFF) << 8);
}

uint16_t SMBEmulator::addrIndirectY() {
  uint8_t addr = fetchByte();
  uint16_t base = readByte(addr) | (readByte((addr + 1) & 0xFF) << 8);
  return base + regY;
}

uint16_t SMBEmulator::addrRelative() {
  int8_t offset = fetchByte();
  return regPC + offset;
}

// Instruction implementations
void SMBEmulator::ADC(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint16_t result = regA + value + (getFlag(FLAG_CARRY) ? 1 : 0);

  setFlag(FLAG_CARRY, result > 0xFF);
  setFlag(FLAG_OVERFLOW, ((regA ^ result) & (value ^ result) & 0x80) != 0);

  regA = result & 0xFF;
  updateZN(regA);
}

void SMBEmulator::AND(uint16_t addr) {
  regA &= readByte(addr);
  updateZN(regA);
}

void SMBEmulator::ASL(uint16_t addr) {
  uint8_t value = readByte(addr);
  setFlag(FLAG_CARRY, (value & 0x80) != 0);
  value <<= 1;
  writeByte(addr, value);
  updateZN(value);
}

void SMBEmulator::ASL_ACC() {
  setFlag(FLAG_CARRY, (regA & 0x80) != 0);
  regA <<= 1;
  updateZN(regA);
}

void SMBEmulator::BCC() {
  if (!getFlag(FLAG_CARRY)) {
    regPC = addrRelative();
  } else {
    regPC++; // Skip the offset byte
  }
}

void SMBEmulator::BCS() {
  if (getFlag(FLAG_CARRY)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void SMBEmulator::BEQ() {
  if (getFlag(FLAG_ZERO)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void SMBEmulator::BIT(uint16_t addr) {
  uint8_t value = readByte(addr);
  setFlag(FLAG_ZERO, (regA & value) == 0);
  setFlag(FLAG_OVERFLOW, (value & 0x40) != 0);
  setFlag(FLAG_NEGATIVE, (value & 0x80) != 0);
}

void SMBEmulator::BMI() {
  if (getFlag(FLAG_NEGATIVE)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void SMBEmulator::BNE() {
  if (!getFlag(FLAG_ZERO)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void SMBEmulator::BPL() {
  if (!getFlag(FLAG_NEGATIVE)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void SMBEmulator::BRK() {
  regPC++; // BRK is 2 bytes
  pushWord(regPC);
  pushByte(regP | FLAG_BREAK);
  setFlag(FLAG_INTERRUPT, true);
  regPC = readWord(0xFFFE); // IRQ vector
}

void SMBEmulator::BVC() {
  if (!getFlag(FLAG_OVERFLOW)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void SMBEmulator::BVS() {
  if (getFlag(FLAG_OVERFLOW)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void SMBEmulator::CLC() { setFlag(FLAG_CARRY, false); }

void SMBEmulator::CLD() { setFlag(FLAG_DECIMAL, false); }

void SMBEmulator::CLI() { setFlag(FLAG_INTERRUPT, false); }

void SMBEmulator::CLV() { setFlag(FLAG_OVERFLOW, false); }

void SMBEmulator::CMP(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint8_t result = regA - value;
  setFlag(FLAG_CARRY, regA >= value);
  updateZN(result);
}

void SMBEmulator::CPX(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint8_t result = regX - value;
  setFlag(FLAG_CARRY, regX >= value);
  updateZN(result);
}

void SMBEmulator::CPY(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint8_t result = regY - value;
  setFlag(FLAG_CARRY, regY >= value);
  updateZN(result);
}

void SMBEmulator::DEC(uint16_t addr) {
  uint8_t value = readByte(addr) - 1;
  writeByte(addr, value);
  updateZN(value);
}

void SMBEmulator::DEX() {
  regX--;
  updateZN(regX);
}

void SMBEmulator::DEY() {
  regY--;
  updateZN(regY);
}

void SMBEmulator::EOR(uint16_t addr) {
  regA ^= readByte(addr);
  updateZN(regA);
}

void SMBEmulator::INC(uint16_t addr) {
  uint8_t value = readByte(addr) + 1;
  writeByte(addr, value);
  updateZN(value);
}

void SMBEmulator::INX() {
  regX++;
  updateZN(regX);
}

void SMBEmulator::INY() {
  regY++;
  updateZN(regY);
}

void SMBEmulator::JMP(uint16_t addr) { regPC = addr; }

void SMBEmulator::JSR(uint16_t addr) {
  pushWord(regPC - 1);
  regPC = addr;
}

void SMBEmulator::LDA(uint16_t addr) {
  regA = readByte(addr);
  updateZN(regA);
}

void SMBEmulator::LDX(uint16_t addr) {
  regX = readByte(addr);
  updateZN(regX);
}

void SMBEmulator::LDY(uint16_t addr) {
  regY = readByte(addr);
  updateZN(regY);
}

void SMBEmulator::LSR(uint16_t addr) {
  uint8_t value = readByte(addr);
  setFlag(FLAG_CARRY, (value & 0x01) != 0);
  value >>= 1;
  writeByte(addr, value);
  updateZN(value);
}

void SMBEmulator::LSR_ACC() {
  setFlag(FLAG_CARRY, (regA & 0x01) != 0);
  regA >>= 1;
  updateZN(regA);
}

void SMBEmulator::NOP() {
  // Do nothing
}

void SMBEmulator::ORA(uint16_t addr) {
  regA |= readByte(addr);
  updateZN(regA);
}

void SMBEmulator::PHA() { pushByte(regA); }

void SMBEmulator::PHP() { pushByte(regP | FLAG_BREAK | FLAG_UNUSED); }

void SMBEmulator::PLA() {
  regA = pullByte();
  updateZN(regA);
}

void SMBEmulator::PLP() {
  regP = pullByte() | FLAG_UNUSED;
  regP &= ~FLAG_BREAK;
}

void SMBEmulator::ROL(uint16_t addr) {
  uint8_t value = readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (value & 0x80) != 0);
  value = (value << 1) | (oldCarry ? 1 : 0);
  writeByte(addr, value);
  updateZN(value);
}

void SMBEmulator::ROL_ACC() {
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (regA & 0x80) != 0);
  regA = (regA << 1) | (oldCarry ? 1 : 0);
  updateZN(regA);
}

void SMBEmulator::ROR(uint16_t addr) {
  uint8_t value = readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (value & 0x01) != 0);
  value = (value >> 1) | (oldCarry ? 0x80 : 0);
  writeByte(addr, value);
  updateZN(value);
}

void SMBEmulator::ROR_ACC() {
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (regA & 0x01) != 0);
  regA = (regA >> 1) | (oldCarry ? 0x80 : 0);
  updateZN(regA);
}

void SMBEmulator::RTI() {
  regP = pullByte() | FLAG_UNUSED;
  regP &= ~FLAG_BREAK;
  regPC = pullWord();
}

void SMBEmulator::RTS() { regPC = pullWord() + 1; }

void SMBEmulator::SBC(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint16_t result = regA - value - (getFlag(FLAG_CARRY) ? 0 : 1);

  setFlag(FLAG_CARRY, result <= 0xFF);
  setFlag(FLAG_OVERFLOW, ((regA ^ result) & (~value ^ result) & 0x80) != 0);

  regA = result & 0xFF;
  updateZN(regA);
}

void SMBEmulator::SEC() { setFlag(FLAG_CARRY, true); }

void SMBEmulator::SED() { setFlag(FLAG_DECIMAL, true); }

void SMBEmulator::SEI() { setFlag(FLAG_INTERRUPT, true); }

void SMBEmulator::STA(uint16_t addr) { writeByte(addr, regA); }

void SMBEmulator::STX(uint16_t addr) { writeByte(addr, regX); }

void SMBEmulator::STY(uint16_t addr) { writeByte(addr, regY); }

void SMBEmulator::TAX() {
  regX = regA;
  updateZN(regX);
}

void SMBEmulator::TAY() {
  regY = regA;
  updateZN(regY);
}

void SMBEmulator::TSX() {
  regX = regSP;
  updateZN(regX);
}

void SMBEmulator::TXA() {
  regA = regX;
  updateZN(regA);
}

void SMBEmulator::TXS() { regSP = regX; }

void SMBEmulator::TYA() {
  regA = regY;
  updateZN(regA);
}

void SMBEmulator::SHA(uint16_t addr) {
  // SHA: Store A & X & (high byte of address + 1)
  // This is an unstable instruction - the high byte interaction is complex
  uint8_t highByte = (addr >> 8) + 1;
  uint8_t result = regA & regX & highByte;
  writeByte(addr, result);
}

void SMBEmulator::SHX(uint16_t addr) {
  // SHX: Store X & (high byte of address + 1)
  uint8_t highByte = (addr >> 8) + 1;
  uint8_t result = regX & highByte;
  writeByte(addr, result);
}

void SMBEmulator::SHY(uint16_t addr) {
  // SHY: Store Y & (high byte of address + 1)
  uint8_t highByte = (addr >> 8) + 1;
  uint8_t result = regY & highByte;
  writeByte(addr, result);
}

void SMBEmulator::TAS(uint16_t addr) {
  // TAS: Transfer A & X to SP, then store A & X & (high byte + 1)
  regSP = regA & regX;
  uint8_t highByte = (addr >> 8) + 1;
  uint8_t result = regA & regX & highByte;
  writeByte(addr, result);
}

void SMBEmulator::LAS(uint16_t addr) {
  // LAS: Load A, X, and SP with memory value & SP
  uint8_t value = readByte(addr);
  uint8_t result = value & regSP;
  regA = result;
  regX = result;
  regSP = result;
  updateZN(result);
}

void SMBEmulator::scaleBuffer16(uint16_t *nesBuffer, uint16_t *screenBuffer,
                                int screenWidth, int screenHeight) {
  // Clear screen with black
  for (int i = 0; i < screenWidth * screenHeight; i++) {
    screenBuffer[i] = 0x0000;
  }

  // Calculate scaling
  int scale_x = screenWidth / 256;
  int scale_y = screenHeight / 240;
  int scale = (scale_x < scale_y) ? scale_x : scale_y;
  if (scale < 1)
    scale = 1;

  int dest_w = 256 * scale;
  int dest_h = 240 * scale;
  int dest_x = (screenWidth - dest_w) / 2;
  int dest_y = (screenHeight - dest_h) / 2;

  // Simple scaling
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 256; x++) {
      uint16_t pixel = nesBuffer[y * 256 + x];

      // Draw scale x scale block
      for (int sy = 0; sy < scale; sy++) {
        for (int sx = 0; sx < scale; sx++) {
          int screen_x = dest_x + x * scale + sx;
          int screen_y = dest_y + y * scale + sy;

          if (screen_x >= 0 && screen_x < screenWidth && screen_y >= 0 &&
              screen_y < screenHeight) {
            screenBuffer[screen_y * screenWidth + screen_x] = pixel;
          }
        }
      }
    }
  }
}

void SMBEmulator::render16(uint16_t *buffer) { 
    //ppu->render(buffer); 
    ppu->render16(buffer); 
}

void SMBEmulator::render(uint32_t *buffer) { 
    ppu->render(buffer); 
}

void SMBEmulator::renderScaled16(uint16_t *buffer, int screenWidth,
                                 int screenHeight) {
  // First render the game using PPU scaling
    static uint16_t nesBuffer[256 * 240];
    //ppu->getFrameBuffer(nesBuffer);
    ppu->render16(nesBuffer);
    scaleBuffer16(nesBuffer, buffer, screenWidth, screenHeight);

    if (zapperEnabled && zapper) {
    // Get the raw mouse coordinates (these should be in NES coordinates 0-255,
    // 0-239)
    int nesMouseX = zapper->getMouseX();
    int nesMouseY = zapper->getMouseY();

    // Calculate scaling factors
    int scale_x = screenWidth / 256;
    int scale_y = screenHeight / 240;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1)
      scale = 1;

    // Calculate actual rendered game area
    int dest_w = 256 * scale;
    int dest_h = 240 * scale;
    int dest_x = (screenWidth - dest_w) / 2;
    int dest_y = (screenHeight - dest_h) / 2;

    // Convert NES coordinates to screen coordinates
    int screenMouseX = (nesMouseX * scale) + dest_x;
    int screenMouseY = (nesMouseY * scale) + dest_y;

    // Bounds check - make sure we're within the game area
    bool inGameArea =
        (screenMouseX >= dest_x && screenMouseX < dest_x + dest_w &&
         screenMouseY >= dest_y && screenMouseY < dest_y + dest_h);

    // CRITICAL: Duck Hunt light detection - DO THIS BEFORE DRAWING CROSSHAIR!
    bool currentTrigger = zapper->isTriggerPressed();

    if (currentTrigger && inGameArea) {
      // Duck Hunt checks for light detection EVERY frame during trigger press
      // IMPORTANT: Check light BEFORE drawing crosshair so we don't detect our
      // own pixels!
      bool lightDetected = zapper->detectLightScaled(
          buffer, screenWidth, screenHeight, screenMouseX, screenMouseY, scale);

      // Set light detection immediately - no delay
      zapper->setLightDetected(lightDetected);

    } else if (!currentTrigger) {
      // Clear light when trigger not pressed
      zapper->setLightDetected(false);
    } else {
      // Trigger pressed but outside game area
      zapper->setLightDetected(false);
    }

    // NOW draw crosshair AFTER light detection - if in game area
    if (inGameArea) {
      zapper->drawCrosshairScaled(buffer, screenWidth, screenHeight,
                                  screenMouseX, screenMouseY, scale);
    }
  }
}

/*void SMBEmulator::renderScaled32(uint32_t* buffer, int screenWidth, int
screenHeight)
{
    ppu->renderScaled32(buffer, screenWidth, screenHeight);
}*/

// Audio functions (delegate to APU)
void SMBEmulator::audioCallback(uint8_t *stream, int length) {
  apu->output(stream, length);
}

void SMBEmulator::toggleAudioMode() { apu->toggleAudioMode(); }

bool SMBEmulator::isUsingMIDIAudio() const { return apu->isUsingMIDI(); }

void SMBEmulator::debugAudioChannels() { apu->debugAudio(); }

// Controller access
Controller &SMBEmulator::getController1() { return *controller1; }

Controller &SMBEmulator::getController2() { return *controller2; }

// CPU state access
SMBEmulator::CPUState SMBEmulator::getCPUState() const {
  CPUState state;
  state.A = regA;
  state.X = regX;
  state.Y = regY;
  state.SP = regSP;
  state.PC = regPC;
  state.P = regP;
  state.cycles = totalCycles;
  return state;
}

uint8_t SMBEmulator::readMemory(uint16_t address) const {
  return const_cast<SMBEmulator *>(this)->readByte(address);
}

void SMBEmulator::writeMemory(uint16_t address, uint8_t value) {
  writeByte(address, value);
}

// Save states
void SMBEmulator::saveState(const std::string &filename) {
  EmulatorSaveState state;
  memset(&state, 0, sizeof(state));

  // Header
  strcpy(state.header, "NESSAVE");
  state.version = 1;

  // CPU state
  state.cpu_A = regA;
  state.cpu_X = regX;
  state.cpu_Y = regY;
  state.cpu_SP = regSP;
  state.cpu_P = regP;
  state.cpu_PC = regPC;
  state.cpu_cycles = totalCycles;

  // RAM
  memcpy(state.ram, ram, sizeof(ram));

  // TODO: Add PPU and APU state

  // Create appropriate filename based on platform
  std::string actualFilename;
#ifdef __DJGPP__
  // DOS 8.3 format
  std::string baseName = filename;
  size_t dotPos = baseName.find_last_of('.');
  if (dotPos != std::string::npos) {
    baseName = baseName.substr(0, dotPos);
  }
  if (baseName.length() > 8) {
    baseName = baseName.substr(0, 8);
  }
  actualFilename = baseName + ".SAV";
#else
  actualFilename = filename;
#endif

  std::ofstream file(actualFilename, std::ios::binary);
  if (file.is_open()) {
    file.write(reinterpret_cast<const char *>(&state), sizeof(state));
    file.close();
    std::cout << "Emulator save state written to: " << actualFilename
              << std::endl;
  } else {
    std::cerr << "Error: Could not save state to: " << actualFilename
              << std::endl;
  }
}

bool SMBEmulator::loadState(const std::string &filename) {
  std::string actualFilename;
#ifdef __DJGPP__
  // DOS 8.3 format
  std::string baseName = filename;
  size_t dotPos = baseName.find_last_of('.');
  if (dotPos != std::string::npos) {
    baseName = baseName.substr(0, dotPos);
  }
  if (baseName.length() > 8) {
    baseName = baseName.substr(0, 8);
  }
  actualFilename = baseName + ".SAV";
#else
  actualFilename = filename;
#endif

  std::ifstream file(actualFilename, std::ios::binary);
  if (!file.is_open()) {
    std::cerr << "Error: Could not open save state: " << actualFilename
              << std::endl;
    return false;
  }

  EmulatorSaveState state;
  file.read(reinterpret_cast<char *>(&state), sizeof(state));
  file.close();

  // Validate header
  if (strcmp(state.header, "NESSAVE") != 0) {
    std::cerr << "Error: Invalid save state file" << std::endl;
    return false;
  }

  // Restore CPU state
  regA = state.cpu_A;
  regX = state.cpu_X;
  regY = state.cpu_Y;
  regSP = state.cpu_SP;
  regP = state.cpu_P;
  regPC = state.cpu_PC;
  totalCycles = state.cpu_cycles;

  // Restore RAM
  memcpy(ram, state.ram, sizeof(ram));

  // TODO: Restore PPU and APU state

  std::cout << "Emulator save state loaded from: " << actualFilename
            << std::endl;
  return true;
}

// CHR ROM access for PPU
uint8_t *SMBEmulator::getCHR() { return chrROM; }

uint8_t SMBEmulator::readData(uint16_t address) { return readByte(address); }

void SMBEmulator::writeData(uint16_t address, uint8_t value) {
  writeByte(address, value);
}

// Illegal opcode implementations
void SMBEmulator::ISC(uint16_t addr) {
  // INC + SBC
  uint8_t value = readByte(addr) + 1;
  writeByte(addr, value);

  // Then do SBC
  uint16_t result = regA - value - (getFlag(FLAG_CARRY) ? 0 : 1);
  setFlag(FLAG_CARRY, result <= 0xFF);
  setFlag(FLAG_OVERFLOW, ((regA ^ result) & (~value ^ result) & 0x80) != 0);
  regA = result & 0xFF;
  updateZN(regA);
}

void SMBEmulator::DCP(uint16_t addr) {
  // DEC + CMP
  uint8_t value = readByte(addr) - 1;
  writeByte(addr, value);

  // Then do CMP
  uint8_t result = regA - value;
  setFlag(FLAG_CARRY, regA >= value);
  updateZN(result);
}

void SMBEmulator::LAX(uint16_t addr) {
  // LDA + LDX
  uint8_t value = readByte(addr);
  regA = value;
  regX = value;
  updateZN(regA);
}

void SMBEmulator::SAX(uint16_t addr) {
  // Store A & X
  writeByte(addr, regA & regX);
}

void SMBEmulator::SLO(uint16_t addr) {
  // ASL + ORA
  uint8_t value = readByte(addr);
  setFlag(FLAG_CARRY, (value & 0x80) != 0);
  value <<= 1;
  writeByte(addr, value);
  regA |= value;
  updateZN(regA);
}

void SMBEmulator::KIL() {
  // KIL/JAM/HLT - Halts the CPU
  // In a real NES, this would lock up the system
  // For emulation, we can either:
  // 1. Actually halt (infinite loop)
  // 2. Treat as NOP and continue
  // 3. Reset the system

  // Option 2: Treat as NOP for compatibility
  // This allows games that accidentally hit illegal opcodes to continue
  totalCycles += 2;
  frameCycles += 2;
}

void SMBEmulator::RLA(uint16_t addr) {
  // ROL + AND
  uint8_t value = readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (value & 0x80) != 0);
  value = (value << 1) | (oldCarry ? 1 : 0);
  writeByte(addr, value);
  regA &= value;
  updateZN(regA);
}

void SMBEmulator::SRE(uint16_t addr) {
  // LSR + EOR
  uint8_t value = readByte(addr);
  setFlag(FLAG_CARRY, (value & 0x01) != 0);
  value >>= 1;
  writeByte(addr, value);
  regA ^= value;
  updateZN(regA);
}

void SMBEmulator::RRA(uint16_t addr) {
  // ROR + ADC
  uint8_t value = readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (value & 0x01) != 0);
  value = (value >> 1) | (oldCarry ? 0x80 : 0);
  writeByte(addr, value);

  uint16_t result = regA + value + (getFlag(FLAG_CARRY) ? 1 : 0);
  setFlag(FLAG_CARRY, result > 0xFF);
  setFlag(FLAG_OVERFLOW, ((regA ^ result) & (value ^ result) & 0x80) != 0);
  regA = result & 0xFF;
  updateZN(regA);
}

void SMBEmulator::ANC(uint16_t addr) {
  // AND + set carry to bit 7
  regA &= readByte(addr);
  updateZN(regA);
  setFlag(FLAG_CARRY, (regA & 0x80) != 0);
}

void SMBEmulator::ALR(uint16_t addr) {
  // AND + LSR
  regA &= readByte(addr);
  setFlag(FLAG_CARRY, (regA & 0x01) != 0);
  regA >>= 1;
  updateZN(regA);
}

void SMBEmulator::ARR(uint16_t addr) {
  // AND + ROR
  regA &= readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (regA & 0x01) != 0);
  regA = (regA >> 1) | (oldCarry ? 0x80 : 0);
  updateZN(regA);
  setFlag(FLAG_OVERFLOW, ((regA >> 6) ^ (regA >> 5)) & 1);
}

void SMBEmulator::XAA(uint16_t addr) {
  // Unstable - just do AND
  regA &= readByte(addr);
  updateZN(regA);
}

void SMBEmulator::AXS(uint16_t addr) {
  // (A & X) - immediate
  uint8_t value = readByte(addr);
  uint8_t result = (regA & regX) - value;
  setFlag(FLAG_CARRY, (regA & regX) >= value);
  regX = result;
  updateZN(regX);
}

void SMBEmulator::writeMMC1Register(uint16_t address, uint8_t value) {
    // CRITICAL: Handle reset condition first - this is where your bug was
    if (value & 0x80) {
        // Reset detected - clear shift register and set control register properly
        mmc1.shiftRegister = 0x10;
        mmc1.shiftCount = 0;
        
        // CRITICAL FIX: Reset sets control to (control | 0x0C) - ensures mode 3
        // Mode 3 = 16KB PRG mode with last bank fixed at $C000
        mmc1.control = mmc1.control | 0x0C;
        
        updateMMC1Banks();
        return;
    }

    // Handle 32KB special case (for smaller ROMs)
    if (prgSize == 32768) {
        // Still process the write for CHR banking and control, but ignore PRG banking
        mmc1.shiftRegister >>= 1;
        mmc1.shiftRegister |= (value & 1) << 4;
        mmc1.shiftCount++;

        if (mmc1.shiftCount == 5) {
            uint8_t data = mmc1.shiftRegister;
            mmc1.shiftRegister = 0x10;
            mmc1.shiftCount = 0;

            if (address < 0xA000) {
                mmc1.control = data;
            } else if (address < 0xC000) {
                mmc1.chrBank0 = data;
            } else if (address < 0xE000) {
                mmc1.chrBank1 = data;
            }
            updateMMC1Banks();
        }
        return;
    }
    
    // Normal MMC1 operation for larger ROMs
    mmc1.shiftRegister >>= 1;
    mmc1.shiftRegister |= (value & 1) << 4;
    mmc1.shiftCount++;

    if (mmc1.shiftCount == 5) {
        uint8_t data = mmc1.shiftRegister;
        mmc1.shiftRegister = 0x10;
        mmc1.shiftCount = 0;

        if (address < 0xA000) {
            // Control register write
            uint8_t oldControl = mmc1.control;
            mmc1.control = data;
        } else if (address < 0xC000) {
           // CHR Bank 0
           uint8_t oldBank = mmc1.chrBank0;
           mmc1.chrBank0 = data;
       } else if (address < 0xE000) {
          // CHR Bank 1
          uint8_t oldBank = mmc1.chrBank1;
          mmc1.chrBank1 = data;
       } else {
            // PRG Bank
            uint8_t oldBank = mmc1.prgBank;
            mmc1.prgBank = data;
        }

        updateMMC1Banks();
    }
}

void SMBEmulator::updateMMC1Banks() {
    uint8_t totalPRGBanks = prgSize / 0x4000; // Number of 16KB PRG banks
    
    // Handle 32KB PRG ROMs (no banking needed)
    if (prgSize == 32768) {
        mmc1.currentPRGBank = 0;
        // Still need to handle CHR banking even for 32KB ROMs
    } else {
        // PRG banking logic
        uint8_t prgMode = (mmc1.control >> 2) & 0x03;
        
        switch (prgMode) {
            case 0:
            case 1:
                // 32KB mode - switch entire 32KB (two 16KB banks)
                mmc1.currentPRGBank = (mmc1.prgBank >> 1) % (totalPRGBanks / 2);
                break;
                
            case 2:
                // 16KB mode: Fix FIRST bank at $8000, switch LAST bank at $C000
                mmc1.currentPRGBank = 0;  // First bank fixed at $8000
                break;
                
            case 3:
            default:
                // 16KB mode: Switch FIRST bank at $8000, fix LAST bank at $C000
                mmc1.currentPRGBank = mmc1.prgBank % totalPRGBanks;
                break;
        }
    }

    // CHR banking - CRITICAL FIX FOR TETRIS
    if (nesHeader.chrROMPages > 0) {
        // CHR-ROM banking
        uint8_t totalCHRBanks = chrSize / 0x1000; // 4KB banks for MMC1
        
        if (mmc1.control & 0x10) {
            // 4KB CHR mode - independent 4KB banks
            mmc1.currentCHRBank0 = mmc1.chrBank0 % totalCHRBanks;
            mmc1.currentCHRBank1 = mmc1.chrBank1 % totalCHRBanks;
        } else {
            // 8KB CHR mode - chrBank0 selects 8KB (two consecutive 4KB banks)
            uint8_t baseBank = (mmc1.chrBank0 & 0xFE); // Force even
            mmc1.currentCHRBank0 = baseBank % totalCHRBanks;
            mmc1.currentCHRBank1 = (baseBank + 1) % totalCHRBanks;
        }
        
        // Debug output for CHR banking
        static int chrDebugCount = 0;
        if (chrDebugCount < 10) {
            printf("MMC1 CHR Update: Mode=%s, Bank0=%d->%d, Bank1=%d->%d (Total: %d)\n",
                   (mmc1.control & 0x10) ? "4KB" : "8KB",
                   mmc1.chrBank0, mmc1.currentCHRBank0,
                   mmc1.chrBank1, mmc1.currentCHRBank1,
                   totalCHRBanks);
            chrDebugCount++;
        }
    } else {
        // CHR-RAM - no banking, direct access
        mmc1.currentCHRBank0 = 0;
        mmc1.currentCHRBank1 = 1;
    }
}

void SMBEmulator::writeGxROMRegister(uint16_t address, uint8_t value) {
  uint8_t oldCHRBank = gxrom.chrBank;

  // Mapper 66: Write to $8000-$FFFF sets both PRG and CHR banks
  gxrom.prgBank = (value >> 4) & 0x03; // Bits 4-5
  gxrom.chrBank = value & 0x03;        // Bits 0-1

  // Invalidate cache if CHR bank changed
  /*if (oldCHRBank != gxrom.chrBank) {
      ppu->invalidateTileCache();
  }*/
}

uint8_t SMBEmulator::readCHRData(uint16_t address) {
  if (address >= 0x2000)
    return 0;

  // Handle CHR reads based on mapper type
  switch (nesHeader.mapper) {
  case 0: // NROM
  {
    if (nesHeader.chrROMPages == 0) {
      // NROM with CHR-RAM - direct access
      if (address < chrSize) {
        return chrROM[address]; // chrROM is actually CHR-RAM
      }
    } else {
      // NROM with CHR-ROM - direct access, no banking
      if (address < chrSize) {
        return chrROM[address];
      }
    }
    return 0;
  }

  case 1: // MMC1 - FIXED IMPLEMENTATION
  {
    if (nesHeader.chrROMPages == 0) {
      // CHR-RAM - direct access, no banking
      if (address < chrSize) {
        return chrROM[address];
      }
    } else {
      // CHR-ROM with banking - THIS IS THE CRITICAL FIX
      uint32_t chrAddr;
      uint8_t totalCHRBanks = chrSize / 0x1000; // 4KB banks for MMC1
      
      if (mmc1.control & 0x10) {
        // 4KB CHR mode - independent 4KB banks
        if (address < 0x1000) {
          // $0000-$0FFF: Use currentCHRBank0
          uint8_t bank = mmc1.currentCHRBank0 % totalCHRBanks;
          chrAddr = (bank * 0x1000) + address;
        } else {
          // $1000-$1FFF: Use currentCHRBank1
          uint8_t bank = mmc1.currentCHRBank1 % totalCHRBanks;
          chrAddr = (bank * 0x1000) + (address - 0x1000);
        }
      } else {
        // 8KB CHR mode - chrBank0 selects both 4KB banks
        uint8_t baseBank = (mmc1.currentCHRBank0 & 0xFE) % totalCHRBanks; // Force even
        if (address < 0x1000) {
          chrAddr = (baseBank * 0x1000) + address;
        } else {
          chrAddr = ((baseBank + 1) % totalCHRBanks * 0x1000) + (address - 0x1000);
        }
      }
      
      if (chrAddr < chrSize) {
        return chrROM[chrAddr];
      }
    }
    return 0;
  }
  case 2: // UxROM
  {
    // UxROM always uses CHR-RAM - direct access, no banking
    if (address < chrSize) {
      return chrROM[address]; // chrROM is actually CHR-RAM
    }
    return 0;
  }

  case 3: // CNROM
  {
    if (nesHeader.chrROMPages == 0) {
      // CNROM with CHR-RAM - direct access
      if (address < chrSize) {
        return chrROM[address];
      }
    } else {
      // CNROM with CHR-ROM - 8KB banking
      uint32_t chrAddr = (cnrom.chrBank * 0x2000) + address;
      if (chrAddr < chrSize) {
        return chrROM[chrAddr];
      }
    }
    return 0;
  }

    case 4: // MMC3
    {
        if (nesHeader.chrROMPages == 0) {
            // CHR-RAM - direct access
            if (address < chrSize) {
                return chrROM[address];
            }
        } else {
            // CHR-ROM with banking
            uint8_t bankIndex = address / 0x400; // Which 1KB bank (0-7)
            uint16_t bankOffset = address % 0x400; // Offset within bank
            
            if (bankIndex < 8) {
                uint8_t physicalBank = mmc3.currentCHRBanks[bankIndex];
                uint32_t chrAddr = (physicalBank * 0x400) + bankOffset;
                
                // Bounds check with debug info
                if (chrAddr >= chrSize) {
                    static int oobCount = 0;
                    if (oobCount < 5) {
                        printf("CHR OOB: addr=$%04X, bank=%d, physical=%d, chrAddr=$%08X, size=$%08X\n",
                               address, bankIndex, physicalBank, chrAddr, chrSize);
                        oobCount++;
                    }
                    return 0;
                }
                
                return chrROM[chrAddr];
            }
        }
  }
  case 66: // GxROM
  {
    if (nesHeader.chrROMPages == 0) {
      // GxROM with CHR-RAM - direct access
      if (address < chrSize) {
        return chrROM[address];
      }
    } else {
      // GxROM with CHR-ROM - 8KB banking
      uint32_t chrAddr = (gxrom.chrBank * 0x2000) + address;
      if (chrAddr < chrSize) {
        return chrROM[chrAddr];
      }
    }
    return 0;
  }

  case 7: // AxROM
  {
    // AxROM uses CHR-RAM - direct access, no banking
    if (address < chrSize) {
      return chrROM[address]; // chrROM is actually CHR-RAM
    }
    return 0;
  }

case 9: // MMC2
{
    if (nesHeader.chrROMPages == 0) {
        // MMC2 with CHR-RAM - shouldn't happen normally
        if (address < chrSize) {
            return chrROM[address];
        }
    } else {
        // MMC2 with CHR-ROM - 4KB banking with latch switching
        uint32_t chrAddr;
        
        if (address < 0x1000) {
            // $0000-$0FFF: Uses currentCHRBank0 (4KB bank)
            chrAddr = (mmc2.currentCHRBank0 * 0x1000) + address;
        } else {
            // $1000-$1FFF: Uses currentCHRBank1 (4KB bank)  
            chrAddr = (mmc2.currentCHRBank1 * 0x1000) + (address - 0x1000);
        }
        
        if (chrAddr < chrSize) {
            uint8_t result = chrROM[chrAddr];
            
            // CRITICAL: Check for latch switching based on tile address within bank
            // The latch switches when tiles $FD or $FE are read (last 16 bytes of pattern data)
            uint16_t tileIndex = (address % 0x1000) / 16;  // Which tile (0-255)
            if (tileIndex == 0xFD || tileIndex == 0xFE) {
                checkMMC2CHRLatch(address, tileIndex);
            }
            
            return result;
        }
    }
    return 0;
}
  case 10: // MMC4
  {
    if (nesHeader.chrROMPages == 0) {
      // MMC2/4 with CHR-RAM - direct access
      if (address < chrSize) {
        return chrROM[address];
      }
    } else {
      // MMC2/4 with CHR-ROM - complex banking (simplified here)
      // For now, treat as direct access - full implementation would need
      // sprite 0 hit detection and banking state tracking
      if (address < chrSize) {
        return chrROM[address];
      }
    }
    return 0;
  }

  case 11: // Color Dreams
  {
    if (nesHeader.chrROMPages == 0) {
      // Color Dreams with CHR-RAM - direct access
      if (address < chrSize) {
        return chrROM[address];
      }
    } else {
      // Color Dreams with CHR-ROM - banking (implementation depends on variant)
      if (address < chrSize) {
        return chrROM[address]; // Simplified - no banking for now
      }
    }
    return 0;
  }

  case 13: // CPROM
  {
    // CPROM uses CHR-RAM with banking
    if (address < chrSize) {
      return chrROM[address]; // Direct access for now
    }
    return 0;
  }

  case 28: // Action 53
  case 30: // UNROM 512
  {
    // Modern homebrew mappers - usually CHR-RAM
    if (address < chrSize) {
      return chrROM[address]; // Direct access
    }
    return 0;
  }

  default: {
    // Unknown mapper - handle gracefully
    if (nesHeader.chrROMPages == 0) {
      // Assume CHR-RAM with direct access
      if (address < chrSize) {
        return chrROM[address];
      }
    } else {
      // Assume CHR-ROM with direct access (no banking)
      if (address < chrSize) {
        return chrROM[address];
      }
    }

    // Debug unknown mapper access
    static int unknownMapperReadCount = 0;
    if (unknownMapperReadCount < 3) {
      printf("Unknown mapper %d CHR read: $%04X\n", nesHeader.mapper, address);
      unknownMapperReadCount++;
    }

    return 0;
  }
  }
}

uint8_t SMBEmulator::readCHRDataFromBank(uint16_t address, uint8_t bank) {
  if (address >= 0x2000)
    return 0;

  // Calculate the address within the specified bank
  uint32_t chrAddr;

  if (nesHeader.mapper == 66) {
    // GxROM uses 8KB CHR banks
    chrAddr = (bank * 0x2000) + address;
  } else if (nesHeader.mapper == 4) {
    // MMC3 uses 1KB CHR banks
    chrAddr = (bank * 0x400) + (address % 0x400);
  } else if (nesHeader.mapper == 1) {
    // MMC1 uses 4KB CHR banks
    chrAddr = (bank * 0x1000) + (address % 0x1000);
  } else if (nesHeader.mapper == 3) {
    // CNROM uses 8KB CHR banks
    chrAddr = (bank * 0x2000) + address;
  } else if (nesHeader.mapper == 2) {
    // UxROM uses CHR-RAM (no banking) - ignore bank parameter
    chrAddr = address;
  } else {
    // Mapper 0 (NROM) - no banking
    chrAddr = address;
  }

  // Bounds check
  if (chrAddr < chrSize) {
    return chrROM[chrAddr];
  }

  return 0;
}

void SMBEmulator::writeUxROMRegister(uint16_t address, uint8_t value) {
  // UxROM: Any write to $8000-$FFFF sets the PRG bank
  // Only the lower bits are used (depends on ROM size)
  uint8_t totalBanks = prgSize / 0x4000; // Number of 16KB banks
  uint8_t bankMask = totalBanks - 1;     // Create mask for valid banks

  uxrom.prgBank = value & bankMask;

  // Debug output
  // printf("UxROM: Set PRG bank to %d (total banks: %d)\n", uxrom.prgBank,
  // totalBanks);
}

void SMBEmulator::enableZapper(bool enable) {
  zapperEnabled = enable;
  if (enable) {
    std::cout << "NES Zapper enabled" << std::endl;
  }
}

void SMBEmulator::updateZapperInput(int mouseX, int mouseY, bool mousePressed) {
  if (!zapperEnabled)
    return;

  // Scale mouse coordinates from screen to NES resolution (256x240)
  // This will need to be adjusted based on actual screen scaling
  zapper->setMousePosition(mouseX, mouseY);
  zapper->setTriggerPressed(mousePressed);

  // Perform light detection on current frame
  if (mousePressed && currentFrameBuffer) {
    bool lightDetected =
        zapper->detectLight(currentFrameBuffer, 256, 240, mouseX, mouseY);
    zapper->setLightDetected(lightDetected);
  } else {
    zapper->setLightDetected(false);
  }
}

void SMBEmulator::writeMMC2Register(uint16_t address, uint8_t value) {
    switch (address & 0xF000) {
        case 0xA000:
            // PRG ROM bank select ($A000-$AFFF)
            mmc2.prgBank = value & 0x0F;  // 4 bits for PRG bank
            break;
            
        case 0xB000:
            // CHR ROM $FD/0000 bank select ($B000-$BFFF)
            mmc2.chrBank0FD = value & 0x1F;  // 5 bits for CHR bank
            updateMMC2Banks();
            break;
            
        case 0xC000:
            // CHR ROM $FE/0000 bank select ($C000-$CFFF)
            mmc2.chrBank0FE = value & 0x1F;  // 5 bits for CHR bank
            updateMMC2Banks();
            break;
            
        case 0xD000:
            // CHR ROM $FD/1000 bank select ($D000-$DFFF)
            mmc2.chrBank1FD = value & 0x1F;  // 5 bits for CHR bank
            updateMMC2Banks();
            break;
            
        case 0xE000:
            // CHR ROM $FE/1000 bank select ($E000-$EFFF)
            mmc2.chrBank1FE = value & 0x1F;  // 5 bits for CHR bank
            updateMMC2Banks();
            break;
            
        case 0xF000:
            // Mirroring ($F000-$FFFF)
            mmc2.mirroring = value & 0x01;  // 0=vertical, 1=horizontal
            // Update PPU mirroring if you have that implemented
            break;
    }
}

void SMBEmulator::updateMMC2Banks() {
    // Update current CHR banks based on latch states
    uint8_t oldBank0 = mmc2.currentCHRBank0;
    uint8_t oldBank1 = mmc2.currentCHRBank1;
    
    mmc2.currentCHRBank0 = mmc2.latch0 ? mmc2.chrBank0FE : mmc2.chrBank0FD;
    mmc2.currentCHRBank1 = mmc2.latch1 ? mmc2.chrBank1FE : mmc2.chrBank1FD;
    
    // Bounds checking
    uint8_t totalCHRBanks = chrSize / 0x1000; // 4KB banks
    mmc2.currentCHRBank0 %= totalCHRBanks;
    mmc2.currentCHRBank1 %= totalCHRBanks;
    
    static int bankSwitchCount = 0;
    if ((oldBank0 != mmc2.currentCHRBank0 || oldBank1 != mmc2.currentCHRBank1) && bankSwitchCount < 10) {
        printf("MMC2 Bank Switch: Bank0 %d->%d, Bank1 %d->%d\n",
               oldBank0, mmc2.currentCHRBank0, oldBank1, mmc2.currentCHRBank1);
        bankSwitchCount++;

    }
}

void SMBEmulator::checkMMC2CHRLatch(uint16_t address, uint8_t tileID) {
    static int latchSwitchCount = 0;
    
    if (address < 0x1000) {
        // Pattern table 0 ($0000-$0FFF)
        if (tileID == 0xFD) {
            if (mmc2.latch0 != false) {
                mmc2.latch0 = false;
                updateMMC2Banks();
                if (latchSwitchCount < 20) {
                    printf("*** MMC2: Latch 0 -> FD (bank %d) at $%04X ***\n", 
                           mmc2.currentCHRBank0, address);
                    latchSwitchCount++;
                }
            }
        } else if (tileID == 0xFE) {
            if (mmc2.latch0 != true) {
                mmc2.latch0 = true;
                updateMMC2Banks();
                if (latchSwitchCount < 20) {
                    printf("*** MMC2: Latch 0 -> FE (bank %d) at $%04X ***\n", 
                           mmc2.currentCHRBank0, address);
                    latchSwitchCount++;
                }
            }
        }
    } else if (address < 0x2000) {
        // Pattern table 1 ($1000-$1FFF)
        if (tileID == 0xFD) {
            if (mmc2.latch1 != false) {
                mmc2.latch1 = false;
                updateMMC2Banks();
                if (latchSwitchCount < 20) {
                    printf("*** MMC2: Latch 1 -> FD (bank %d) at $%04X ***\n", 
                           mmc2.currentCHRBank1, address);
                    latchSwitchCount++;
                }
            }
        } else if (tileID == 0xFE) {
            if (mmc2.latch1 != true) {
                mmc2.latch1 = true;
                updateMMC2Banks();
                if (latchSwitchCount < 20) {
                    printf("*** MMC2: Latch 1 -> FE (bank %d) at $%04X ***\n", 
                           mmc2.currentCHRBank1, address);
                    latchSwitchCount++;
                }
            }
        }
    }
}
