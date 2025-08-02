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


void WarpNES::writeMMC1Register(uint16_t address, uint8_t value) {
    // Handle reset condition first
    if (value & 0x80) {
        mmc1.shiftRegister = 0x10;
        mmc1.shiftCount = 0;
        mmc1.control = mmc1.control | 0x0C;
        updateMMC1Banks();
        return;
    }

    // Normal MMC1 operation
    mmc1.shiftRegister >>= 1;
    mmc1.shiftRegister |= (value & 1) << 4;
    mmc1.shiftCount++;

    if (mmc1.shiftCount == 5) {
        uint8_t data = mmc1.shiftRegister;
        mmc1.shiftRegister = 0x10;
        mmc1.shiftCount = 0;

        if (address < 0xA000) {
            // Control register write
            mmc1.control = data;
        } else if (address < 0xC000) {
            // CHR Bank 0 - THIS WAS BEING SKIPPED!
            mmc1.chrBank0 = data;
        } else if (address < 0xE000) {
            // CHR Bank 1 - THIS WAS BEING SKIPPED!
            mmc1.chrBank1 = data;
        } else {
            // PRG Bank
            mmc1.prgBank = data;
        }

        updateMMC1Banks();
    }
}


void WarpNES::updateMMC1Banks() {
    uint8_t totalPRGBanks = prgSize / 0x4000;

    // Handle PRG banking only
    if (prgSize == 32768) {
        mmc1.currentPRGBank = 0;
    } else {
        uint8_t prgMode = (mmc1.control >> 2) & 0x03;
        switch (prgMode) {
            case 0:
            case 1:
                mmc1.currentPRGBank = (mmc1.prgBank >> 1) % (totalPRGBanks / 2);
                break;
            case 2:
                mmc1.currentPRGBank = 0;
                break;
            case 3:
            default:
                mmc1.currentPRGBank = mmc1.prgBank % totalPRGBanks;
                break;
        }
    }

    // DON'T TOUCH CHR BANKS - leave mmc1.chrBank0 and mmc1.chrBank1 alone
    // The readCHRData function will use them directly
}

void PPU::renderBackgroundScanlineMMC1(int scanline) {
   if (scanline < 0 || scanline >= 240) return;
   int scrollX = scanlineScrollX[scanline];
   int scrollY = scanlineScrollY[scanline];
   uint8_t ctrl = scanlineCtrl[scanline];
   uint8_t baseNametable = ctrl & 0x01;
   uint8_t baseNametableY = (ctrl & 0x02) >> 1;
   int worldY = scanline + scrollY;
   int tileY = (worldY / 8) % 30;
   int fineY = worldY % 8;
   uint16_t nametableAddrY = baseNametableY ? 0x0800 : 0x0000;
   int startTileX = scrollX / 8;
   int endTileX = (scrollX + 256) / 8;
   for (int tileX = startTileX; tileX <= endTileX; tileX++) {
       int screenX = (tileX * 8) - scrollX;
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
       uint16_t nametableAddr = 0x2000 + nametableAddrX + nametableAddrY;
       uint16_t tileAddr = nametableAddr + (tileY * 32) + localTileX;
       uint8_t tileIndex = readByte(tileAddr);
       uint8_t attribute = getAttributeTableValue(tileAddr);
       uint16_t patternBase = tileIndex * 16;
       if (ctrl & 0x10) patternBase += 0x1000;
       uint8_t patternLo = readCHR(patternBase + fineY);
       uint8_t patternHi = readCHR(patternBase + fineY + 8);
       for (int pixelX = 0; pixelX < 8; pixelX++) {
           int screenPixelX = screenX + pixelX;
           if (screenPixelX < 0 || screenPixelX >= 256) continue;
           uint8_t pixelValue = 0;
           if (patternLo & (0x80 >> pixelX)) pixelValue |= 1;
           if (patternHi & (0x80 >> pixelX)) pixelValue |= 2;
           int bufferIndex = scanline * 256 + screenPixelX;
           if (pixelValue == 0) {
               backgroundMask[bufferIndex] = 1;
           } else {
               backgroundMask[bufferIndex] = 0;
           }
           uint8_t colorIndex;
           if (pixelValue == 0) {
               colorIndex = palette[0];
           } else {
               colorIndex = palette[(attribute & 0x03) * 4 + pixelValue];
           }
           uint32_t color32 = paletteRGB[colorIndex];
           uint16_t pixel = ((color32 & 0xF80000) >> 8) | ((color32 & 0x00FC00) >> 5) | ((color32 & 0x0000F8) >> 3);
           frameBuffer[scanline * 256 + screenPixelX] = pixel;
       }
   }
}

