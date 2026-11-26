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
  // The MMC3 counter is clocked by rising edges of PPU A12. With the usual
  // setup (background at $0000, sprites at $1000) that happens once per
  // scanline when the sprite patterns are fetched, around dot 260. If the
  // tables are the other way round, the edge comes from the background
  // prefetch for the next line at dot 324. Only while rendering is on, and
  // only on visible lines plus the pre-render line.
  if (!isMMC3Family()) return;
  if (!(ppu->getMask() & 0x18)) return;
  if (scanline >= 240 && scanline != 261) return;

  uint8_t ctrl = ppu->getControl();
  bool bgHigh = (ctrl & 0x10) != 0;
  bool spritesHigh = (ctrl & 0x20) ? true : (ctrl & 0x08) != 0; // 8x16: assume $1000
  int clockDot = (bgHigh && !spritesHigh) ? 324 : 260;
  if (bgHigh && spritesHigh) return; // A12 stays high: no edges

  if (cycle == clockDot) {
    stepMMC3IRQ();
  }
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
  uint32_t totalPRGBanks = prgSize / 0x2000; // Number of 8KB banks
  uint32_t totalCHRBanks = chrSize / 0x400;  // Number of 1KB banks
  if (totalCHRBanks == 0) totalCHRBanks = 1;

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

// $A001 PRG-RAM protect: bit 7 enable, bit 6 deny writes. Only honoured for
// NES 2.0 dumps; plain iNES MMC3 dumps include StarTropics (MMC6), which
// uses the register differently.
bool WarpNES::mmc3WramReadable() const {
  if (!nesHeader.isNES2 || nesHeader.submapper == 1) return true;
  return (mmc3.prgRamProtect & 0x80) != 0;
}

bool WarpNES::mmc3WramWritable() const {
  if (!nesHeader.isNES2 || nesHeader.submapper == 1) return true;
  return (mmc3.prgRamProtect & 0xC0) == 0x80;
}

// Raw bank register value behind 1 KB CHR slot 0-7 (2 KB registers give
// their even/odd halves). Used by TxSROM (118) and TQROM (119).
uint8_t WarpNES::mmc3RawCHRBank(int slot) const {
  int s = (mmc3.bankSelect & 0x80) ? (slot ^ 4) : slot;
  if (s < 4) {
    uint8_t r = mmc3.bankData[s >> 1];
    return (r & ~1) | (s & 1);
  }
  return mmc3.bankData[s - 2];
}

void WarpNES::stepMMC3IRQ() {
  // Counter clock: reload from the latch when it is zero or a reload was
  // requested ($C001), otherwise count down. Reaching zero with IRQs
  // enabled asserts the IRQ line, which stays asserted until $E000.
  if (mmc3.irqCounter == 0 || mmc3.irqReload) {
    mmc3.irqCounter = mmc3.irqLatch;
    mmc3.irqReload = false;
  } else {
    mmc3.irqCounter--;
  }

  if (mmc3.irqCounter == 0 && mmc3.irqEnable) {
    mmc3.irqPending = true;
  }
}
