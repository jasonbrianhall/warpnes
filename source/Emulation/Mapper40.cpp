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

void WarpNES::writeMapper40Register(uint16_t address, uint8_t value) {
    // Mapper 40 (NTDEC 2722) register writes
    // This is a discrete logic implementation, not an ASIC
    
    switch (address & 0xE000) {
        case 0x8000: // $8000-$9FFF: IRQ control
            if (address & 0x1000) {
                // $9000-$9FFF: IRQ disable and acknowledge
                mapper40.irqEnable = false;
                mapper40.irqPending = false;
                
                static int irqDisableCount = 0;
                if (irqDisableCount < 5) {
                    printf("Mapper 40: IRQ disabled at $%04X\n", address);
                    irqDisableCount++;
                }
            } else {
                // $8000-$8FFF: IRQ enable and reset counter
                mapper40.irqEnable = true;
                mapper40.irqCounter = 0x1000; // 4096 cycles (CD4020 13-bit counter)
                mapper40.irqPending = false;
                
                static int irqEnableCount = 0;
                if (irqEnableCount < 5) {
                    printf("Mapper 40: IRQ enabled at $%04X, counter reset to %d\n", 
                           address, mapper40.irqCounter);
                    irqEnableCount++;
                }
            }
            break;
            
        case 0xA000: // $A000-$BFFF: Unused on NTDEC 2722
        case 0xC000: // $C000-$DFFF: Unused on NTDEC 2722
            // These ranges do nothing on the discrete logic implementation
            break;
            
        case 0xE000: // $E000-$FFFF: PRG bank select
            {
                uint8_t oldBank = mapper40.prgBank;
                mapper40.prgBank = value & 0x07; // 3 bits for PRG bank (8 possible banks)
                
                static int bankSwitchCount = 0;
                if (bankSwitchCount < 10 || oldBank != mapper40.prgBank) {
                    printf("Mapper 40: PRG bank switch from %d to %d at $%04X\n", 
                           oldBank, mapper40.prgBank, address);
                    bankSwitchCount++;
                }
            }
            break;
            
        default:
            printf("Mapper 40: Unexpected write $%02X to $%04X\n", value, address);
            break;
    }
}

void WarpNES::stepMapper40IRQ() {
    if (!mapper40.irqEnable) return;
    
    // CD4020 is a 13-bit counter that decrements every CPU cycle
    if (mapper40.irqCounter > 0) {
        mapper40.irqCounter--;
        
        if (mapper40.irqCounter == 0) {
            mapper40.irqPending = true;
            
            static int irqTriggerCount = 0;
            if (irqTriggerCount < 10) {
                printf("Mapper 40: IRQ triggered! (count: %d)\n", irqTriggerCount + 1);
                irqTriggerCount++;
            }
        }
    }
}

void WarpNES::checkMapper40IRQ() {
    if (mapper40.irqPending && !getFlag(FLAG_INTERRUPT)) {
        mapper40.irqPending = false;
        
        // Standard 6502 IRQ sequence
        pushWord(regPC);
        pushByte(regP & ~FLAG_BREAK);
        setFlag(FLAG_INTERRUPT, true);
        
        // Jump to IRQ vector
        regPC = readWord(0xFFFE);
        
        totalCycles += 7; // IRQ takes 7 cycles
        frameCycles += 7;
        
        static int irqHandledCount = 0;
        if (irqHandledCount < 5) {
            printf("Mapper 40: IRQ handled, jumping to $%04X\n", regPC);
            irqHandledCount++;
        }
    }
}


