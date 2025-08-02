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
    
    // Calculate which tiles to render
    int startTileX = scrollX / 8;
    int endTileX = (scrollX + 256) / 8 + 1;
    
    for (int tileX = startTileX; tileX <= endTileX; tileX++) {
        int screenX = (tileX * 8) - scrollX;
        
        if (screenX + 8 <= 0 || screenX >= 256) continue;
        
        // Determine nametable with proper horizontal wrapping
        uint16_t nametableAddrX;
        int localTileX = tileX;
        
        if (localTileX < 0) {
            localTileX = (localTileX % 32 + 32) % 32;
            nametableAddrX = baseNametable ? 0x0000 : 0x0400;
        } else if (localTileX < 32) {
            nametableAddrX = baseNametable ? 0x0400 : 0x0000;
        } else {
            localTileX = localTileX % 32;
            nametableAddrX = baseNametable ? 0x0000 : 0x0400;
        }
        
        // Combine horizontal and vertical nametable addresses
        uint16_t nametableAddr = 0x2000 + nametableAddrX + nametableAddrY;
        
        if (tileY >= 30) continue;
        
        uint16_t tileAddr = nametableAddr + (tileY * 32) + localTileX;
        uint8_t tileIndex = readByte(tileAddr);
        uint8_t attribute = getAttributeTableValue(tileAddr);
        
        // Get pattern data
        uint16_t patternBase = tileIndex * 16;
        if (ctrl & 0x10) patternBase += 0x1000;
        
        uint8_t patternLo = readCHR(patternBase + fineY);
        uint8_t patternHi = readCHR(patternBase + fineY + 8);
        
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
