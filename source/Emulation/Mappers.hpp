#ifndef WARPNES_MAPPERS_HPP
#define WARPNES_MAPPERS_HPP

// Additional cartridge mappers. Each one keeps simple bank tables (8 KB PRG
// pages, 1 KB CHR pages, 1 KB nametable pages) that WarpNES and the PPU read
// through, so a mapper only has to update those tables when its registers
// change. Mappers 0-9, 40, 66, 118 and 119 live in WarpNES itself.

#include <cstdint>
#include <cstddef>
#include <iostream>

struct MapperInfo {
  int number = 0;
  int submapper = 0;
  uint8_t* prg = nullptr;
  uint32_t prgSize = 0;
  uint8_t* chr = nullptr;
  uint32_t chrSize = 0;
  bool chrRam = false;        // chr[] is CHR-RAM (no CHR-ROM in the file)
  bool verticalMirroring = false;
  bool fourScreen = false;
  bool battery = false;
  uint8_t* wram = nullptr;    // $6000-$7FFF PRG-RAM (WarpNES's SRAM buffer)
  uint32_t wramSize = 0;
};

class Mapper {
public:
  enum { MIR_H = 0, MIR_V = 1, MIR_A = 2, MIR_B = 3 };

  explicit Mapper(const MapperInfo& info);
  virtual ~Mapper() {}

  virtual void reset() {}

  // CPU side. cpuRead handles $8000-$FFFF. cpuReadLow handles $4020-$7FFF
  // and returns false to fall back to plain PRG-RAM. cpuWrite sees every
  // write to $4020-$FFFF and returns true if a $6000-$7FFF write must not
  // also land in PRG-RAM.
  virtual uint8_t cpuRead(uint16_t a) { return prg[prgMap[(a >> 13) & 3] + (a & 0x1FFF)]; }
  virtual bool cpuReadLow(uint16_t, uint8_t&) { return false; }
  virtual bool cpuWrite(uint16_t a, uint8_t v) = 0;

  // PPU side
  virtual uint8_t chrRead(uint16_t a) { return chr[chrMap[(a >> 10) & 7] + (a & 0x3FF)]; }
  virtual void chrWrite(uint16_t a, uint8_t v) {
    if (chrWritable[(a >> 10) & 7]) chr[chrMap[(a >> 10) & 7] + (a & 0x3FF)] = v;
  }
  bool ntFromRom(int table) const { return ntRom[table & 3]; }
  int ntPage(int table) const { return nt[table & 3]; }
  uint8_t ntRomRead(uint16_t a) const { return chr[ntRomOffset[(a >> 10) & 3] + (a & 0x3FF)]; }

  // Timing hooks
  bool wantsCpuClock = false;
  bool wantsScanline = false;
  virtual void cpuClock(int) {}
  virtual void scanline() {}     // once per rendered line (A12 rise, like MMC3)
  bool irq = false;

  // Expansion audio: advance n CPU cycles, return the output summed over
  // them (in the same units as the APU's mixed output).
  bool hasAudio = false;
  virtual float audio(uint32_t) { return 0.0f; }

  // Save states
  void saveState(std::ostream& out);
  void loadState(std::istream& in);

  int number() const { return info.number; }

protected:
  MapperInfo info;
  uint8_t* prg;
  uint8_t* chr;
  uint32_t prgMap[4] = {0, 0, 0, 0};
  uint32_t chrMap[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  bool chrWritable[8] = {false, false, false, false, false, false, false, false};
  int nt[4] = {0, 0, 1, 1};
  bool ntRom[4] = {false, false, false, false};
  uint32_t ntRomOffset[4] = {0, 0, 0, 0};

  // Per-mapper register block, saved verbatim in save states.
  virtual void* regs() { return nullptr; }
  virtual size_t regsSize() const { return 0; }
  virtual void afterLoad() { sync(); }
  virtual void sync() {}

  int prgPages8() const { return (int)(info.prgSize / 0x2000); }
  int chrPages1() const { return (int)(info.chrSize / 0x400); }

  // Bank helpers. Negative banks count from the end (-1 = last).
  void setPrg8(int slot, int bank);
  void setPrg16(int slot, int bank) { setPrg8(slot * 2, bank * 2); setPrg8(slot * 2 + 1, bank * 2 + 1); }
  void setPrg32(int bank) { for (int i = 0; i < 4; i++) setPrg8(i, bank * 4 + i); }
  void setChr1(int slot, int bank);
  void setChr2(int slot, int bank) { setChr1(slot * 2, bank * 2); setChr1(slot * 2 + 1, bank * 2 + 1); }
  void setChr4(int slot, int bank) { for (int i = 0; i < 4; i++) setChr1(slot * 4 + i, bank * 4 + i); }
  void setChr8(int bank) { for (int i = 0; i < 8; i++) setChr1(i, bank * 8 + i); }
  void mirror(int mode);
  void mirrorPages(int a, int b, int c, int d);
  void setNtRom(int table, int bank1k);
  void headerMirroring() { mirror(info.verticalMirroring ? MIR_V : MIR_H); }
};

// Returns nullptr if the mapper number isn't handled here.
Mapper* createMapper(const MapperInfo& info);

// True if createMapper() handles this mapper number.
bool mapperImplemented(int mapper);

// Size of CHR-RAM to allocate when the file has no CHR-ROM.
uint32_t mapperChrRamSize(int mapper);

#endif
