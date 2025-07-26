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

void WarpNES::writeMMC1Register(uint16_t address, uint8_t value) {
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
    // Still process the write for CHR banking and control, but ignore PRG
    // banking
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

void WarpNES::updateMMC1Banks() {
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
      mmc1.currentPRGBank = 0; // First bank fixed at $8000
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

  } else {
    // CHR-RAM - no banking, direct access
    mmc1.currentCHRBank0 = 0;
    mmc1.currentCHRBank1 = 1;
  }
}
