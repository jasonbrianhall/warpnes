#include "../Emulation/PPU.hpp"
#include <fstream>
#include <iostream>
#include <string>
#include "WarpNES.hpp"
#include "../Configuration.hpp"
#include "../Emulation/APU.hpp"

#ifdef ALLEGRO_BUILD
#include "../Emulation/Controller.hpp"
#else
#include "../Emulation/ControllerSDL.hpp"
#endif

#include "../Zapper.hpp"


// 6502 instruction cycle counts
const uint8_t WarpNES::instructionCycles[256] = {
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

WarpNES::WarpNES()
    : regA(0), regX(0), regY(0), regSP(0xFF), regPC(0), regP(0x24),
      totalCycles(0), frameCycles(0), prgROM(nullptr), chrROM(nullptr),
      prgSize(0), chrSize(0), romLoaded(false), masterCycles(0), ppuCycles(0),
      nmiPending(false), sram(nullptr), sramSize(0), sramEnabled(false),
      sramDirty(false) {
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

WarpNES::~WarpNES() {
  delete apu;
  delete ppu;

  delete controller1;
  delete controller2;

  delete zapper;
  unloadROM();
  cleanupSRAM();
}

void WarpNES::writeCNROMRegister(uint16_t address, uint8_t value) {
  uint8_t oldCHRBank = cnrom.chrBank;

  // Mapper 3: Write to $8000-$FFFF sets CHR bank
  cnrom.chrBank = value & 0x03; // Only 2 bits for CHR bank

  // Invalidate cache if CHR bank changed
  /*if (oldCHRBank != cnrom.chrBank) {
      ppu->invalidateTileCache();
  }*/
}

void WarpNES::writeCHRData(uint16_t address, uint8_t value) {
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
bool WarpNES::loadROM(const std::string &filename) {
  std::ifstream file(filename, std::ios::binary);
  if (!file.is_open()) {
    std::cerr << "Failed to open ROM file: " << filename << std::endl;
    return false;
  }

  // Extract base filename for save files
  size_t lastSlash = filename.find_last_of("/\\");
  size_t lastDot = filename.find_last_of('.');
  if (lastSlash == std::string::npos)
    lastSlash = 0;
  else
    lastSlash++;
  if (lastDot == std::string::npos || lastDot < lastSlash)
    lastDot = filename.length();

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

void WarpNES::unloadROM() {
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

bool WarpNES::parseNESHeader(std::ifstream &file) {
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

bool WarpNES::loadPRGROM(std::ifstream &file) {
  prgSize = nesHeader.prgROMPages * 16384; // 16KB pages
  if (prgSize == 0)
    return false;

  prgROM = new uint8_t[prgSize];
  file.read(reinterpret_cast<char *>(prgROM), prgSize);

  return file.good();
}

bool WarpNES::loadCHRROM(std::ifstream &file) {
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

void WarpNES::handleNMI() {
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

void WarpNES::update() {
  if (!romLoaded)
    return;
  updateCycleAccurate();
}

void WarpNES::updateFrameBased() {
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

void WarpNES::updateCycleAccurate() {
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
  const int CPU_DIVIDER = 3; // CPU runs every 3 PPU cycles

  // Frame timing state
  bool frameEven = (totalCycles / (TOTAL_SCANLINES * CYCLES_PER_SCANLINE / CPU_DIVIDER)) % 2 == 0;
  int cpuCycleDebt = 0;

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
      cyclesThisScanline = 340; // Skip cycle 340 on odd frames when rendering
    }

    for (int cycle = 0; cycle < cyclesThisScanline; cycle++) {
      ppuCycleState.cycle = cycle;

      // VBlank flag set on cycle 1 of scanline 241
      if (scanline == VBLANK_START_SCANLINE && cycle == 1) {
        ppu->setVBlankFlag(true);
        ppu->captureFrameScroll();

        // Fire NMI immediately if enabled
        if (ppu->getControl() & 0x80) {
          handleNMI();
          cpuCycleDebt += 7; // Account for NMI cycles
        }
      }

      // VBlank flag cleared on cycle 1 of pre-render scanline (261)
      if (scanline == 261 && cycle == 1) {
        ppu->setVBlankFlag(false);
      }

      // Sprite 0 hit detection
      if (scanline >= 0 && scanline < VISIBLE_SCANLINES && ppuCycleState.renderingEnabled) {
        checkSprite0Hit(scanline, cycle);
      }

      // Step PPU cycle
      ppu->stepCycle(scanline, cycle);

      // CRITICAL: MMC3 IRQ handling with proper timing
      if (nesHeader.mapper == 4) {
        checkMMC3IRQ(scanline, cycle);
      }

      // CPU execution with proper cycle accounting
      cpuCycleDebt++;

      if (cpuCycleDebt >= CPU_DIVIDER) {
        cpuCycleDebt -= CPU_DIVIDER;

        // Execute CPU instruction
        uint64_t cyclesBefore = totalCycles;
        executeInstruction();
        uint64_t cyclesUsed = totalCycles - cyclesBefore;

        // Account for multi-cycle instructions
        if (cyclesUsed > 1) {
          cpuCycleDebt += (cyclesUsed - 1) * CPU_DIVIDER;
        }

        masterCycles = totalCycles;
      }
    }
  }

  // End of frame cleanup
  ppu->setVBlankFlag(false);
  ppu->setSprite0Hit(false);

  // Audio frame advance
  if (Configuration::getAudioEnabled()) {
    apu->stepFrame();
  }
}

bool WarpNES::isPixelBright(uint16_t pixelColor) {
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

void WarpNES::checkSprite0Hit(int scanline, int cycle) {
  if (ppu->getStatus() & 0x40)
    return; // Already hit
  if (!ppuCycleState.renderingEnabled)
    return; // Rendering disabled
  if (scanline >= 240)
    return; // Not visible scanline

  uint8_t sprite0_y = ppu->getOAM()[0];
  uint8_t sprite0_x = ppu->getOAM()[3];

  sprite0_y++; // Sprite delay

  // Simple collision detection - if we're in sprite 0's area
  if (scanline >= sprite0_y && scanline < sprite0_y + 8) {
    if (cycle >= sprite0_x && cycle < sprite0_x + 8) {
      // WE GOT A HIT!
      ppu->setSprite0Hit(true);
      // printf("*** SPRITE 0 HIT DETECTED *** at scanline %d, cycle %d (sprite
      // at x=%d, y=%d)\n",  scanline, cycle, sprite0_x, sprite0_y-1);
    }
  }
}

void WarpNES::reset() {
  if (!romLoaded)
    return;

  // Reset CPU state
  regA = regX = regY = 0;
  regSP = 0xFF;
  regP = 0x24;
  totalCycles = frameCycles = 0;
  if (nesHeader.mapper == 1) {
    // Reset MMC1 state properly
    mmc1 = MMC1State(); // Reset to default constructor state

    // CRITICAL: Force proper reset state according to MMC1 spec
    mmc1.control = 0x0C; // Mode 3: 16KB PRG mode, last bank fixed at $C000
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

void WarpNES::checkCHRLatch(uint16_t address, uint8_t tileID) {
  if (nesHeader.mapper == 9) {
    checkMMC2CHRLatch(address, tileID);
  }
}

void WarpNES::step() {
  if (!romLoaded)
    return;
  executeInstruction();
}

void WarpNES::stepPPUCycle() {
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

void WarpNES::stepPPUFetchNametable() {
  // Nametable fetch - doesn't usually affect mappers
  // But MMC3 A12 line changes can happen here
  if (nesHeader.mapper == 4) {
    // Check for A12 transitions during nametable access
    stepMMC3A12Transition(false); // Nametable access
  }
}

void WarpNES::stepPPUFetchAttribute() {
  // Attribute fetch - similar to nametable
  if (nesHeader.mapper == 4) {
    stepMMC3A12Transition(false);
  }
}

void WarpNES::stepPPUFetchPatternLow() {
  // Pattern table fetch - this is where CHR banking matters!
  if (nesHeader.mapper == 4) {
    stepMMC3A12Transition(true); // Pattern table access (A12 high)
  }

  // For other mappers, CHR data is fetched here
  // This is where CHR-RAM updates would be visible
}

void WarpNES::stepPPUFetchPatternHigh() {
  // Second pattern table fetch
  if (nesHeader.mapper == 4) {
    stepMMC3A12Transition(true);
  }
}

void WarpNES::stepPPUSpriteEvaluation() {
  // Sprite evaluation logic
  // This affects sprite 0 hit detection
}

void WarpNES::stepPPUEndOfScanline(int scanline) {
  // End-of-scanline events
  // Scroll register updates, etc.
}

void WarpNES::executeInstruction() {

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

void WarpNES::catchUpPPU() {
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

void WarpNES::checkPendingInterrupts() {
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

    }
  }

  // Handle NMI
  if (nmiPending) {
    handleNMI();
    nmiPending = false;
  }
}

uint8_t WarpNES::readByte(uint16_t address) {
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
    return 0; // Open bus if SRAM not available
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
      uint32_t romAddr =
          (mmc3.currentPRGBanks[bankIndex] * 0x2000) + bankOffset;

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
        uint8_t totalBanks = prgSize / 0x2000; // 8KB banks
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

void WarpNES::writeByte(uint16_t address, uint8_t value) {
  if (address < 0x2000) {
    ram[address & 0x7FF] = value;
  } else if (address < 0x4000) {
    // PPU registers
    ppu->writeRegister(0x2000 + (address & 0x7), value);
  } else if (address < 0x4020) {
    switch (address) {
    case 0x4014:
      ppu->writeDMA(value);
      masterCycles += 513; // DMA cycles
      break;

    case 0x4016:
      controller1->writeByte(value);
      controller2->writeByte(value);
      break;

    default:
      apu->writeRegister(address, value);
      break;
    }
  } else if (address >= 0x6000 && address < 0x8000) {
    // SRAM area ($6000-$7FFF)
    if (sramEnabled && sram && nesHeader.battery) {
      uint16_t sramAddr = address - 0x6000;
      if (sramAddr < sramSize) {
        sram[sramAddr] = value;
        sramDirty = true; // Mark SRAM as needing to be saved
                          // No auto-save - player controls when to save
      }
    }
  } else if (address >= 0x8000) {
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

void WarpNES::scaleBuffer16(uint16_t *nesBuffer, uint16_t *screenBuffer,
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

void WarpNES::render16(uint16_t *buffer) {
  // ppu->render(buffer);
  ppu->render16(buffer);
}

void WarpNES::render(uint32_t *buffer) { ppu->render(buffer); }

void WarpNES::renderScaled16(uint16_t *buffer, int screenWidth,
                                 int screenHeight) {
  // First render the game using PPU scaling
  static uint16_t nesBuffer[256 * 240];
  // ppu->getFrameBuffer(nesBuffer);
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

// Audio functions (delegate to APU)
void WarpNES::audioCallback(uint8_t *stream, int length) {
  apu->output(stream, length);
}

void WarpNES::toggleAudioMode() { apu->toggleAudioMode(); }

bool WarpNES::isUsingMIDIAudio() const { return apu->isUsingMIDI(); }

void WarpNES::debugAudioChannels() { apu->debugAudio(); }

// Controller access
Controller &WarpNES::getController1() { return *controller1; }

Controller &WarpNES::getController2() { return *controller2; }

// CPU state access
WarpNES::CPUState WarpNES::getCPUState() const {
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

uint8_t WarpNES::readMemory(uint16_t address) const {
  return const_cast<WarpNES *>(this)->readByte(address);
}

void WarpNES::writeMemory(uint16_t address, uint8_t value) {
  writeByte(address, value);
}

// Save states
void WarpNES::saveState(const std::string &filename) {
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

bool WarpNES::loadState(const std::string &filename) {
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
uint8_t *WarpNES::getCHR() { return chrROM; }

uint8_t WarpNES::readData(uint16_t address) { return readByte(address); }

void WarpNES::writeData(uint16_t address, uint8_t value) {
  writeByte(address, value);
}

uint8_t WarpNES::readCHRData(uint16_t address) {
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
        uint8_t baseBank =
            (mmc1.currentCHRBank0 & 0xFE) % totalCHRBanks; // Force even
        if (address < 0x1000) {
          chrAddr = (baseBank * 0x1000) + address;
        } else {
          chrAddr =
              ((baseBank + 1) % totalCHRBanks * 0x1000) + (address - 0x1000);
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
      uint8_t bankIndex = address / 0x400;   // Which 1KB bank (0-7)
      uint16_t bankOffset = address % 0x400; // Offset within bank

      if (bankIndex < 8) {
        uint8_t physicalBank = mmc3.currentCHRBanks[bankIndex];
        uint32_t chrAddr = (physicalBank * 0x400) + bankOffset;

        // Bounds check with debug info
        if (chrAddr >= chrSize) {
          static int oobCount = 0;
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
      if (address < chrSize) {
        return chrROM[address];
      }
    } else {
      uint32_t chrAddr;

      if (address < 0x1000) {
        chrAddr = (mmc2.currentCHRBank0 * 0x1000) + address;
      } else {
        chrAddr = (mmc2.currentCHRBank1 * 0x1000) + (address - 0x1000);
      }

      if (chrAddr >= chrSize) {
        return 0;
      }

      uint8_t data = chrROM[chrAddr];
      return data;
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

    return 0;
  }
  }
}

uint8_t WarpNES::readCHRDataFromBank(uint16_t address, uint8_t bank) {
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

void WarpNES::enableZapper(bool enable) {
  zapperEnabled = enable;
  if (enable) {
    std::cout << "NES Zapper enabled" << std::endl;
  }
}

void WarpNES::updateZapperInput(int mouseX, int mouseY, bool mousePressed) {
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

void WarpNES::handlePPUCHRRead(uint16_t address) {
  if (nesHeader.mapper == 9) {
    // Only check latch on actual PPU pattern table reads
    if (address < 0x2000) {
      checkMMC2CHRLatch(address, 0); // Keep tileID parameter for compatibility
    }
  }
}
