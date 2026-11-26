#include "WarpNES.hpp"

#include "PPU.hpp"
#include "Mappers.hpp"

std::vector<FlipCacheEntry> PPU::g_flipCache;
std::unordered_map<uint32_t, size_t> PPU::g_flipCacheIndex;
PPU::ScalingCache PPU::g_scalingCache;

static const uint8_t nametableMirrorLookup[][4] = {
    {0, 0, 1, 1}, // Horizontal ($2000=$2400, $2800=$2C00)
    {0, 1, 0, 1}, // Vertical   ($2000=$2800, $2400=$2C00)
    {0, 0, 0, 0}, // One-screen, lower bank
    {1, 1, 1, 1}  // One-screen, upper bank
};

/**
 * Default hardcoded palette.
 */
static constexpr const uint32_t defaultPaletteRGB[64] = {
    0x7c7c7c,
    0x0000fc,
    0x0000bc,
    0x4428bc,
    0x940084,
    0xa80020,
    0xa81000,
    0x881400,
    0x503000,
    0x007800,
    0x006800,
    0x005800,
    0x004058,
    0x000000,
    0x000000,
    0x000000,
    0xbcbcbc,
    0x0078f8,
    0x0058f8,
    0x6844fc,
    0xd800cc,
    0xe40058,
    0xf83800,
    0xe45c10,
    0xac7c00,
    0x00b800,
    0x00a800,
    0x00a844,
    0x008888,
    0x000000,
    0x000000,
    0x000000,
    0xf8f8f8,
    0x3cbcfc,
    0x6888fc,
    0x9878f8,
    0xf878f8,
    0xf85898,
    0xf87858,
    0xfca044,
    0xf8b800,
    0xb8f818,
    0x58d854,
    0x58f898,
    0x00e8d8,
    0x787878,
    0x000000,
    0x000000,
    0xfcfcfc,
    0xa4e4fc,
    0xb8b8f8,
    0xd8b8f8,
    0xf8b8f8,
    0xf8a4c0,
    0xf0d0b0,
    0xfce0a8,
    0xf8d878,
    0xd8f878,
    0xb8f8b8,
    0xb8f8d8,
    0x00fcfc,
    0xf8d8f8,
    0x000000,
    0x000000
};

/**
 * RGB representation of the NES palette.
 */
const uint32_t* paletteRGB = defaultPaletteRGB;

PPU::PPU(WarpNES& engine) :
    engine(engine)
{
    // Add these new initializations at the beginning:
    ppuCycles = 0;
    currentScanline = 0;
    currentCycle = 0;
    inVBlank = true;  // Start in VBlank
    frameOdd = false;
    
    // Keep all existing initialization code:
    currentAddress = 0;
    tempAddress = 0;
    fineX = 0;
    memset(lineV, 0, sizeof(lineV));
    memset(lineFineX, 0, sizeof(lineFineX));
    writeToggle = false;
    
    ppuCtrl = 0x00;
    ppuMask = 0x00;
    ppuStatus = 0x80;
    oamAddress = 0x00;
    ppuScrollX = 0x00;
    ppuScrollY = 0x00;
    vramBuffer = 0x00;
    
    // Clear all memory
    memset(palette, 0, sizeof(palette));
    memset(nametable, 0, sizeof(nametable));
    memset(oam, 0, sizeof(oam));
    sprite0Hit = false;
    sprite0HitX = -1;
    // Set default background color (usually black)
    palette[0] = 0x0F;  // Black
    cachedScrollX = 0;
    cachedScrollY = 0;
    cachedCtrl = 0;
    renderScrollX = 0;
    renderScrollY = 0;
    renderCtrl = 0;
    gameAreaScrollX = 0;
    ignoreNextScrollWrite = false;
    frameScrollX = 0;
    frameCtrl = 0;
    for (int i = 0; i < 240; i++) {
        scanlineScrollY[i] = 0;
        scanlineScrollX[i] = 0;
    }
    frameScrollY = 0;
}

uint8_t PPU::getAttributeTableValue(uint16_t nametableAddress)
{
    nametableAddress = getNametableIndex(nametableAddress);

    // Get the tile position within the nametable (32x30 tiles)
    int tileX = nametableAddress & 0x1f;  // 0-31
    int tileY = (nametableAddress >> 5) & 0x1f;  // 0-29 (but we only use 0-29)
    
    // Convert tile position to attribute table position (16x16 pixel groups = 2x2 tiles)
    int attrX = tileX / 4;  // 0-7 (8 groups horizontally)
    int attrY = tileY / 4;  // 0-7 (8 groups vertically)
    
    // Calculate which quadrant within the 4x4 tile group (32x32 pixels)
    int quadX = (tileX / 2) & 1;  // 0 or 1
    int quadY = (tileY / 2) & 1;  // 0 or 1
    
    // Calculate the shift amount for this quadrant
    int shift = (quadY * 4) + (quadX * 2);
    
    // Get the nametable base (which 1KB nametable we're in)
    int nametableBase = (nametableAddress >= 0x400) ? 0x400 : 0x000;
    
    // Attribute table starts at +0x3C0 from nametable base
    int attrOffset = nametableBase + 0x3C0 + (attrY * 8) + attrX;
    
    // Extract the 2-bit palette value and return it
    return (nametable[attrOffset] >> shift) & 0x03;
}

uint16_t PPU::getNametableIndex(uint16_t address) {
    address = (address - 0x2000) % 0x1000;
    int table = address / 0x400;
    int offset = address % 0x400;
    // Use the actual mirroring from the ROM header
    int mode = engine.nesHeader.mirroring;  // 0=horizontal, 1=vertical
    if (engine.ext) {
        return engine.ext->ntPage(table) * 0x400 + offset;
    }
    if (engine.nesHeader.fourScreen) {
        return address;
    }
    if (engine.getMapper() == 118) {
        // TxSROM: CHR bank register bit 7 picks the CIRAM page
        return ((engine.mmc3RawCHRBank(table) >> 7) & 1) * 0x400 + offset;
    }
    if (engine.isMMC2Family()) {
        // MMC2 controls mirroring at $F000: 0=vertical, 1=horizontal
        mode = engine.getMMC2Mirroring() ? 0 : 1;
    } else if (engine.getMapper() == 7) {
        // AxROM: one-screen mirroring selected by bit 4 of the register
        mode = engine.getAxROMUpperNametable() ? 3 : 2;
    } else if (engine.isMMC3Family()) {
        // MMC3 $A000: 0=vertical, 1=horizontal
        mode = engine.getMMC3Mirroring() ? 0 : 1;
    } else if (engine.getMapper() == 1) {
        // MMC1 control register: 0=one-screen lower, 1=one-screen upper,
        // 2=vertical, 3=horizontal
        static const int mmc1Modes[4] = {2, 3, 1, 0};
        mode = mmc1Modes[engine.getMMC1Mirroring()];
    }

    return (nametableMirrorLookup[mode][table] * 0x400 + offset) % 2048;
}

uint8_t PPU::readByte(uint16_t address)
{
    // Mirror all addresses above $3fff
    address &= 0x3fff;

    if (address < 0x2000)
    {
        // CHR (bank-aware, no MMC2 latch side effects)
        return engine.readCHRData(address);
    }
    else if (address < 0x3f00)
    {
        // Nametable
        if (engine.getMapper() == 5) return engine.mmc5NametableRead(address, nametable);
        if (engine.ext && engine.ext->ntFromRom((address >> 10) & 3)) return engine.ext->ntRomRead(address);
        return nametable[getNametableIndex(address)];
    }

    return 0;
}

void PPU::checkCHRLatch(uint16_t address, uint8_t tileID) {
    // Forward to the engine's CHR latch checking
    engine.checkCHRLatch(address, tileID);
}

// A real PPU pattern fetch. On MMC2 the latch flips *after* the trigger
// tile ($FD/$FE) has been fetched, so the trigger tile itself still comes
// from the old bank and only subsequent fetches see the new one.
uint8_t PPU::readCHR(int index)
{
    if (index < 0 || index >= 0x2000)
        return 0;

    uint8_t result = engine.readCHRData(index);
    if (engine.isMMC2Family()) {
        engine.checkCHRLatch(index, 0);  // May switch banks for later fetches
    }
    return result;
}

// Read pattern data without touching mapper state. Use for anything that is
// not a genuine PPU rendering fetch (sprite-0 helpers, debug, zapper, etc.),
// otherwise those extra reads would flip the MMC2 latches.
uint8_t PPU::peekCHR(int index)
{
    if (index < 0 || index >= 0x2000)
        return 0;
    return engine.readCHRData(index);
}

int PPU::spriteHeight() const
{
    return (ppuCtrl & 0x20) ? 16 : 8;
}

// Address of the low bitplane for a given sprite row (row already flipped,
// 0..height-1). Handles both 8x8 and 8x16 sprites.
uint16_t PPU::spritePatternAddr(uint8_t tileIndex, int row) const
{
    if (ppuCtrl & 0x20) {
        // 8x16: bit 0 of tile index picks the pattern table, top tile is even
        uint16_t table = (tileIndex & 0x01) ? 0x1000 : 0x0000;
        uint8_t tile = tileIndex & 0xFE;
        if (row >= 8) {
            tile++;
            row -= 8;
        }
        return table + tile * 16 + row;
    }
    uint16_t table = (ppuCtrl & 0x08) ? 0x1000 : 0x0000;
    return table + tileIndex * 16 + row;
}

uint8_t PPU::readDataRegister()
{
    uint8_t value;
    
    if (currentAddress < 0x3F00) {
        // Normal VRAM - return buffered value
        value = vramBuffer;
        vramBuffer = readByte(currentAddress);
        // MMC2/MMC4 watch every PPU bus read, including $2007 reads
        if ((currentAddress & 0x3FFF) < 0x2000 && engine.isMMC2Family())
            engine.checkCHRLatch(currentAddress & 0x3FFF, 0);
    } else {
        // Palette RAM - return immediately but also update buffer
        value = readByte(currentAddress);
        vramBuffer = readByte(currentAddress - 0x1000); // Mirror to nametable
    }

    // Increment address
    if (ppuCtrl & 0x04) {
        currentAddress += 32;  // Vertical increment
    } else {
        currentAddress += 1;   // Horizontal increment
    }
    
    return value;
}

void PPU::setSprite0Hit(bool hit) {
    sprite0Hit = hit;
}

uint8_t PPU::readRegister(uint16_t address)
{
   switch(address)
   {
case 0x2002: // PPUSTATUS
{
    uint8_t status = ppuStatus;
    
    if (sprite0Hit) {
        status |= 0x40;
    }
    
    writeToggle = false;
    
    // DON'T clear VBlank flag here in cycle-accurate mode
    // Let the cycle-accurate timing handle it
    //sprite0Hit = false;
    ppuStatus &= 0xBF;   // Only clear sprite 0 hit flag
    
    return status;
}

   case 0x2004: // OAMDATA
       return oam[oamAddress];
       
   case 0x2007: // PPUDATA
       return readDataRegister();
       
   default:
       break;
   }

   return 0;
}

void PPU::setVBlankFlag(bool flag)
{
    if (flag) {
        ppuStatus |= 0x80;  // Set VBlank flag
    } else {
        ppuStatus &= 0x7F;  // Clear VBlank flag
    }
}

void PPU::setPaletteRAM(uint8_t* data) {
    memcpy(palette, data, 32); 
}


void PPU::render16(uint16_t* buffer) {
    memcpy(buffer, frameBuffer, sizeof(frameBuffer));
}


void PPU::updateRenderRegisters()
{
    uint8_t oldRenderScrollX = renderScrollX;
    
    // Use the game area scroll value instead of the alternating one
    renderScrollX = gameAreaScrollX;
    renderScrollY = ppuScrollY;
    renderCtrl = ppuCtrl;
    
}

void PPU::writeAddressRegister(uint8_t value)
{
    if (!writeToggle)
    {
        // Upper byte (bit 14 is cleared)
        tempAddress = (tempAddress & 0x00FF) | ((uint16_t)(value & 0x3F) << 8);
    }
    else
    {
        // Lower byte, then t is copied to v. Games use this for mid-frame
        // scroll splits (e.g. Super Mario Bros. 3's status bar).
        tempAddress = (tempAddress & 0x7F00) | (uint16_t)value;
        currentAddress = tempAddress;
    }
    writeToggle = !writeToggle;
}

// Where on this scanline sprite 0 first overlaps the background with both
// pixels opaque. Uses side-effect-free pattern reads (no mapper latches).
int PPU::findSprite0HitX(int scanline)
{
    if ((ppuMask & 0x18) != 0x18) return -1;  // needs both BG and sprites on

    uint8_t spriteY = oam[0];
    uint8_t tileIndex = oam[1];
    uint8_t attributes = oam[2];
    uint8_t spriteX = oam[3];
    int height = spriteHeight();
    int row = scanline - (spriteY + 1);
    if (row < 0 || row >= height) return -1;
    if (attributes & 0x80) row = height - 1 - row;

    engine.setMMC5SpriteFetch(true);
    uint16_t spriteAddr = spritePatternAddr(tileIndex, row);
    uint8_t sLo = peekCHR(spriteAddr);
    uint8_t sHi = peekCHR(spriteAddr + 8);
    engine.setMMC5SpriteFetch(false);
    if (!(sLo | sHi)) return -1;

    uint16_t v = currentAddress;  // this line's scroll position
    int fx = fineX;
    int fineY = (v >> 12) & 0x07;
    uint16_t patternTable = (ppuCtrl & 0x10) ? 0x1000 : 0x0000;
    bool exAttr = engine.mmc5ExtendedAttributes();
    bool leftClip = (ppuMask & 0x06) != 0x06;

    for (int px = 0; px < 8; px++) {
        int x = spriteX + px;
        if (x >= 255) break;              // never at x=255
        if (x < 8 && leftClip) continue;
        int bit = (attributes & 0x40) ? px : 7 - px;
        if (!(((sLo >> bit) & 1) | ((sHi >> bit) & 1))) continue;

        // Background pixel at x
        int pos = fx + x;
        uint16_t vv = v;
        for (int t = 0; t < pos / 8; t++) {
            if ((vv & 0x001F) == 31) { vv &= ~0x001F; vv ^= 0x0400; } else vv++;
        }
        uint16_t ntAddr = 0x2000 | (vv & 0x0FFF);
        uint8_t tile = readByte(ntAddr);
        uint8_t bLo, bHi;
        if (exAttr) {
            uint8_t ex = engine.mmc5ExramByte(ntAddr & 0x3FF);
            uint32_t base = (uint32_t)((ex & 0x3F) | (engine.mmc5ChrUpper() << 6)) * 0x1000 + tile * 16 + fineY;
            bLo = engine.readCHRAbsolute(base);
            bHi = engine.readCHRAbsolute(base + 8);
        } else {
            uint16_t a = patternTable + tile * 16 + fineY;
            bLo = peekCHR(a);
            bHi = peekCHR(a + 8);
        }
        int bbit = 7 - (pos & 7);
        if (((bLo >> bbit) & 1) | ((bHi >> bbit) & 1)) return x;
    }
    return -1;
}

// PPUCTRL value in effect at an absolute dot of the current frame.
uint8_t PPU::ctrlAtDot(int absDot) const
{
    for (auto it = ctrlTimeline.rbegin(); it != ctrlTimeline.rend(); ++it) {
        if (it->first <= absDot) return it->second;
    }
    return frameStartCtrl;
}

// Increment the vertical part of v at the end of a rendered scanline.
void PPU::scrollIncrementY()
{
    uint16_t v = currentAddress;
    if ((v & 0x7000) != 0x7000) {
        v += 0x1000;                       // fine Y
    } else {
        v &= ~0x7000;
        int coarseY = (v & 0x03E0) >> 5;
        if (coarseY == 29) {
            coarseY = 0;
            v ^= 0x0800;                   // switch vertical nametable
        } else if (coarseY == 31) {
            coarseY = 0;                   // out-of-range Y wraps without switching
        } else {
            coarseY++;
        }
        v = (v & ~0x03E0) | (coarseY << 5);
    }
    currentAddress = v;
}

// Background for one scanline, driven by the loopy v/x registers captured at
// the start of the line. Fetches 34 tiles like the real PPU (the MMC2 latch
// relies on the off-screen ones).
void PPU::renderBackgroundScanline(int scanline)
{
    if (scanline < 0 || scanline >= 240) return;

    uint16_t v = lineV[scanline];
    int fx = lineFineX[scanline];
    int fineY = (v >> 12) & 0x07;
    bool clipLeft = !(ppuMask & 0x02);
    bool ctrlChanges = !ctrlTimeline.empty();
    bool exAttr = engine.mmc5ExtendedAttributes();
    engine.setMMC5SpriteFetch(false);

    for (int tile = 0; tile < 34; tile++) {
        uint16_t ntAddr = 0x2000 | (v & 0x0FFF);
        uint16_t atAddr = 0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 0x07);
        uint8_t tileIndex = readByte(ntAddr);
        uint8_t attrByte = readByte(atAddr);
        int shift = ((v >> 4) & 0x04) | (v & 0x02);
        uint8_t paletteIndex = (attrByte >> shift) & 0x03;

        // Pattern table in effect when the real PPU fetches this tile: tiles
        // 0-1 at the end of the previous line (dots 321-336), tile n>=2 at
        // dot (n-2)*8+5 of this line.
        uint8_t ctrl = scanlineCtrl[scanline];
        if (ctrlChanges) {
            int fetchDot = (tile < 2) ? (scanline - 1) * 341 + 325 + tile * 8
                                      : scanline * 341 + (tile - 2) * 8 + 5;
            ctrl = ctrlAtDot(fetchDot);
        }
        uint16_t patternTable = (ctrl & 0x10) ? 0x1000 : 0x0000;

        uint8_t patternLo, patternHi;
        if (exAttr) {
            // MMC5 extended attributes: ExRAM byte per tile picks a 4KB CHR
            // bank (bits 0-5, plus $5130) and the palette (bits 6-7)
            uint8_t ex = engine.mmc5ExramByte(ntAddr & 0x3FF);
            paletteIndex = ex >> 6;
            uint32_t base = (uint32_t)((ex & 0x3F) | (engine.mmc5ChrUpper() << 6)) * 0x1000 + tileIndex * 16 + fineY;
            patternLo = engine.readCHRAbsolute(base);
            patternHi = engine.readCHRAbsolute(base + 8);
        } else {
            uint16_t patternAddr = patternTable + tileIndex * 16 + fineY;
            patternLo = readCHR(patternAddr);
            patternHi = readCHR(patternAddr + 8);
        }

        int screenX = tile * 8 - fx;
        for (int px = 0; px < 8; px++) {
            int x = screenX + px;
            if (x < 0 || x >= 256) continue;
            uint8_t pixelValue = ((patternLo >> (7 - px)) & 1) | (((patternHi >> (7 - px)) & 1) << 1);
            if (clipLeft && x < 8) pixelValue = 0;

            int bufferIndex = scanline * 256 + x;
            backgroundMask[bufferIndex] = pixelValue == 0 ? 1 : 0;
            uint8_t colorIndex = pixelValue == 0 ? palette[0] : palette[paletteIndex * 4 + pixelValue];
            uint32_t color32 = paletteRGB[colorIndex & 0x3F];
            frameBuffer[bufferIndex] = ((color32 & 0xF80000) >> 8) | ((color32 & 0x00FC00) >> 5) | ((color32 & 0x0000F8) >> 3);
        }

        // Coarse X increment, wrapping into the horizontal neighbour nametable
        if ((v & 0x001F) == 31) {
            v &= ~0x001F;
            v ^= 0x0400;
        } else {
            v++;
        }
    }
}

void PPU::writeByte(uint16_t address, uint8_t value)
{
    // Mirror all addresses above $3fff
    address &= 0x3fff;

    if (address < 0x2000)
    {
        // CHR-RAM write
        engine.writeCHRData(address, value);
    }
    else if (address < 0x3f00)
    {
        // Nametable write
        if (engine.getMapper() == 5) {
            engine.mmc5NametableWrite(address, value, nametable);
            return;
        }
        if (engine.ext && engine.ext->ntFromRom((address >> 10) & 3)) return;
        uint16_t index = getNametableIndex(address);
    
        nametable[index] = value;
    }
    else if (address < 0x3f20)
    {
        uint8_t paletteIndex = address - 0x3f00;
        uint8_t oldPaletteValue = palette[paletteIndex];
        value &= 0x3f;
        if (oldPaletteValue != value) {
            palette[paletteIndex] = value;

            // Handle mirroring
            if (address == 0x3f10 || address == 0x3f14 || address == 0x3f18 || address == 0x3f1c)
            {
                palette[address - 0x3f10] = value;
            }
        }
    }
}

void PPU::writeDataRegister(uint8_t value)
{
    static bool debugPalette = true;
    
    writeByte(currentAddress, value);
    if (!(ppuCtrl & (1 << 2)))
    {
        currentAddress++;
    }
    else
    {
        currentAddress += 32;
    }
}

void PPU::writeDMA(uint8_t page)
{
    uint16_t address = (uint16_t)page << 8;
    uint8_t startOAMAddr = oamAddress;  // Save starting address
    
    for (int i = 0; i < 256; i++)
    {
        oam[(startOAMAddr + i) & 0xFF] = engine.readData(address);
        address++;
        // DON'T increment oamAddress here!
    }
    
    // DMA cycles
    addCycles(513 * 3);
}


void PPU::writeRegister(uint16_t address, uint8_t value)
{
    switch(address)
    {
    // PPUCTRL
    case 0x2000:
        if (currentScanline < 240 || currentScanline == 261) {
            int line = (currentScanline == 261) ? -1 : currentScanline;
            ctrlTimeline.push_back({line * 341 + currentCycle + writeDotOffset, value});
        }
        ppuCtrl = value;
        tempAddress = (tempAddress & ~0x0C00) | ((value & 0x03) << 10);
        break;
        
    // PPUMASK  
    case 0x2001:
        ppuMask = value;
        break;
        
    // OAMADDR
    case 0x2003:
        oamAddress = value;
        break;
        
    // OAMDATA
    case 0x2004:
        oam[oamAddress] = value;
        oamAddress++;
        break;
        
    // PPUSCROLL
    case 0x2005:
        if (!writeToggle) {
            tempAddress = (tempAddress & ~0x001F) | (value >> 3);
            fineX = value & 0x07;
        } else {
            tempAddress = (tempAddress & ~0x73E0) | ((value & 0x07) << 12) | ((value & 0xF8) << 2);
        }
        if (!writeToggle) {
            ppuScrollX = value;
            // Only set future scanlines, not all scanlines
            if (currentScanline >= 0 && currentScanline < 240) {
                // Set scroll for remaining scanlines in this frame (this rounds up to the nearest 8 sprite); needed for Duck Tales and Super Mario Brothers
                int tempScanline = (currentScanline + 8) & ~7; // rounds up to next multiple of 8

                for (int i = tempScanline; i < 240; i++) {
                    scanlineScrollX[i] = value;
                }
            } else {
                // Not mid-frame, set all scanlines
                for (int i = 0; i < 240; i++) {
                    scanlineScrollX[i] = value;
                }
            }

        
            writeToggle = !writeToggle;
        } else {
            ppuScrollY = value;
        
            if (currentScanline >= 0 && currentScanline < 240) {
                for (int i = currentScanline; i < 240; i++) {
                    scanlineScrollY[i] = value;
                }
            } else {
                for (int i = 0; i < 240; i++) {
                    scanlineScrollY[i] = value;
                }
            }
        
            writeToggle = !writeToggle;
        }
        break;
    // PPUADDR
    case 0x2006:
        writeAddressRegister(value);
        break;
        
    // PPUDATA
    case 0x2007:
        writeDataRegister(value);
        break;
        
    default:
        break;
    }
}

PPU::ScalingCache::ScalingCache() 
    : scaledBuffer(nullptr), sourceToDestX(nullptr), sourceToDestY(nullptr),
      scaleFactor(0), destWidth(0), destHeight(0), destOffsetX(0), destOffsetY(0),
      screenWidth(0), screenHeight(0), isValid(false) 
{
}

PPU::ScalingCache::~ScalingCache() 
{
    cleanup();
}

void PPU::ScalingCache::cleanup() 
{
    if (scaledBuffer) { 
        delete[] scaledBuffer; 
        scaledBuffer = nullptr; 
    }
    if (sourceToDestX) { 
        delete[] sourceToDestX; 
        sourceToDestX = nullptr; 
    }
    if (sourceToDestY) { 
        delete[] sourceToDestY; 
        sourceToDestY = nullptr; 
    }
    isValid = false;
}

void PPU::renderScaled(uint16_t* buffer, int screenWidth, int screenHeight)
{
    // Clear the screen buffer
    uint32_t bgColor32 = paletteRGB[palette[0]];
    //uint16_t bgColor16 = ((bgColor32 & 0xF80000) >> 8) | ((bgColor32 & 0x00FC00) >> 5) | ((bgColor32 & 0x0000F8) >> 3);
    uint16_t bgColor16 = 0x0000; // Pure black in RGB565

    for (int i = 0; i < screenWidth * screenHeight; i++) {
        buffer[i] = bgColor16;
    }
    
    // Update scaling cache if needed
    if (!isScalingCacheValid(screenWidth, screenHeight)) {
        updateScalingCache(screenWidth, screenHeight);
    }
    
    // Render NES frame to temporary buffer
    static uint16_t nesBuffer[256 * 240];
    render16(nesBuffer);
    
    // Apply scaling based on cache
    const int scale = g_scalingCache.scaleFactor;

    if (scale == 1) {
        renderScaled1x1(nesBuffer, buffer, screenWidth, screenHeight);
    } else if (scale == 2) {
        renderScaled2x(nesBuffer, buffer, screenWidth, screenHeight);
    } else if (scale == 3) {
        renderScaled3x(nesBuffer, buffer, screenWidth, screenHeight);
    } else {
        renderScaledGeneric(nesBuffer, buffer, screenWidth, screenHeight, scale);
    }
}

void PPU::renderScaled32(uint32_t* buffer, int screenWidth, int screenHeight)
{
    // Create temporary 16-bit buffer
    uint16_t* buffer16 = new uint16_t[screenWidth * screenHeight];
    
    // Call the existing renderScaled method
    renderScaled(buffer16, screenWidth, screenHeight);
    
    // Convert 16-bit buffer back to 32-bit
    for (int i = 0; i < screenWidth * screenHeight; i++)
    {
        uint16_t pixel16 = buffer16[i];
        
        // Convert RGB565 back to RGB888
        uint32_t r = ((pixel16 >> 11) & 0x1F) << 3;  // 5 bits -> 8 bits
        uint32_t g = ((pixel16 >> 5) & 0x3F) << 2;   // 6 bits -> 8 bits  
        uint32_t b = (pixel16 & 0x1F) << 3;          // 5 bits -> 8 bits
        
        // Combine into 32-bit ARGB format with full alpha
        buffer[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
    
    // Clean up temporary buffer
    delete[] buffer16;
}

void PPU::updateScalingCache(int screenWidth, int screenHeight)
{
    // Calculate optimal scaling
    int scale_x = screenWidth / 256;
    int scale_y = screenHeight / 240;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1) scale = 1;
    
    // Check if cache needs updating
    if (g_scalingCache.isValid && 
        g_scalingCache.scaleFactor == scale &&
        g_scalingCache.screenWidth == screenWidth &&
        g_scalingCache.screenHeight == screenHeight) {
        return; // Cache is still valid
    }
    
    // Clean up old cache
    g_scalingCache.cleanup();
    
    // Calculate new dimensions
    g_scalingCache.scaleFactor = scale;
    g_scalingCache.destWidth = 256 * scale;
    g_scalingCache.destHeight = 240 * scale;
    g_scalingCache.destOffsetX = (screenWidth - g_scalingCache.destWidth) / 2;
    g_scalingCache.destOffsetY = (screenHeight - g_scalingCache.destHeight) / 2;
    g_scalingCache.screenWidth = screenWidth;
    g_scalingCache.screenHeight = screenHeight;
    
    // Allocate coordinate mapping tables
    g_scalingCache.sourceToDestX = new int[256];
    g_scalingCache.sourceToDestY = new int[240];
    
    // Pre-calculate coordinate mappings
    for (int x = 0; x < 256; x++) {
        g_scalingCache.sourceToDestX[x] = x * scale + g_scalingCache.destOffsetX;
    }
    
    for (int y = 0; y < 240; y++) {
        g_scalingCache.sourceToDestY[y] = y * scale + g_scalingCache.destOffsetY;
    }
    
    g_scalingCache.isValid = true;
}

bool PPU::isScalingCacheValid(int screenWidth, int screenHeight)
{
    return g_scalingCache.isValid && 
           g_scalingCache.screenWidth == screenWidth &&
           g_scalingCache.screenHeight == screenHeight;
}

void PPU::renderScaled1x1(uint16_t* nesBuffer, uint16_t* screenBuffer, int screenWidth, int screenHeight)
{
    const int dest_x = g_scalingCache.destOffsetX;
    const int dest_y = g_scalingCache.destOffsetY;

    // Direct 1:1 copy
    for (int y = 0; y < 240; y++) {
        int screen_y = y + dest_y;
        if (screen_y < 0 || screen_y >= screenHeight) continue;
        
        uint16_t* src_row = &nesBuffer[y * 256];
        uint16_t* dest_row = &screenBuffer[screen_y * screenWidth + dest_x];
        
        int copy_width = 256;
        if (dest_x + copy_width > screenWidth) {
            copy_width = screenWidth - dest_x;
        }
        if (dest_x < 0) {
            src_row -= dest_x;
            dest_row -= dest_x;
            copy_width += dest_x;
        }
        
        if (copy_width > 0) {
            memcpy(dest_row, src_row, copy_width * sizeof(uint16_t));
        }
    }
}

void PPU::renderScaled2x(uint16_t* nesBuffer, uint16_t* screenBuffer, int screenWidth, int screenHeight)
{
    const int dest_x = g_scalingCache.destOffsetX;
    const int dest_y = g_scalingCache.destOffsetY;
    
    // Optimized 2x scaling
    for (int y = 0; y < 240; y++) {
        int dest_y1 = y * 2 + dest_y;
        int dest_y2 = dest_y1 + 1;
        
        if (dest_y2 >= screenHeight) break;
        if (dest_y1 < 0) continue;
        
        uint16_t* src_row = &nesBuffer[y * 256];
        uint16_t* dest_row1 = &screenBuffer[dest_y1 * screenWidth + dest_x];
        uint16_t* dest_row2 = &screenBuffer[dest_y2 * screenWidth + dest_x];
        
        // Process pixels in groups for better cache utilization
        for (int x = 0; x < 256; x += 4) {
            if ((x * 2 + dest_x + 8) > screenWidth) break;
            
            uint16_t p1 = src_row[x];
            uint16_t p2 = src_row[x + 1];
            uint16_t p3 = src_row[x + 2];
            uint16_t p4 = src_row[x + 3];
            
            int dest_base = x * 2;
            
            // First row
            dest_row1[dest_base]     = p1; dest_row1[dest_base + 1] = p1;
            dest_row1[dest_base + 2] = p2; dest_row1[dest_base + 3] = p2;
            dest_row1[dest_base + 4] = p3; dest_row1[dest_base + 5] = p3;
            dest_row1[dest_base + 6] = p4; dest_row1[dest_base + 7] = p4;
            
            // Second row (duplicate)
            dest_row2[dest_base]     = p1; dest_row2[dest_base + 1] = p1;
            dest_row2[dest_base + 2] = p2; dest_row2[dest_base + 3] = p2;
            dest_row2[dest_base + 4] = p3; dest_row2[dest_base + 5] = p3;
            dest_row2[dest_base + 6] = p4; dest_row2[dest_base + 7] = p4;
        }
    }
}

void PPU::renderScaled3x(uint16_t* nesBuffer, uint16_t* screenBuffer, int screenWidth, int screenHeight)
{
    const int dest_x = g_scalingCache.destOffsetX;
    const int dest_y = g_scalingCache.destOffsetY;
    
    for (int y = 0; y < 240; y++) {
        int dest_y1 = y * 3 + dest_y;
        int dest_y2 = dest_y1 + 1;
        int dest_y3 = dest_y1 + 2;
        
        if (dest_y3 >= screenHeight) break;
        if (dest_y1 < 0) continue;
        
        uint16_t* src_row = &nesBuffer[y * 256];
        uint16_t* dest_row1 = &screenBuffer[dest_y1 * screenWidth + dest_x];
        uint16_t* dest_row2 = &screenBuffer[dest_y2 * screenWidth + dest_x];
        uint16_t* dest_row3 = &screenBuffer[dest_y3 * screenWidth + dest_x];
        
        for (int x = 0; x < 256; x++) {
            if ((x * 3 + dest_x + 3) > screenWidth) break;
            
            uint16_t pixel = src_row[x];
            int dest_base = x * 3;
            
            // Triple each pixel
            dest_row1[dest_base] = dest_row1[dest_base + 1] = dest_row1[dest_base + 2] = pixel;
            dest_row2[dest_base] = dest_row2[dest_base + 1] = dest_row2[dest_base + 2] = pixel;
            dest_row3[dest_base] = dest_row3[dest_base + 1] = dest_row3[dest_base + 2] = pixel;
        }
    }
}

void PPU::renderScaledGeneric(uint16_t* nesBuffer, uint16_t* screenBuffer, int screenWidth, int screenHeight, int scale)
{
    // Generic scaling using pre-calculated coordinate tables
    for (int y = 0; y < 240; y++) {
        uint16_t* src_row = &nesBuffer[y * 256];
        int dest_y_start = g_scalingCache.sourceToDestY[y];
        
        for (int scale_y = 0; scale_y < scale; scale_y++) {
            int dest_y = dest_y_start + scale_y;
            if (dest_y < 0 || dest_y >= screenHeight) continue;
            
            uint16_t* dest_row = &screenBuffer[dest_y * screenWidth];
            
            for (int x = 0; x < 256; x++) {
                uint16_t pixel = src_row[x];
                int dest_x_start = g_scalingCache.sourceToDestX[x];
                
                for (int scale_x = 0; scale_x < scale; scale_x++) {
                    int dest_x = dest_x_start + scale_x;
                    if (dest_x >= 0 && dest_x < screenWidth) {
                        dest_row[dest_x] = pixel;
                    }
                }
            }
        }
    }
}

void PPU::captureFrameScroll() {
    frameScrollX = ppuScrollX;
    frameScrollY = ppuScrollY;
    frameCtrl = ppuCtrl;
    
    // Initialize scanline arrays with current values
    /*for (int i = 0; i < 240; i++) {
        scanlineScrollX[i] = ppuScrollX;
        scanlineScrollY[i] = ppuScrollY;
        scanlineCtrl[i] = ppuCtrl;
    }*/
}

void PPU::stepCycle(int scanline, int cycle, int mapper) {
    currentScanline = scanline;
    currentCycle = cycle;
    
    bool renderingOn = (ppuMask & 0x18) != 0;

    // Pre-render scanline (261)
    if (scanline == 261) {
        if (cycle == 0) {
            ctrlTimeline.clear();
            frameStartCtrl = ppuCtrl;
        }
        if (renderingOn) {
            if (cycle == 256) scrollIncrementY();
            if (cycle == 257) scrollCopyX();
            if (cycle == 280) scrollCopyY();   // dots 280-304
        }
        if (cycle == 1) {
            ppuStatus &= 0x7F;  // Clear VBlank
            ppuStatus &= 0xBF;  // Clear sprite 0 hit
            sprite0Hit = false;
            inVBlank = false;
            frameComplete = false;
            currentRenderScanline = 0;
        }
        
/*        // Odd frame cycle skip (skip cycle 340 on odd frames when rendering enabled)
        if (cycle == 339 && frameOdd && (ppuMask & 0x18)) {
            // Skip to next frame early
            return;
        }
        
        if (cycle == 340) {
            frameOdd = !frameOdd;
        }
        return; */
    }
    
    // Visible scanlines (0-239) + potential overscan (240-260)
    if (scanline >= 0 && scanline < 240) {
        // Latch control register and scroll position at start of scanline
        if (cycle == 0) {
            scanlineCtrl[scanline] = ppuCtrl;
            lineV[scanline] = currentAddress;
            lineFineX[scanline] = fineX;
            // Keep the legacy per-line scroll arrays meaningful for code
            // that still reads them (sprite-0 helpers)
            scanlineScrollX[scanline] = ((currentAddress & 0x1F) << 3) | fineX;
            scanlineScrollY[scanline] = (((currentAddress >> 5) & 0x1F) << 3) | ((currentAddress >> 12) & 7);
            scanlineCtrl[scanline] = (ppuCtrl & ~0x03) | ((currentAddress >> 10) & 0x03);
        }
        
        // Render the scanline at cycle 256 (end of visible portion)
        if (cycle == 256) {
            renderScanline(scanline, mapper);
            if (renderingOn) scrollIncrementY();
        }
        if (cycle == 257 && renderingOn) {
            scrollCopyX();
        }
        
        // Sprite 0 hit: raised when the PPU reaches the first overlapping
        // opaque pixel (dot x+1 for pixel x)
        if (cycle == 0) {
            sprite0HitX = sprite0Hit ? -1 : findSprite0HitX(scanline);
        }
        if (sprite0HitX >= 0 && cycle == sprite0HitX + 1) {
            sprite0Hit = true;
            ppuStatus |= 0x40;
            sprite0HitX = -1;
        }
        
        return;
    }
    
    // VBlank scanlines (241-260)
    if (scanline == 241) {
        if (cycle == 1) {
            ppuStatus |= 0x80;   // Set VBlank flag
            inVBlank = true;
            frameComplete = true;
            captureFrameScroll();
            // NMI would be triggered here if enabled (ppuCtrl & 0x80)
        }
        return;
    }
    
    // Handle other VBlank scanlines (242-260)
    if (scanline >= 242 && scanline <= 260) {
        // Nothing special happens during most VBlank scanlines
        return;
    }
    
    ppuCycles++;
}

void PPU::clearScanline(int scanline) {
    // Fill scanline with background color
    uint8_t bgColorIndex = palette[0];
    uint32_t bgColor32 = paletteRGB[bgColorIndex];
    uint16_t bgColor16 = ((bgColor32 & 0xF80000) >> 8) | 
                        ((bgColor32 & 0x00FC00) >> 5) | 
                        ((bgColor32 & 0x0000F8) >> 3);
    
    uint16_t* scanlineStart = &frameBuffer[scanline * 256];
    uint8_t* maskStart = &backgroundMask[scanline * 256];
    
    for (int x = 0; x < 256; x++) {
        scanlineStart[x] = bgColor16;
        maskStart[x] = 1;  // Mark as transparent by default
    }
}

void PPU::checkSprite0HitScanline(int scanline) {
    if (sprite0Hit) return;  // Already hit this frame
    if (!(ppuMask & 0x18)) return;  // Both sprite and background rendering must be enabled
    
    // Only check visible scanlines
    if (scanline < 0 || scanline >= 240) return;
    
    // Get sprite 0 properties
    uint8_t sprite0Y = oam[0];
    uint8_t sprite0Tile = oam[1];
    uint8_t sprite0Attr = oam[2];
    uint8_t sprite0X = oam[3];
    
    // Skip if sprite 0 is off-screen
    if (sprite0Y >= 0xEF || sprite0X >= 0xF9) return;
    
    // Check if sprite 0 is on this scanline (accounting for 1-line delay)
    int height = spriteHeight();
    if (scanline < sprite0Y + 1 || scanline >= sprite0Y + 1 + height) return;
    
    // Calculate sprite row for this scanline
    int spriteRow = scanline - (sprite0Y + 1);
    
    // Handle vertical flip
    if (sprite0Attr & 0x80) {
        spriteRow = height - 1 - spriteRow;
    }
    
    // Get sprite 0 pattern data for this row
    uint16_t spriteAddr = spritePatternAddr(sprite0Tile, spriteRow);
    uint8_t spriteLo = peekCHR(spriteAddr);
    uint8_t spriteHi = peekCHR(spriteAddr + 8);
    
    // Check each pixel in the sprite row for collision
    for (int col = 0; col < 8; col++) {
        int screenX = sprite0X + col;
        
        // Skip if pixel is off-screen
        if (screenX >= 256) break;
        if (screenX < 0) continue;
        
        // Calculate sprite pixel column (handle horizontal flip)
        int spriteCol = col;
        if (sprite0Attr & 0x40) {
            spriteCol = 7 - col;
        }
        
        // FIX: Use consistent bit extraction with sprite rendering
        uint8_t spritePixel = 0;
        if (spriteLo & (0x80 >> spriteCol)) spritePixel |= 1;
        if (spriteHi & (0x80 >> spriteCol)) spritePixel |= 2;
        
        // Skip if sprite pixel is transparent
        if (spritePixel == 0) continue;
        
        // Get background pixel at this location - inline the logic
        int scrollX = (scanline < 240 && scanlineScrollX[scanline] != 0) ? scanlineScrollX[scanline] : frameScrollX;
        int scrollY = (scanline < 240 && scanlineScrollY[scanline] != 0) ? scanlineScrollY[scanline] : frameScrollY;
        
        // Calculate background tile position
        int worldX = screenX + scrollX;
        int worldY = scanline + scrollY;
        
        int tileX = worldX / 8;
        int tileY = worldY / 8;
        int pixelX = worldX % 8;
        int pixelY = worldY % 8;
        
        // Determine which nametable to use
        uint16_t nametableAddr = 0x2000;
        int localTileX = tileX % 32;
        int localTileY = tileY % 30;
        
        // Handle nametable mirroring
        if (ppuCtrl & 0x01) {  // Base nametable bit
            nametableAddr = 0x2400;
        }
        
        // Handle horizontal nametable wrapping
        if (tileX >= 32) {
            nametableAddr = (nametableAddr == 0x2000) ? 0x2400 : 0x2000;
            localTileX = tileX - 32;
        }
        
        // Handle vertical nametable wrapping
        if (tileY >= 30) {
            if (ppuCtrl & 0x02) {
                nametableAddr += (nametableAddr < 0x2800) ? 0x800 : -0x800;
            }
            localTileY = tileY - 30;
        }
        
        // Bounds check
        if (localTileX < 0 || localTileX >= 32 || localTileY < 0 || localTileY >= 30) {
            continue; // Skip out-of-bounds pixels
        }
        
        // Get background tile data
        uint16_t tileAddr = nametableAddr + (localTileY * 32) + localTileX;
        uint8_t bgTileIndex = readByte(tileAddr);
        
        // Get background pattern data
        uint16_t bgPatternBase = bgTileIndex * 16;
        if (ppuCtrl & 0x10) {  // Background pattern table select
            bgPatternBase += 0x1000;
        }
        
        uint8_t bgLo = peekCHR(bgPatternBase + pixelY);
        uint8_t bgHi = peekCHR(bgPatternBase + pixelY + 8);
        
        // Extract background pixel value
        uint8_t bgPixel = 0;
        if (bgLo & (0x80 >> pixelX)) bgPixel |= 1;
        if (bgHi & (0x80 >> pixelX)) bgPixel |= 2;
        
        // Sprite 0 hit occurs when both sprite and background pixels are non-transparent
        if (bgPixel != 0) {
            sprite0Hit = true;
            ppuStatus |= 0x40;
            return;
        }
    }
}

void PPU::renderScanline(int scanline, int mapper) {
    if (scanline < 0 || scanline >= 240) return;
    
    // Clear scanline with background color
    uint8_t bgColorIndex = palette[0];
    uint32_t bgColor32 = paletteRGB[bgColorIndex];
    uint16_t bgColor16 = ((bgColor32 & 0xF80000) >> 8) | 
                        ((bgColor32 & 0x00FC00) >> 5) | 
                        ((bgColor32 & 0x0000F8) >> 3);
    
    for (int x = 0; x < 256; x++) {
        frameBuffer[scanline * 256 + x] = bgColor16;
    }
    
    // Render background first
    if (ppuMask & 0x08) {
        renderBackgroundScanline(scanline);
    }
    
    // Render sprites the correct way: in order 0-63, but only draw if no higher priority sprite already drew there
    if (ppuMask & 0x10) {
        // Clear sprite priority mask for this scanline
        memset(&spritePriorityMask[scanline * 256], 0, 256);
        
        // Render sprites in hardware order (0 to 63). The PPU only performs
        // pattern fetches for the first 8 sprites on a line; only those
        // fetches are visible to the mapper (matters for MMC2 latches).
        int height = spriteHeight();
        int fetched = 0;
        engine.setMMC5SpriteFetch(true);
        for (int spriteIndex = 0; spriteIndex < 64; spriteIndex++) {
            uint8_t spriteY = oam[spriteIndex * 4];
            bool onLine = scanline >= spriteY + 1 && scanline < spriteY + 1 + height;
            bool realFetch = onLine && fetched < 8;
            if (realFetch) fetched++;
            uint8_t attributes = oam[spriteIndex * 4 + 2];
            bool behindBackground = (attributes & 0x20) != 0;
            renderSingleSprite(scanline, spriteIndex, behindBackground, realFetch);
        }

        // Unused sprite slots still fetch tile $FF. In 8x16 mode that hits
        // $1FE8, which resets MMC2 latch 1 to $FE every scanline; Punch-Out!!
        // depends on this.
        engine.setMMC5SpriteFetch(false);
        if (engine.isMMC2Family()) {
            for (int slot = fetched; slot < 8; slot++) {
                uint16_t addr = spritePatternAddr(0xFF, 0);
                readCHR(addr);
                readCHR(addr + 8);
            }
        }
    }
}

void PPU::renderSingleSprite(int scanline, int spriteIndex, bool behindBackground, bool mapperVisibleFetch) {
    uint8_t spriteY = oam[spriteIndex * 4];
    uint8_t tileIndex = oam[spriteIndex * 4 + 1];
    uint8_t attributes = oam[spriteIndex * 4 + 2];
    uint8_t spriteX = oam[spriteIndex * 4 + 3];
    
    int height = spriteHeight();
    if (scanline < spriteY + 1 || scanline >= spriteY + 1 + height) return;
    if (spriteY >= 0xEF) return;
    
    int spriteRow = scanline - (spriteY + 1);
    if (attributes & 0x80) spriteRow = height - 1 - spriteRow;
    
    uint16_t patternAddr = spritePatternAddr(tileIndex, spriteRow);
    uint8_t patternLo, patternHi;
    if (mapperVisibleFetch) {
        patternLo = readCHR(patternAddr);
        patternHi = readCHR(patternAddr + 8);
    } else {
        patternLo = peekCHR(patternAddr);
        patternHi = peekCHR(patternAddr + 8);
    }
    
    for (int pixelX = 0; pixelX < 8; pixelX++) {
        uint8_t paletteIndex = 0;
        if (patternLo & (0x80 >> pixelX)) paletteIndex |= 1;
        if (patternHi & (0x80 >> pixelX)) paletteIndex |= 2;
        
        if (paletteIndex == 0) continue; // Transparent
        
        int xPixel = spriteX + ((attributes & 0x40) ? (7 - pixelX) : pixelX);
        if (xPixel < 0 || xPixel >= 256) continue;
        if (xPixel < 8 && !(ppuMask & 0x04)) continue;  // Left-edge sprite clipping
        
        int bufferIndex = scanline * 256 + xPixel;
        
        // Check if a higher priority sprite already drew here
        if (spritePriorityMask[bufferIndex]) continue;
        
        // Mark this pixel as drawn by a sprite
        spritePriorityMask[bufferIndex] = 1;
        
        uint8_t colorIndex = palette[0x10 + (attributes & 0x03) * 4 + paletteIndex];
        uint32_t color32 = paletteRGB[colorIndex];
        uint16_t spritePixel = ((color32 & 0xF80000) >> 8) | 
                              ((color32 & 0x00FC00) >> 5) | 
                              ((color32 & 0x0000F8) >> 3);
        
        if (behindBackground) {
            // Only draw if background pixel is transparent
            if (backgroundMask[bufferIndex] == 1) {
                frameBuffer[bufferIndex] = spritePixel;
            }
        } else {
            // Always draw (sprite in front)
            frameBuffer[bufferIndex] = spritePixel;
        }
    }
}

/*void PPU::catchUp(uint64_t targetCycles)
{
    // Safety check to prevent infinite loops
    if (ppuCycles >= targetCycles || (targetCycles - ppuCycles) > 100000) {
        return;
    }
    
    while (ppuCycles < targetCycles) {
        // Calculate current position in frame
        uint64_t framePos = ppuCycles % (CYCLES_PER_SCANLINE * TOTAL_SCANLINES);
        int scanline = framePos / CYCLES_PER_SCANLINE;
        int cycle = framePos % CYCLES_PER_SCANLINE;
        
        // Step one PPU cycle
        stepCycle(scanline, cycle);
        ppuCycles++;
        
        // Handle frame transitions
        if (scanline == PRERENDER_SCANLINE && cycle == CYCLES_PER_SCANLINE - 1) {
            // End of frame
            frameOdd = !frameOdd;
            
            // Odd frame cycle skip when rendering enabled
            if (frameOdd && (ppuMask & 0x18)) {
                ppuCycles++; // Skip one cycle
            }
        }
        
        // Safety valve
        if ((ppuCycles % 1000) == 0) {
            break;
        }
    }
}*/

void PPU::handleVBlankStart()
{
    inVBlank = true;
    ppuStatus |= 0x80;  // Set VBlank flag
    
    // Capture scroll values for next frame
    captureFrameScroll();
    
    // NMI will be handled by the emulator if enabled
}

void PPU::handleVBlankEnd()
{
    inVBlank = false;
    ppuStatus &= 0x7F;  // Clear VBlank flag
    sprite0Hit = false; // Clear sprite 0 hit
    ppuStatus &= 0xBF;  // Clear sprite 0 hit flag
}

void PPU::handleSpriteEvaluation()
{
    // Simplified sprite evaluation for cycle accuracy
    // Real hardware does complex sprite evaluation here
    // For now, just mark that sprite evaluation is happening
    
    // This is where sprite 0 hit detection would be more accurate
    // but we'll keep the simplified version for compatibility
}

void PPU::handleBackgroundFetch()
{
    // Background tile fetching happens in 8-cycle groups
    int fetchCycle = currentCycle % 8;
    
    switch (fetchCycle) {
        case 1: // Fetch nametable byte
            // This is where pattern table banking matters for MMC3
            break;
            
        case 3: // Fetch attribute byte
            break;
            
        case 5: // Fetch pattern table low byte
            // This triggers A12 line changes for MMC3 IRQ
            break;
            
        case 7: // Fetch pattern table high byte
            // This also triggers A12 line changes
            break;
    }
}

void PPU::checkSprite0Hit()
{
    // Don't check if already hit this frame
    if (sprite0Hit) return;
    
    // Don't check if rendering is disabled
    if (!(ppuMask & 0x18)) return; // Both sprite and background rendering must be enabled
    
    // Don't check during VBlank or pre-render scanline
    if (currentScanline < 0 || currentScanline >= 240) return;
    
    // Don't check if we're not in the visible area
    if (currentCycle < 1 || currentCycle > 256) return;
    
    // Get sprite 0 properties
    uint8_t sprite0_y = oam[0];
    uint8_t sprite0_tile = oam[1];
    uint8_t sprite0_attr = oam[2];
    uint8_t sprite0_x = oam[3];
    
    // Skip if sprite 0 is off-screen
    if (sprite0_y >= 0xEF || sprite0_x >= 0xF9) return;
    
    // Sprite coordinates are delayed by 1 scanline
    sprite0_y++;
    
    // Check if current scanline intersects with sprite 0
    if (currentScanline < sprite0_y || currentScanline >= sprite0_y + 8) return;
    
    // Check if current cycle intersects with sprite 0
    if (currentCycle < sprite0_x || currentCycle >= sprite0_x + 8) return;
    
    // Calculate pixel position within sprite 0
    int spriteRow = currentScanline - sprite0_y;
    int spriteCol = currentCycle - sprite0_x;
    
    // Handle vertical flip
    if (sprite0_attr & 0x80) {
        spriteRow = 7 - spriteRow;
    }
    
    // Handle horizontal flip
    if (sprite0_attr & 0x40) {
        spriteCol = 7 - spriteCol;
    }
    
    // Get sprite 0 pattern data
    uint16_t patternBase = sprite0_tile * 16;
    if (ppuCtrl & 0x08) {  // Sprite pattern table select
        patternBase += 0x1000;
    }
    
    uint8_t spriteLo = peekCHR(patternBase + spriteRow);
    uint8_t spriteHi = peekCHR(patternBase + spriteRow + 8);
    
    // Extract sprite pixel value
    uint8_t spritePixel = 0;
    if (spriteLo & (1 << spriteCol)) spritePixel |= 1;
    if (spriteHi & (1 << spriteCol)) spritePixel |= 2;
    
    // Skip if sprite pixel is transparent
    if (spritePixel == 0) return;
    
    // Now check background pixel at the same location
    int bgX = currentCycle - 1; // Convert cycle to pixel coordinate
    int bgY = currentScanline;
    
    // Calculate background tile position
    int scrollX = frameScrollX;
    int scrollY = frameScrollY;
    
    int worldX = bgX + scrollX;
    int worldY = bgY + scrollY;
    
    int tileX = worldX / 8;
    int tileY = worldY / 8;
    int pixelX = worldX % 8;
    int pixelY = worldY % 8;
    
    // Determine which nametable to use
    uint16_t nametableAddr = 0x2000;
    int localTileX = tileX % 32;
    int localTileY = tileY % 30;
    
    // Handle nametable mirroring
    if (ppuCtrl & 0x01) {  // Base nametable bit
        nametableAddr = 0x2400;
    }
    
    // Handle horizontal nametable wrapping
    if (tileX >= 32) {
        nametableAddr = (nametableAddr == 0x2000) ? 0x2400 : 0x2000;
        localTileX = tileX - 32;
    }
    
    // Handle vertical nametable wrapping
    if (tileY >= 30) {
        // Switch to lower nametable
        if (ppuCtrl & 0x02) {
            nametableAddr += (nametableAddr < 0x2800) ? 0x800 : -0x800;
        }
        localTileY = tileY - 30;
    }
    
    // Get background tile data
    uint16_t tileAddr = nametableAddr + (localTileY * 32) + localTileX;
    uint8_t bgTileIndex = readByte(tileAddr);
    
    // Get background pattern data
    uint16_t bgPatternBase = bgTileIndex * 16;
    if (ppuCtrl & 0x10) {  // Background pattern table select
        bgPatternBase += 0x1000;
    }
    
    uint8_t bgLo = peekCHR(bgPatternBase + pixelY);
    uint8_t bgHi = peekCHR(bgPatternBase + pixelY + 8);
    
    // Extract background pixel value
    uint8_t bgPixel = 0;
    if (bgLo & (0x80 >> pixelX)) bgPixel |= 1;
    if (bgHi & (0x80 >> pixelX)) bgPixel |= 2;
    
    // Sprite 0 hit occurs when both sprite and background pixels are non-transparent
    if (spritePixel != 0 && bgPixel != 0) {
        sprite0Hit = true;
        ppuStatus |= 0x40; // Set sprite 0 hit flag
    }
}

void PPU::stepScanline()
{
    // Called at the end of each scanline
    currentCycle = 0;
    currentScanline++;
    
    if (currentScanline >= TOTAL_SCANLINES) {
        currentScanline = 0;
        frameOdd = !frameOdd;
    }
}

uint16_t PPU::getCurrentPixelColor(int x, int y) {
    // Bounds check
    if (x < 0 || x >= 256 || y < 0 || y >= 240) {
        return 0x0000; // Return black for out-of-bounds
    }
    
    // If rendering is disabled, return background color
    if (!(ppuMask & 0x18)) {
        uint8_t bgColorIndex = palette[0];
        uint32_t bgColor32 = paletteRGB[bgColorIndex];
        return ((bgColor32 & 0xF80000) >> 8) | ((bgColor32 & 0x00FC00) >> 5) | ((bgColor32 & 0x0000F8) >> 3);
    }
    
    // Start with background color
    uint8_t bgColorIndex = palette[0];
    uint32_t finalColor32 = paletteRGB[bgColorIndex];
    uint16_t finalPixel = ((finalColor32 & 0xF80000) >> 8) | ((finalColor32 & 0x00FC00) >> 5) | ((finalColor32 & 0x0000F8) >> 3);
    
    // Render background if enabled
    if (ppuMask & 0x08) {
        // Get background pixel color
        uint16_t bgPixel = getBackgroundPixelColor(x, y);
        if (bgPixel != finalPixel) {  // Non-transparent background
            finalPixel = bgPixel;
        }
    }
    
    // Render sprites if enabled (check all sprites for this pixel)
    if (ppuMask & 0x10) {
        // Check sprites in reverse priority order (highest priority last)
        for (int spriteIndex = 63; spriteIndex >= 0; spriteIndex--) {
            uint8_t spriteY = oam[spriteIndex * 4];
            uint8_t tileIndex = oam[spriteIndex * 4 + 1];
            uint8_t attributes = oam[spriteIndex * 4 + 2];
            uint8_t spriteX = oam[spriteIndex * 4 + 3];
            
            // Skip sprites not covering this pixel
            if (x < spriteX || x >= spriteX + 8) continue;
            if (y < spriteY + 1 || y >= spriteY + 9) continue;  // +1 for sprite delay
            
            // Skip off-screen sprites
            if (spriteY >= 0xEF || spriteX >= 0xF9) continue;
            
            // Get sprite pixel color
            uint16_t spritePixel = getSpritePixelColor(x, y, spriteIndex);
            
            if (spritePixel != 0) {  // Non-transparent sprite pixel
                bool behindBackground = (attributes & 0x20) != 0;
                
                // Check background collision for priority
                if (!behindBackground) {
                    // Sprite in front - always visible
                    finalPixel = spritePixel;
                } else {
                    // Sprite behind background - only visible if background is transparent
                    uint32_t bgColor32 = paletteRGB[palette[0]];
                    uint16_t bgColor16 = ((bgColor32 & 0xF80000) >> 8) | ((bgColor32 & 0x00FC00) >> 5) | ((bgColor32 & 0x0000F8) >> 3);
                    
                    if (finalPixel == bgColor16) {  // Background is transparent
                        finalPixel = spritePixel;
                    }
                }
            }
        }
    }
    
    return finalPixel;
}

uint16_t PPU::getBackgroundPixelColor(int x, int y) {
    // Get current scroll values
    int scrollX = frameScrollX;
    uint8_t ctrl = frameCtrl;
        
    // Calculate tile position
    int worldX = x + scrollX;
    int tileX = worldX / 8;
    int tileY = y / 8;
    int pixelX = worldX % 8;
    int pixelY = y % 8;
    
    // Determine nametable
    uint16_t nametableAddr = 0x2000;
    int localTileX = tileX % 32;
    
    if (ctrl & 0x01) {  // Base nametable bit
        nametableAddr = 0x2400;
    }
    
    // Handle horizontal nametable wrapping
    if (tileX >= 32) {
        nametableAddr = (nametableAddr == 0x2000) ? 0x2400 : 0x2000;
        localTileX = tileX - 32;
    }
    
    // Get tile data
    uint16_t tileAddr = nametableAddr + (tileY * 32) + localTileX;
    uint8_t tileIndex = readByte(tileAddr);
    uint8_t attribute = getAttributeTableValue(tileAddr);
    
    // Get pattern data
    uint16_t patternBase = tileIndex * 16;
    if (ctrl & 0x10) {  // Background pattern table select
        patternBase += 0x1000;
    }
    
    uint8_t patternLo = peekCHR(patternBase + pixelY);
    uint8_t patternHi = peekCHR(patternBase + pixelY + 8);
    
    // Extract pixel value
    uint8_t pixelValue = 0;
    if (patternLo & (0x80 >> pixelX)) pixelValue |= 1;
    if (patternHi & (0x80 >> pixelX)) pixelValue |= 2;
    
    // Get final color
    uint8_t colorIndex;
    if (pixelValue == 0) {
        colorIndex = palette[0];  // Background color
    } else {
        colorIndex = palette[(attribute & 0x03) * 4 + pixelValue];
    }
    
    uint32_t color32 = paletteRGB[colorIndex];
    return ((color32 & 0xF80000) >> 8) | ((color32 & 0x00FC00) >> 5) | ((color32 & 0x0000F8) >> 3);
}

uint16_t PPU::getSpritePixelColor(int x, int y, int spriteIndex) {
    uint8_t spriteY = oam[spriteIndex * 4];
    uint8_t tileIndex = oam[spriteIndex * 4 + 1];
    uint8_t attributes = oam[spriteIndex * 4 + 2];
    uint8_t spriteX = oam[spriteIndex * 4 + 3];
    
    // Calculate pixel position within sprite
    int pixelX = x - spriteX;
    int pixelY = y - (spriteY + 1);  // +1 for sprite delay
    
    // Handle flipping
    bool flipX = (attributes & 0x40) != 0;
    bool flipY = (attributes & 0x80) != 0;
    
    if (flipX) pixelX = 7 - pixelX;
    if (flipY) pixelY = 7 - pixelY;
    
    // Get pattern data
    uint16_t patternBase = tileIndex * 16;
    if (ppuCtrl & 0x08) {  // Sprite pattern table select
        patternBase += 0x1000;
    }
    
    uint8_t patternLo = peekCHR(patternBase + pixelY);
    uint8_t patternHi = peekCHR(patternBase + pixelY + 8);
    
    // Extract pixel value - match the working render logic exactly
    uint8_t paletteIndex = 0;
    if (patternLo & (1 << pixelX)) paletteIndex |= 1;
    if (patternHi & (1 << pixelX)) paletteIndex |= 2;
    
    // Return 0 for transparent pixels
    if (paletteIndex == 0) return 0;
    
    // Get sprite color
    uint8_t colorIndex = palette[0x10 + (attributes & 0x03) * 4 + paletteIndex];
    uint32_t color32 = paletteRGB[colorIndex];
    return ((color32 & 0xF80000) >> 8) | ((color32 & 0x00FC00) >> 5) | ((color32 & 0x0000F8) >> 3);
}
