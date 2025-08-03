#include <cstdio>
#include <string>

std::string sanitizeFilename(const std::string& filename) {
    std::string sanitized = filename;
    
    // Replace problematic characters with safe alternatives
    for (char& c : sanitized) {
        switch (c) {
            case '!': c = '_'; break;  // Punch-Out!! becomes Punch-Out__
            case '?': c = '_'; break;
            case ':': c = '_'; break;
            case '*': c = '_'; break;
            case '<': c = '_'; break;
            case '>': c = '_'; break;
            case '|': c = '_'; break;
            case '"': c = '_'; break;
            case '/': c = '_'; break;
            case '\\': c = '_'; break;
            case ' ': c = '_'; break;  // Spaces can be problematic on some systems
            default: break;
        }
    }
    
    return sanitized;
}
