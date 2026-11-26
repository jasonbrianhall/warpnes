#include "WarpNES.hpp"
#include "../Configuration.hpp"
#include "APU.hpp"
#ifdef ALLEGRO_BUILD
#include "Controller.hpp"
#else
#include "ControllerSDL.hpp"
#endif 
#include "PPU.hpp"
#include "../Zapper.hpp"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

void WarpNES::initializeSRAM() {
  cleanupSRAM();  // Clean up any existing SRAM
  
  // Always provide 8KB of PRG RAM at $6000-$7FFF. Many boards have work RAM
  // without a battery (e.g. Super Mario Bros. 3); only battery-backed RAM is
  // loaded from / saved to disk (see loadSRAM/saveSRAM).
  // MMC5 boards carry up to 64KB of PRG RAM (banked via $5113-$5116)
  sramSize = (nesHeader.mapper == 5) ? 65536 : 8192;
  sram = new uint8_t[sramSize];
  memset(sram, 0, sramSize);  // Initialize to zero
  sramEnabled = true;
  sramDirty = false;
  
  if (nesHeader.battery) {
    printf("SRAM: Initialized %d bytes for battery save\n", sramSize);
  }
  
  // Load existing save if it exists
  loadSRAM();
}

void WarpNES::loadSRAM() {
  if (!sram || !nesHeader.battery || romBaseName.empty()) {
    return;
  }
  
  // Create battery save filename with .srm extension
  std::string saveFilename = romBaseName + ".srm";
  
  std::ifstream file(saveFilename, std::ios::binary);
  if (!file.is_open()) {
    printf("SRAM: No existing battery save found (%s)\n", saveFilename.c_str());
    return;
  }
  
  // Read SRAM data
  file.read(reinterpret_cast<char*>(sram), sramSize);
  file.close();
  
  printf("SRAM: Loaded battery save from %s\n", saveFilename.c_str());
}

void WarpNES::saveSRAM() {
  if (!sram || !nesHeader.battery || !sramDirty || romBaseName.empty()) {
    return;
  }
  
  // Create battery save filename with .srm extension
  std::string saveFilename = romBaseName + ".srm";
  
  std::ofstream file(saveFilename, std::ios::binary);
  if (!file.is_open()) {
    printf("SRAM: Error - Could not save battery data to %s\n", saveFilename.c_str());
    return;
  }
  
  // Write SRAM data
  file.write(reinterpret_cast<const char*>(sram), sramSize);
  file.close();
  
  printf("SRAM: Saved battery data to %s\n", saveFilename.c_str());
  sramDirty = false;
}

void WarpNES::cleanupSRAM() {
  if (sram) {
    delete[] sram;
    sram = nullptr;
  }
  sramSize = 0;
  sramEnabled = false;
  sramDirty = false;
}

// Add public method to manually save (good for clean shutdown):
void WarpNES::forceSRAMSave() {
    if (sramDirty && nesHeader.battery) {
        saveSRAM();
    }
}
