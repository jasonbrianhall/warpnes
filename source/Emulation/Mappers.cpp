// Additional cartridge mappers for WarpNES. See Mappers.hpp.
//
// Register layouts follow the nesdev wiki. Expansion audio is implemented
// for VRC6, Sunsoft 5B and Namco 163. VRC7's FM chip is not emulated, so
// its games run but without music.

#include "Mappers.hpp"
#include <cmath>
#include <cstring>

#define MAPPER_REGS(T)                                              \
  T r;                                                              \
  void* regs() override { return &r; }                              \
  size_t regsSize() const override { return sizeof(r); }

// ---------------------------------------------------------------------------
// Base class
// ---------------------------------------------------------------------------

Mapper::Mapper(const MapperInfo& in) : info(in), prg(in.prg), chr(in.chr)
{
  for (int i = 0; i < 8; i++) chrWritable[i] = in.chrRam;
  setPrg32(0);
  setPrg16(1, -1);
  setChr8(0);
  headerMirroring();
}

void Mapper::setPrg8(int slot, int bank)
{
  int n = prgPages8();
  if (n <= 0) return;
  bank %= n;
  if (bank < 0) bank += n;
  prgMap[slot & 3] = (uint32_t)bank * 0x2000;
}

void Mapper::setChr1(int slot, int bank)
{
  int n = chrPages1();
  if (n <= 0) return;
  bank %= n;
  if (bank < 0) bank += n;
  chrMap[slot & 7] = (uint32_t)bank * 0x400;
}

void Mapper::mirrorPages(int a, int b, int c, int d)
{
  if (info.fourScreen) {
    a = 0; b = 1; c = 2; d = 3;
  }
  nt[0] = a; nt[1] = b; nt[2] = c; nt[3] = d;
  for (int i = 0; i < 4; i++) ntRom[i] = false;
}

void Mapper::mirror(int mode)
{
  switch (mode & 3) {
  case MIR_H: mirrorPages(0, 0, 1, 1); break;
  case MIR_V: mirrorPages(0, 1, 0, 1); break;
  case MIR_A: mirrorPages(0, 0, 0, 0); break;
  case MIR_B: mirrorPages(1, 1, 1, 1); break;
  }
}

void Mapper::setNtRom(int table, int bank1k)
{
  int n = chrPages1();
  if (n <= 0 || info.chrRam) return;
  bank1k %= n;
  if (bank1k < 0) bank1k += n;
  ntRom[table & 3] = true;
  ntRomOffset[table & 3] = (uint32_t)bank1k * 0x400;
}

void Mapper::saveState(std::ostream& out)
{
  uint8_t flags = irq ? 1 : 0;
  out.write(reinterpret_cast<const char*>(&flags), 1);
  if (regsSize()) out.write(reinterpret_cast<const char*>(regs()), regsSize());
}

void Mapper::loadState(std::istream& in)
{
  uint8_t flags = 0;
  in.read(reinterpret_cast<char*>(&flags), 1);
  irq = (flags & 1) != 0;
  if (regsSize()) in.read(reinterpret_cast<char*>(regs()), regsSize());
  afterLoad();
}

static const int kMirror4[4] = {Mapper::MIR_V, Mapper::MIR_H, Mapper::MIR_A, Mapper::MIR_B};

// ---------------------------------------------------------------------------
// Simple discrete-logic boards. One register, decoded by a small function.
// ---------------------------------------------------------------------------

class DiscreteMapper : public Mapper {
public:
  explicit DiscreteMapper(const MapperInfo& in) : Mapper(in) { reset(); }

  void reset() override {
    r.value = 0;
    r.addr = 0;
    r.outer = 0;
    r.chrOn = 1;
    r.mirrorSet = 0;
    switch (info.number) {
    case 180: setPrg16(0, 0); break;
    case 97: setPrg16(0, -1); setPrg16(1, 0); break;
    case 232: r.outer = 0; break;
    default: break;
    }
    apply(r.value, r.addr, true);
  }

  bool cpuWrite(uint16_t a, uint8_t v) override {
    switch (info.number) {
    case 79:
    case 113:
      if (a >= 0x4100 && a < 0x6000 && (a & 0x4100) == 0x4100) apply(v, a, false);
      return false;
    case 86:
      if (a >= 0x6000 && a < 0x7000) apply(v, a, false);
      return a >= 0x6000 && a < 0x8000;
    case 87:
    case 140:
    case 184:
      if (a >= 0x6000 && a < 0x8000) { apply(v, a, false); return true; }
      return false;
    case 34:
      if (info.chrSize > 0x2000 && !info.chrRam) {
        // NINA-001: registers overlay PRG-RAM at $7FFD-$7FFF
        if (a >= 0x7FFD && a <= 0x7FFF) apply(v, a, false);
        return false;
      }
      if (a >= 0x8000) apply(v, a, false);
      return false;
    default:
      if (a >= 0x8000) apply(v, a, false);
      return false;
    }
  }

  uint8_t chrRead(uint16_t a) override {
    if (!r.chrOn) return 0xFF;   // mapper 185 copy protection: CHR disabled
    return Mapper::chrRead(a);
  }

protected:
  struct R {
    uint8_t value;
    uint16_t addr;
    uint8_t outer;
    uint8_t inner;
    uint8_t chrOn;
    uint8_t mirrorSet;
    uint8_t chrLo, chrHi;
  };
  MAPPER_REGS(R)

  void afterLoad() override { apply(r.value, r.addr, true); }

  void apply(uint8_t v, uint16_t a, bool replay) {
    r.value = v;
    r.addr = a;
    switch (info.number) {
    case 11:  // Color Dreams
      setPrg32(v & 3);
      setChr8(v >> 4);
      break;
    case 13:  // CPROM: fixed 4K + switchable 4K of 16K CHR-RAM
      setChr4(0, 0);
      setChr4(1, v & 3);
      break;
    case 30:  // UNROM 512
      setPrg16(0, v & 0x1F);
      setPrg16(1, -1);
      setChr8((v >> 5) & 3);
      if (oneScreen30) mirror(v & 0x80 ? MIR_B : MIR_A);
      break;
    case 34:
      if (info.chrSize > 0x2000 && !info.chrRam) {
        if (replay) {
          setPrg32(r.outer);
          setChr4(0, r.chrLo);
          setChr4(1, r.chrHi);
          break;
        }
        if (a == 0x7FFD) { r.outer = v & 1; setPrg32(v & 1); }
        else if (a == 0x7FFE) { r.chrLo = v & 15; setChr4(0, v & 15); }
        else if (a == 0x7FFF) { r.chrHi = v & 15; setChr4(1, v & 15); }
      } else {
        setPrg32(v);
      }
      break;
    case 70:
      setPrg16(0, v >> 4);
      setChr8(v & 15);
      break;
    case 71:  // Camerica
      if (replay) {
        setPrg16(0, r.inner);
        if (r.mirrorSet) mirror(r.outer ? MIR_B : MIR_A);
        break;
      }
      if (a >= 0xC000) { r.inner = v; setPrg16(0, v); }
      else if (a >= 0x9000 && a < 0xA000) {   // Fire Hawk one-screen select
        r.mirrorSet = 1;
        r.outer = (v >> 4) & 1;
        mirror(r.outer ? MIR_B : MIR_A);
      }
      break;
    case 72:
      if (replay) { setPrg16(0, r.inner); setChr8(r.outer); break; }
      if (v & 0x80) { r.inner = v & 15; setPrg16(0, v & 15); }
      if (v & 0x40) { r.outer = v & 15; setChr8(v & 15); }
      break;
    case 92:
      if (replay) { setPrg16(0, 0); setPrg16(1, r.inner); setChr8(r.outer); break; }
      setPrg16(0, 0);
      if (v & 0x80) { r.inner = v & 15; setPrg16(1, v & 15); }
      if (v & 0x40) { r.outer = v & 15; setChr8(v & 15); }
      break;
    case 78:
      setPrg16(0, v & 7);
      setChr8(v >> 4);
      if (holyDiver) mirror(v & 8 ? MIR_V : MIR_H);
      else mirror(v & 8 ? MIR_B : MIR_A);
      break;
    case 79:
      setPrg32((v >> 3) & 1);
      setChr8(v & 7);
      break;
    case 113:
      setPrg32((v >> 3) & 7);
      setChr8((v & 7) | ((v >> 3) & 8));
      mirror(v & 0x80 ? MIR_V : MIR_H);
      break;
    case 86:
      setPrg32((v >> 4) & 3);
      setChr8((v & 3) | ((v >> 4) & 4));
      break;
    case 87:
      setChr8(((v & 1) << 1) | ((v & 2) >> 1));
      break;
    case 89:
      setPrg16(0, (v >> 4) & 7);
      setChr8((v & 7) | ((v & 0x80) >> 4));
      mirror(v & 8 ? MIR_B : MIR_A);
      break;
    case 93:
      setPrg16(0, (v >> 4) & 7);
      for (int i = 0; i < 8; i++) chrWritable[i] = info.chrRam && (v & 1);
      break;
    case 94:
      setPrg16(0, (v >> 2) & 7);
      break;
    case 97: {
      static const int m[4] = {MIR_A, MIR_H, MIR_V, MIR_B};
      setPrg16(0, -1);
      setPrg16(1, v & 0x1F);
      mirror(m[(v >> 6) & 3]);
      break;
    }
    case 140:
      setPrg32((v >> 4) & 3);
      setChr8(v & 15);
      break;
    case 152:
      setPrg16(0, (v >> 4) & 7);
      setChr8(v & 15);
      mirror(v & 0x80 ? MIR_B : MIR_A);
      break;
    case 180:
      setPrg16(0, 0);
      setPrg16(1, v & 7);
      break;
    case 184:
      setChr4(0, v & 7);
      setChr4(1, 4 | ((v >> 4) & 3));
      break;
    case 185:
      if (info.submapper >= 4) r.chrOn = ((v & 3) == (info.submapper & 3));
      else r.chrOn = ((v & 0x0F) != 0 && v != 0x13);
      if (replay && a == 0) r.chrOn = 1;
      break;
    case 228: {  // Action 52 / Cheetahmen II
      int chip = (a >> 11) & 3;
      if (chip == 3) chip = 2;
      int page = ((a >> 6) & 0x1F) | (chip << 5);
      if (a & 0x20) { setPrg16(0, page); setPrg16(1, page); }
      else { setPrg16(0, page & ~1); setPrg16(1, page | 1); }
      setChr8(((a & 0x0F) << 2) | (v & 3));
      mirror(a & 0x2000 ? MIR_H : MIR_V);
      break;
    }
    case 232: {  // Camerica Quattro
      if (replay) {
        setPrg16(0, r.outer * 4 + r.inner);
        setPrg16(1, r.outer * 4 + 3);
        break;
      }
      if (a < 0xC000) {
        r.outer = (info.submapper == 1) ? (((v >> 4) & 1) | ((v >> 2) & 2)) : ((v >> 3) & 3);
      } else {
        r.inner = v & 3;
      }
      setPrg16(0, r.outer * 4 + r.inner);
      setPrg16(1, r.outer * 4 + 3);
      break;
    }
    default:
      break;
    }
  }

public:
  bool holyDiver = false;
  bool oneScreen30 = false;
};

// ---------------------------------------------------------------------------
// VRC IRQ counter, shared by VRC4, VRC6 and VRC7
// ---------------------------------------------------------------------------

struct VrcIrq {
  uint8_t latch = 0, counter = 0, control = 0;
  int16_t prescaler = 341;

  void writeControl(uint8_t v, bool& irq) {
    control = v & 7;
    if (control & 2) { counter = latch; prescaler = 341; }
    irq = false;
  }
  void ack(bool& irq) {
    irq = false;
    control = (control & ~2) | ((control & 1) << 1);
  }
  void tick(bool& irq) {
    if (counter == 0xFF) { counter = latch; irq = true; }
    else counter++;
  }
  void clock(int n, bool& irq) {
    if (!(control & 2)) return;
    while (n-- > 0) {
      if (control & 4) tick(irq);
      else {
        prescaler -= 3;
        if (prescaler <= 0) { prescaler += 341; tick(irq); }
      }
    }
  }
};

// ---------------------------------------------------------------------------
// Konami VRC1 (75)
// ---------------------------------------------------------------------------

class VRC1 : public Mapper {
public:
  explicit VRC1(const MapperInfo& in) : Mapper(in) { memset(&r, 0, sizeof(r)); sync(); }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    switch (a & 0xF000) {
    case 0x8000: r.prg[0] = v & 15; break;
    case 0x9000: r.ctl = v; break;
    case 0xA000: r.prg[1] = v & 15; break;
    case 0xC000: r.prg[2] = v & 15; break;
    case 0xE000: r.chr[0] = v & 15; break;
    case 0xF000: r.chr[1] = v & 15; break;
    }
    sync();
    return false;
  }
protected:
  struct R { uint8_t prg[3], chr[2], ctl; };
  MAPPER_REGS(R)
  void sync() override {
    setPrg8(0, r.prg[0]); setPrg8(1, r.prg[1]); setPrg8(2, r.prg[2]); setPrg8(3, -1);
    setChr4(0, r.chr[0] | ((r.ctl & 2) ? 16 : 0));
    setChr4(1, r.chr[1] | ((r.ctl & 4) ? 16 : 0));
    mirror(r.ctl & 1 ? MIR_H : MIR_V);
  }
};

// ---------------------------------------------------------------------------
// Konami VRC2 / VRC4 (21, 22, 23, 25)
// ---------------------------------------------------------------------------

class VRC24 : public Mapper {
public:
  explicit VRC24(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    r.irq = VrcIrq();
    sync();
    wantsCpuClock = (in.number != 22);
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    int b0, b1;
    switch (info.number) {
    case 21: b0 = ((a >> 1) | (a >> 6)) & 1; b1 = ((a >> 2) | (a >> 7)) & 1; break;
    case 22: b0 = (a >> 1) & 1; b1 = a & 1; break;
    case 25: b0 = ((a >> 1) | (a >> 3)) & 1; b1 = (a | (a >> 2)) & 1; break;
    default: b0 = (a | (a >> 2)) & 1; b1 = ((a >> 1) | (a >> 3)) & 1; break;  // 23
    }
    int reg = (a & 0xF000) | (b1 << 1) | b0;
    bool vrc2 = (info.number == 22);
    switch (reg & 0xF000) {
    case 0x8000: r.prg0 = v & 0x1F; break;
    case 0xA000: r.prg1 = v & 0x1F; break;
    case 0x9000:
      if (vrc2) r.mir = v & 1;
      else if ((reg & 3) <= 1) r.mir = v & 3;
      else if ((reg & 3) == 2) r.swap = (v >> 1) & 1;
      break;
    case 0xB000: case 0xC000: case 0xD000: case 0xE000: {
      int idx = (((reg >> 12) - 0xB) << 1) | b1;
      if (b0 == 0) r.chr[idx] = (r.chr[idx] & 0x1F0) | (v & 0x0F);
      else r.chr[idx] = (r.chr[idx] & 0x00F) | ((v & 0x1F) << 4);
      break;
    }
    case 0xF000:
      if (vrc2) break;
      switch (reg & 3) {
      case 0: r.irq.latch = (r.irq.latch & 0xF0) | (v & 0x0F); break;
      case 1: r.irq.latch = (r.irq.latch & 0x0F) | ((v & 0x0F) << 4); break;
      case 2: r.irq.writeControl(v, irq); break;
      case 3: r.irq.ack(irq); break;
      }
      break;
    }
    sync();
    return false;
  }
  void cpuClock(int n) override { r.irq.clock(n, irq); }
protected:
  struct R { uint8_t prg0, prg1, mir, swap; uint16_t chr[8]; VrcIrq irq; };
  MAPPER_REGS(R)
  void sync() override {
    if (r.swap) { setPrg8(0, -2); setPrg8(2, r.prg0); }
    else { setPrg8(0, r.prg0); setPrg8(2, -2); }
    setPrg8(1, r.prg1);
    setPrg8(3, -1);
    for (int i = 0; i < 8; i++) setChr1(i, info.number == 22 ? (r.chr[i] >> 1) : r.chr[i]);
    if (info.number == 22) mirror(r.mir & 1 ? MIR_H : MIR_V);
    else mirror(kMirror4[r.mir & 3]);
  }
};

// ---------------------------------------------------------------------------
// Konami VRC3 (73)
// ---------------------------------------------------------------------------

class VRC3 : public Mapper {
public:
  explicit VRC3(const MapperInfo& in) : Mapper(in) { memset(&r, 0, sizeof(r)); wantsCpuClock = true; sync(); }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    switch (a & 0xF000) {
    case 0x8000: case 0x9000: case 0xA000: case 0xB000: {
      int shift = ((a >> 12) - 8) * 4;
      r.latch = (r.latch & ~(0xF << shift)) | ((v & 0xF) << shift);
      break;
    }
    case 0xC000:
      r.ctl = v & 7;
      if (v & 2) r.counter = r.latch;
      irq = false;
      break;
    case 0xD000:
      irq = false;
      r.ctl = (r.ctl & ~2) | ((r.ctl & 1) << 1);
      break;
    case 0xF000: r.prg = v & 7; sync(); break;
    }
    return false;
  }
  void cpuClock(int n) override {
    if (!(r.ctl & 2)) return;
    if (r.ctl & 4) {
      uint32_t low = (r.counter & 0xFF) + n;
      if (low > 0xFF) { irq = true; low = (r.latch & 0xFF) + (low - 0x100); }
      r.counter = (r.counter & 0xFF00) | (low & 0xFF);
    } else {
      uint32_t c = (uint32_t)r.counter + n;
      if (c > 0xFFFF) { irq = true; c = r.latch + (c - 0x10000); }
      r.counter = (uint16_t)c;
    }
  }
protected:
  struct R { uint16_t latch, counter; uint8_t ctl, prg; };
  MAPPER_REGS(R)
  void sync() override { setPrg16(0, r.prg); setPrg16(1, -1); }
};

// ---------------------------------------------------------------------------
// Konami VRC6 (24, 26) with expansion audio
// ---------------------------------------------------------------------------

class VRC6 : public Mapper {
public:
  explicit VRC6(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    r.irq = VrcIrq();
    wantsCpuClock = true;
    hasAudio = true;
    sync();
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    int lo = a & 3;
    if (info.number == 26) lo = ((lo & 1) << 1) | ((lo & 2) >> 1);
    int reg = (a & 0xF000) | lo;
    switch (reg) {
    case 0x8000: case 0x8001: case 0x8002: case 0x8003: r.prg16 = v & 0x0F; break;
    case 0x9000: r.p[0].ctl = v; break;
    case 0x9001: r.p[0].lo = v; break;
    case 0x9002: r.p[0].hi = v; break;
    case 0x9003: r.freqCtl = v; break;
    case 0xA000: r.p[1].ctl = v; break;
    case 0xA001: r.p[1].lo = v; break;
    case 0xA002: r.p[1].hi = v; break;
    case 0xB000: r.sawRate = v & 0x3F; break;
    case 0xB001: r.sawLo = v; break;
    case 0xB002: r.sawHi = v; if (!(v & 0x80)) { r.sawStep = 0; r.sawAcc = 0; } break;
    case 0xB003: r.ppuMode = v; break;
    case 0xC000: case 0xC001: case 0xC002: case 0xC003: r.prg8 = v & 0x1F; break;
    case 0xD000: case 0xD001: case 0xD002: case 0xD003: r.chr[reg & 3] = v; break;
    case 0xE000: case 0xE001: case 0xE002: case 0xE003: r.chr[4 + (reg & 3)] = v; break;
    case 0xF000: r.irq.latch = v; break;
    case 0xF001: r.irq.writeControl(v, irq); break;
    case 0xF002: r.irq.ack(irq); break;
    }
    sync();
    return false;
  }
  void cpuClock(int n) override { r.irq.clock(n, irq); }

  float audio(uint32_t n) override {
    float sum = 0.0f;
    bool halt = (r.freqCtl & 1) != 0;
    for (uint32_t c = 0; c < n; c++) {
      int out = 0;
      for (int i = 0; i < 2; i++) {
        Pulse& p = r.p[i];
        if (!(p.hi & 0x80)) continue;
        if (!halt && --p.timer < 0) {
          p.timer = ((p.hi & 0x0F) << 8) | p.lo;
          p.step = (p.step + 1) & 15;
        }
        int duty = (p.ctl >> 4) & 7;
        if ((p.ctl & 0x80) || p.step <= duty) out += p.ctl & 0x0F;
      }
      if (r.sawHi & 0x80) {
        if (!halt && --r.sawTimer < 0) {
          r.sawTimer = ((r.sawHi & 0x0F) << 8) | r.sawLo;
          r.sawStep++;
          if (r.sawStep >= 14) { r.sawStep = 0; r.sawAcc = 0; }
          else if (!(r.sawStep & 1)) r.sawAcc = (uint8_t)(r.sawAcc + r.sawRate);
        }
        out += r.sawAcc >> 3;
      }
      sum += (float)out;
    }
    return sum * 0.0098f;
  }

protected:
  struct Pulse { uint8_t ctl, lo, hi; int16_t timer; uint8_t step; };
  struct R {
    uint8_t prg16, prg8, ppuMode, freqCtl;
    uint8_t chr[8];
    VrcIrq irq;
    Pulse p[2];
    uint8_t sawRate, sawLo, sawHi, sawStep, sawAcc;
    int16_t sawTimer;
  };
  MAPPER_REGS(R)
  void sync() override {
    setPrg16(0, r.prg16);
    setPrg8(2, r.prg8);
    setPrg8(3, -1);
    switch (r.ppuMode & 3) {
    case 0:
      for (int i = 0; i < 8; i++) setChr1(i, r.chr[i]);
      break;
    case 1:
      for (int i = 0; i < 4; i++) setChr2(i, r.chr[i]);
      break;
    default:
      for (int i = 0; i < 4; i++) setChr1(i, r.chr[i]);
      setChr2(2, r.chr[4]);
      setChr2(3, r.chr[5]);
      break;
    }
    mirror(kMirror4[(r.ppuMode >> 2) & 3]);
  }
};

// ---------------------------------------------------------------------------
// Konami VRC7 (85). Banking and IRQ only; the FM sound chip is not emulated.
// ---------------------------------------------------------------------------

class VRC7 : public Mapper {
public:
  explicit VRC7(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    r.irq = VrcIrq();
    wantsCpuClock = true;
    sync();
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    int second = (a & 0x18) ? 1 : 0;
    switch (a & 0xF000) {
    case 0x8000: if (second) r.prg[1] = v & 0x3F; else r.prg[0] = v & 0x3F; break;
    case 0x9000: if (!second) r.prg[2] = v & 0x3F; break;   // $9010/$9030: audio
    case 0xA000: r.chr[0 + second] = v; break;
    case 0xB000: r.chr[2 + second] = v; break;
    case 0xC000: r.chr[4 + second] = v; break;
    case 0xD000: r.chr[6 + second] = v; break;
    case 0xE000: if (second) r.irq.latch = v; else r.ctl = v; break;
    case 0xF000: if (second) r.irq.ack(irq); else r.irq.writeControl(v, irq); break;
    }
    sync();
    return false;
  }
  void cpuClock(int n) override { r.irq.clock(n, irq); }
protected:
  struct R { uint8_t prg[3], chr[8], ctl; VrcIrq irq; };
  MAPPER_REGS(R)
  void sync() override {
    for (int i = 0; i < 3; i++) setPrg8(i, r.prg[i]);
    setPrg8(3, -1);
    for (int i = 0; i < 8; i++) setChr1(i, r.chr[i]);
    mirror(kMirror4[r.ctl & 3]);
  }
};

// ---------------------------------------------------------------------------
// Namco 108 family: 206 (DxROM), 76, 88, 95, 154
// ---------------------------------------------------------------------------

class Namco108 : public Mapper {
public:
  explicit Namco108(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    r.reg[0] = 0; r.reg[1] = 2; r.reg[2] = 4; r.reg[3] = 5; r.reg[4] = 6; r.reg[5] = 7;
    r.reg[6] = 0; r.reg[7] = 1;
    sync();
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    if (info.number == 154) r.oneScreen = (v >> 6) & 1;
    if (a < 0xA000) {
      if (a & 1) r.reg[r.cmd & 7] = v;
      else r.cmd = v;
    }
    sync();
    return false;
  }
protected:
  struct R { uint8_t cmd, reg[8], oneScreen; };
  MAPPER_REGS(R)
  void sync() override {
    setPrg8(0, r.reg[6] & 0x0F);
    setPrg8(1, r.reg[7] & 0x0F);
    setPrg8(2, -2);
    setPrg8(3, -1);
    switch (info.number) {
    case 76:
      for (int i = 0; i < 4; i++) setChr2(i, r.reg[2 + i] & 0x3F);
      break;
    case 88:
    case 154:
      setChr1(0, r.reg[0] & 0x3E); setChr1(1, (r.reg[0] & 0x3E) | 1);
      setChr1(2, r.reg[1] & 0x3E); setChr1(3, (r.reg[1] & 0x3E) | 1);
      for (int i = 0; i < 4; i++) setChr1(4 + i, r.reg[2 + i] | 0x40);
      break;
    default:  // 206, 95
      setChr1(0, r.reg[0] & 0x3E); setChr1(1, (r.reg[0] & 0x3E) | 1);
      setChr1(2, r.reg[1] & 0x3E); setChr1(3, (r.reg[1] & 0x3E) | 1);
      for (int i = 0; i < 4; i++) setChr1(4 + i, r.reg[2 + i] & 0x3F);
      break;
    }
    if (info.number == 154) mirror(r.oneScreen ? MIR_B : MIR_A);
    else if (info.number == 95) {
      int p0 = (r.reg[0] >> 5) & 1, p1 = (r.reg[1] >> 5) & 1;
      mirrorPages(p0, p0, p1, p1);
    }
  }
};

// ---------------------------------------------------------------------------
// Tengen RAMBO-1 (64), and TLSROM-style nametables (158)
// ---------------------------------------------------------------------------

class Rambo1 : public Mapper {
public:
  explicit Rambo1(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    wantsCpuClock = true;
    wantsScanline = true;
    sync();
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    switch (a & 0xE001) {
    case 0x8000: r.cmd = v; break;
    case 0x8001: r.reg[r.cmd & 0x0F] = v; break;
    case 0xA000: r.mir = v & 1; break;
    case 0xC000: r.latch = v; break;
    case 0xC001: r.cpuMode = v & 1; r.reload = 1; r.prescaler = 0; break;
    case 0xE000: r.irqEnable = 0; irq = false; break;
    case 0xE001: r.irqEnable = 1; break;
    }
    sync();
    return false;
  }
  void scanline() override { if (!r.cpuMode) clockCounter(); }
  void cpuClock(int n) override {
    if (!r.cpuMode) return;
    r.prescaler += n;
    while (r.prescaler >= 4) { r.prescaler -= 4; clockCounter(); }
  }
protected:
  struct R { uint8_t cmd, reg[16], mir, latch, cpuMode, reload, irqEnable; int16_t counter; int32_t prescaler; };
  MAPPER_REGS(R)
  void clockCounter() {
    if (r.reload) {
      r.counter = (r.latch <= 1) ? r.latch + 1 : r.latch + 2;
      r.reload = 0;
    } else if (r.counter == 0) {
      r.counter = r.latch + 1;
    }
    r.counter--;
    if (r.counter == 0 && r.irqEnable) irq = true;
  }
  void sync() override {
    int banks[8];
    if (r.cmd & 0x20) {
      banks[0] = r.reg[0]; banks[1] = r.reg[8]; banks[2] = r.reg[1]; banks[3] = r.reg[9];
    } else {
      banks[0] = r.reg[0] & 0xFE; banks[1] = r.reg[0] | 1;
      banks[2] = r.reg[1] & 0xFE; banks[3] = r.reg[1] | 1;
    }
    for (int i = 0; i < 4; i++) banks[4 + i] = r.reg[2 + i];
    int inv = (r.cmd & 0x80) ? 4 : 0;
    for (int i = 0; i < 8; i++) setChr1(i ^ inv, banks[i]);
    if (r.cmd & 0x40) { setPrg8(0, r.reg[15]); setPrg8(1, r.reg[6]); setPrg8(2, r.reg[7]); }
    else { setPrg8(0, r.reg[6]); setPrg8(1, r.reg[7]); setPrg8(2, r.reg[15]); }
    setPrg8(3, -1);
    if (info.number == 158) {
      mirrorPages((banks[0] >> 7) & 1, (banks[1] >> 7) & 1, (banks[2] >> 7) & 1, (banks[3] >> 7) & 1);
    } else {
      mirror(r.mir ? MIR_H : MIR_V);
    }
  }
};

// ---------------------------------------------------------------------------
// Irem G-101 (32), H3001 (65)
// ---------------------------------------------------------------------------

class IremG101 : public Mapper {
public:
  explicit IremG101(const MapperInfo& in) : Mapper(in) { memset(&r, 0, sizeof(r)); sync(); }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    switch (a & 0xF000) {
    case 0x8000: r.prg0 = v & 0x1F; break;
    case 0x9000: r.mode = v; break;
    case 0xA000: r.prg1 = v & 0x1F; break;
    case 0xB000: r.chr[a & 7] = v; break;
    }
    sync();
    return false;
  }
protected:
  struct R { uint8_t prg0, prg1, mode, chr[8]; };
  MAPPER_REGS(R)
  void sync() override {
    if (r.mode & 2) { setPrg8(0, -2); setPrg8(2, r.prg0); }
    else { setPrg8(0, r.prg0); setPrg8(2, -2); }
    setPrg8(1, r.prg1);
    setPrg8(3, -1);
    for (int i = 0; i < 8; i++) setChr1(i, r.chr[i]);
    if (info.submapper == 1) mirror(MIR_B);
    else mirror(r.mode & 1 ? MIR_H : MIR_V);
  }
};

class IremH3001 : public Mapper {
public:
  explicit IremH3001(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    r.prg[0] = 0; r.prg[1] = 1; r.prg[2] = 0xFE;
    wantsCpuClock = true;
    sync();
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    switch (a) {
    case 0x8000: r.prg[0] = v; break;
    case 0xA000: r.prg[1] = v; break;
    case 0xC000: r.prg[2] = v; break;
    case 0x9001: r.mir = v >> 7; break;
    case 0x9003: r.irqEnable = v >> 7; irq = false; break;
    case 0x9004: r.counter = r.latch; irq = false; break;
    case 0x9005: r.latch = (r.latch & 0x00FF) | (v << 8); break;
    case 0x9006: r.latch = (r.latch & 0xFF00) | v; break;
    default:
      if ((a & 0xFFF8) == 0xB000) r.chr[a & 7] = v;
      break;
    }
    sync();
    return false;
  }
  void cpuClock(int n) override {
    if (!r.irqEnable || r.counter == 0) return;
    if (r.counter <= n) { r.counter = 0; irq = true; }
    else r.counter -= n;
  }
protected:
  struct R { uint8_t prg[3], chr[8], mir, irqEnable; uint16_t latch; uint16_t counter; };
  MAPPER_REGS(R)
  void sync() override {
    for (int i = 0; i < 3; i++) setPrg8(i, r.prg[i]);
    setPrg8(3, -1);
    for (int i = 0; i < 8; i++) setChr1(i, r.chr[i]);
    mirror(r.mir ? MIR_H : MIR_V);
  }
};

// ---------------------------------------------------------------------------
// Sunsoft-3 (67), Sunsoft-4 (68)
// ---------------------------------------------------------------------------

class Sunsoft3 : public Mapper {
public:
  explicit Sunsoft3(const MapperInfo& in) : Mapper(in) { memset(&r, 0, sizeof(r)); wantsCpuClock = true; sync(); }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    switch (a & 0xF800) {
    case 0x8000: irq = false; break;
    case 0x8800: r.chr[0] = v; break;
    case 0x9800: r.chr[1] = v; break;
    case 0xA800: r.chr[2] = v; break;
    case 0xB800: r.chr[3] = v; break;
    case 0xC800:
      if (!r.toggle) r.counter = (r.counter & 0x00FF) | (v << 8);
      else r.counter = (r.counter & 0xFF00) | v;
      r.toggle ^= 1;
      break;
    case 0xD800: r.irqEnable = (v >> 4) & 1; r.toggle = 0; break;
    case 0xE800: r.mir = v & 3; break;
    case 0xF800: r.prg = v; break;
    }
    sync();
    return false;
  }
  void cpuClock(int n) override {
    if (!r.irqEnable) return;
    int c = (int)r.counter - n;
    if (c < 0) { irq = true; r.irqEnable = 0; c = 0xFFFF; }
    r.counter = (uint16_t)c;
  }
protected:
  struct R { uint8_t chr[4], mir, prg, toggle, irqEnable; uint16_t counter; };
  MAPPER_REGS(R)
  void sync() override {
    for (int i = 0; i < 4; i++) setChr2(i, r.chr[i]);
    setPrg16(0, r.prg);
    setPrg16(1, -1);
    mirror(kMirror4[r.mir & 3]);
  }
};

class Sunsoft4 : public Mapper {
public:
  explicit Sunsoft4(const MapperInfo& in) : Mapper(in) { memset(&r, 0, sizeof(r)); sync(); }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    switch (a & 0xF000) {
    case 0x8000: r.chr[0] = v; break;
    case 0x9000: r.chr[1] = v; break;
    case 0xA000: r.chr[2] = v; break;
    case 0xB000: r.chr[3] = v; break;
    case 0xC000: r.ntReg[0] = v; break;
    case 0xD000: r.ntReg[1] = v; break;
    case 0xE000: r.ctl = v; break;
    case 0xF000: r.prg = v; break;
    }
    sync();
    return false;
  }
protected:
  struct R { uint8_t chr[4], ntReg[2], ctl, prg; };
  MAPPER_REGS(R)
  void sync() override {
    for (int i = 0; i < 4; i++) setChr2(i, r.chr[i]);
    setPrg16(0, r.prg & 0x0F);
    setPrg16(1, -1);
    mirror(kMirror4[r.ctl & 3]);
    if (r.ctl & 0x10) {
      for (int t = 0; t < 4; t++) setNtRom(t, r.ntReg[nt[t] & 1] | 0x80);
    }
  }
};

// ---------------------------------------------------------------------------
// Sunsoft FME-7 / 5B (69)
// ---------------------------------------------------------------------------

class FME7 : public Mapper {
public:
  explicit FME7(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    r.noiseLfsr = 1;
    wantsCpuClock = true;
    hasAudio = true;
    for (int i = 0; i < 16; i++) amp[i] = i ? std::pow(10.0f, (float)(i - 15) * 1.5f / 10.0f) : 0.0f;
    sync();
  }
  bool cpuReadLow(uint16_t a, uint8_t& v) override {
    if (a < 0x6000) return false;
    uint8_t b = r.reg[8];
    if (b & 0x40) {
      if ((b & 0x80) && info.wram && (uint32_t)(a - 0x6000) < info.wramSize) v = info.wram[a - 0x6000];
      else v = a >> 8;  // RAM disabled: open bus
    } else {
      int n = prgPages8();
      v = prg[(uint32_t)((b & 0x3F) % n) * 0x2000 + (a & 0x1FFF)];
    }
    return true;
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) {
      if (a >= 0x6000) {
        uint8_t b = r.reg[8];
        if ((b & 0xC0) == 0xC0 && info.wram && (uint32_t)(a - 0x6000) < info.wramSize)
          info.wram[a - 0x6000] = v;
        return true;
      }
      return false;
    }
    switch (a & 0xE000) {
    case 0x8000: r.cmd = v & 0x0F; break;
    case 0xA000:
      r.reg[r.cmd] = v;
      if (r.cmd == 0x0D) { r.irqCtl = v; irq = false; }
      else if (r.cmd == 0x0E) r.counter = (r.counter & 0xFF00) | v;
      else if (r.cmd == 0x0F) r.counter = (r.counter & 0x00FF) | (v << 8);
      sync();
      break;
    case 0xC000: r.sbSel = v & 0x0F; break;
    case 0xE000:
      r.sb[r.sbSel] = v;
      if (r.sbSel == 13) { r.envStep = 0; r.envHold = 0; r.envTimer = 0; }
      break;
    }
    return false;
  }
  void cpuClock(int n) override {
    if (!(r.irqCtl & 0x80)) return;
    int c = (int)r.counter - n;
    if (c < 0) { c += 0x10000; if (r.irqCtl & 1) irq = true; }
    r.counter = (uint16_t)c;
  }
  float audio(uint32_t n) override {
    float sum = 0.0f;
    for (uint32_t c = 0; c < n; c++) {
      for (int i = 0; i < 3; i++) {
        if (--r.toneTimer[i] <= 0) {
          int period = ((r.sb[i * 2 + 1] & 0x0F) << 8) | r.sb[i * 2];
          if (period == 0) period = 1;
          r.toneTimer[i] = period * 16;
          r.toneOut[i] ^= 1;
        }
      }
      if (--r.noiseTimer <= 0) {
        int period = r.sb[6] & 0x1F;
        if (period == 0) period = 1;
        r.noiseTimer = period * 32;
        uint32_t bit = (r.noiseLfsr ^ (r.noiseLfsr >> 3)) & 1;
        r.noiseLfsr = (r.noiseLfsr >> 1) | (bit << 16);
      }
      if (--r.envTimer <= 0) {
        int period = r.sb[11] | (r.sb[12] << 8);
        if (period == 0) period = 1;
        r.envTimer = period * 32;
        if (!r.envHold) {
          r.envStep++;
          if (r.envStep >= 16) {
            uint8_t shape = r.sb[13];
            if (!(shape & 8) || (shape & 1)) { r.envHold = 1; r.envStep = 15; }
            else { r.envStep = 0; if (shape & 2) r.envAltFlip ^= 1; }
          }
        }
      }
      uint8_t shape = r.sb[13];
      int env;
      bool attack = (shape & 4) != 0;
      if (r.envHold) {
        if (!(shape & 8)) env = 0;
        else env = (attack ^ ((shape & 2) != 0)) ? 15 : 0;
      } else {
        bool up = attack ^ (r.envAltFlip != 0);
        env = up ? r.envStep : 15 - r.envStep;
      }
      for (int i = 0; i < 3; i++) {
        bool toneOn = r.toneOut[i] || (r.sb[7] & (1 << i));
        bool noiseOn = (r.noiseLfsr & 1) || (r.sb[7] & (8 << i));
        if (!(toneOn && noiseOn)) continue;
        uint8_t vol = r.sb[8 + i];
        sum += amp[(vol & 0x10) ? env : (vol & 0x0F)];
      }
    }
    return sum * 0.11f;
  }
protected:
  struct R {
    uint8_t cmd, reg[16], irqCtl;
    uint16_t counter;
    uint8_t sbSel, sb[16];
    int32_t toneTimer[3];
    uint8_t toneOut[3];
    int32_t noiseTimer;
    uint32_t noiseLfsr;
    int32_t envTimer;
    uint8_t envStep, envHold, envAltFlip;
  };
  MAPPER_REGS(R)
  float amp[16];
  void sync() override {
    for (int i = 0; i < 8; i++) setChr1(i, r.reg[i]);
    setPrg8(0, r.reg[9] & 0x3F);
    setPrg8(1, r.reg[10] & 0x3F);
    setPrg8(2, r.reg[11] & 0x3F);
    setPrg8(3, -1);
    mirror(kMirror4[r.reg[12] & 3]);
  }
};

// ---------------------------------------------------------------------------
// Bandai FCG / LZ93D50 (16, 159)
// ---------------------------------------------------------------------------

class BandaiFCG : public Mapper {
public:
  explicit BandaiFCG(const MapperInfo& in) : Mapper(in) { memset(&r, 0, sizeof(r)); wantsCpuClock = true; sync(); }
  bool cpuReadLow(uint16_t a, uint8_t& v) override {
    if (a >= 0x6000 && !info.battery) { v = 0; return true; }   // EEPROM data line (not emulated)
    return false;
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x6000) return false;
    switch (a & 0x0F) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7: r.chr[a & 7] = v; break;
    case 8: r.prg = v & 0x0F; break;
    case 9: r.mir = v & 3; break;
    case 0x0A: r.irqEnable = v & 1; r.counter = r.latch; irq = false; break;
    case 0x0B: r.latch = (r.latch & 0xFF00) | v; r.counter = (r.counter & 0xFF00) | v; break;
    case 0x0C: r.latch = (r.latch & 0x00FF) | (v << 8); r.counter = (r.counter & 0x00FF) | (v << 8); break;
    default: break;
    }
    sync();
    return a < 0x8000 && !info.battery;
  }
  void cpuClock(int n) override {
    if (!r.irqEnable) return;
    int c = (int)r.counter - n;
    if (r.counter > 0 && c <= 0) irq = true;
    r.counter = (uint16_t)(c & 0xFFFF);
  }
protected:
  struct R { uint8_t chr[8], prg, mir, irqEnable; uint16_t latch, counter; };
  MAPPER_REGS(R)
  void sync() override {
    for (int i = 0; i < 8; i++) setChr1(i, r.chr[i]);
    setPrg16(0, r.prg);
    setPrg16(1, -1);
    mirror(kMirror4[r.mir]);
  }
};

// ---------------------------------------------------------------------------
// Jaleco SS88006 (18)
// ---------------------------------------------------------------------------

class JalecoSS88006 : public Mapper {
public:
  explicit JalecoSS88006(const MapperInfo& in) : Mapper(in) { memset(&r, 0, sizeof(r)); wantsCpuClock = true; sync(); }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    int reg = a & 0xF003;
    v &= 0x0F;
    if (reg >= 0xA000 && reg < 0xE000) {
      int idx = (((reg >> 12) - 0xA) << 1) | ((reg >> 1) & 1);
      if (reg & 1) r.chr[idx] = (r.chr[idx] & 0x0F) | (v << 4);
      else r.chr[idx] = (r.chr[idx] & 0xF0) | v;
    } else {
      switch (reg) {
      case 0x8000: r.prg[0] = (r.prg[0] & 0xF0) | v; break;
      case 0x8001: r.prg[0] = (r.prg[0] & 0x0F) | (v << 4); break;
      case 0x8002: r.prg[1] = (r.prg[1] & 0xF0) | v; break;
      case 0x8003: r.prg[1] = (r.prg[1] & 0x0F) | (v << 4); break;
      case 0x9000: r.prg[2] = (r.prg[2] & 0xF0) | v; break;
      case 0x9001: r.prg[2] = (r.prg[2] & 0x0F) | (v << 4); break;
      case 0xE000: case 0xE001: case 0xE002: case 0xE003: {
        int shift = (reg & 3) * 4;
        r.latch = (r.latch & ~(0xF << shift)) | (v << shift);
        break;
      }
      case 0xF000: r.counter = r.latch; irq = false; break;
      case 0xF001: r.ctl = v; irq = false; break;
      case 0xF002: r.mir = v & 3; break;
      }
    }
    sync();
    return false;
  }
  void cpuClock(int n) override {
    if (!(r.ctl & 1)) return;
    uint16_t mask = (r.ctl & 8) ? 0x000F : (r.ctl & 4) ? 0x00FF : (r.ctl & 2) ? 0x0FFF : 0xFFFF;
    int low = r.counter & mask;
    if (low < n) irq = true;
    low = (low - n) & mask;
    r.counter = (r.counter & ~mask) | low;
  }
protected:
  struct R { uint8_t prg[3], chr[8], mir, ctl; uint16_t latch, counter; };
  MAPPER_REGS(R)
  void sync() override {
    for (int i = 0; i < 3; i++) setPrg8(i, r.prg[i]);
    setPrg8(3, -1);
    for (int i = 0; i < 8; i++) setChr1(i, r.chr[i]);
    static const int m[4] = {MIR_H, MIR_V, MIR_A, MIR_B};
    mirror(m[r.mir]);
  }
};

// ---------------------------------------------------------------------------
// Taito TC0190 (33), TC0690 (48)
// ---------------------------------------------------------------------------

class TaitoTC0190 : public Mapper {
public:
  explicit TaitoTC0190(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    wantsScanline = (in.number == 48);
    sync();
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x8000) return false;
    switch (a & 0xE003) {
    case 0x8000: r.prg0 = v & 0x3F; if (info.number == 33) r.mir = (v >> 6) & 1; break;
    case 0x8001: r.prg1 = v & 0x3F; break;
    case 0x8002: r.chr[0] = v; break;
    case 0x8003: r.chr[1] = v; break;
    case 0xA000: case 0xA001: case 0xA002: case 0xA003: r.chr[2 + (a & 3)] = v; break;
    case 0xC000: r.latch = v; break;
    case 0xC001: r.reload = 1; r.counter = 0; break;
    case 0xC002: r.irqEnable = 1; break;
    case 0xC003: r.irqEnable = 0; irq = false; break;
    case 0xE000: if (info.number == 48) r.mir = (v >> 6) & 1; break;
    }
    sync();
    return false;
  }
  void scanline() override {
    if (r.counter == 0 || r.reload) { r.counter = (uint16_t)((r.latch ^ 0xFF) + 1); r.reload = 0; }
    else r.counter--;
    if (r.counter == 0 && r.irqEnable) irq = true;
  }
protected:
  struct R { uint8_t prg0, prg1, chr[6], mir, latch, reload, irqEnable; uint16_t counter; };
  MAPPER_REGS(R)
  void sync() override {
    setPrg8(0, r.prg0); setPrg8(1, r.prg1); setPrg8(2, -2); setPrg8(3, -1);
    setChr2(0, r.chr[0]);
    setChr2(1, r.chr[1]);
    for (int i = 0; i < 4; i++) setChr1(4 + i, r.chr[2 + i]);
    mirror(r.mir ? MIR_H : MIR_V);
  }
};

// ---------------------------------------------------------------------------
// Taito X1-005 (80, 207), X1-017 (82)
// ---------------------------------------------------------------------------

class TaitoX1 : public Mapper {
public:
  explicit TaitoX1(const MapperInfo& in) : Mapper(in) { memset(&r, 0, sizeof(r)); sync(); }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x7EF0 || a > 0x7EFF) return false;
    int reg = a & 0x0F;
    if (info.number == 82) {
      switch (reg) {
      case 0: case 1: case 2: case 3: case 4: case 5: r.chr[reg] = v; break;
      case 6: r.mir = v; break;
      case 0xA: r.prg[0] = v >> 2; break;
      case 0xB: r.prg[1] = v >> 2; break;
      case 0xC: r.prg[2] = v >> 2; break;
      }
    } else {
      switch (reg) {
      case 0: case 1: case 2: case 3: case 4: case 5: r.chr[reg] = v; break;
      case 6: case 7: r.mir = v; break;
      case 0xA: case 0xB: r.prg[0] = v; break;
      case 0xC: case 0xD: r.prg[1] = v; break;
      case 0xE: case 0xF: r.prg[2] = v; break;
      }
    }
    sync();
    return true;
  }
protected:
  struct R { uint8_t chr[6], mir, prg[3]; };
  MAPPER_REGS(R)
  void sync() override {
    for (int i = 0; i < 3; i++) setPrg8(i, r.prg[i]);
    setPrg8(3, -1);
    int banks[8];
    int mask = (info.number == 207) ? 0x7F : 0xFF;
    banks[0] = (r.chr[0] & mask) & ~1; banks[1] = (r.chr[0] & mask) | 1;
    banks[2] = (r.chr[1] & mask) & ~1; banks[3] = (r.chr[1] & mask) | 1;
    for (int i = 0; i < 4; i++) banks[4 + i] = r.chr[2 + i] & mask;
    int inv = (info.number == 82 && (r.mir & 2)) ? 4 : 0;
    for (int i = 0; i < 8; i++) setChr1(i ^ inv, banks[i]);
    if (info.number == 207) {
      int p0 = r.chr[0] >> 7, p1 = r.chr[1] >> 7;
      mirrorPages(p0, p0, p1, p1);
    } else {
      mirror(r.mir & 1 ? MIR_V : MIR_H);
    }
  }
};

// ---------------------------------------------------------------------------
// Namco 163 (19) with audio, Namco 175 / 340 (210)
// ---------------------------------------------------------------------------

class Namco163 : public Mapper {
public:
  explicit Namco163(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    r.prg[0] = 0; r.prg[1] = 1; r.prg[2] = 0xFE;
    r.n340 = (in.number == 210 && in.submapper == 2) ? 1 : 0;
    wantsCpuClock = (in.number == 19);
    hasAudio = (in.number == 19);
    sync();
  }
  bool cpuReadLow(uint16_t a, uint8_t& v) override {
    if (info.number != 19 || a >= 0x6000) return false;
    if (a >= 0x4800 && a < 0x5000) {
      v = r.ram[r.addr & 0x7F];
      if (r.addr & 0x80) r.addr = 0x80 | ((r.addr + 1) & 0x7F);
      return true;
    }
    if (a >= 0x5000 && a < 0x5800) { v = r.counter & 0xFF; return true; }
    if (a >= 0x5800 && a < 0x6000) { v = r.counter >> 8; return true; }
    return false;
  }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a < 0x6000) {
      if (info.number != 19) return false;
      if (a >= 0x4800 && a < 0x5000) {
        r.ram[r.addr & 0x7F] = v;
        if (r.addr & 0x80) r.addr = 0x80 | ((r.addr + 1) & 0x7F);
      } else if (a >= 0x5000 && a < 0x5800) {
        r.counter = (r.counter & 0xFF00) | v; irq = false;
      } else if (a >= 0x5800 && a < 0x6000) {
        r.counter = (r.counter & 0x00FF) | (v << 8); irq = false;
      }
      return false;
    }
    if (a < 0x8000) return false;
    if (a < 0xC000) r.chr[(a - 0x8000) >> 11] = v;
    else if (a < 0xE000) { if (info.number == 19) r.ntr[(a - 0xC000) >> 11] = v; }
    else if (a < 0xE800) {
      r.prg[0] = v & 0x3F;
      r.e000 = v;
      if (info.number == 210 && (v & 0xC0)) r.n340 = 1;
    }
    else if (a < 0xF000) { r.prg[1] = v & 0x3F; r.e800 = v; }
    else if (a < 0xF800) r.prg[2] = v & 0x3F;
    else r.addr = v;
    sync();
    return false;
  }
  void cpuClock(int n) override {
    if (!(r.counter & 0x8000)) return;
    uint32_t low = r.counter & 0x7FFF;
    if (low == 0x7FFF) return;
    low += n;
    if (low >= 0x7FFF) { low = 0x7FFF; irq = true; }
    r.counter = 0x8000 | low;
  }
  float audio(uint32_t n) override {
    if (r.e000 & 0x40) return 0.0f;
    int channels = ((r.ram[0x7F] >> 4) & 7) + 1;
    float sum = 0.0f;
    for (uint32_t c = 0; c < n; c++) {
      if (++r.tick >= 15) {
        r.tick = 0;
        int ch = 7 - r.cur;
        int b = 0x40 + ch * 8;
        uint32_t freq = r.ram[b] | (r.ram[b + 2] << 8) | ((r.ram[b + 4] & 3) << 16);
        uint32_t phase = r.ram[b + 1] | (r.ram[b + 3] << 8) | (r.ram[b + 5] << 16);
        uint32_t len = 256 - (r.ram[b + 4] & 0xFC);
        phase = (phase + freq) % (len << 16);
        r.ram[b + 1] = phase & 0xFF;
        r.ram[b + 3] = (phase >> 8) & 0xFF;
        r.ram[b + 5] = (phase >> 16) & 0xFF;
        uint32_t idx = ((phase >> 16) + r.ram[b + 6]) & 0xFF;
        uint8_t byte = r.ram[(idx >> 1) & 0x7F];
        int sample = (idx & 1) ? (byte >> 4) : (byte & 0x0F);
        r.chOut[ch] = (int16_t)((sample - 8) * (r.ram[b + 7] & 0x0F));
        r.cur = (r.cur + 1) % channels;
      }
      int mix = 0;
      for (int i = 0; i < channels; i++) mix += r.chOut[7 - i];
      sum += (float)mix / (float)channels;
    }
    return sum * 0.0026f;
  }
protected:
  struct R {
    uint8_t chr[8], ntr[4], prg[3], e000, e800, addr, n340, cur, tick;
    uint16_t counter;
    uint8_t ram[128];
    int16_t chOut[8];
  };
  MAPPER_REGS(R)
  void sync() override {
    for (int i = 0; i < 3; i++) setPrg8(i, r.prg[i]);
    setPrg8(3, -1);
    for (int i = 0; i < 8; i++) setChr1(i, r.chr[i]);
    if (info.number == 19) {
      for (int t = 0; t < 4; t++) {
        uint8_t v = r.ntr[t];
        if (v >= 0xE0) { nt[t] = v & 1; ntRom[t] = false; }
        else setNtRom(t, v);
      }
    } else if (r.n340) {
      static const int m[4] = {MIR_A, MIR_V, MIR_H, MIR_B};
      mirror(m[(r.e000 >> 6) & 3]);
    }
  }
  void afterLoad() override { sync(); }
};

// ---------------------------------------------------------------------------
// Action 53 (28)
// ---------------------------------------------------------------------------

class Action53 : public Mapper {
public:
  explicit Action53(const MapperInfo& in) : Mapper(in) {
    memset(&r, 0, sizeof(r));
    r.outer = 0xFF;
    r.mode = 0x00;
    sync();
  }
  void reset() override { r.sel = 0; r.outer = 0xFF; r.mode = 0; sync(); }
  bool cpuWrite(uint16_t a, uint8_t v) override {
    if (a >= 0x5000 && a < 0x6000) { r.sel = v & 0x81; return false; }
    if (a < 0x8000) return false;
    switch (r.sel) {
    case 0x00:
      r.chr = v & 3;
      if (!(r.mode & 2)) r.mode = (r.mode & ~1) | ((v >> 4) & 1);
      break;
    case 0x01:
      r.inner = v & 0x0F;
      if (!(r.mode & 2)) r.mode = (r.mode & ~1) | ((v >> 4) & 1);
      break;
    case 0x80: r.mode = v & 0x3F; break;
    case 0x81: r.outer = v; break;
    }
    sync();
    return false;
  }
protected:
  struct R { uint8_t sel, chr, inner, mode, outer; };
  MAPPER_REGS(R)
  void sync() override {
    int bankMode = (r.mode >> 2) & 3;
    int mask = (2 << ((r.mode >> 4) & 3)) - 1;
    for (int a14 = 0; a14 < 2; a14++) {
      int bank;
      // mode 2: $8000 fixed, mode 3: $C000 fixed, modes 0-1: 32K
      bool fixedHalf = (bankMode == 2) ? (a14 == 0) : (bankMode == 3) ? (a14 == 1) : false;
      if (fixedHalf) {
        bank = (r.outer << 1) | a14;
      } else {
        int inner16 = (bankMode & 2) ? r.inner : ((r.inner << 1) | a14);
        bank = ((r.outer << 1) & ~mask) | (inner16 & mask);
      }
      setPrg16(a14, bank);
    }
    setChr8(r.chr);
    static const int m[4] = {MIR_A, MIR_B, MIR_V, MIR_H};
    mirror(m[r.mode & 3]);
  }
};

// ---------------------------------------------------------------------------

bool mapperImplemented(int mapper)
{
  MapperInfo info;
  info.number = mapper;
  Mapper* m = createMapper(info);
  bool ok = (m != nullptr);
  delete m;
  return ok;
}

uint32_t mapperChrRamSize(int mapper)
{
  switch (mapper) {
  case 13: return 0x4000;
  case 28: return 0x8000;
  case 30: return 0x8000;
  default: return 0x2000;
  }
}

Mapper* createMapper(const MapperInfo& in)
{
  switch (in.number) {
  case 11: case 13: case 34: case 70: case 71: case 72: case 78: case 79:
  case 86: case 87: case 89: case 92: case 93: case 94: case 97: case 113:
  case 140: case 152: case 180: case 184: case 185: case 228: case 232: {
    DiscreteMapper* m = new DiscreteMapper(in);
    if (in.number == 78) {
      m->holyDiver = in.fourScreen || in.submapper == 3;
      if (m->holyDiver) {
        MapperInfo i2 = in;
        i2.fourScreen = false;
        delete m;
        m = new DiscreteMapper(i2);
        m->holyDiver = true;
        m->reset();
      }
    }
    return m;
  }
  case 30: {
    MapperInfo i2 = in;
    bool oneScreen = in.fourScreen && !in.verticalMirroring;
    if (oneScreen) i2.fourScreen = false;
    DiscreteMapper* m = new DiscreteMapper(i2);
    m->oneScreen30 = oneScreen;
    m->reset();
    return m;
  }
  case 28: return new Action53(in);
  case 16: case 159: return new BandaiFCG(in);
  case 18: return new JalecoSS88006(in);
  case 19: case 210: return new Namco163(in);
  case 21: case 22: case 23: case 25: return new VRC24(in);
  case 24: case 26: return new VRC6(in);
  case 32: return new IremG101(in);
  case 33: case 48: return new TaitoTC0190(in);
  case 64: case 158: return new Rambo1(in);
  case 65: return new IremH3001(in);
  case 67: return new Sunsoft3(in);
  case 68: return new Sunsoft4(in);
  case 69: return new FME7(in);
  case 73: return new VRC3(in);
  case 75: return new VRC1(in);
  case 76: case 88: case 95: case 154: case 206: return new Namco108(in);
  case 80: case 82: case 207: return new TaitoX1(in);
  case 85: return new VRC7(in);
  default: return nullptr;
  }
}
