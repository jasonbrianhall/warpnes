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

void WarpNES::updateMMC2Banks() {

  uint8_t totalCHRBanks = chrSize / 0x1000; // 4KB banks (0x1000 = 4096)

  // Update current banks based on latch states
  mmc2.currentCHRBank0 =
      (mmc2.latch0 ? mmc2.chrBank0FE : mmc2.chrBank0FD) % totalCHRBanks;
  mmc2.currentCHRBank1 =
      (mmc2.latch1 ? mmc2.chrBank1FE : mmc2.chrBank1FD) % totalCHRBanks;

}

void WarpNES::writeMMC2Register(uint16_t address, uint8_t value) {
  switch (address & 0xF000) {
  case 0xA000:
    // PRG ROM bank select ($A000-$AFFF)
    mmc2.prgBank = value & 0x0F; // 4 bits for PRG bank
    break;

  case 0xB000:
    // CHR ROM $FD/0000 bank select ($B000-$BFFF)
    mmc2.chrBank0FD = value & 0x1F; // 5 bits for CHR bank
    updateMMC2Banks();
    break;

  case 0xC000:
    // CHR ROM $FE/0000 bank select ($C000-$CFFF)
    mmc2.chrBank0FE = value & 0x1F; // 5 bits for CHR bank
    updateMMC2Banks();
    break;

  case 0xD000:
    // CHR ROM $FD/1000 bank select ($D000-$DFFF)
    mmc2.chrBank1FD = value & 0x1F; // 5 bits for CHR bank
    updateMMC2Banks();
    break;

  case 0xE000:
    // CHR ROM $FE/1000 bank select ($E000-$EFFF)
    mmc2.chrBank1FE = value & 0x1F; // 5 bits for CHR bank
    updateMMC2Banks();
    break;

  case 0xF000:
    // Mirroring ($F000-$FFFF)
    mmc2.mirroring = value & 0x01; // 0=vertical, 1=horizontal
    // Update PPU mirroring if you have that implemented
    break;
  }
}

void WarpNES::checkMMC2CHRLatch(uint16_t address, uint8_t tileID) {
  static int latchSwitchCount = 0;

  // From FCEUX: Check if address corresponds to tiles $FD or $FE
  uint8_t h = address >> 8;
  uint8_t l = address & 0xF0;

  // Must be in pattern table area and accessing tile data
  if (h >= 0x20 || ((h & 0xF) != 0xF))
    return;

  if (h < 0x10) {
    // Pattern table 0 ($0000-$0FFF)
    if (l == 0xD0) {
      // Accessing tile $FD
      if (mmc2.latch0 != false) {
        mmc2.latch0 = false;
        updateMMC2Banks();
      }
    } else if (l == 0xE0) {
      // Accessing tile $FE
      if (mmc2.latch0 != true) {
        mmc2.latch0 = true;
        updateMMC2Banks();
      }
    }
  } else {
    // Pattern table 1 ($1000-$1FFF)
    if (l == 0xD0) {
      // Accessing tile $FD
      if (mmc2.latch1 != false) {
        mmc2.latch1 = false;
        updateMMC2Banks();
      }
    } else if (l == 0xE0) {
      // Accessing tile $FE
      if (mmc2.latch1 != true) {
        mmc2.latch1 = true;
        updateMMC2Banks();
      }
    }
  }
}
