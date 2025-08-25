#ifndef GAME_GENIE_HPP
#define GAME_GENIE_HPP

#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <cstdint>
#include "WarpNES.hpp"

/**
 * Game Genie code manipulation system for NES ROMs
 * Supports both 6-letter and 8-letter Game Genie codes
 * 
 * Game Genie codes allow runtime patching of ROM data to modify game behavior.
 * 6-letter codes: Simple address/value patches
 * 8-letter codes: Include compare value for conditional patching
 */
class GameGenie {
public:
    /**
     * Constructor
     * @param nes Pointer to the WarpNES emulator instance
     */
    explicit GameGenie(WarpNES* nes);
    
    /**
     * Destructor - restores all original ROM values
     */
    ~GameGenie();

    /**
     * Add a Game Genie code
     * @param code The Game Genie code (6 or 8 letters, case insensitive)
     * @param description Human-readable description of what the code does
     * @return true if code was successfully added and applied
     */
    bool addCode(const std::string& code, const std::string& description = "");

    /**
     * Remove a Game Genie code and restore original value
     * @param code The Game Genie code to remove
     * @return true if code was found and removed
     */
    bool removeCode(const std::string& code);

    /**
     * Enable or disable a Game Genie code without removing it
     * @param code The Game Genie code to enable/disable
     * @param enable true to enable, false to disable
     */
    void enableCode(const std::string& code, bool enable);

    /**
     * Remove all Game Genie codes and restore original ROM
     */
    void clearAllCodes();

    /**
     * List all loaded Game Genie codes with their status
     */
    void listCodes() const;

    /**
     * Load Game Genie codes from a text file
     * File format: one code per line, optionally with description after ':'
     * Lines starting with # or ; are treated as comments
     * @param filename Path to the codes file
     * @return true if at least one code was loaded successfully
     */
    bool loadCodesFromFile(const std::string& filename);

    /**
     * Save current Game Genie codes to a text file
     * @param filename Path to save the codes file
     * @return true if file was saved successfully
     */
    bool saveCodesToFile(const std::string& filename) const;

    /**
     * Reapply all enabled codes (useful after ROM reload)
     */
    void reapplyAllCodes();

    /**
     * Get the number of loaded codes
     */
    size_t getCodeCount() const { return codes.size(); }

    /**
     * Get the number of enabled codes
     */
    size_t getEnabledCodeCount() const;

private:
    /**
     * Structure representing a decoded Game Genie code
     */
    struct GameGenieCode {
        std::string originalCode;    // Original code string (e.g. "SXIOPO")
        std::string description;     // Human-readable description
        uint16_t address;           // CPU address to patch
        uint8_t value;              // New value to write
        uint8_t compareValue;       // Compare value (8-letter codes only)
        bool hasCompare;            // Whether this code has a compare value
        bool enabled;               // Whether this code is currently active
        uint8_t originalValue;      // Original ROM value (for restoration)
        uint32_t romOffset;         // ROM file offset where patch is applied

        GameGenieCode() : address(0), value(0), compareValue(0), 
                         hasCompare(false), enabled(false), 
                         originalValue(0), romOffset(0) {}
    };

    WarpNES* nesEmulator;           // Pointer to NES emulator
    std::vector<GameGenieCode> codes; // List of loaded Game Genie codes

    // Game Genie character decode table
    std::map<char, uint8_t> decodeTable;

    /**
     * Validate if a string is a valid Game Genie code format
     * @param code The code to validate
     * @return true if format is valid
     */
    bool isValidCode(const std::string& code) const;

    /**
     * Clean up a code string by removing invalid characters and converting to uppercase
     * @param code The code to clean up
     * @return Cleaned code string
     */
    std::string cleanupCode(const std::string& code) const;

    /**
     * Decode a Game Genie code string into address/value/compare
     * @param code The Game Genie code string (cleaned)
     * @param ggCode Output structure to fill with decoded values
     * @return true if decoding was successful
     */
    bool decodeCode(const std::string& code, GameGenieCode& ggCode) const;

    /**
     * Decode a 6-letter Game Genie code (no compare value)
     * @param code 6-letter code string
     * @param ggCode Output structure
     * @return true if successful
     */
    bool decode6LetterCode(const std::string& code, GameGenieCode& ggCode) const;

    /**
     * Decode an 8-letter Game Genie code (with compare value)
     * @param code 8-letter code string
     * @param ggCode Output structure
     * @return true if successful
     */
    bool decode8LetterCode(const std::string& code, GameGenieCode& ggCode) const;

    /**
     * Apply a Game Genie code to ROM memory
     * @param code The code to apply
     * @return true if patch was applied successfully
     */
    bool applyCode(const GameGenieCode& code);

    /**
     * Restore original ROM value for a Game Genie code
     * @param code The code to restore
     */
    void restoreCode(const GameGenieCode& code);

    /**
     * Convert CPU address to ROM file offset
     * @param cpuAddress CPU address (e.g. $8000-$FFFF)
     * @param romOffset Output ROM file offset
     * @return true if conversion was successful
     */
    bool cpuAddressToROMOffset(uint16_t cpuAddress, uint32_t& romOffset);

    /**
     * Get direct pointer to PRG ROM data
     * @return Pointer to PRG ROM, or nullptr if not available
     */
    uint8_t* getPRGROMPointer();
};

// Inline implementation for simple methods
inline size_t GameGenie::getEnabledCodeCount() const {
    size_t count = 0;
    for (const auto& code : codes) {
        if (code.enabled) count++;
    }
    return count;
}

#endif // GAME_GENIE_HPP
