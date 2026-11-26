// Mapper 5: Nintendo MMC5 (ExROM)
//
// Implemented: PRG banking modes 0-3 with PRG-RAM mappable into $6000-$DFFF,
// CHR banking modes 0-3 with separate sprite ("A") and background ("B")
// register sets for 8x16 sprites, nametable mapping ($5105) including ExRAM
// and fill mode, ExRAM modes 0-3 including extended attributes, the scanline
// IRQ ($5203/$5204) and the 8x8 multiplier ($5205/$5206).
// Not implemented: vertical split mode ($5200-$5202) and the MMC5 expansion
// audio ($5000-$5015, writes are ignored).

#include "WarpNES.hpp"
#include "PPU.hpp"

// ---------------------------------------------------------------------------
// PRG
// ---------------------------------------------------------------------------
bool WarpNES::mapMMC5PRG(uint16_t address, uint32_t& offset, bool& isRam) {
  uint8_t reg;
  int bank8k;  // 8KB bank number (bit 7 = ROM select stripped later)

  if (address < 0x6000) return false;

  if (address < 0x8000) {
    isRam = true;
    bank8k = mmc5.prgRegs[0] & 0x07;
    offset = (uint32_t)bank8k * 0x2000 + (address & 0x1FFF);
    return true;
  }

  int slot = (address - 0x8000) >> 13;  // 0..3 for $8000/$A000/$C000/$E000
  switch (mmc5.prgMode & 3) {
  case 0:  // one 32KB bank from $5117
    reg = mmc5.prgRegs[4];
    bank8k = (reg & 0x7C) + slot;
    reg |= 0x80;
    break;
  case 1:  // 16KB at $8000 ($5115), 16KB at $C000 ($5117)
    if (slot < 2) { reg = mmc5.prgRegs[2]; bank8k = (reg & 0x7E) + slot; }
    else          { reg = mmc5.prgRegs[4] | 0x80; bank8k = (reg & 0x7E) + (slot - 2); }
    break;
  case 2:  // 16KB at $8000 ($5115), 8KB at $C000 ($5116), 8KB at $E000 ($5117)
    if (slot < 2)       { reg = mmc5.prgRegs[2]; bank8k = (reg & 0x7E) + slot; }
    else if (slot == 2) { reg = mmc5.prgRegs[3]; bank8k = reg & 0x7F; }
    else                { reg = mmc5.prgRegs[4] | 0x80; bank8k = reg & 0x7F; }
    break;
  default: // 4 x 8KB: $5114-$5117
    reg = mmc5.prgRegs[1 + slot];
    if (slot == 3) reg |= 0x80;
    bank8k = reg & 0x7F;
    break;
  }

  isRam = (reg & 0x80) == 0;
  if (isRam) {
    offset = (uint32_t)(bank8k & 0x07) * 0x2000 + (address & 0x1FFF);
  } else {
    offset = ((uint32_t)bank8k * 0x2000 + (address & 0x1FFF)) % prgSize;
  }
  return true;
}

// ---------------------------------------------------------------------------
// CHR
// ---------------------------------------------------------------------------
uint32_t WarpNES::mapMMC5CHR(uint16_t address) {
  address &= 0x1FFF;
  bool largeSprites = (ppu->getControl() & 0x20) != 0;
  bool useB = largeSprites ? !mmc5.fetchingSprites : mmc5.lastSetB;

  uint32_t bank1k;
  if (!useB) {
    switch (mmc5.chrMode & 3) {
    case 0:  bank1k = (uint32_t)mmc5.chrA[7] * 8 + (address >> 10); break;
    case 1:  bank1k = (uint32_t)mmc5.chrA[(address >> 12) * 4 + 3] * 4 + ((address >> 10) & 3); break;
    case 2:  bank1k = (uint32_t)mmc5.chrA[(address >> 11) * 2 + 1] * 2 + ((address >> 10) & 1); break;
    default: bank1k = mmc5.chrA[address >> 10]; break;
    }
  } else {
    // B set covers $0000-$0FFF and is mirrored at $1000-$1FFF
    uint16_t a = address & 0x0FFF;
    switch (mmc5.chrMode & 3) {
    case 0:  bank1k = (uint32_t)mmc5.chrB[3] * 8 + (address >> 10); break;
    case 1:  bank1k = (uint32_t)mmc5.chrB[3] * 4 + ((a >> 10) & 3); break;
    case 2:  bank1k = (uint32_t)mmc5.chrB[(a >> 11) * 2 + 1] * 2 + ((a >> 10) & 1); break;
    default: bank1k = mmc5.chrB[a >> 10]; break;
    }
  }
  return (bank1k * 0x400 + (address & 0x3FF)) % chrSize;
}

// ---------------------------------------------------------------------------
// Nametables ($5105 mapping, ExRAM, fill mode)
// ---------------------------------------------------------------------------
uint8_t WarpNES::mmc5NametableRead(uint16_t address, const uint8_t* ciram) {
  int quadrant = (address >> 10) & 3;
  uint16_t offset = address & 0x3FF;
  switch ((mmc5.ntMapping >> (quadrant * 2)) & 3) {
  case 0: return ciram[offset];
  case 1: return ciram[0x400 + offset];
  case 2: return mmc5.exramMode <= 1 ? mmc5.exram[offset] : 0;
  default:
    if (offset >= 0x3C0) return (mmc5.fillAttr & 3) * 0x55;
    return mmc5.fillTile;
  }
}

void WarpNES::mmc5NametableWrite(uint16_t address, uint8_t value, uint8_t* ciram) {
  int quadrant = (address >> 10) & 3;
  uint16_t offset = address & 0x3FF;
  switch ((mmc5.ntMapping >> (quadrant * 2)) & 3) {
  case 0: ciram[offset] = value; break;
  case 1: ciram[0x400 + offset] = value; break;
  case 2: if (mmc5.exramMode <= 1) mmc5.exram[offset] = value; break;
  default: break;  // fill mode is read-only
  }
}

// ---------------------------------------------------------------------------
// CPU interface ($5000-$FFFF)
// ---------------------------------------------------------------------------
uint8_t WarpNES::readMMC5(uint16_t address) {
  if (address >= 0x6000) {
    uint32_t offset;
    bool isRam;
    mapMMC5PRG(address, offset, isRam);
    if (isRam) return (sram && offset < sramSize) ? sram[offset] : 0;
    return prgROM[offset];
  }

  if (address >= 0x5C00) {
    // ExRAM is readable by the CPU in modes 2 and 3
    return mmc5.exramMode >= 2 ? mmc5.exram[address - 0x5C00] : 0;
  }

  switch (address) {
  case 0x5204: {
    uint8_t status = (mmc5.irqPending ? 0x80 : 0) | (mmc5.inFrame ? 0x40 : 0);
    mmc5.irqPending = false;  // reading acknowledges the IRQ
    return status;
  }
  case 0x5205: return (uint8_t)((mmc5.mulA * mmc5.mulB) & 0xFF);
  case 0x5206: return (uint8_t)((mmc5.mulA * mmc5.mulB) >> 8);
  default: return 0;
  }
}

void WarpNES::writeMMC5(uint16_t address, uint8_t value) {
  if (address >= 0x6000) {
    uint32_t offset;
    bool isRam;
    mapMMC5PRG(address, offset, isRam);
    bool writable = (mmc5.ramProtect1 & 3) == 2 && (mmc5.ramProtect2 & 3) == 1;
    if (isRam && writable && sram && offset < sramSize) {
      sram[offset] = value;
      sramDirty = true;
    }
    return;
  }

  if (address >= 0x5C00) {
    if (mmc5.exramMode != 3) mmc5.exram[address - 0x5C00] = value;
    return;
  }

  if (address >= 0x5120 && address <= 0x5127) {
    mmc5.chrA[address - 0x5120] = value | (mmc5.chrUpper << 8);
    mmc5.lastSetB = false;
    return;
  }
  if (address >= 0x5128 && address <= 0x512B) {
    mmc5.chrB[address - 0x5128] = value | (mmc5.chrUpper << 8);
    mmc5.lastSetB = true;
    return;
  }
  if (address >= 0x5113 && address <= 0x5117) {
    mmc5.prgRegs[address - 0x5113] = value;
    return;
  }

  switch (address) {
  case 0x5100: mmc5.prgMode = value & 3; break;
  case 0x5101: mmc5.chrMode = value & 3; break;
  case 0x5102: mmc5.ramProtect1 = value; break;
  case 0x5103: mmc5.ramProtect2 = value; break;
  case 0x5104: mmc5.exramMode = value & 3; break;
  case 0x5105: mmc5.ntMapping = value; break;
  case 0x5106: mmc5.fillTile = value; break;
  case 0x5107: mmc5.fillAttr = value & 3; break;
  case 0x5130: mmc5.chrUpper = value & 3; break;
  case 0x5203: mmc5.irqTarget = value; break;
  case 0x5204: mmc5.irqEnable = (value & 0x80) != 0; break;
  case 0x5205: mmc5.mulA = value; break;
  case 0x5206: mmc5.mulB = value; break;
  default: break;  // $5000-$5015 audio, $5200-$5202 split: not emulated
  }
}

// ---------------------------------------------------------------------------
// Scanline detection / IRQ
// ---------------------------------------------------------------------------
// The real chip spots the PPU's repeated nametable fetches at the end of each
// rendered line (dots 337-340). The first one after vblank (on the pre-render
// line) starts the frame with the counter at 0; each following one advances it,
// and reaching the $5203 value sets the IRQ flag, i.e. just before that
// scanline begins.
void WarpNES::stepMMC5Scanline(int scanline, int cycle) {
  if (cycle != 337) return;

  bool rendering = (ppu->getMask() & 0x18) != 0;
  if (!rendering || (scanline >= 239 && scanline != 261)) {
    mmc5.inFrame = false;
    return;
  }

  if (scanline == 261 || !mmc5.inFrame) {
    mmc5.inFrame = true;
    mmc5.scanlineCounter = 0;
    return;
  }

  mmc5.scanlineCounter++;
  if (mmc5.irqTarget != 0 && mmc5.scanlineCounter == mmc5.irqTarget) {
    mmc5.irqPending = true;
  }
}
