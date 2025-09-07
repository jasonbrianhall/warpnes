#include <fstream>
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <cstdint>
#include <SDL2/SDL.h>
#include "APU.hpp"

// NSF Header structure
struct NSFHeader {
    char magic[5];          // "NESM" + 0x1A
    uint8_t version;        // Version number
    uint8_t totalSongs;     // Total number of songs
    uint8_t startSong;      // Starting song number (1-based)
    uint16_t loadAddr;      // Load address of data
    uint16_t initAddr;      // Init routine address
    uint16_t playAddr;      // Play routine address
    char songName[32];      // Song name (null-terminated)
    char artist[32];        // Artist name (null-terminated)
    char copyright[32];     // Copyright (null-terminated)
    uint16_t playSpeedNTSC; // Play speed for NTSC (microseconds)
    uint8_t bankSwitch[8];  // Bank switching init values
    uint16_t playSpeedPAL;  // Play speed for PAL (microseconds)
    uint8_t palNtscBits;    // PAL/NTSC bits
    uint8_t extraChips;     // Extra sound chip support
    uint8_t reserved[4];    // Reserved bytes
};

// Simple CPU emulator for NSF playback
class NSFCpu {
public:
    // 6502 registers
    uint8_t regA, regX, regY, regSP, regP;
    uint16_t regPC;
    
    // Status flags
    static const uint8_t FLAG_CARRY = 0x01;
    static const uint8_t FLAG_ZERO = 0x02;
    static const uint8_t FLAG_INTERRUPT = 0x04;
    static const uint8_t FLAG_DECIMAL = 0x08;
    static const uint8_t FLAG_BREAK = 0x10;
    static const uint8_t FLAG_UNUSED = 0x20;
    static const uint8_t FLAG_OVERFLOW = 0x40;
    static const uint8_t FLAG_NEGATIVE = 0x80;
    
    // Memory
    uint8_t ram[0x800];     // 2KB internal RAM
    uint8_t* prgRom;        // PRG ROM data
    uint16_t prgSize;
    uint16_t loadAddr;
    
    APU* apu;
    
    NSFCpu() : regA(0), regX(0), regY(0), regSP(0xFD), regP(FLAG_UNUSED | FLAG_INTERRUPT), regPC(0) {
        memset(ram, 0, sizeof(ram));
        prgRom = nullptr;
        prgSize = 0;
        loadAddr = 0;
        apu = new APU();
    }
    
    ~NSFCpu() {
        if (apu) delete apu;
    }
    
    void reset() {
        regA = regX = regY = 0;
        regSP = 0xFD;
        regP = FLAG_UNUSED | FLAG_INTERRUPT;
        regPC = 0;
        memset(ram, 0, sizeof(ram));
    }
    
    // Memory access
    uint8_t readByte(uint16_t addr) {
        if (addr < 0x2000) {
            return ram[addr & 0x7FF];
        } else if (addr >= 0x4000 && addr <= 0x4017) {
            return 0;
        } else if (addr >= loadAddr && addr < 0x10000) {
            uint16_t offset = addr - loadAddr;
            if (offset < prgSize) {
                return prgRom[offset];
            }
        }
        return 0;
    }
    
    void writeByte(uint16_t addr, uint8_t value) {
        if (addr < 0x2000) {
            ram[addr & 0x7FF] = value;
        } else if (addr >= 0x4000 && addr <= 0x4017) {
            // Debug APU writes
            static int writeCount = 0;
            if (writeCount < 20) {
                printf("APU Write: 0x%04X = 0x%02X\n", addr, value);
                writeCount++;
            }
            apu->writeRegister(addr, value);
        }
    }
    
    uint16_t readWord(uint16_t addr) {
        return readByte(addr) | (readByte(addr + 1) << 8);
    }
    
    // Stack operations
    void pushByte(uint8_t value) {
        writeByte(0x100 + regSP, value);
        regSP--;
    }
    
    uint8_t pullByte() {
        regSP++;
        return readByte(0x100 + regSP);
    }
    
    void pushWord(uint16_t value) {
        pushByte(value >> 8);
        pushByte(value & 0xFF);
    }
    
    uint16_t pullWord() {
        uint8_t lo = pullByte();
        uint8_t hi = pullByte();
        return lo | (hi << 8);
    }
    
    // Instruction fetch
    uint8_t fetchByte() { return readByte(regPC++); }
    
    uint16_t fetchWord() {
        uint16_t value = readWord(regPC);
        regPC += 2;
        return value;
    }
    
    // Flag operations
    void setFlag(uint8_t flag, bool value) {
        if (value) {
            regP |= flag;
        } else {
            regP &= ~flag;
        }
    }
    
    bool getFlag(uint8_t flag) const { return (regP & flag) != 0; }
    
    void updateZN(uint8_t value) {
        setFlag(FLAG_ZERO, value == 0);
        setFlag(FLAG_NEGATIVE, (value & 0x80) != 0);
    }
    
    // Addressing modes
    uint16_t addrImmediate() { return regPC++; }
    uint16_t addrZeroPage() { return fetchByte(); }
    uint16_t addrZeroPageX() { return (fetchByte() + regX) & 0xFF; }
    uint16_t addrZeroPageY() { return (fetchByte() + regY) & 0xFF; }
    uint16_t addrAbsolute() { return fetchWord(); }
    uint16_t addrAbsoluteX() { return fetchWord() + regX; }
    uint16_t addrAbsoluteY() { return fetchWord() + regY; }
    
    uint16_t addrIndirect() {
        uint16_t addr = fetchWord();
        if ((addr & 0xFF) == 0xFF) {
            return readByte(addr) | (readByte(addr & 0xFF00) << 8);
        } else {
            return readWord(addr);
        }
    }
    
    uint16_t addrIndirectX() {
        uint8_t addr = (fetchByte() + regX) & 0xFF;
        return readByte(addr) | (readByte((addr + 1) & 0xFF) << 8);
    }
    
    uint16_t addrIndirectY() {
        uint8_t addr = fetchByte();
        uint16_t base = readByte(addr) | (readByte((addr + 1) & 0xFF) << 8);
        return base + regY;
    }
    
    uint16_t addrRelative() {
        int8_t offset = fetchByte();
        return regPC + offset;
    }
    
    // Essential instructions for NSF playback
    void LDA(uint16_t addr) { regA = readByte(addr); updateZN(regA); }
    void LDX(uint16_t addr) { regX = readByte(addr); updateZN(regX); }
    void LDY(uint16_t addr) { regY = readByte(addr); updateZN(regY); }
    void STA(uint16_t addr) { writeByte(addr, regA); }
    void STX(uint16_t addr) { writeByte(addr, regX); }
    void STY(uint16_t addr) { writeByte(addr, regY); }
    void TAX() { regX = regA; updateZN(regX); }
    void TAY() { regY = regA; updateZN(regY); }
    void TXA() { regA = regX; updateZN(regA); }
    void TYA() { regA = regY; updateZN(regA); }
    void TXS() { regSP = regX; }
    void TSX() { regX = regSP; updateZN(regX); }
    void PHA() { pushByte(regA); }
    void PLA() { regA = pullByte(); updateZN(regA); }
    void PHP() { pushByte(regP | FLAG_BREAK | FLAG_UNUSED); }
    void PLP() { regP = pullByte() | FLAG_UNUSED; regP &= ~FLAG_BREAK; }
    void INX() { regX++; updateZN(regX); }
    void INY() { regY++; updateZN(regY); }
    void DEX() { regX--; updateZN(regX); }
    void DEY() { regY--; updateZN(regY); }
    void NOP() { /* Do nothing */ }
    void JSR(uint16_t addr) { pushWord(regPC - 1); regPC = addr; }
    void RTS() { regPC = pullWord() + 1; }
    void JMP(uint16_t addr) { regPC = addr; }
    void BRK() { regPC++; pushWord(regPC); pushByte(regP | FLAG_BREAK); setFlag(FLAG_INTERRUPT, true); regPC = readWord(0xFFFE); }
    void RTI() { regP = pullByte() | FLAG_UNUSED; regP &= ~FLAG_BREAK; regPC = pullWord(); }
    
    // Branch instructions
    void BEQ() { if (getFlag(FLAG_ZERO)) regPC = addrRelative(); else regPC++; }
    void BNE() { if (!getFlag(FLAG_ZERO)) regPC = addrRelative(); else regPC++; }
    void BCC() { if (!getFlag(FLAG_CARRY)) regPC = addrRelative(); else regPC++; }
    void BCS() { if (getFlag(FLAG_CARRY)) regPC = addrRelative(); else regPC++; }
    void BPL() { if (!getFlag(FLAG_NEGATIVE)) regPC = addrRelative(); else regPC++; }
    void BMI() { if (getFlag(FLAG_NEGATIVE)) regPC = addrRelative(); else regPC++; }
    void BVC() { if (!getFlag(FLAG_OVERFLOW)) regPC = addrRelative(); else regPC++; }
    void BVS() { if (getFlag(FLAG_OVERFLOW)) regPC = addrRelative(); else regPC++; }
    
    // Flag operations
    void CLC() { setFlag(FLAG_CARRY, false); }
    void SEC() { setFlag(FLAG_CARRY, true); }
    void CLI() { setFlag(FLAG_INTERRUPT, false); }
    void SEI() { setFlag(FLAG_INTERRUPT, true); }
    void CLV() { setFlag(FLAG_OVERFLOW, false); }
    void CLD() { setFlag(FLAG_DECIMAL, false); }
    void SED() { setFlag(FLAG_DECIMAL, true); }
    
    // Arithmetic and logic
    void ADC(uint16_t addr) {
        uint8_t value = readByte(addr);
        uint16_t result = regA + value + (getFlag(FLAG_CARRY) ? 1 : 0);
        setFlag(FLAG_CARRY, result > 0xFF);
        setFlag(FLAG_OVERFLOW, ((regA ^ result) & (value ^ result) & 0x80) != 0);
        regA = result & 0xFF;
        updateZN(regA);
    }
    
    void SBC(uint16_t addr) {
        uint8_t value = readByte(addr);
        uint16_t result = regA - value - (getFlag(FLAG_CARRY) ? 0 : 1);
        setFlag(FLAG_CARRY, result <= 0xFF);
        setFlag(FLAG_OVERFLOW, ((regA ^ result) & (~value ^ result) & 0x80) != 0);
        regA = result & 0xFF;
        updateZN(regA);
    }
    
    void AND(uint16_t addr) { regA &= readByte(addr); updateZN(regA); }
    void ORA(uint16_t addr) { regA |= readByte(addr); updateZN(regA); }
    void EOR(uint16_t addr) { regA ^= readByte(addr); updateZN(regA); }
    
    void CMP(uint16_t addr) {
        uint8_t value = readByte(addr);
        uint8_t result = regA - value;
        setFlag(FLAG_CARRY, regA >= value);
        updateZN(result);
    }
    
    void CPX(uint16_t addr) {
        uint8_t value = readByte(addr);
        uint8_t result = regX - value;
        setFlag(FLAG_CARRY, regX >= value);
        updateZN(result);
    }
    
    void CPY(uint16_t addr) {
        uint8_t value = readByte(addr);
        uint8_t result = regY - value;
        setFlag(FLAG_CARRY, regY >= value);
        updateZN(result);
    }
    
    // Execute one instruction
    int executeInstruction() {
        uint8_t opcode = fetchByte();
        
        switch (opcode) {
            // LDA
            case 0xA9: LDA(addrImmediate()); return 2;
            case 0xA5: LDA(addrZeroPage()); return 3;
            case 0xB5: LDA(addrZeroPageX()); return 4;
            case 0xAD: LDA(addrAbsolute()); return 4;
            case 0xBD: LDA(addrAbsoluteX()); return 4;
            case 0xB9: LDA(addrAbsoluteY()); return 4;
            case 0xA1: LDA(addrIndirectX()); return 6;
            case 0xB1: LDA(addrIndirectY()); return 5;
            
            // LDX
            case 0xA2: LDX(addrImmediate()); return 2;
            case 0xA6: LDX(addrZeroPage()); return 3;
            case 0xB6: LDX(addrZeroPageY()); return 4;
            case 0xAE: LDX(addrAbsolute()); return 4;
            case 0xBE: LDX(addrAbsoluteY()); return 4;
            
            // LDY
            case 0xA0: LDY(addrImmediate()); return 2;
            case 0xA4: LDY(addrZeroPage()); return 3;
            case 0xB4: LDY(addrZeroPageX()); return 4;
            case 0xAC: LDY(addrAbsolute()); return 4;
            case 0xBC: LDY(addrAbsoluteX()); return 4;
            
            // STA
            case 0x85: STA(addrZeroPage()); return 3;
            case 0x95: STA(addrZeroPageX()); return 4;
            case 0x8D: STA(addrAbsolute()); return 4;
            case 0x9D: STA(addrAbsoluteX()); return 5;
            case 0x99: STA(addrAbsoluteY()); return 5;
            case 0x81: STA(addrIndirectX()); return 6;
            case 0x91: STA(addrIndirectY()); return 6;
            
            // STX
            case 0x86: STX(addrZeroPage()); return 3;
            case 0x96: STX(addrZeroPageY()); return 4;
            case 0x8E: STX(addrAbsolute()); return 4;
            
            // STY
            case 0x84: STY(addrZeroPage()); return 3;
            case 0x94: STY(addrZeroPageX()); return 4;
            case 0x8C: STY(addrAbsolute()); return 4;
            
            // Transfers
            case 0xAA: TAX(); return 2;
            case 0xA8: TAY(); return 2;
            case 0x8A: TXA(); return 2;
            case 0x98: TYA(); return 2;
            case 0x9A: TXS(); return 2;
            case 0xBA: TSX(); return 2;
            
            // Stack operations
            case 0x48: PHA(); return 3;
            case 0x68: PLA(); return 4;
            case 0x08: PHP(); return 3;
            case 0x28: PLP(); return 4;
            
            // Increments/Decrements
            case 0xE8: INX(); return 2;
            case 0xC8: INY(); return 2;
            case 0xCA: DEX(); return 2;
            case 0x88: DEY(); return 2;
            
            // Jumps/Calls
            case 0x20: JSR(addrAbsolute()); return 6;
            case 0x60: RTS(); return 6;
            case 0x4C: JMP(addrAbsolute()); return 3;
            case 0x6C: JMP(addrIndirect()); return 5;
            
            // Branches
            case 0xF0: BEQ(); return 2;
            case 0xD0: BNE(); return 2;
            case 0x90: BCC(); return 2;
            case 0xB0: BCS(); return 2;
            case 0x10: BPL(); return 2;
            case 0x30: BMI(); return 2;
            case 0x50: BVC(); return 2;
            case 0x70: BVS(); return 2;
            
            // Flags
            case 0x18: CLC(); return 2;
            case 0x38: SEC(); return 2;
            case 0x58: CLI(); return 2;
            case 0x78: SEI(); return 2;
            case 0xB8: CLV(); return 2;
            case 0xD8: CLD(); return 2;
            case 0xF8: SED(); return 2;
            
            // ADC
            case 0x69: ADC(addrImmediate()); return 2;
            case 0x65: ADC(addrZeroPage()); return 3;
            case 0x75: ADC(addrZeroPageX()); return 4;
            case 0x6D: ADC(addrAbsolute()); return 4;
            case 0x7D: ADC(addrAbsoluteX()); return 4;
            case 0x79: ADC(addrAbsoluteY()); return 4;
            case 0x61: ADC(addrIndirectX()); return 6;
            case 0x71: ADC(addrIndirectY()); return 5;
            
            // SBC
            case 0xE9: SBC(addrImmediate()); return 2;
            case 0xE5: SBC(addrZeroPage()); return 3;
            case 0xF5: SBC(addrZeroPageX()); return 4;
            case 0xED: SBC(addrAbsolute()); return 4;
            case 0xFD: SBC(addrAbsoluteX()); return 4;
            case 0xF9: SBC(addrAbsoluteY()); return 4;
            case 0xE1: SBC(addrIndirectX()); return 6;
            case 0xF1: SBC(addrIndirectY()); return 5;
            
            // NOP
            case 0xEA: NOP(); return 2;
            
            // System
            case 0x00: BRK(); return 7;
            case 0x40: RTI(); return 6;
            
            default:
                NOP();
                return 2;
        }
    }
};

class NSFPlayer {
private:
    NSFCpu cpu;
    NSFHeader header;
    std::vector<uint8_t> nsfData;
    bool isPlaying;
    int currentSong;
    SDL_AudioDeviceID audioDevice;
    
    // Audio callback - runs in separate thread
    static void audioCallback(void* userdata, Uint8* stream, int len) {
        NSFPlayer* player = static_cast<NSFPlayer*>(userdata);
        player->generateAudio(stream, len);
    }
    
    void generateAudio(Uint8* stream, int len) {
        if (!isPlaying) {
            memset(stream, 128, len); // Silence
            return;
        }
        
        static int sampleCount = 0;
        const int samplesPerFrame = 48000 / 60; // 800 samples per frame at 48kHz, 60fps
        
        for (int i = 0; i < len; i++) {
            // Time to call play routine?
            if (sampleCount % samplesPerFrame == 0) {
                cpu.regPC = header.playAddr;
                for (int j = 0; j < 1000; j++) {
                    cpu.executeInstruction();
                }
                cpu.apu->stepFrame();
            }
            
            // Get one audio sample from APU
            uint8_t sample;
            cpu.apu->output(&sample, 1);
            stream[i] = sample;
            sampleCount++;
        }
    }
    
public:
    NSFPlayer() : isPlaying(false), currentSong(1), audioDevice(0) {
        if (SDL_Init(SDL_INIT_AUDIO) < 0) {
            std::cerr << "SDL Init failed: " << SDL_GetError() << std::endl;
        }
    }
    
    ~NSFPlayer() {
        if (audioDevice) {
            SDL_CloseAudioDevice(audioDevice);
        }
        SDL_Quit();
    }
    
    bool loadNSF(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file) {
            std::cerr << "Error: Could not open NSF file: " << filename << std::endl;
            return false;
        }
        
        // Read header
        file.read(reinterpret_cast<char*>(&header), sizeof(NSFHeader));
        
        // Verify magic
        if (strncmp(header.magic, "NESM\x1A", 5) != 0) {
            std::cerr << "Error: Invalid NSF file format" << std::endl;
            return false;
        }
        
        // Read the rest of the file
        file.seekg(0, std::ios::end);
        size_t fileSize = file.tellg();
        file.seekg(sizeof(NSFHeader), std::ios::beg);
        
        size_t dataSize = fileSize - sizeof(NSFHeader);
        nsfData.resize(dataSize);
        file.read(reinterpret_cast<char*>(nsfData.data()), dataSize);
        
        // Setup CPU memory
        cpu.prgRom = nsfData.data();
        cpu.prgSize = dataSize;
        cpu.loadAddr = header.loadAddr;
        
        // Setup SDL Audio
        SDL_AudioSpec wanted, obtained;
        wanted.freq = 48000;
        wanted.format = AUDIO_U8;
        wanted.channels = 1;
        wanted.samples = 1024;
        wanted.callback = audioCallback;
        wanted.userdata = this;
        
        audioDevice = SDL_OpenAudioDevice(NULL, 0, &wanted, &obtained, 0);
        if (audioDevice == 0) {
            std::cerr << "Failed to open audio device: " << SDL_GetError() << std::endl;
            return false;
        }
        
        std::cout << "Loaded NSF: " << header.songName << std::endl;
        std::cout << "Artist: " << header.artist << std::endl;
        std::cout << "Copyright: " << header.copyright << std::endl;
        std::cout << "Songs: " << (int)header.totalSongs << std::endl;
        
        return true;
    }
    
    void initSong(int songNum) {
        if (songNum < 1 || songNum > header.totalSongs) {
            std::cerr << "Invalid song number: " << songNum << std::endl;
            return;
        }
        
        currentSong = songNum;
        
        // Reset CPU
        cpu.reset();
        
        // Set up registers for NSF init
        cpu.regA = songNum - 1;  // Song number (0-based)
        cpu.regX = 0;            // PAL/NTSC flag (0 = NTSC)
        cpu.regPC = header.initAddr;
        
        std::cout << "Initialized song " << songNum << std::endl;
        
        // Run init routine
        for (int i = 0; i < 10000; i++) {
            cpu.executeInstruction();
        }
    }
    
    void play() {
        if (nsfData.empty()) {
            std::cerr << "No NSF loaded" << std::endl;
            return;
        }
        
        if (!audioDevice) {
            std::cerr << "No audio device available" << std::endl;
            return;
        }
        
        initSong(currentSong);
        isPlaying = true;
        
        // Start audio playback
        SDL_PauseAudioDevice(audioDevice, 0);
        
        std::cout << "Playing song " << currentSong << " - Press Enter to stop" << std::endl;
        
        // Wait for user input
        std::string input;
        std::getline(std::cin, input);
        
        // Stop audio
        SDL_PauseAudioDevice(audioDevice, 1);
        isPlaying = false;
    }
    
    void stop() {
        isPlaying = false;
        if (audioDevice) {
            SDL_PauseAudioDevice(audioDevice, 1);
        }
    }
    
    void nextSong() {
        if (currentSong < header.totalSongs) {
            currentSong++;
            std::cout << "Next song: " << currentSong << std::endl;
        }
    }
    
    void prevSong() {
        if (currentSong > 1) {
            currentSong--;
            std::cout << "Previous song: " << currentSong << std::endl;
        }
    }
    
    void toggleAudioMode() {
        cpu.apu->toggleAudioMode();
    }
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "Usage: " << argv[0] << " <nsf_file>" << std::endl;
        return 1;
    }
    
    NSFPlayer player;
    
    if (!player.loadNSF(argv[1])) {
        return 1;
    }
    
    std::cout << "NSF Player Commands:" << std::endl;
    std::cout << "  p: Play current song" << std::endl;
    std::cout << "  s: Stop playing" << std::endl;
    std::cout << "  n: Next song" << std::endl;
    std::cout << "  b: Previous song" << std::endl;
    std::cout << "  t: Toggle audio mode" << std::endl;
    std::cout << "  q: Quit" << std::endl;
    
    std::string input;
    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);
        
        if (input == "p") {
            player.play();
        } else if (input == "s") {
            player.stop();
        } else if (input == "q") {
            break;
        } else if (input == "n") {
            player.nextSong();
        } else if (input == "b") {
            player.prevSong();
        } else if (input == "t") {
            player.toggleAudioMode();
        } else {
            std::cout << "Unknown command: " << input << std::endl;
        }
    }
    
    player.stop();
    return 0;
}
