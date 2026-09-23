#pragma once
#include <string>
#include <cstdint>
#include <algorithm>

namespace muisc {

// ---------------------------------------------------------------------------
// ASCII-only case folding.
//
// std::tolower/std::toupper are locale-dependent and defined over single
// bytes. Run over a UTF-8 string they are actively destructive: main() calls
// setlocale(LC_ALL, ""), so on a machine whose locale is a single-byte code
// page, a byte like 0xE4 -- which in UTF-8 is only ever the *lead* byte of a
// three-byte sequence (any CJK character, for instance) -- is classified as a
// letter and case-mapped to a different byte, corrupting the sequence. The
// result is a string that is no longer valid UTF-8, which then renders as
// partial text or replacement characters.
//
// Case-insensitive matching in this codebase only ever needs to fold ASCII
// (hotkey names, file extensions, latin search terms). These leave every byte
// >= 0x80 exactly as it was, so a UTF-8 string round-trips untouched.
inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline char ascii_upper(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

inline std::string ascii_lower_str(std::string s) {
    for (char& c : s) c = ascii_lower(c);
    return s;
}

inline std::string ascii_upper_str(std::string s) {
    for (char& c : s) c = ascii_upper(c);
    return s;
}

inline bool ascii_alnum(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline int utf8_seq_len(unsigned char lead) {
    if ((lead & 0x80) == 0x00) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

inline uint32_t utf8_decode(const std::string& s, size_t& i) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    int len = utf8_seq_len(c);
    len = static_cast<int>(std::min<size_t>(static_cast<size_t>(len), s.size() - i));
    uint32_t cp;
    if (len <= 1) {
        cp = c;
    } else {
        cp = c & (0xFF >> (len + 1));
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
    }
    i += static_cast<size_t>(len);
    return cp;
}

} // namespace muisc
