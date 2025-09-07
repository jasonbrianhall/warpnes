#include <fstream>
#include <iostream>
#include <string>
#include "APU.h"
#include "Instructions.h"


uint16_t WarpNES::readWord(uint16_t address) {
  return readByte(address) | (readByte(address + 1) << 8);
}

void WarpNES::writeWord(uint16_t address, uint16_t value) {
  writeByte(address, value & 0xFF);
  writeByte(address + 1, value >> 8);
}

// Stack operations
void WarpNES::pushByte(uint8_t value) {
  writeByte(0x100 + regSP, value);
  regSP--;
}

uint8_t WarpNES::pullByte() {
  regSP++;
  return readByte(0x100 + regSP);
}

void WarpNES::pushWord(uint16_t value) {
  pushByte(value >> 8);
  pushByte(value & 0xFF);
}

uint16_t WarpNES::pullWord() {
  uint8_t lo = pullByte();
  uint8_t hi = pullByte();
  return lo | (hi << 8);
}

// Instruction fetch
uint8_t WarpNES::fetchByte() { return readByte(regPC++); }

uint16_t WarpNES::fetchWord() {
  uint16_t value = readWord(regPC);
  regPC += 2;
  return value;
}

// Status flag helpers
void WarpNES::setFlag(uint8_t flag, bool value) {
  if (value) {
    regP |= flag;
  } else {
    regP &= ~flag;
  }
}

bool WarpNES::getFlag(uint8_t flag) const { return (regP & flag) != 0; }

void WarpNES::updateZN(uint8_t value) {
  setFlag(FLAG_ZERO, value == 0);
  setFlag(FLAG_NEGATIVE, (value & 0x80) != 0);
}

// Addressing modes
uint16_t WarpNES::addrImmediate() { return regPC++; }

uint16_t WarpNES::addrZeroPage() { return fetchByte(); }

uint16_t WarpNES::addrZeroPageX() { return (fetchByte() + regX) & 0xFF; }

uint16_t WarpNES::addrZeroPageY() { return (fetchByte() + regY) & 0xFF; }

uint16_t WarpNES::addrAbsolute() { return fetchWord(); }

uint16_t WarpNES::addrAbsoluteX() { return fetchWord() + regX; }

uint16_t WarpNES::addrAbsoluteY() { return fetchWord() + regY; }

uint16_t WarpNES::addrIndirect() {
  uint16_t addr = fetchWord();
  // 6502 bug: if address is $xxFF, high byte is fetched from $xx00
  if ((addr & 0xFF) == 0xFF) {
    return readByte(addr) | (readByte(addr & 0xFF00) << 8);
  } else {
    return readWord(addr);
  }
}

uint16_t WarpNES::addrIndirectX() {
  uint8_t addr = (fetchByte() + regX) & 0xFF;
  return readByte(addr) | (readByte((addr + 1) & 0xFF) << 8);
}

uint16_t WarpNES::addrIndirectY() {
  uint8_t addr = fetchByte();
  uint16_t base = readByte(addr) | (readByte((addr + 1) & 0xFF) << 8);
  return base + regY;
}

uint16_t WarpNES::addrRelative() {
  int8_t offset = fetchByte();
  return regPC + offset;
}

// Instruction implementations
void WarpNES::ADC(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint16_t result = regA + value + (getFlag(FLAG_CARRY) ? 1 : 0);

  setFlag(FLAG_CARRY, result > 0xFF);
  setFlag(FLAG_OVERFLOW, ((regA ^ result) & (value ^ result) & 0x80) != 0);

  regA = result & 0xFF;
  updateZN(regA);
}

void WarpNES::AND(uint16_t addr) {
  regA &= readByte(addr);
  updateZN(regA);
}

void WarpNES::ASL(uint16_t addr) {
  uint8_t value = readByte(addr);
  setFlag(FLAG_CARRY, (value & 0x80) != 0);
  value <<= 1;
  writeByte(addr, value);
  updateZN(value);
}

void WarpNES::ASL_ACC() {
  setFlag(FLAG_CARRY, (regA & 0x80) != 0);
  regA <<= 1;
  updateZN(regA);
}

void WarpNES::BCC() {
  if (!getFlag(FLAG_CARRY)) {
    regPC = addrRelative();
  } else {
    regPC++; // Skip the offset byte
  }
}

void WarpNES::BCS() {
  if (getFlag(FLAG_CARRY)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void WarpNES::BEQ() {
  if (getFlag(FLAG_ZERO)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void WarpNES::BIT(uint16_t addr) {
  uint8_t value = readByte(addr);
  setFlag(FLAG_ZERO, (regA & value) == 0);
  setFlag(FLAG_OVERFLOW, (value & 0x40) != 0);
  setFlag(FLAG_NEGATIVE, (value & 0x80) != 0);
}

void WarpNES::BMI() {
  if (getFlag(FLAG_NEGATIVE)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void WarpNES::BNE() {
  if (!getFlag(FLAG_ZERO)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void WarpNES::BPL() {
  if (!getFlag(FLAG_NEGATIVE)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void WarpNES::BRK() {
  regPC++; // BRK is 2 bytes
  pushWord(regPC);
  pushByte(regP | FLAG_BREAK);
  setFlag(FLAG_INTERRUPT, true);
  regPC = readWord(0xFFFE); // IRQ vector
}

void WarpNES::BVC() {
  if (!getFlag(FLAG_OVERFLOW)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void WarpNES::BVS() {
  if (getFlag(FLAG_OVERFLOW)) {
    regPC = addrRelative();
  } else {
    regPC++;
  }
}

void WarpNES::CLC() { setFlag(FLAG_CARRY, false); }

void WarpNES::CLD() { setFlag(FLAG_DECIMAL, false); }

void WarpNES::CLI() { setFlag(FLAG_INTERRUPT, false); }

void WarpNES::CLV() { setFlag(FLAG_OVERFLOW, false); }

void WarpNES::CMP(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint8_t result = regA - value;
  setFlag(FLAG_CARRY, regA >= value);
  updateZN(result);
}

void WarpNES::CPX(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint8_t result = regX - value;
  setFlag(FLAG_CARRY, regX >= value);
  updateZN(result);
}

void WarpNES::CPY(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint8_t result = regY - value;
  setFlag(FLAG_CARRY, regY >= value);
  updateZN(result);
}

void WarpNES::DEC(uint16_t addr) {
  uint8_t value = readByte(addr) - 1;
  writeByte(addr, value);
  updateZN(value);
}

void WarpNES::DEX() {
  regX--;
  updateZN(regX);
}

void WarpNES::DEY() {
  regY--;
  updateZN(regY);
}

void WarpNES::EOR(uint16_t addr) {
  regA ^= readByte(addr);
  updateZN(regA);
}

void WarpNES::INC(uint16_t addr) {
  uint8_t value = readByte(addr) + 1;
  writeByte(addr, value);
  updateZN(value);
}

void WarpNES::INX() {
  regX++;
  updateZN(regX);
}

void WarpNES::INY() {
  regY++;
  updateZN(regY);
}

void WarpNES::JMP(uint16_t addr) { regPC = addr; }

void WarpNES::JSR(uint16_t addr) {
  pushWord(regPC - 1);
  regPC = addr;
}

void WarpNES::LDA(uint16_t addr) {
  regA = readByte(addr);
  updateZN(regA);
}

void WarpNES::LDX(uint16_t addr) {
  regX = readByte(addr);
  updateZN(regX);
}

void WarpNES::LDY(uint16_t addr) {
  regY = readByte(addr);
  updateZN(regY);
}

void WarpNES::LSR(uint16_t addr) {
  uint8_t value = readByte(addr);
  setFlag(FLAG_CARRY, (value & 0x01) != 0);
  value >>= 1;
  writeByte(addr, value);
  updateZN(value);
}

void WarpNES::LSR_ACC() {
  setFlag(FLAG_CARRY, (regA & 0x01) != 0);
  regA >>= 1;
  updateZN(regA);
}

void WarpNES::NOP() {
  // Do nothing
}

void WarpNES::ORA(uint16_t addr) {
  regA |= readByte(addr);
  updateZN(regA);
}

void WarpNES::PHA() { pushByte(regA); }

void WarpNES::PHP() { pushByte(regP | FLAG_BREAK | FLAG_UNUSED); }

void WarpNES::PLA() {
  regA = pullByte();
  updateZN(regA);
}

void WarpNES::PLP() {
  regP = pullByte() | FLAG_UNUSED;
  regP &= ~FLAG_BREAK;
}

void WarpNES::ROL(uint16_t addr) {
  uint8_t value = readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (value & 0x80) != 0);
  value = (value << 1) | (oldCarry ? 1 : 0);
  writeByte(addr, value);
  updateZN(value);
}

void WarpNES::ROL_ACC() {
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (regA & 0x80) != 0);
  regA = (regA << 1) | (oldCarry ? 1 : 0);
  updateZN(regA);
}

void WarpNES::ROR(uint16_t addr) {
  uint8_t value = readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (value & 0x01) != 0);
  value = (value >> 1) | (oldCarry ? 0x80 : 0);
  writeByte(addr, value);
  updateZN(value);
}

void WarpNES::ROR_ACC() {
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (regA & 0x01) != 0);
  regA = (regA >> 1) | (oldCarry ? 0x80 : 0);
  updateZN(regA);
}

void WarpNES::RTI() {
  regP = pullByte() | FLAG_UNUSED;
  regP &= ~FLAG_BREAK;
  regPC = pullWord();
}

void WarpNES::RTS() { regPC = pullWord() + 1; }

void WarpNES::SBC(uint16_t addr) {
  uint8_t value = readByte(addr);
  uint16_t result = regA - value - (getFlag(FLAG_CARRY) ? 0 : 1);

  setFlag(FLAG_CARRY, result <= 0xFF);
  setFlag(FLAG_OVERFLOW, ((regA ^ result) & (~value ^ result) & 0x80) != 0);

  regA = result & 0xFF;
  updateZN(regA);
}

void WarpNES::SEC() { setFlag(FLAG_CARRY, true); }

void WarpNES::SED() { setFlag(FLAG_DECIMAL, true); }

void WarpNES::SEI() { setFlag(FLAG_INTERRUPT, true); }

void WarpNES::STA(uint16_t addr) { writeByte(addr, regA); }

void WarpNES::STX(uint16_t addr) { writeByte(addr, regX); }

void WarpNES::STY(uint16_t addr) { writeByte(addr, regY); }

void WarpNES::TAX() {
  regX = regA;
  updateZN(regX);
}

void WarpNES::TAY() {
  regY = regA;
  updateZN(regY);
}

void WarpNES::TSX() {
  regX = regSP;
  updateZN(regX);
}

void WarpNES::TXA() {
  regA = regX;
  updateZN(regA);
}

void WarpNES::TXS() { regSP = regX; }

void WarpNES::TYA() {
  regA = regY;
  updateZN(regA);
}

void WarpNES::SHA(uint16_t addr) {
  // SHA: Store A & X & (high byte of address + 1)
  // This is an unstable instruction - the high byte interaction is complex
  uint8_t highByte = (addr >> 8) + 1;
  uint8_t result = regA & regX & highByte;
  writeByte(addr, result);
}

void WarpNES::SHX(uint16_t addr) {
  // SHX: Store X & (high byte of address + 1)
  uint8_t highByte = (addr >> 8) + 1;
  uint8_t result = regX & highByte;
  writeByte(addr, result);
}

void WarpNES::SHY(uint16_t addr) {
  // SHY: Store Y & (high byte of address + 1)
  uint8_t highByte = (addr >> 8) + 1;
  uint8_t result = regY & highByte;
  writeByte(addr, result);
}

void WarpNES::TAS(uint16_t addr) {
  // TAS: Transfer A & X to SP, then store A & X & (high byte + 1)
  regSP = regA & regX;
  uint8_t highByte = (addr >> 8) + 1;
  uint8_t result = regA & regX & highByte;
  writeByte(addr, result);
}

void WarpNES::LAS(uint16_t addr) {
  // LAS: Load A, X, and SP with memory value & SP
  uint8_t value = readByte(addr);
  uint8_t result = value & regSP;
  regA = result;
  regX = result;
  regSP = result;
  updateZN(result);
}

// Illegal opcode implementations
void WarpNES::ISC(uint16_t addr) {
  // INC + SBC
  uint8_t value = readByte(addr) + 1;
  writeByte(addr, value);

  // Then do SBC
  uint16_t result = regA - value - (getFlag(FLAG_CARRY) ? 0 : 1);
  setFlag(FLAG_CARRY, result <= 0xFF);
  setFlag(FLAG_OVERFLOW, ((regA ^ result) & (~value ^ result) & 0x80) != 0);
  regA = result & 0xFF;
  updateZN(regA);
}

void WarpNES::DCP(uint16_t addr) {
  // DEC + CMP
  uint8_t value = readByte(addr) - 1;
  writeByte(addr, value);

  // Then do CMP
  uint8_t result = regA - value;
  setFlag(FLAG_CARRY, regA >= value);
  updateZN(result);
}

void WarpNES::LAX(uint16_t addr) {
  // LDA + LDX
  uint8_t value = readByte(addr);
  regA = value;
  regX = value;
  updateZN(regA);
}

void WarpNES::SAX(uint16_t addr) {
  // Store A & X
  writeByte(addr, regA & regX);
}

void WarpNES::SLO(uint16_t addr) {
  // ASL + ORA
  uint8_t value = readByte(addr);
  setFlag(FLAG_CARRY, (value & 0x80) != 0);
  value <<= 1;
  writeByte(addr, value);
  regA |= value;
  updateZN(regA);
}

void WarpNES::KIL() {
  // KIL/JAM/HLT - Halts the CPU
  // In a real NES, this would lock up the system
  // For emulation, we can either:
  // 1. Actually halt (infinite loop)
  // 2. Treat as NOP and continue
  // 3. Reset the system

  // Option 2: Treat as NOP for compatibility
  // This allows games that accidentally hit illegal opcodes to continue
  totalCycles += 2;
  frameCycles += 2;
}

void WarpNES::RLA(uint16_t addr) {
  // ROL + AND
  uint8_t value = readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (value & 0x80) != 0);
  value = (value << 1) | (oldCarry ? 1 : 0);
  writeByte(addr, value);
  regA &= value;
  updateZN(regA);
}

void WarpNES::SRE(uint16_t addr) {
  // LSR + EOR
  uint8_t value = readByte(addr);
  setFlag(FLAG_CARRY, (value & 0x01) != 0);
  value >>= 1;
  writeByte(addr, value);
  regA ^= value;
  updateZN(regA);
}

void WarpNES::RRA(uint16_t addr) {
  // ROR + ADC
  uint8_t value = readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (value & 0x01) != 0);
  value = (value >> 1) | (oldCarry ? 0x80 : 0);
  writeByte(addr, value);

  uint16_t result = regA + value + (getFlag(FLAG_CARRY) ? 1 : 0);
  setFlag(FLAG_CARRY, result > 0xFF);
  setFlag(FLAG_OVERFLOW, ((regA ^ result) & (value ^ result) & 0x80) != 0);
  regA = result & 0xFF;
  updateZN(regA);
}

void WarpNES::ANC(uint16_t addr) {
  // AND + set carry to bit 7
  regA &= readByte(addr);
  updateZN(regA);
  setFlag(FLAG_CARRY, (regA & 0x80) != 0);
}

void WarpNES::ALR(uint16_t addr) {
  // AND + LSR
  regA &= readByte(addr);
  setFlag(FLAG_CARRY, (regA & 0x01) != 0);
  regA >>= 1;
  updateZN(regA);
}

void WarpNES::ARR(uint16_t addr) {
  // AND + ROR
  regA &= readByte(addr);
  bool oldCarry = getFlag(FLAG_CARRY);
  setFlag(FLAG_CARRY, (regA & 0x01) != 0);
  regA = (regA >> 1) | (oldCarry ? 0x80 : 0);
  updateZN(regA);
  setFlag(FLAG_OVERFLOW, ((regA >> 6) ^ (regA >> 5)) & 1);
}

void WarpNES::XAA(uint16_t addr) {
  // Unstable - just do AND
  regA &= readByte(addr);
  updateZN(regA);
}

void WarpNES::AXS(uint16_t addr) {
  // (A & X) - immediate
  uint8_t value = readByte(addr);
  uint8_t result = (regA & regX) - value;
  setFlag(FLAG_CARRY, (regA & regX) >= value);
  regX = result;
  updateZN(regX);
}

// Memory access implementation for NSF player
uint8_t WarpNES::readByte(uint16_t address) {
    if (address < 0x2000) {
        // RAM (mirrored every 2KB)
        return ram[address & 0x7FF];
    } else if (address >= 0x4000 && address < 0x4020) {
        // APU registers - NSF needs this for sound
        switch (address) {
        case 0x4015:
            // APU status register - return a basic status
            // For NSF playback, we typically just return 0 or a simple status
            return 0x1F; // All channels enabled
        default:
            return 0; // Other APU registers return 0 on read
        }
    } else if (address >= 0x6000 && address < 0x8000) {
        // SRAM area ($6000-$7FFF) - NSF might use this for work RAM
        if (sramEnabled && sram) {
            uint16_t sramAddr = address - 0x6000;
            if (sramAddr < sramSize) {
                return sram[sramAddr];
            }
        }
        return 0; // Open bus if SRAM not available
    } else if (address >= 0x8000) {
        // PRG ROM with mapper handling - NSF code lives here
        return readPRGByte(address);
    }
    
    return 0; // Open bus
}

void WarpNES::writeByte(uint16_t address, uint8_t value) {
    if (address < 0x2000) {
        ram[address & 0x7FF] = value;
    } else if (address >= 0x4000 && address < 0x4020) {
        // APU registers - NSF writes to these for sound
        if (apu) {
            apu->writeRegister(address, value);
        }
    } else if (address >= 0x6000 && address < 0x8000) {
        // SRAM area ($6000-$7FFF) - NSF work RAM
        if (sramEnabled && sram) {
            uint16_t sramAddr = address - 0x6000;
            if (sramAddr < sramSize) {
                sram[sramAddr] = value;
                sramDirty = true;
            }
        }
    } else if (address >= 0x8000) {
        // Mapper registers - NSF may use banking
        writeMapperRegister(address, value);
    }
}

uint8_t WarpNES::readPRGByte(uint16_t address) {
    // For NSF, we typically use simple linear ROM mapping
    // Most NSF files don't use complex mappers
    uint32_t romAddr = address - 0x8000;
    
    if (romAddr < prgSize) {
        return prgROM[romAddr];
    }
    
    return 0; // Open bus
}

void WarpNES::writeMapperRegister(uint16_t address, uint8_t value) {
    // NSF files typically use simple mappers or no mapping
    // For now, just ignore mapper writes since NSF doesn't usually need them
    // In the future, could add support for NSF banking if needed
}

// Stub implementations for mapper functions (not typically needed for NSF)
void WarpNES::writeMMC1Register(uint16_t address, uint8_t value) {
    // Not typically used in NSF
}

void WarpNES::writeUxROMRegister(uint16_t address, uint8_t value) {
    // Not typically used in NSF
}

void WarpNES::writeMMC3Register(uint16_t address, uint8_t value) {
    // Not typically used in NSF
}

void WarpNES::writeMMC2Register(uint16_t address, uint8_t value) {
    // Not typically used in NSF
}

void WarpNES::writeMapper40Register(uint16_t address, uint8_t value) {
    // Not typically used in NSF
}

void WarpNES::writeGxROMRegister(uint16_t address, uint8_t value) {
    // Not typically used in NSF
}

void WarpNES::writeCNROMRegister(uint16_t address, uint8_t value) {
    // Not typically used in NSF
}

// Stub implementations for bank update functions
void WarpNES::updateMMC1Banks() {
    // Not needed for NSF
}

void WarpNES::updateMMC3Banks() {
    // Not needed for NSF
}

void WarpNES::updateMMC2Banks() {
    // Not needed for NSF
}

// Stub implementations for IRQ functions
void WarpNES::checkMMC3IRQ(int scanline, int cycle) {
    // Not needed for NSF
}

void WarpNES::checkMapper40IRQ() {
    // Not needed for NSF
}

void WarpNES::stepMMC3A12Transition(bool a12High) {
    // Not needed for NSF
}

void WarpNES::checkMMC2CHRLatch(uint16_t address, uint8_t tileID) {
    // Not needed for NSF
}

// SRAM functions for NSF work RAM
void WarpNES::initializeSRAM() {
    if (!sram && sramSize > 0) {
        sram = new uint8_t[sramSize];
        memset(sram, 0, sramSize);
        sramEnabled = true;
        sramDirty = false;
    }
}

void WarpNES::cleanupSRAM() {
    if (sram) {
        delete[] sram;
        sram = nullptr;
        sramSize = 0;
        sramEnabled = false;
        sramDirty = false;
    }
}

void WarpNES::loadSRAM() {
    // NSF doesn't typically load/save SRAM
}

void WarpNES::saveSRAM() {
    // NSF doesn't typically load/save SRAM
}

// Stub main function as requested
int main(int argc, char* argv[]) {
    // TODO: Implement main function for NSF player
    // This should handle command line arguments, load NSF files,
    // initialize the CPU and APU, and start the audio playback loop
    return 0;
}
