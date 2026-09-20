#include "cache_manager.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "path_utf8.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace muisc {

CacheManager::CacheManager() {
    const char* home = std::getenv("HOME");
    // HOME is stored as UTF-8 by win_bootstrap_env() (it comes out of
    // GetEnvironmentVariableW, which is UTF-16). fs::path(std::string)
    // would re-read those bytes as ANSI, so a user profile with a
    // non-ASCII name produced a garbled base directory.
    fs::path base = home ? path_from_utf8(home) : fs::path(".");
    cache_dir_ = base / ".cache" / "mousiki";
    std::error_code ec;
    fs::create_directories(cache_dir_, ec); // ignore failure, we surface it on first write instead
}

// Turns a track title into a filename.
//
// The previous version kept only [A-Za-z0-9] (lowercased) plus space/dash/
// underscore and dropped every other byte. For a UTF-8 title that is a
// disaster: a Japanese, Cyrillic or Greek title has no ASCII alphanumerics at
// all, so it collapsed to "untitled" -- and a mixed title like
// "\u591c\u306b\u99c6\u3051\u308b (YOASOBI)" kept only the latin fragment, which is exactly the
// "part of the title" showing up in the local list, since the list labels
// cached tracks by their filename stem. Worse, std::isalnum() is
// locale-dependent: under a single-byte locale it accepts bytes >= 0x80, so
// some UTF-8 lead bytes survived and were then case-mapped individually,
// producing broken sequences rather than clean removal.
//
// Non-ASCII characters are perfectly legal in NTFS filenames. The only
// characters Windows actually forbids are ASCII (< > : " / \\ | ? * and the
// control range), all of which this already excludes by construction. So the
// rule is now: fold ASCII letters to lowercase, keep ASCII digits, turn
// space/dash/underscore into a single underscore, keep every non-ASCII
// codepoint verbatim, and drop the rest.
std::string CacheManager::sanitize(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());

    size_t i = 0;
    while (i < raw.size()) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c < 0x80) {
            if (ascii_alnum(c)) out += ascii_lower(static_cast<char>(c));
            else if (c == ' ' || c == '-' || c == '_') out += '_';
            // everything else (slashes, quotes, colons, ...) is dropped
            ++i;
            continue;
        }
        // Multi-byte sequence: copy it whole, or skip a malformed lead byte.
        size_t len = static_cast<size_t>(utf8_seq_len(c));
        if (len <= 1 || i + len > raw.size()) { ++i; continue; }
        bool well_formed = true;
        for (size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(raw[i + k]) & 0xC0) != 0x80) { well_formed = false; break; }
        }
        if (!well_formed) { ++i; continue; }
        out.append(raw, i, len);
        i += len;
    }

    while (out.find("__") != std::string::npos) {
        out.replace(out.find("__"), 2, "_");
    }
    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    // A trailing dot or space is silently stripped by Windows itself, which
    // would make the name we write and the name we later look up disagree.
    while (!out.empty() && (out.back() == '_' || out.back() == '.' || out.back() == ' ')) out.pop_back();

    // Keep the stem well inside MAX_PATH once the cache directory and
    // extension are prepended. Cut on a codepoint boundary so the result is
    // still valid UTF-8.
    constexpr size_t kMaxStemBytes = 120;
    if (out.size() > kMaxStemBytes) {
        size_t cut = kMaxStemBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }

    if (out.empty()) out = "untitled";
    return out;
}

// The pre-UTF-8 rule, kept for one purpose only: finding tracks that were
// already downloaded under the old naming scheme, so changing the scheme
// doesn't silently orphan an existing cache and re-download everything.
std::string CacheManager::legacy_sanitize(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (ascii_alnum(c)) out += ascii_lower(static_cast<char>(c));
        else if (c == ' ' || c == '-' || c == '_') out += '_';
    }
    while (out.find("__") != std::string::npos) {
        out.replace(out.find("__"), 2, "_");
    }
    if (out.empty()) out = "untitled";
    return out;
}

fs::path CacheManager::path_for(const std::string& title, const std::string& ext) const {
    fs::path current = cache_dir_ / (sanitize(title) + "." + ext);
    std::error_code ec;
    if (fs::exists(current, ec)) return current;

    // Nothing under the current scheme -- fall back to a file left behind by
    // the old one, if there is one, so previously cached tracks keep playing.
    std::string legacy_stem = legacy_sanitize(title);
    if (legacy_stem != sanitize(title)) {
        fs::path legacy = cache_dir_ / (legacy_stem + "." + ext);
        if (fs::exists(legacy, ec)) return legacy;
    }
    return current;   // doesn't exist yet: this is the name a download writes
}

bool CacheManager::is_cached(const std::string& title, const std::string& ext) const {
    std::error_code ec;
    auto p = path_for(title, ext);
    return fs::exists(p, ec) && fs::file_size(p, ec) > 0;
}

} // namespace muisc
