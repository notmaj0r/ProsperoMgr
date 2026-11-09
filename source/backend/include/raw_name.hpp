#pragma once
// File names are raw bytes on the console, but JSON and URLs carry UTF-8. A byte that is not valid
// UTF-8 travels as the private-use character U+F700 + byte, and is turned back before a path is used.

#include <cstdint>
#include <string>

namespace prospero {

// Length of the valid UTF-8 sequence starting at `text[pos]`, or 0 if the bytes there are not one.
inline size_t ValidUtf8Length(const std::string &text, size_t pos) {
    auto at = [&](size_t i) { return static_cast<unsigned char>(text[i]); };
    unsigned char lead = at(pos);
    size_t need = lead < 0x80 ? 1 : (lead >= 0xC2 && lead <= 0xDF) ? 2 : (lead >= 0xE0 && lead <= 0xEF) ? 3 : (lead >= 0xF0 && lead <= 0xF4) ? 4 : 0;
    if (need == 0 || pos + need > text.size()) return 0;
    for (size_t i = 1; i < need; i++) {
        if ((at(pos + i) & 0xC0) != 0x80) return 0;
    }
    if (need == 3 && ((lead == 0xE0 && at(pos + 1) < 0xA0) || (lead == 0xED && at(pos + 1) >= 0xA0))) return 0;
    if (need == 4 && ((lead == 0xF0 && at(pos + 1) < 0x90) || (lead == 0xF4 && at(pos + 1) >= 0x90))) return 0;
    return need;
}

// Raw name bytes -> valid UTF-8.
inline std::string NameToUtf8(const std::string &raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        size_t length = ValidUtf8Length(raw, i);
        if (length > 0) {
            out.append(raw, i, length);
            i += length;
            continue;
        }
        unsigned code = 0xF700u + static_cast<unsigned char>(raw[i]);
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
        i++;
    }
    return out;
}

// UTF-8 from the browser -> raw name bytes, undoing NameToUtf8.
inline std::string NameFromUtf8(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        auto at = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
        // U+F780..U+F7FF is EF 9E 80..BF and EF 9F 80..BF.
        if (at(i) == 0xEF && i + 2 < text.size() && (at(i + 1) == 0x9E || at(i + 1) == 0x9F) && (at(i + 2) & 0xC0) == 0x80) {
            unsigned code = ((at(i) & 0x0F) << 12) | ((at(i + 1) & 0x3F) << 6) | (at(i + 2) & 0x3F);
            out += static_cast<char>(code - 0xF700u);
            i += 2;
        } else {
            out += text[i];
        }
    }
    return out;
}

} // namespace prospero
