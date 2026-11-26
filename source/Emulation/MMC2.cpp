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

extern const uint32_t* paletteRGB;

void WarpNES::updateMMC2Banks() {

  uint32_t totalCHRBanks = chrSize / 0x1000; // 4KB banks (0x1000 = 4096)
  if (totalCHRBanks == 0)
    totalCHRBanks = 1;

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
    mmc2.prgBank = value & 0x0F; // 8 KB bank (MMC2) or 16 KB bank (MMC4)
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
    // Applied by PPU::getNametableIndex() via getMMC2Mirroring()
    break;
  }
}

void WarpNES::checkMMC2CHRLatch(uint16_t address, uint8_t tileID) {
  // The MMC2 watches the PPU address bus. After the PPU fetches the high
  // bitplane of tile $FD or $FE, the corresponding latch flips. The switch
  // takes effect for the *next* fetch (the trigger tile itself comes from the
  // old bank), so this must be called after the CHR byte has been read.
  //
  //   PPU $0FD8        -> latch 0 = $FD   (only this exact address on MMC2)
  //   PPU $0FE8        -> latch 0 = $FE
  //   PPU $1FD8-$1FDF  -> latch 1 = $FD
  //   PPU $1FE8-$1FEF  -> latch 1 = $FE
  (void)tileID;
  if (address >= 0x2000)
    return;

  // MMC4 decodes a range for latch 0 as well
  if (nesHeader.mapper == 10 && (address & 0xFFF8) == 0x0FD8) address = 0x0FD8;
  if (nesHeader.mapper == 10 && (address & 0xFFF8) == 0x0FE8) address = 0x0FE8;
  if (address == 0x0FD8) {
    if (mmc2.latch0) { mmc2.latch0 = false; updateMMC2Banks(); }
  } else if (address == 0x0FE8) {
    if (!mmc2.latch0) { mmc2.latch0 = true; updateMMC2Banks(); }
  } else if ((address & 0xFFF8) == 0x1FD8) {
    if (mmc2.latch1) { mmc2.latch1 = false; updateMMC2Banks(); }
  } else if ((address & 0xFFF8) == 0x1FE8) {
    if (!mmc2.latch1) { mmc2.latch1 = true; updateMMC2Banks(); }
  }
}

void PPU::renderBackgroundScanlineMMC2(int scanline) {
    if (scanline < 0 || scanline >= 240) return;
    // Use per-scanline values when available, with proper fallback
    int scrollX = scanlineScrollX[scanline];
    int scrollY = scanlineScrollY[scanline];
    uint8_t ctrl = scanlineCtrl[scanline];
        
    uint8_t baseNametable = ctrl & 0x01;
    uint8_t baseNametableY = (ctrl & 0x02) >> 1;
    
    // Calculate Y position with vertical nametable support
    int worldY = scanline + scrollY;
    int tileY = worldY / 8;
    int fineY = worldY % 8;
    
    // Handle vertical nametable wrapping
    uint16_t nametableAddrY = baseNametableY ? 0x0800 : 0x0000;
    if (tileY >= 30) {
        tileY = tileY % 30;
        nametableAddrY = baseNametableY ? 0x0000 : 0x0800;
    }
    
    // The real PPU fetches 34 tiles per scanline starting at the coarse X
    // scroll: up to 33 can be visible (fine X), the 34th never is. MMC2 sees
    // every one of those fetches, including the off-screen ones. Punch-Out!!
    // relies on this: it places $FD tiles just past the right edge so the
    // latch is set for the *next* scanline (status bar and ring ropes).
    int startTileX = scrollX / 8;
    int endTileX = startTileX + 33;
    
    for (int tileX = startTileX; tileX <= endTileX; tileX++) {
        int screenX = (tileX * 8) - scrollX;
        
        // Determine nametable with proper horizontal wrapping
        int localTileX = tileX & 31;
        bool otherNametable = ((tileX >> 5) & 1) != 0;
        uint16_t nametableAddrX = (baseNametable ^ (otherNametable ? 1 : 0)) ? 0x0400 : 0x0000;
        
        // Combine horizontal and vertical nametable addresses
        uint16_t nametableAddr = 0x2000 + nametableAddrX + nametableAddrY;
        
        if (tileY >= 30) continue;
        
        uint16_t tileAddr = nametableAddr + (tileY * 32) + localTileX;
        uint8_t tileIndex = readByte(tileAddr);
        uint8_t attribute = getAttributeTableValue(tileAddr);
        
        // Get pattern data (this is the fetch the MMC2 latch watches)
        uint16_t patternBase = tileIndex * 16;
        if (ctrl & 0x10) patternBase += 0x1000;
        
        uint8_t patternLo = readCHR(patternBase + fineY);
        uint8_t patternHi = readCHR(patternBase + fineY + 8);
        
        // Off-screen fetches only affect the mapper, nothing to draw
        if (screenX + 8 <= 0 || screenX >= 256) continue;
        
        // Render pixels
        for (int pixelX = 0; pixelX < 8; pixelX++) {
            int screenPixelX = screenX + pixelX;
            if (screenPixelX < 0 || screenPixelX >= 256) continue;
            
            uint8_t pixelValue = 0;
            if (patternLo & (0x80 >> pixelX)) pixelValue |= 1;
            if (patternHi & (0x80 >> pixelX)) pixelValue |= 2;
            
            int bufferIndex = scanline * 256 + screenPixelX;
            if (pixelValue == 0) {
                backgroundMask[bufferIndex] = 1;  // Transparent
            } else {
                backgroundMask[bufferIndex] = 0;  // Opaque
            }
            
            uint8_t colorIndex;
            if (pixelValue == 0) {
                colorIndex = palette[0];
            } else {
                colorIndex = palette[(attribute & 0x03) * 4 + pixelValue];
            }
            
            uint32_t color32 = paletteRGB[colorIndex];
            uint16_t pixel = ((color32 & 0xF80000) >> 8) | 
                           ((color32 & 0x00FC00) >> 5) | 
                           ((color32 & 0x0000F8) >> 3);
            
            frameBuffer[scanline * 256 + screenPixelX] = pixel;
        }
    }
}
