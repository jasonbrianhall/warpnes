#ifndef INSTRUCTIONS_HPP
#define INSTRUCTIONS_HPP

#include <fstream>
#include <iostream>
#include <string>
#include <cstdint>
#include <cstring>
#include "APU.h"

// Forward declarations
class APU;

// Mapper state structures (simplified for NSF - most aren't needed)
struct MMC1State {
    uint8_t control = 0x0C;
    uint8_t shiftRegister = 0x10;
    uint8_t shiftCount = 0;
    uint8_t prgBank = 0;
    uint8_t chrBank0 = 0;
    uint8_t chrBank1 = 0;
    uint8_t currentPRGBank = 0;
    uint8_t currentCHRBank0 = 0;
    uint8_t currentCHRBank1 = 0;
};

struct UxROMState {
    uint8_t prgBank = 0;
};

struct CNROMState {
    uint8_t chrBank = 0;
};

struct MMC3State {
    uint8_t bankSelect = 0;
    uint8_t bankData[8] = {0};
    uint8_t currentPRGBanks[4] = {0};
    uint8_t currentCHRBanks[8] = {0};
    bool irqEnable = false;
    bool irqPending = false;
    uint8_t irqCounter = 0;
    uint8_t irqReload = 0;
};

struct MMC2State {
    uint8_t prgBank = 0;
    uint8_t chrBank0 = 0;
    uint8_t chrBank1 = 0;
    uint8_t currentCHRBank0 = 0;
    uint8_t currentCHRBank1 = 0;
};

struct GxROMState {
    uint8_t prgBank = 0;
    uint8_t chrBank = 0;
};

struct Mapper40State {
    uint8_t prgBank = 0;
    uint16_t irqCounter = 0;
    bool irqEnable = false;
    bool irqPending = false;
};

// NES Header structure (simplified for NSF)
struct NESHeader {
    uint8_t prgROMPages = 0;
    uint8_t chrROMPages = 0;
    uint8_t mapper = 0;
    bool mirroring = false;
    bool battery = false;
    bool trainer = false;
};

class WarpNES {
private:
    // CPU Registers
    uint8_t regA;       // Accumulator
    uint8_t regX;       // X register
    uint8_t regY;       // Y register
    uint8_t regSP;      // Stack pointer
    uint16_t regPC;     // Program counter
    uint8_t regP;       // Status register

    // Cycle counters
    uint64_t totalCycles;
    uint32_t frameCycles;
    uint64_t masterCycles;

    // Memory
    uint8_t ram[2048];  // 2KB internal RAM
    
    // ROM data
    uint8_t* prgROM;
    uint32_t prgSize;
    bool romLoaded;
    
    // SRAM/Work RAM for NSF
    uint8_t* sram;
    uint32_t sramSize;
    bool sramEnabled;
    bool sramDirty;
    
    // NES Header
    NESHeader nesHeader;
    
    // Audio component (main focus for NSF)
    APU* apu;
    
    // Mapper states (simplified - most NSF don't use complex mappers)
    MMC1State mmc1;
    UxROMState uxrom;
    CNROMState cnrom;
    MMC3State mmc3;
    MMC2State mmc2;
    GxROMState gxrom;
    Mapper40State mapper40;
    
    // Other state
    bool nmiPending;

    // Status flag constants
    static const uint8_t FLAG_CARRY     = 0x01;
    static const uint8_t FLAG_ZERO      = 0x02;
    static const uint8_t FLAG_INTERRUPT = 0x04;
    static const uint8_t FLAG_DECIMAL   = 0x08;
    static const uint8_t FLAG_BREAK     = 0x10;
    static const uint8_t FLAG_UNUSED    = 0x20;
    static const uint8_t FLAG_OVERFLOW  = 0x40;
    static const uint8_t FLAG_NEGATIVE  = 0x80;

public:
    // Memory access
    uint8_t readByte(uint16_t address);
    void writeByte(uint16_t address, uint8_t value);
    
    // Memory access helpers
    uint16_t readWord(uint16_t address);
    void writeWord(uint16_t address, uint16_t value);
    
    // PRG ROM access
    uint8_t readPRGByte(uint16_t address);
    
    // Mapper functions (simplified for NSF)
    void writeMapperRegister(uint16_t address, uint8_t value);
    void writeMMC1Register(uint16_t address, uint8_t value);
    void writeUxROMRegister(uint16_t address, uint8_t value);
    void writeCNROMRegister(uint16_t address, uint8_t value);
    void writeMMC3Register(uint16_t address, uint8_t value);
    void writeMMC2Register(uint16_t address, uint8_t value);
    void writeMapper40Register(uint16_t address, uint8_t value);
    void writeGxROMRegister(uint16_t address, uint8_t value);
    
    // Bank update functions (stubs for NSF)
    void updateMMC1Banks();
    void updateMMC3Banks();
    void updateMMC2Banks();
    
    // IRQ functions (stubs for NSF)
    void checkMMC3IRQ(int scanline, int cycle);
    void checkMapper40IRQ();
    void stepMMC3A12Transition(bool a12High);
    void checkMMC2CHRLatch(uint16_t address, uint8_t tileID);
    
    // SRAM functions (for NSF work RAM)
    void initializeSRAM();
    void cleanupSRAM();
    void loadSRAM();
    void saveSRAM();

    // Stack operations
    void pushByte(uint8_t value);
    uint8_t pullByte();
    void pushWord(uint16_t value);
    uint16_t pullWord();

    // Instruction fetch
    uint8_t fetchByte();
    uint16_t fetchWord();

    // Status flag helpers
    void setFlag(uint8_t flag, bool value);
    bool getFlag(uint8_t flag) const;
    void updateZN(uint8_t value);

    // Addressing modes
    uint16_t addrImmediate();
    uint16_t addrZeroPage();
    uint16_t addrZeroPageX();
    uint16_t addrZeroPageY();
    uint16_t addrAbsolute();
    uint16_t addrAbsoluteX();
    uint16_t addrAbsoluteY();
    uint16_t addrIndirect();
    uint16_t addrIndirectX();
    uint16_t addrIndirectY();
    uint16_t addrRelative();

    // Instruction implementations
    void ADC(uint16_t addr);
    void AND(uint16_t addr);
    void ASL(uint16_t addr);
    void ASL_ACC();
    void BCC();
    void BCS();
    void BEQ();
    void BIT(uint16_t addr);
    void BMI();
    void BNE();
    void BPL();
    void BRK();
    void BVC();
    void BVS();
    void CLC();
    void CLD();
    void CLI();
    void CLV();
    void CMP(uint16_t addr);
    void CPX(uint16_t addr);
    void CPY(uint16_t addr);
    void DEC(uint16_t addr);
    void DEX();
    void DEY();
    void EOR(uint16_t addr);
    void INC(uint16_t addr);
    void INX();
    void INY();
    void JMP(uint16_t addr);
    void JSR(uint16_t addr);
    void LDA(uint16_t addr);
    void LDX(uint16_t addr);
    void LDY(uint16_t addr);
    void LSR(uint16_t addr);
    void LSR_ACC();
    void NOP();
    void ORA(uint16_t addr);
    void PHA();
    void PHP();
    void PLA();
    void PLP();
    void ROL(uint16_t addr);
    void ROL_ACC();
    void ROR(uint16_t addr);
    void ROR_ACC();
    void RTI();
    void RTS();
    void SBC(uint16_t addr);
    void SEC();
    void SED();
    void SEI();
    void STA(uint16_t addr);
    void STX(uint16_t addr);
    void STY(uint16_t addr);
    void TAX();
    void TAY();
    void TSX();
    void TXA();
    void TXS();
    void TYA();

    // Undocumented/illegal instructions
    void SHA(uint16_t addr);
    void SHX(uint16_t addr);
    void SHY(uint16_t addr);
    void TAS(uint16_t addr);
    void LAS(uint16_t addr);
    void ISC(uint16_t addr);
    void DCP(uint16_t addr);
    void LAX(uint16_t addr);
    void SAX(uint16_t addr);
    void SLO(uint16_t addr);
    void KIL();
    void RLA(uint16_t addr);
    void SRE(uint16_t addr);
    void RRA(uint16_t addr);
    void ANC(uint16_t addr);
    void ALR(uint16_t addr);
    void ARR(uint16_t addr);
    void XAA(uint16_t addr);
    void AXS(uint16_t addr);
};

#endif // INSTRUCTIONS_HPP
