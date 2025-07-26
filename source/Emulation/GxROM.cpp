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

void WarpNES::writeGxROMRegister(uint16_t address, uint8_t value) {
  uint8_t oldCHRBank = gxrom.chrBank;

  // Mapper 66: Write to $8000-$FFFF sets both PRG and CHR banks
  gxrom.prgBank = (value >> 4) & 0x03; // Bits 4-5
  gxrom.chrBank = value & 0x03;        // Bits 0-1

}
