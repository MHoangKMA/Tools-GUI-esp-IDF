#include "ascii_fold.h"

#include <cstdint>

namespace {

// Maps one code point to a printable ASCII character (Vietnamese accents are dropped).
char FoldCodepoint(uint32_t cp) {
    if (cp >= 32 && cp < 127) {
        return static_cast<char>(cp);
    }
    static const char kLatin1Upper[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTs";  // U+00C0..U+00DF
    static const char kLatin1Lower[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";  // U+00E0..U+00FF
    if (cp >= 0xC0 && cp <= 0xDF) {
        return kLatin1Upper[cp - 0xC0];
    }
    if (cp >= 0xE0 && cp <= 0xFF) {
        return kLatin1Lower[cp - 0xE0];
    }
    switch (cp) {
        case 0x0102: return 'A';
        case 0x0103: return 'a';
        case 0x0110: return 'D';
        case 0x0111: return 'd';
        case 0x0128: return 'I';
        case 0x0129: return 'i';
        case 0x0168: return 'U';
        case 0x0169: return 'u';
        case 0x01A0: return 'O';
        case 0x01A1: return 'o';
        case 0x01AF: return 'U';
        case 0x01B0: return 'u';
        default: break;
    }
    // Vietnamese letters with tone marks: U+1EA0..U+1EF9, even = upper case, odd = lower case
    struct Range {
        uint32_t first;
        uint32_t last;
        char upper;
    };
    static const Range ranges[] = {
        {0x1EA0, 0x1EB7, 'A'}, {0x1EB8, 0x1EC7, 'E'}, {0x1EC8, 0x1ECB, 'I'},
        {0x1ECC, 0x1EE3, 'O'}, {0x1EE4, 0x1EF1, 'U'}, {0x1EF2, 0x1EF9, 'Y'},
    };
    for (const auto& r : ranges) {
        if (cp >= r.first && cp <= r.last) {
            return ((cp - r.first) % 2 == 0) ? r.upper : static_cast<char>(r.upper + 32);
        }
    }
    return '?';
}

}  // namespace

std::string AsciiFold(const std::string& utf8) {
    std::string out;
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = utf8[i];
        uint32_t cp = c;
        size_t extra = 0;
        if (c >= 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else if (c >= 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if (c >= 0xC0) {
            cp = c & 0x1F;
            extra = 1;
        }
        if (extra > 0 && i + extra >= utf8.size()) {
            break;  // truncated sequence
        }
        for (size_t k = 1; k <= extra; k++) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
        }
        i += extra + 1;
        out += FoldCodepoint(cp);
    }
    return out;
}
