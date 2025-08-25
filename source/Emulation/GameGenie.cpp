#include "GameGenie.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>

GameGenie::GameGenie(WarpNES* nes) : nesEmulator(nes) {
    // Initialize the Game Genie decode table
    // Game Genie uses a specific character encoding
    decodeTable['A'] = 0x0; decodeTable['P'] = 0x1; decodeTable['Z'] = 0x2; decodeTable['L'] = 0x3;
    decodeTable['G'] = 0x4; decodeTable['I'] = 0x5; decodeTable['T'] = 0x6; decodeTable['Y'] = 0x7;
    decodeTable['E'] = 0x8; decodeTable['O'] = 0x9; decodeTable['X'] = 0xA; decodeTable['U'] = 0xB;
    decodeTable['K'] = 0xC; decodeTable['S'] = 0xD; decodeTable['V'] = 0xE; decodeTable['N'] = 0xF;
}

GameGenie::~GameGenie() {
    clearAllCodes();
}

bool GameGenie::addCode(const std::string& code, const std::string& description) {
    if (!isValidCode(code)) {
        std::cerr << "Invalid Game Genie code: " << code << std::endl;
        return false;
    }

    GameGenieCode ggCode;
    if (!decodeCode(code, ggCode)) {
        std::cerr << "Failed to decode Game Genie code: " << code << std::endl;
        return false;
    }

    ggCode.originalCode = code;
    ggCode.description = description;
    ggCode.enabled = true;

    // Check if code already exists
    auto it = std::find_if(codes.begin(), codes.end(),
        [&](const GameGenieCode& existing) {
            return existing.originalCode == code;
        });

    if (it != codes.end()) {
        // Update existing code
        *it = ggCode;
        std::cout << "Updated Game Genie code: " << code << " - " << description << std::endl;
    } else {
        // Add new code
        codes.push_back(ggCode);
        std::cout << "Added Game Genie code: " << code << " - " << description << std::endl;
    }

    applyCode(ggCode);
    return true;
}

bool GameGenie::removeCode(const std::string& code) {
    auto it = std::find_if(codes.begin(), codes.end(),
        [&](const GameGenieCode& existing) {
            return existing.originalCode == code;
        });

    if (it != codes.end()) {
        restoreCode(*it);
        codes.erase(it);
        std::cout << "Removed Game Genie code: " << code << std::endl;
        return true;
    }

    return false;
}

void GameGenie::enableCode(const std::string& code, bool enable) {
    auto it = std::find_if(codes.begin(), codes.end(),
        [&](const GameGenieCode& existing) {
            return existing.originalCode == code;
        });

    if (it != codes.end()) {
        if (it->enabled != enable) {
            it->enabled = enable;
            if (enable) {
                applyCode(*it);
                std::cout << "Enabled Game Genie code: " << code << std::endl;
            } else {
                restoreCode(*it);
                std::cout << "Disabled Game Genie code: " << code << std::endl;
            }
        }
    }
}

void GameGenie::clearAllCodes() {
    // Restore all original values before clearing
    for (const auto& code : codes) {
        if (code.enabled) {
            restoreCode(code);
        }
    }
    codes.clear();
    std::cout << "Cleared all Game Genie codes" << std::endl;
}

void GameGenie::listCodes() const {
    if (codes.empty()) {
        std::cout << "No Game Genie codes loaded." << std::endl;
        return;
    }

    std::cout << "=== Game Genie Codes ===" << std::endl;
    for (size_t i = 0; i < codes.size(); i++) {
        const auto& code = codes[i];
        std::cout << std::setw(2) << (i + 1) << ". " 
                  << code.originalCode << " "
                  << (code.enabled ? "[ON] " : "[OFF]")
                  << "- " << code.description << std::endl;
        std::cout << "    Address: $" << std::hex << std::uppercase 
                  << code.address << ", Value: $" << std::setw(2) 
                  << std::setfill('0') << (int)code.value;
        if (code.hasCompare) {
            std::cout << ", Compare: $" << std::setw(2) << (int)code.compareValue;
        }
        std::cout << std::dec << std::endl;
    }
}

bool GameGenie::isValidCode(const std::string& code) const {
    // Remove any spaces or dashes
    std::string cleanCode = cleanupCode(code);
    
    // Game Genie codes are either 6 or 8 characters long
    if (cleanCode.length() != 6 && cleanCode.length() != 8) {
        return false;
    }

    // Check if all characters are valid Game Genie characters
    // Use the decode table instead of hardcoded string to ensure consistency
    for (char c : cleanCode) {
        char upperC = std::toupper(c);
        if (decodeTable.find(upperC) == decodeTable.end()) {
            return false;
        }
    }

    return true;
}

std::string GameGenie::cleanupCode(const std::string& code) const {
    std::string result;
    for (char c : code) {
        if (std::isalpha(c)) {
            result += std::toupper(c);
        }
    }
    return result;
}

bool GameGenie::decodeCode(const std::string& code, GameGenieCode& ggCode) const {
    std::string cleanCode = cleanupCode(code);
    
    if (cleanCode.length() == 6) {
        return decode6LetterCode(cleanCode, ggCode);
    } else if (cleanCode.length() == 8) {
        return decode8LetterCode(cleanCode, ggCode);
    }
    
    return false;
}

bool GameGenie::decode6LetterCode(const std::string& code, GameGenieCode& ggCode) const {
    if (code.length() != 6) {
        std::cerr << "Error: Code length is not 6\n";
        return false;
    }

    ggCode.hasCompare = false;

    // Official Game Genie character table
    static const std::string table = "APZLGITYEOXUKSVN";

    int n[6];
    std::cout << "Decoding Game Genie code: " << code << "\n";

    // Translate each character to its nibble value (index in table)
    for (int i = 0; i < 6; ++i) {
        char c = std::toupper(code[i]);
        size_t pos = table.find(c);
        if (pos == std::string::npos) {
            std::cerr << "Error: Invalid character '" << c << "' at position " << i << "\n";
            return false;
        }
        n[i] = static_cast<int>(pos);  // This is the actual nibble value
        std::cout << "  Char '" << c << "' → nibble value " << std::hex << n[i] << "\n";
    }

    // Decode address (from your original C logic)
    int address_int = 0x8000 +
        (((n[3] & 7) << 12) |
         ((n[5] & 7) << 8)  |
         ((n[4] & 8) << 8)  |
         ((n[2] & 7) << 4)  |
         ((n[1] & 8) << 4)  |
         (n[4] & 7)         |
         (n[3] & 8));

    std::cout << "Decoded address = 0x" << std::hex << address_int << " (" << std::dec << address_int << ")\n";

    // Decode value (from your original C logic)
    int data_int =
        ((n[1] & 7) << 4) |
        ((n[0] & 8) << 4) |
        (n[0] & 7)       |
        (n[5] & 8);

    std::cout << "Decoded value = 0x" << std::hex << data_int << "\n";

    // Validate and assign
    if (address_int >= 0x8000) {
        ggCode.address = static_cast<uint16_t>(address_int);
        ggCode.value = static_cast<uint8_t>(data_int);
        std::cout << "Game Genie code decoded successfully\n";
        return true;
    }

    std::cerr << "Error: Decoded address is below 0x8000, invalid for PRG ROM patch\n";
    return false;
}



bool GameGenie::decode8LetterCode(const std::string& code, GameGenieCode& ggCode) const {
    if (code.length() != 8) {
        std::cerr << "Error: Code length is not 8\n";
        return false;
    }

    ggCode.hasCompare = true;

    // Official Game Genie character table (same as 6-letter)
    static const std::string table = "APZLGITYEOXUKSVN";

    int n[8];
    std::cout << "Decoding 8-letter Game Genie code: " << code << "\n";

    // Translate each character to its nibble value (index in table)
    for (int i = 0; i < 8; ++i) {
        char c = std::toupper(code[i]);
        size_t pos = table.find(c);
        if (pos == std::string::npos) {
            std::cerr << "Error: Invalid character '" << c << "' at position " << i << "\n";
            return false;
        }
        n[i] = static_cast<int>(pos);  // This is the actual nibble value
        std::cout << "  Char '" << c << "' → nibble value " << std::hex << n[i] << "\n";
    }

    // Decode address (8-letter format)
    int address_int = 0x8000 +
        (((n[3] & 7) << 12) |
         ((n[5] & 7) << 8)  |
         ((n[4] & 8) << 8)  |
         ((n[2] & 7) << 4)  |
         ((n[1] & 8) << 4)  |
         (n[4] & 7)         |
         (n[3] & 8));

    std::cout << "Decoded address = 0x" << std::hex << address_int << " (" << std::dec << address_int << ")\n";

    // Decode value (8-letter format)
    int data_int =
        ((n[1] & 7) << 4) |
        ((n[0] & 8) << 4) |
        (n[0] & 7)       |
        (n[5] & 8);

    std::cout << "Decoded value = 0x" << std::hex << data_int << "\n";

    // Decode compare value (8-letter format)
    int compare_int =
        ((n[7] & 7) << 4) |
        ((n[6] & 8) << 4) |
        (n[6] & 7)       |
        (n[5] & 8);

    std::cout << "Decoded compare = 0x" << std::hex << compare_int << "\n";

    // Validate and assign
    if (address_int >= 0x8000) {
        ggCode.address = static_cast<uint16_t>(address_int);
        ggCode.value = static_cast<uint8_t>(data_int);
        ggCode.compareValue = static_cast<uint8_t>(compare_int);
        std::cout << "8-letter Game Genie code decoded successfully\n";
        return true;
    }

    std::cerr << "Error: Decoded address is below 0x8000, invalid for PRG ROM patch\n";
    return false;
}

bool GameGenie::applyCode(const GameGenieCode& code) {
    if (!nesEmulator || !nesEmulator->isROMLoaded()) {
        std::cerr << "Error: No ROM loaded, cannot apply Game Genie code" << std::endl;
        return false;
    }

    // Convert CPU address to ROM offset
    uint32_t romOffset = 0;
    if (!cpuAddressToROMOffset(code.address, romOffset)) {
        std::cerr << "Error: Cannot map address $" << std::hex << code.address 
                  << " to ROM offset" << std::dec << std::endl;
        return false;
    }

    // Get direct access to PRG ROM (we'll need to add this method to WarpNES)
    uint8_t* prgROM = getPRGROMPointer();
    if (!prgROM) {
        std::cerr << "Error: Cannot access PRG ROM data" << std::endl;
        return false;
    }

    // Store original value before patching
    GameGenieCode& mutableCode = const_cast<GameGenieCode&>(code);
    mutableCode.originalValue = prgROM[romOffset];
    mutableCode.romOffset = romOffset;

    // Apply the patch if compare value matches (or no compare value)
    if (!code.hasCompare || prgROM[romOffset] == code.compareValue) {
        prgROM[romOffset] = code.value;
        
        std::cout << "Applied Game Genie patch: $" << std::hex << std::uppercase
                  << code.address << " = $" << std::setw(2) << std::setfill('0')
                  << (int)code.value << " (was $" << (int)mutableCode.originalValue 
                  << ")" << std::dec << std::endl;
        return true;
    } else {
        std::cerr << "Game Genie compare failed: expected $" << std::hex 
                  << (int)code.compareValue << " but found $" 
                  << (int)prgROM[romOffset] << std::dec << std::endl;
        return false;
    }
}

void GameGenie::restoreCode(const GameGenieCode& code) {
    if (!nesEmulator || !nesEmulator->isROMLoaded()) {
        return;
    }

    uint8_t* prgROM = getPRGROMPointer();
    if (!prgROM) {
        return;
    }

    // Restore original value
    prgROM[code.romOffset] = code.originalValue;
    
    std::cout << "Restored original value: $" << std::hex << std::uppercase
              << code.address << " = $" << std::setw(2) << std::setfill('0')
              << (int)code.originalValue << std::dec << std::endl;
}

bool GameGenie::cpuAddressToROMOffset(uint16_t cpuAddress, uint32_t& romOffset) {
    if (cpuAddress < 0x8000) {
        return false; // Not in ROM area
    }

    uint8_t mapper = nesEmulator->getMapper();
    uint32_t prgSize = nesEmulator->getPRGSize();
    
    switch (mapper) {
    case 0: // NROM
        romOffset = cpuAddress - 0x8000;
        // Handle 16KB ROM mirroring
        if (prgSize == 0x4000) { // 16KB
            romOffset &= 0x3FFF;
        }
        return romOffset < prgSize;
        
    case 1: // MMC1
        // For Game Genie patching, we patch the ROM data directly
        // The banking system will handle runtime mapping
        romOffset = cpuAddress - 0x8000;
        // Ensure we're within ROM bounds
        if (prgSize == 0x4000) { // 16KB ROM
            romOffset &= 0x3FFF;
        } else if (prgSize == 0x8000) { // 32KB ROM
            romOffset &= 0x7FFF;
        }
        return romOffset < prgSize;
        
    case 2: // UxROM  
        // UxROM: $8000-$BFFF switchable, $C000-$FFFF fixed to last bank
        if (cpuAddress < 0xC000) {
            // Switchable area - patch at offset 0
            romOffset = cpuAddress - 0x8000;
        } else {
            // Fixed area - patch at the last 16KB of ROM
            romOffset = (prgSize - 0x4000) + (cpuAddress - 0xC000);
        }
        return romOffset < prgSize;
        
    case 3: // CNROM
        // CNROM has no PRG banking, just direct mapping
        romOffset = cpuAddress - 0x8000;
        return romOffset < prgSize;
        
    case 4: // MMC3
        // MMC3: Complex 8KB banking, but for Game Genie we patch ROM directly
        romOffset = cpuAddress - 0x8000;
        return romOffset < prgSize;
        
    case 66: // GxROM
        // GxROM: 32KB PRG banking, but patch ROM directly
        romOffset = cpuAddress - 0x8000;
        return romOffset < prgSize;
        
    case 40: // Mapper 40 (SMB2j hack)
        // Mapper 40: $8000-$9FFF switchable, rest fixed
        if (cpuAddress < 0xA000) {
            // Switchable 8KB bank - patch at ROM start
            romOffset = cpuAddress - 0x8000;
        } else {
            // Fixed banks - calculate offset from end of ROM
            uint8_t totalBanks = prgSize / 0x2000; // 8KB banks
            if (cpuAddress < 0xC000) {
                // $A000-$BFFF: second-to-last bank
                romOffset = ((totalBanks - 2) * 0x2000) + (cpuAddress - 0xA000);
            } else {
                // $C000-$FFFF: last bank
                romOffset = ((totalBanks - 1) * 0x2000) + (cpuAddress - 0xC000);
            }
        }
        return romOffset < prgSize;
        
    default:
        // For unknown mappers, try simple direct mapping
        romOffset = cpuAddress - 0x8000;
        // Apply mirroring for small ROMs
        if (prgSize == 0x4000) {
            romOffset &= 0x3FFF;
        }
        return romOffset < prgSize;
    }
}

uint8_t* GameGenie::getPRGROMPointer() {
    if (!nesEmulator) {
        return nullptr;
    }
    return nesEmulator->getPRGROM();
}

void GameGenie::reapplyAllCodes() {
    std::cout << "Reapplying all Game Genie codes..." << std::endl;
    for (const auto& code : codes) {
        if (code.enabled) {
            applyCode(code);
        }
    }
}

bool GameGenie::loadCodesFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot open Game Genie codes file: " << filename << std::endl;
        return false;
    }

    std::string line;
    int lineNumber = 0;
    int codesLoaded = 0;
    
    while (std::getline(file, line)) {
        lineNumber++;
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // Parse line format: "CODE:Description"
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) {
            // Try without description
            if (addCode(line, "Loaded from file")) {
                codesLoaded++;
            }
        } else {
            std::string code = line.substr(0, colonPos);
            std::string description = line.substr(colonPos + 1);
            
            // Trim whitespace
            code.erase(0, code.find_first_not_of(" \t"));
            code.erase(code.find_last_not_of(" \t") + 1);
            description.erase(0, description.find_first_not_of(" \t"));
            description.erase(description.find_last_not_of(" \t") + 1);
            
            if (addCode(code, description)) {
                codesLoaded++;
            }
        }
    }
    
    file.close();
    
    std::cout << "Loaded " << codesLoaded << " Game Genie codes from " << filename << std::endl;
    return codesLoaded > 0;
}

bool GameGenie::saveCodesToFile(const std::string& filename) const {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Cannot create Game Genie codes file: " << filename << std::endl;
        return false;
    }
    
    file << "# Game Genie Codes\n";
    file << "# Format: CODE:Description\n";
    file << "#\n";
    
    for (const auto& code : codes) {
        file << code.originalCode << ":" << code.description << "\n";
    }
    
    file.close();
    
    std::cout << "Saved " << codes.size() << " Game Genie codes to " << filename << std::endl;
    return true;
}

GameGenie::CodeInfo GameGenie::getCodeInfo(size_t index) const {
    CodeInfo info;
    if (index < codes.size()) {
        const auto& code = codes[index];
        info.code = code.originalCode;
        info.description = code.description;
        info.enabled = code.enabled;
        info.address = code.address;
        info.value = code.value;
        info.hasCompare = code.hasCompare;
        info.compareValue = code.compareValue;
    }
    return info;
}

bool GameGenie::toggleCode(size_t index) {
    if (index >= codes.size()) return false;
    
    codes[index].enabled = !codes[index].enabled;
    
    if (codes[index].enabled) {
        applyCode(codes[index]);
    } else {
        restoreCode(codes[index]);
    }
    
    return true;
}

bool GameGenie::removeCodeByIndex(size_t index) {
    if (index >= codes.size()) return false;
    
    if (codes[index].enabled) {
        restoreCode(codes[index]);
    }
    
    codes.erase(codes.begin() + index);
    return true;
}

bool GameGenie::isCodeEnabled(size_t index) const {
    if (index >= codes.size()) return false;
    return codes[index].enabled;
}
