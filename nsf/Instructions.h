#ifndef INSTRUCTIONS_HPP
#define INSTRUCTIONS_HPP

#include <fstream>
#include <iostream>
#include <string>
#include <cstdint>
#include "APU.h"

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
    // Memory access (these need to be implemented elsewhere)
    uint8_t readByte(uint16_t address);
    void writeByte(uint16_t address, uint8_t value);

    // Memory access helpers
    uint16_t readWord(uint16_t address);
    void writeWord(uint16_t address, uint16_t value);

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
