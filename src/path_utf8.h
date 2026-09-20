#pragma once
// ---------------------------------------------------------------------------
// Encoding-safe conversions between std::filesystem::path and the UTF-8
// std::strings the rest of this codebase speaks.
//
// THE BUG THIS EXISTS TO KILL
//
// On Windows a path's native representation is UTF-16 (wchar_t). MSVC's
// fs::path::string() converts that to the process's *ANSI code page* -- and
// when a character has no representation in that code page (an umlaut on a
// non-Latin-1 ACP, a Japanese title, an em dash, a curly apostrophe, ...) the
// MS STL raises ERROR_NO_UNICODE_TRANSLATION and path::string() THROWS
// std::system_error:
//
//     "No mapping for the Unicode character exists in the target multi-byte
//      code page"
//
// Every .string() / .stem().string() / .extension().string() on a path that
// came off disk is therefore a landmine that goes off on the first non-ASCII
// filename in the library. Where that happens on a worker thread with no
// handler, the exception escapes the thread function and std::terminate()
// kills the whole process instantly, with no message -- the exact symptom of
// "point mousiki at my music folder and it dies a few seconds later".
//
// The same trap exists in the other direction: fs::path(std::string) also
// converts *from* the ANSI code page, so handing it a UTF-8 string (a path
// read out of config.txt, a snapshot, or the folder filter) silently
// produces a mangled path that then "doesn't exist".
//
// path_utf8() / path_from_utf8() route through UTF-16 <-> UTF-8 on Windows,
// which cannot fail for any valid path, and are plain passthroughs
// everywhere else (where the native encoding is already UTF-8). Every
// command line built here is handed to CreateProcessW after being widened
// from UTF-8 (see process_util.cpp), so UTF-8 is also the encoding the
// subprocess layer expects.
//
// RULE: never call .string() on a path. Use path_utf8(). Never build a path
// from a narrow string with fs::path(s). Use path_from_utf8().
// ---------------------------------------------------------------------------

#include <filesystem>
#include <string>

#if defined(_WIN32)
#include "win_compat.h"
#endif

namespace muisc {

namespace fs = std::filesystem;

// Native path -> UTF-8. Never throws for a valid path.
inline std::string path_utf8(const fs::path& p) {
#if defined(_WIN32)
    return win_path_to_utf8(p.native());
#else
    return p.string();
#endif
}

// UTF-8 -> native path. The inverse of path_utf8(); round-trips exactly.
inline fs::path path_from_utf8(const std::string& s) {
#if defined(_WIN32)
    return fs::path(win_utf8_to_wide(s));
#else
    return fs::path(s);
#endif
}

} // namespace muisc
