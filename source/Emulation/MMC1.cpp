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
