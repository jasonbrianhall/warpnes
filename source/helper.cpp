#include <regex>

std::string sanitizeFilename(const std::string& filename) {
    std::string sanitized;
    
    // Use regex to match only safe ASCII characters for filenames
    // Allow: letters (a-z, A-Z), numbers (0-9), hyphens (-), underscores (_), and periods (.)
    // This will automatically filter out emojis, special symbols, and other problematic characters
    std::regex validChars("[a-zA-Z0-9._-]");
    
    for (char c : filename) {
        std::string charStr(1, c);
        if (std::regex_match(charStr, validChars)) {
            sanitized += c;
        } else {
            // Replace any invalid character with underscore
            sanitized += '_';
        }
    }
    
    // Remove any leading/trailing periods or spaces that might have been converted to underscores
    while (!sanitized.empty() && (sanitized.front() == '_' || sanitized.front() == '.')) {
        sanitized.erase(0, 1);
    }
    while (!sanitized.empty() && (sanitized.back() == '_' || sanitized.back() == '.')) {
        sanitized.pop_back();
    }
    
    // Ensure we have at least something if the entire filename was invalid
    if (sanitized.empty()) {
        sanitized = "game";
    }
    
    // Limit length for DOS compatibility (8 characters max for base name)
    #ifdef __DJGPP__
    if (sanitized.length() > 8) {
        sanitized = sanitized.substr(0, 8);
    }
    #endif
    
    return sanitized;
}
