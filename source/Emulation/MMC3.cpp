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

void WarpNES::checkMMC3IRQ(int scanline, int cycle) {
  if (nesHeader.mapper != 4) return;
  if (!ppuCycleState.renderingEnabled) return;
  if (scanline >= 240 && scanline < 261) return; // Skip VBlank

  // CRITICAL FIX: More accurate A12 detection
  bool a12High = false;
  
  // A12 goes high during pattern table fetches
  if ((cycle >= 1 && cycle <= 256) || (cycle >= 321 && cycle <= 336)) {
    int fetchCycle = cycle % 8;
    
    // Pattern table fetches happen on cycles 5 and 7 of each 8-cycle group
    if (fetchCycle == 5 || fetchCycle == 7) {
      uint8_t ppuCtrl = ppu->getControl();
      
      if (fetchCycle == 5) {
        // Background pattern fetch - A12 based on PPUCTRL bit 4
        a12High = (ppuCtrl & 0x10) != 0;
      } else if (fetchCycle == 7) {
        // Could be background or sprite pattern fetch
        // For background: A12 based on PPUCTRL bit 4
        // For sprites: A12 based on PPUCTRL bit 3
        a12High = (ppuCtrl & 0x10) != 0; // Simplified - use background
      }
    }
  }
  
  // Sprite pattern fetches during sprite loading
  if (cycle >= 257 && cycle <= 320) {
    uint8_t ppuCtrl = ppu->getControl();
    a12High = (ppuCtrl & 0x08) != 0; // Sprite pattern table selection
  }

  // Call the A12 transition detector
  stepMMC3A12Transition(a12High);
}

void WarpNES::stepMMC3A12Transition(bool a12High) {
  static bool lastA12 = false;
  static int filterCounter = 0;

  // CRITICAL FIX: Proper edge detection with filtering
  if (a12High != lastA12) {
    filterCounter++;
    if (filterCounter >= 1) { // Reduced filter requirement
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

void WarpNES::writeMMC3Register(uint16_t address, uint8_t value) {
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
    break;

  case 0xC001: // IRQ reload
    mmc3.irqReload = true;
    break;

  case 0xE000: // IRQ disable
    mmc3.irqEnable = false;
    mmc3.irqPending = false; // Clear any pending IRQ
    break;

  case 0xE001: // IRQ enable
    mmc3.irqEnable = true;
    break;
  }
}

void WarpNES::updateMMC3Banks() {
  uint8_t totalPRGBanks = prgSize / 0x2000; // Number of 8KB banks
  uint8_t totalCHRBanks = chrSize / 0x400;  // Number of 1KB banks

  // PRG banking - ensure last two banks are properly fixed
  bool prgSwap = (mmc3.bankSelect & 0x40) != 0;

  if (prgSwap) {
    // PRG mode 1: $8000 swapped with $C000
    mmc3.currentPRGBanks[0] =
        (totalPRGBanks - 2) % totalPRGBanks; // Fixed second-to-last
    mmc3.currentPRGBanks[1] = mmc3.bankData[7] % totalPRGBanks;    // R7
    mmc3.currentPRGBanks[2] = mmc3.bankData[6] % totalPRGBanks;    // R6
    mmc3.currentPRGBanks[3] = (totalPRGBanks - 1) % totalPRGBanks; // Fixed last
  } else {
    // PRG mode 0: Normal layout
    mmc3.currentPRGBanks[0] = mmc3.bankData[6] % totalPRGBanks; // R6
    mmc3.currentPRGBanks[1] = mmc3.bankData[7] % totalPRGBanks; // R7
    mmc3.currentPRGBanks[2] =
        (totalPRGBanks - 2) % totalPRGBanks; // Fixed second-to-last
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

}

void WarpNES::stepMMC3IRQ() {
  static int irqDebugCount = 0;

  // Handle reload first
  if (mmc3.irqReload) {
    mmc3.irqCounter = mmc3.irqLatch;
    mmc3.irqReload = false;

    return;
  }

  // Handle normal countdown - ONLY if counter is > 0
  if (mmc3.irqCounter > 0) {
    mmc3.irqCounter--;

    // Trigger IRQ when counter reaches 0 AND IRQ is enabled
    if (mmc3.irqCounter == 0 && mmc3.irqEnable) {
      mmc3.irqPending = true;
    }
  }
}
