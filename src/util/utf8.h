// UMA Serve - UTF-8 validation helper
#pragma once

#include <string_view>

namespace uma::util {

// Validates UTF-8 byte sequences, rejecting overlong encodings and invalid ranges.
inline bool is_valid_utf8(std::string_view s) {
    unsigned int remaining = 0;
    unsigned char lead = 0;
    unsigned int pos = 0;
    for (unsigned char c : s) {
        if (remaining == 0) {
            if (c <= 0x7F) {
                continue;
            } else if (c >= 0xC2 && c <= 0xDF) {
                remaining = 1;
                lead = c;
                pos = 0;
            } else if (c >= 0xE0 && c <= 0xEF) {
                remaining = 2;
                lead = c;
                pos = 0;
            } else if (c >= 0xF0 && c <= 0xF4) {
                remaining = 3;
                lead = c;
                pos = 0;
            } else {
                return false; // overlong leads C0/C1 or > F4
            }
        } else {
            if ((c & 0xC0) != 0x80)
                return false;
            ++pos;
            if (pos == 1) {
                // first continuation has extra constraints for some leads
                if (lead == 0xE0 && c < 0xA0)
                    return false; // overlong 3-byte
                if (lead == 0xED && c > 0x9F)
                    return false; // UTF-16 surrogate
                if (lead == 0xF0 && c < 0x90)
                    return false; // overlong 4-byte
                if (lead == 0xF4 && c > 0x8F)
                    return false; // > U+10FFFF
            }
            if (--remaining == 0) {
                lead = 0;
                pos = 0;
            }
        }
    }
    return remaining == 0;
}

} // namespace uma::util

