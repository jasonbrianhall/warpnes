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
    // 6-letter codes: no compare value
    ggCode.hasCompare = false;
    
    uint8_t decoded[6];
    for (int i = 0; i < 6; i++) {
        auto it = decodeTable.find(code[i]);
        if (it == decodeTable.end()) return false;
        decoded[i] = it->second;
    }

    // Standard Game Genie 6-letter decoding algorithm
    // Based on the official Game Genie documentation
    
    uint16_t address = 0;
    uint8_t value = 0;

    // Address bits extraction (corrected algorithm)
    address |= ((decoded[3] & 0x7) << 12);   // Address bits 14-12
    address |= ((decoded[3] & 0x8) << 8);    // Address bit 15 
    address |= ((decoded[4] & 0x7) << 8);    // Address bits 11-9
    address |= ((decoded[5] & 0x7) << 5);    // Address bits 8-6
    address |= ((decoded[1] & 0x8) << 4);    // Address bit 7
    address |= ((decoded[2] & 0x7) << 1);    // Address bits 5-3
    address |= ((decoded[4] & 0x8) >> 3);    // Address bit 2
    address |= ((decoded[5] & 0x8) >> 3);    // Address bit 1
    address |= ((decoded[1] & 0x7) >> 2);    // Address bit 0

    // Value bits extraction  
    value |= ((decoded[0] & 0x7) << 4);      // Value bits 7-5
    value |= ((decoded[0] & 0x8) >> 1);      // Value bit 4
    value |= ((decoded[1] & 0x7));           // Value bits 3-1
    value |= ((decoded[2] & 0x8) >> 7);      // Value bit 0

    // Validate address is in PRG ROM range
    if (address >= 0x8000) {
        ggCode.address = address;
        ggCode.value = value;
        return true;
    }

    return false;
}


bool GameGenie::decode8LetterCode(const std::string& code, GameGenieCode& ggCode) const {
    // 8-letter codes: include compare value
    ggCode.hasCompare = true;
    
    uint8_t decoded[8];
    for (int i = 0; i < 8; i++) {
        auto it = decodeTable.find(code[i]);
        if (it == decodeTable.end()) return false;
        decoded[i] = it->second;
    }

    // Standard Game Genie 8-letter decoding algorithm
    
    uint16_t address = 0;
    uint8_t value = 0;
    uint8_t compare = 0;

    // Address bits (8-letter format)
    address |= ((decoded[3] & 0x7) << 12);   // Address bits 14-12
    address |= ((decoded[3] & 0x8) << 8);    // Address bit 15
    address |= ((decoded[4] & 0x7) << 8);    // Address bits 11-9  
    address |= ((decoded[5] & 0x7) << 5);    // Address bits 8-6
    address |= ((decoded[1] & 0x8) << 4);    // Address bit 7
    address |= ((decoded[2] & 0x7) << 1);    // Address bits 5-3
    address |= ((decoded[4] & 0x8) >> 3);    // Address bit 2
    address |= ((decoded[5] & 0x8) >> 3);    // Address bit 1
    address |= ((decoded[1] & 0x7) >> 2);    // Address bit 0

    // Value bits (8-letter format)
    value |= ((decoded[0] & 0x7) << 4);      // Value bits 7-5
    value |= ((decoded[0] & 0x8) >> 1);      // Value bit 4
    value |= ((decoded[1] & 0x7));           // Value bits 3-1
    value |= ((decoded[2] & 0x8) >> 7);      // Value bit 0

    // Compare value bits
    compare |= ((decoded[6] & 0x7) << 4);    // Compare bits 7-5
    compare |= ((decoded[6] & 0x8) >> 1);    // Compare bit 4
    compare |= ((decoded[7] & 0x7));         // Compare bits 3-1
    compare |= ((decoded[2] & 0x8) >> 7);    // Compare bit 0

    // Validate address is in PRG ROM range
    if (address >= 0x8000) {
        ggCode.address = address;
        ggCode.value = value;
        ggCode.compareValue = compare;
        return true;
    }

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

    // This is a simplified mapping - you may need to enhance this based on
    // the specific mapper being used
    uint8_t mapper = nesEmulator->getMapper();
    
    switch (mapper) {
    case 0: // NROM
        romOffset = cpuAddress - 0x8000;
        // Handle 16KB ROM mirroring
        if (nesEmulator->getPRGSize() == 0x4000) { // 16KB
            romOffset &= 0x3FFF;
        }
        return romOffset < nesEmulator->getPRGSize();
        
    case 1: // MMC1
    case 2: // UxROM  
    case 3: // CNROM
    case 4: // MMC3
        // For banked mappers, we patch the ROM data directly
        // The banking will handle the runtime mapping
        romOffset = cpuAddress - 0x8000;
        return romOffset < nesEmulator->getPRGSize();
        
    default:
        // For unknown mappers, try direct mapping
        romOffset = cpuAddress - 0x8000;
        return romOffset < nesEmulator->getPRGSize();
    }
}

uint8_t* GameGenie::getPRGROMPointer() {
    // This requires adding a public method to WarpNES to access PRG ROM
    // For now, we'll need to add this to WarpNES.hpp:
    // uint8_t* getPRGROM() { return prgROM; }
    // uint32_t getPRGSize() const { return prgSize; }
    
    // Return nullptr for now - you'll need to implement the getter in WarpNES
    return nullptr;
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
