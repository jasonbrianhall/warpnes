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

void WarpNES::writeUxROMRegister(uint16_t address, uint8_t value) {
  // UxROM: Any write to $8000-$FFFF sets the PRG bank
  // Only the lower bits are used (depends on ROM size)
  uint8_t totalBanks = prgSize / 0x4000; // Number of 16KB banks
  uint8_t bankMask = totalBanks - 1;     // Create mask for valid banks

  uxrom.prgBank = value & bankMask;

}
