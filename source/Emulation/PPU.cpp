#include "WarpNES.hpp"

#include "PPU.hpp"

ComprehensiveTileCache PPU::g_comprehensiveCache[512 * 8];
bool PPU::g_comprehensiveCacheInit = false;
std::vector<FlipCacheEntry> PPU::g_flipCache;
std::unordered_map<uint32_t, size_t> PPU::g_flipCacheIndex;
PPU::ScalingCache PPU::g_scalingCache;

static const uint8_t nametableMirrorLookup[][4] = {
    {0, 0, 1, 1}, // Vertical
    {0, 1, 0, 1}  // Horizontal
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

    if (!g_comprehensiveCacheInit) {
        memset(g_comprehensiveCache, 0, sizeof(g_comprehensiveCache));
        for (int i = 0; i < 512 * 8; i++) {
            g_comprehensiveCache[i].is_valid = false;
        }
        g_comprehensiveCacheInit = true;
    }
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
    
    return (nametableMirrorLookup[mode][table] * 0x400 + offset) % 2048;
}

uint8_t PPU::readByte(uint16_t address)
{
    // Mirror all addresses above $3fff
    address &= 0x3fff;

    if (address < 0x2000)
    {
        // CHR
        return engine.getCHR()[address];
    }
    else if (address < 0x3f00)
    {
        // Nametable
        return nametable[getNametableIndex(address)];
    }

    return 0;
}

void PPU::checkCHRLatch(uint16_t address, uint8_t tileID) {
    // Forward to the engine's CHR latch checking
    engine.checkCHRLatch(address, tileID);
}

uint8_t PPU::readCHR(int index)
{
    if (index < 0x2000)
    {
        // CRITICAL FIX: Check latch BEFORE reading data
        if (engine.getMapper() == 9) {
            engine.checkCHRLatch(index, 0);  // This may switch banks
        }
        
        // NOW read from the correct (possibly switched) bank
        uint8_t result = engine.readCHRData(index);        
        return result;
    }
    else
    {
        return 0;
    }
}

uint8_t PPU::readDataRegister()
{
    uint8_t value;
    
    if (currentAddress < 0x3F00) {
        // Normal VRAM - return buffered value
        value = vramBuffer;
        vramBuffer = readByte(currentAddress);
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
    sprite0Hit = false;
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



void PPU::renderTile(uint32_t* buffer, int index, int xOffset, int yOffset)
{
    // Lookup the pattern table entry
    uint16_t tile = readByte(index) + (ppuCtrl & (1 << 4) ? 256 : 0);
    uint8_t attribute = getAttributeTableValue(index);

    // Read the pixels of the tile
    for( int row = 0; row < 8; row++ )
    {
        uint8_t plane1 = readCHR(tile * 16 + row);
        uint8_t plane2 = readCHR(tile * 16 + row + 8);

        for( int column = 0; column < 8; column++ )
        {
            uint8_t paletteIndex = (((plane1 & (1 << column)) ? 1 : 0) + ((plane2 & (1 << column)) ? 2 : 0));
            uint8_t colorIndex = palette[attribute * 4 + paletteIndex];
            if( paletteIndex == 0 )
            {
                // skip transparent pixels
                //colorIndex = palette[0];
                continue;
            }
            uint32_t pixel = 0xff000000 | paletteRGB[colorIndex];

            int x = (xOffset + (7 - column));
            int y = (yOffset + row);
            if (x < 0 || x >= 256 || y < 0 || y >= 240)
            {
                continue;
            }
            buffer[y * 256 + x] = pixel;
        }
    }

}

void PPU::render(uint32_t* buffer)
{
    // Clear the buffer with the background color
    for (int index = 0; index < 256 * 240; index++)
    {
        buffer[index] = paletteRGB[palette[0]];
    }

    // Draw the background (nametable)
    if (ppuMask & (1 << 3)) // Is the background enabled?
    {
        int scrollX = (int)ppuScrollX + ((ppuCtrl & (1 << 0)) ? 256 : 0);
        int xMin = scrollX / 8;
        int xMax = ((int)scrollX + 256) / 8;

        for (int x = xMin; x <= xMax; x++)
        {
            for (int y = 4; y < 30; y++)
            {
                // Determine the index of the tile to render
                int index;
                if (x < 32)
                {
                    index = 0x2000 + 32 * y + x;
                }
                else if (x < 64)
                {
                    index = 0x2400 + 32 * y + (x - 32);
                }
                else
                {
                    index = 0x2800 + 32 * y + (x - 64);
                }

                // Render the tile
                renderTile(buffer, index, (x * 8) - (int)scrollX, (y * 8));
            }
        }
    }

    // Draw all sprites with proper priority handling
    if (ppuMask & (1 << 4)) // Are sprites enabled?
    {
        // Render sprites in reverse order (63 to 0) for proper priority
        // Lower index sprites have higher priority and are drawn last
        for (int i = 63; i >= 0; i--)
        {
            // Read OAM for the sprite
            uint8_t y          = oam[i * 4];
            uint8_t index      = oam[i * 4 + 1];
            uint8_t attributes = oam[i * 4 + 2];
            uint8_t x          = oam[i * 4 + 3];

            // Check if the sprite is visible
            if (y >= 0xef || x >= 0xf9)
            {
                continue;
            }

            // Increment y by one since sprite data is delayed by one scanline
            y++;

            // Determine the tile to use
            uint16_t tile = index + (ppuCtrl & (1 << 3) ? 256 : 0);
            bool flipX = attributes & (1 << 6);
            bool flipY = attributes & (1 << 7);
            bool behindBackground = attributes & (1 << 5);

            // Copy pixels to the framebuffer
            for (int row = 0; row < 8; row++)
            {
                uint8_t plane1 = readCHR(tile * 16 + row);
                uint8_t plane2 = readCHR(tile * 16 + row + 8);

                for (int column = 0; column < 8; column++)
                {
                    uint8_t paletteIndex = (((plane1 & (1 << column)) ? 1 : 0) + ((plane2 & (1 << column)) ? 2 : 0));
                    uint8_t colorIndex = palette[0x10 + (attributes & 0x03) * 4 + paletteIndex];
                    if (paletteIndex == 0)
                    {
                        // Skip transparent pixels
                        continue;
                    }
                    uint32_t pixel = 0xff000000 | paletteRGB[colorIndex];

                    int xOffset = 7 - column;
                    if (flipX)
                    {
                        xOffset = column;
                    }
                    int yOffset = row;
                    if (flipY)
                    {
                        yOffset = 7 - row;
                    }

                    int xPixel = (int)x + xOffset;
                    int yPixel = (int)y + yOffset;
                    if (xPixel < 0 || xPixel >= 256 || yPixel < 0 || yPixel >= 240)
                    {
                        continue;
                    }

                    // Priority handling
                    if (behindBackground)
                    {
                        // Sprite is behind background - only draw if background pixel is transparent
                        uint32_t backgroundPixel = buffer[yPixel * 256 + xPixel];
                        uint32_t backgroundColor = paletteRGB[palette[0]];
                        
                        if (backgroundPixel == backgroundColor)
                        {
                            buffer[yPixel * 256 + xPixel] = pixel;
                        }
                    }
                    else
                    {
                        // Sprite is in front of background - always draw
                        buffer[yPixel * 256 + xPixel] = pixel;
                        
                        if (i == 0 && index == 0xff && row == 5 && column > 3 && column < 6)
                        {
                            // Don't draw these specific pixels for the coin indicator
                            buffer[yPixel * 256 + xPixel] = paletteRGB[palette[0]];
                        }
                    }
                }
            }
        }
    }
}


void PPU::setPaletteRAM(uint8_t* data) {
    memcpy(palette, data, 32); 
    // Invalidate ALL tile caches when palette changes
    if (g_comprehensiveCacheInit) {
        memset(g_comprehensiveCache, 0, sizeof(g_comprehensiveCache));
        for (int i = 0; i < 512 * 8; i++) {
            g_comprehensiveCache[i].is_valid = false;
        }
        // Clear old flip cache
        g_flipCache.clear();
        g_flipCacheIndex.clear();
    }
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

void PPU::renderTile16(uint16_t* buffer, int index, int xOffset, int yOffset)
{
    uint16_t tile = readByte(index) + (ppuCtrl & (1 << 4) ? 256 : 0);
    uint8_t attribute = getAttributeTableValue(index);

    for (int row = 0; row < 8; row++)
    {
        uint8_t plane1 = readCHR(tile * 16 + row);
        uint8_t plane2 = readCHR(tile * 16 + row + 8);

        for (int column = 0; column < 8; column++)
        {
            uint8_t paletteIndex = (((plane1 & (1 << column)) ? 1 : 0) + ((plane2 & (1 << column)) ? 2 : 0));
            
            uint8_t colorIndex;
            if (paletteIndex == 0) {
                colorIndex = palette[0];  // Universal background color
            } else {
                colorIndex = palette[(attribute & 0x03) * 4 + paletteIndex];  // Mask attribute to 2 bits
            }
            
            uint32_t pixel32 = paletteRGB[colorIndex];
            uint16_t pixel16 = ((pixel32 & 0xF80000) >> 8) | ((pixel32 & 0x00FC00) >> 5) | ((pixel32 & 0x0000F8) >> 3);

            int x = (xOffset + (7 - column));
            int y = (yOffset + row);
            if (x >= 0 && x < 256 && y >= 0 && y < 240)
            {
                buffer[y * 256 + x] = pixel16;
            }
        }
    }
}

void PPU::writeAddressRegister(uint8_t value)
{
    if (!writeToggle)
    {
        // Upper byte
        currentAddress = (currentAddress & 0xff) | (((uint16_t)value << 8) & 0xff00);
    }
    else
    {
        // Lower byte
        currentAddress = (currentAddress & 0xff00) | (uint16_t)value;
    }
    writeToggle = !writeToggle;
}

void PPU::writeByte(uint16_t address, uint8_t value)
{
    // Mirror all addresses above $3fff
    address &= 0x3fff;

    if (address < 0x2000)
    {
        // CHR-RAM write
        engine.writeCHRData(address, value);
        // NO CACHE INVALIDATION - most games use CHR-ROM which never changes
    }
else if (address < 0x3f00)
{
    // Nametable write
    uint16_t index = getNametableIndex(address);
    
    // DEBUG: Log nametable writes
    nametable[index] = value;
}
    else if (address < 0x3f20)
    {
        // Palette data - ONLY invalidate if the palette actually changed
        uint8_t paletteIndex = address - 0x3f00;
        uint8_t oldPaletteValue = palette[paletteIndex];
        
        if (oldPaletteValue != value) {
            palette[paletteIndex] = value;

            // ONLY invalidate when palette changes (this is rare)
            if (g_comprehensiveCacheInit) {
                // Simple approach: invalidate ALL cache on any palette change
                // This is still rare enough to not hurt performance
                memset(g_comprehensiveCache, 0, sizeof(g_comprehensiveCache));
                for (int i = 0; i < 512 * 8; i++) {
                    g_comprehensiveCache[i].is_valid = false;
                }
                
                // Clear flip cache too
                g_flipCache.clear();
                g_flipCacheIndex.clear();
            }

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
        ppuCtrl = value;
        cachedCtrl = value;
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
        ppuScrollX = value;
        
        // Only set future scanlines, not all scanlines
        if (currentScanline >= 0 && currentScanline < 240) {
            // Set scroll for remaining scanlines in this frame
            for (int i = currentScanline; i < 240; i++) {
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

uint32_t PPU::getFlipCacheKey(uint16_t tile, uint8_t palette_type, uint8_t attribute, uint8_t flip_flags)
{
    // No need to include bank in key since each bank has its own flip cache
    return (tile << 16) | (palette_type << 8) | (attribute << 4) | flip_flags;
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

void PPU::convertNESToScreen32(uint16_t* nesBuffer, uint32_t* screenBuffer, int screenWidth, int screenHeight)
{
    // Convert 16-bit RGB565 to 32-bit RGBA
    for (int i = 0; i < screenWidth * screenHeight; i++) {
        uint16_t pixel16 = nesBuffer[i];
        
        // Extract RGB565 components
        int r = (pixel16 >> 11) & 0x1F;
        int g = (pixel16 >> 5) & 0x3F;
        int b = pixel16 & 0x1F;
        
        // Scale to 8-bit
        r = (r << 3) | (r >> 2);
        g = (g << 2) | (g >> 4);
        b = (b << 3) | (b >> 2);
        
        // Pack into 32-bit with alpha
        screenBuffer[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

void PPU::captureFrameScroll() {
    frameScrollX = ppuScrollX;
    frameScrollY = ppuScrollY;
    frameCtrl = ppuCtrl;
    
    // Initialize scanline arrays with current values
    for (int i = 0; i < 240; i++) {
        scanlineScrollX[i] = ppuScrollX;
        scanlineScrollY[i] = ppuScrollY;
        scanlineCtrl[i] = ppuCtrl;
    }
}

void PPU::stepCycle(int scanline, int cycle) {
    currentScanline = scanline;
    currentCycle = cycle;
    
    // Pre-render scanline (261)
    if (scanline == 261) {
        if (cycle == 1) {
            ppuStatus &= 0x7F;  // Clear VBlank
            ppuStatus &= 0xBF;  // Clear sprite 0 hit
            sprite0Hit = false;
            inVBlank = false;
            frameComplete = false;
            currentRenderScanline = 0;
        }
        
        if (cycle == 339 && frameOdd && (ppuMask & 0x18)) {
            frameOdd = !frameOdd;
            return;
        }
        
        if (cycle == 340) {
            frameOdd = !frameOdd;
        }
        return;
    }
    
    // Visible scanlines (0-239)
    if (scanline >= 0 && scanline < 240) {
        
        // Latch control register at start of scanline
        if (cycle == 0) {
            scanlineCtrl[scanline] = ppuCtrl;
            // DON'T override scanlineScrollX here - it should be set by register writes
            // Only set it if it hasn't been set by a mid-frame write
            // This preserves the scroll values set by the PPUSCROLL register writes
        }
        
        // Render the scanline at cycle 256 (end of visible portion)
        if (cycle == 256) {
            renderScanline(scanline);
        }
        
        // Sprite 0 hit detection
        if (cycle == 340) {
            checkSprite0HitScanline(scanline);
        }
        
        return;
    }
    
    // VBlank scanlines (241-260)
    if (scanline == 241) {
        if (cycle == 1) {
            ppuStatus |= 0x80;
            inVBlank = true;
            frameComplete = true;
            captureFrameScroll();
        }
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

void PPU::renderBackgroundScanline(int scanline) {
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
    if (scanline < sprite0Y + 1 || scanline >= sprite0Y + 9) return;
    
    // Calculate sprite row for this scanline
    int spriteRow = scanline - (sprite0Y + 1);
    
    // Handle vertical flip
    if (sprite0Attr & 0x80) {
        spriteRow = 7 - spriteRow;
    }
    
    // Get sprite 0 pattern data for this row
    uint16_t patternBase = sprite0Tile * 16;
    if (ppuCtrl & 0x08) {  // Sprite pattern table select
        patternBase += 0x1000;
    }
    
    uint8_t spriteLo = readCHR(patternBase + spriteRow);
    uint8_t spriteHi = readCHR(patternBase + spriteRow + 8);
    
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
        
        // Extract sprite pixel value
        uint8_t spritePixel = 0;
        if (spriteLo & (1 << spriteCol)) spritePixel |= 1;
        if (spriteHi & (1 << spriteCol)) spritePixel |= 2;
        
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
        
        uint8_t bgLo = readCHR(bgPatternBase + pixelY);
        uint8_t bgHi = readCHR(bgPatternBase + pixelY + 8);
        
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

void PPU::renderScanline(int scanline) {
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
    
    // Then render ALL sprites in one pass with proper priority
    if (ppuMask & 0x10) {
        for (int spriteIndex = 63; spriteIndex >= 0; spriteIndex--) {
            uint8_t attributes = oam[spriteIndex * 4 + 2];
            bool behindBackground = (attributes & 0x20) != 0;
            renderSingleSprite(scanline, spriteIndex, behindBackground);
        }
    }
}

void PPU::renderSingleSprite(int scanline, int spriteIndex, bool behindBackground) {
   uint8_t spriteY = oam[spriteIndex * 4];
   uint8_t tileIndex = oam[spriteIndex * 4 + 1];
   uint8_t attributes = oam[spriteIndex * 4 + 2];
   uint8_t spriteX = oam[spriteIndex * 4 + 3];
   
   if (scanline < spriteY + 1 || scanline >= spriteY + 9) return;
   if (spriteY >= 0xEF || spriteX >= 0xF9) return;
   
   int spriteRow = scanline - (spriteY + 1);
   if (attributes & 0x80) spriteRow = 7 - spriteRow;
   
   uint16_t patternBase = tileIndex * 16;
   if (ppuCtrl & 0x08) patternBase += 0x1000;
   
   uint8_t patternLo = readCHR(patternBase + spriteRow);
   uint8_t patternHi = readCHR(patternBase + spriteRow + 8);
   
   for (int pixelX = 0; pixelX < 8; pixelX++) {
       uint8_t paletteIndex = 0;
       if (patternLo & (0x80 >> pixelX)) paletteIndex |= 1;
       if (patternHi & (0x80 >> pixelX)) paletteIndex |= 2;
       
       if (paletteIndex == 0) continue;
       
       int xPixel = spriteX + ((attributes & 0x40) ? (7 - pixelX) : pixelX);
       
       if (xPixel < 0 || xPixel >= 256) continue;
       
       uint8_t colorIndex = palette[0x10 + (attributes & 0x03) * 4 + paletteIndex];
       uint32_t color32 = paletteRGB[colorIndex];
       uint16_t spritePixel = ((color32 & 0xF80000) >> 8) | 
                             ((color32 & 0x00FC00) >> 5) | 
                             ((color32 & 0x0000F8) >> 3);
       
if (behindBackground) {
    // For behind-background sprites, only draw if background pixel is palette index 0
    int bufferIndex = scanline * 256 + xPixel;
    if (bufferIndex >= 0 && bufferIndex < 256 * 240 && backgroundMask[bufferIndex] == 1) {
        frameBuffer[bufferIndex] = spritePixel;
    }
} else {
    frameBuffer[scanline * 256 + xPixel] = spritePixel;
}
   }
}

void PPU::catchUp(uint64_t targetCycles)
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
}

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
    
    uint8_t spriteLo = readCHR(patternBase + spriteRow);
    uint8_t spriteHi = readCHR(patternBase + spriteRow + 8);
    
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
    
    uint8_t bgLo = readCHR(bgPatternBase + pixelY);
    uint8_t bgHi = readCHR(bgPatternBase + pixelY + 8);
    
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
    
    uint8_t patternLo = readCHR(patternBase + pixelY);
    uint8_t patternHi = readCHR(patternBase + pixelY + 8);
    
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
    
    uint8_t patternLo = readCHR(patternBase + pixelY);
    uint8_t patternHi = readCHR(patternBase + pixelY + 8);
    
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
