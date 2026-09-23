#include "win_compat.h"
#include "terminal_ui.h" // kKeyHome

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Older SDKs (and some MinGW-w64 header sets) predate these; defining them
// only when absent keeps the build working without requiring a specific SDK.
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif
#ifndef ENABLE_VIRTUAL_TERMINAL_INPUT
#define ENABLE_VIRTUAL_TERMINAL_INPUT 0x0200
#endif

#include <io.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace muisc {

namespace {

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16
// ---------------------------------------------------------------------------

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace

std::string win_path_to_utf8(const std::wstring& native) {
    return narrow(native);
}

std::wstring win_utf8_to_wide(const std::string& utf8) {
    return widen(utf8);
}

namespace {

// ---------------------------------------------------------------------------
// Saved console state
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// UTF-8-correct stdout
//
// The whole render loop funnels through exactly one call site --
// `std::cout << render_frame(term)` in app.cpp -- so this is the one place
// that needs to be airtight, and swapping std::cout's streambuf here makes
// it airtight for every call site without touching app.cpp or terminal_ui.cpp
// at all.
//
// Why not just SetConsoleOutputCP(CP_UTF8) and leave std::cout alone (which
// is what the first version of this file did): that call only changes how
// WriteConsoleA *interprets* the bytes the CRT hands it, and that path is
// inconsistent across console hosts, fonts, and CRT buffering behavior --
// it's the textbook cause of exactly the mojibake this class exists to
// eliminate (UTF-8 bytes like E2 94 80 read back one byte at a time as
// CP1252: "\xe2\x94\x80" -> "â\x94\x80" -> "â”€"). WriteConsoleW has no such
// ambiguity: it takes UTF-16 code units and the console displays them
// directly, so converting ourselves removes the codepage guess entirely.
class Utf8ConsoleStreambuf : public std::streambuf {
public:
    explicit Utf8ConsoleStreambuf(HANDLE h) : handle_(h) {}

protected:
    std::streamsize xsputn(const char* s, std::streamsize count) override {
        pending_.append(s, static_cast<size_t>(count));
        flush_complete();
        return count;
    }

    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            pending_.push_back(static_cast<char>(ch));
            flush_complete();
        }
        return ch;
    }

    int sync() override {
        // A non-empty leftover here means the stream was handed a truncated
        // UTF-8 sequence, which shouldn't happen given every string in the
        // codebase is well-formed -- but writing it out (rather than
        // silently dropping it) is the safer failure mode.
        if (!pending_.empty()) {
            write_utf16(pending_);
            pending_.clear();
        }
        return 0;
    }

private:
    HANDLE handle_;
    std::string pending_;

    // How many bytes at the *end* of `s` are the start of a UTF-8 sequence
    // that isn't finished yet -- i.e. how much to hold back. Needed because
    // operator<<'s underlying sputn() call boundaries don't line up with
    // codepoint boundaries: a wide character sitting right at the edge of an
    // internal buffer chunk can have its lead byte and continuation bytes
    // arrive in two separate xsputn() calls, and converting each half
    // separately would garble it.
    static size_t incomplete_tail(const std::string& s) {
        size_t n = s.size();
        size_t limit = n < 4 ? n : 4;
        for (size_t back = 1; back <= limit; ++back) {
            unsigned char b = static_cast<unsigned char>(s[n - back]);
            if ((b & 0xC0) == 0x80) continue;   // continuation byte, keep scanning back
            int seq_len = (b < 0x80) ? 1
                        : ((b & 0xE0) == 0xC0) ? 2
                        : ((b & 0xF0) == 0xE0) ? 3
                        : ((b & 0xF8) == 0xF0) ? 4
                        : 1;   // invalid lead byte -- treat as complete, let MultiByteToWideChar cope
            return (static_cast<size_t>(seq_len) > back) ? back : 0;
        }
        return 0;   // 4+ continuation bytes with no lead -- already-corrupt input, flush as-is
    }

    void flush_complete() {
        size_t tail = incomplete_tail(pending_);
        size_t complete_len = pending_.size() - tail;
        if (complete_len == 0) return;
        write_utf16(pending_.substr(0, complete_len));
        pending_.erase(0, complete_len);
    }

    void write_utf16(const std::string& utf8) {
        if (utf8.empty()) return;
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0) return;
        std::wstring wide(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &wide[0], wlen);
        DWORD written = 0;
        WriteConsoleW(handle_, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
    }
};

struct ConsoleState {
    HANDLE out = INVALID_HANDLE_VALUE;
    HANDLE in  = INVALID_HANDLE_VALUE;
    DWORD  orig_out_mode = 0;
    DWORD  orig_in_mode  = 0;
    UINT   orig_out_cp   = 0;
    UINT   orig_in_cp    = 0;
    bool   have_out_mode = false;
    bool   have_in_mode  = false;
    bool   vt_enabled    = false;
};

ConsoleState g_con;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_restored{false};
Utf8ConsoleStreambuf* g_cout_buf = nullptr;
std::streambuf* g_cout_orig_buf = nullptr;

// Emitted directly through WriteConsoleW rather than std::cout, because the
// Ctrl+C handler runs on its own thread while the render loop may be halfway
// through a cout << chain -- touching the same stream from there is a data
// race, and at exit time it may already have been torn down.
void write_raw(const wchar_t* s) {
    if (g_con.out == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteConsoleW(g_con.out, s, static_cast<DWORD>(wcslen(s)), &written, nullptr);
}

BOOL WINAPI ctrl_handler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            win_console_restore();
            break;
        default:
            break;
    }
    // FALSE == "not handled", so the default handler still terminates us.
    // We only wanted the chance to put the terminal back first.
    return FALSE;
}

} // namespace

// ---------------------------------------------------------------------------
// Console
// ---------------------------------------------------------------------------

bool win_console_init() {
    if (g_initialized.exchange(true)) return g_con.vt_enabled;

    g_con.out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_con.in  = GetStdHandle(STD_INPUT_HANDLE);

    g_con.orig_out_cp = GetConsoleOutputCP();
    g_con.orig_in_cp  = GetConsoleCP();
    // Kept for anything else that writes through WriteConsoleA/printf-style
    // paths (child processes inherit nothing here, since spawn_capture()
    // detaches their handles entirely -- see process_util.cpp -- but this
    // stays as a harmless default for the process as a whole). Correctness
    // for the app's own rendering does NOT depend on this: see
    // Utf8ConsoleStreambuf above for why relying on it was the actual bug.
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    // Every frame the app draws goes through std::cout, so redirecting it to
    // WriteConsoleW here is what actually guarantees box-drawing characters,
    // the spectrum blocks, the spinning-disk glyphs, and any non-Latin track
    // title or lyric line render correctly -- regardless of the console
    // host's codepage handling.
    g_cout_orig_buf = std::cout.rdbuf();
    g_cout_buf = new Utf8ConsoleStreambuf(g_con.out);
    std::cout.rdbuf(g_cout_buf);

    if (GetConsoleMode(g_con.out, &g_con.orig_out_mode)) {
        g_con.have_out_mode = true;
        DWORD mode = g_con.orig_out_mode | ENABLE_PROCESSED_OUTPUT |
                     ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        // Note: DISABLE_NEWLINE_AUTO_RETURN is deliberately NOT set. On
        // POSIX the app runs with ONLCR still on (the termios code clears
        // only ECHO and ICANON), so a bare "\n" carries an implicit carriage
        // return. Leaving the Windows default alone reproduces that exactly.
        if (SetConsoleMode(g_con.out, mode)) {
            g_con.vt_enabled = true;
        }
    }

    if (GetConsoleMode(g_con.in, &g_con.orig_in_mode)) {
        g_con.have_in_mode = true;
    }

    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    return g_con.vt_enabled;
}

void win_console_restore() {
    if (!g_initialized.load()) return;
    if (g_restored.exchange(true)) return;

    // Leave the alternate screen and unhide the cursor while VT processing
    // is still on -- after the SetConsoleMode below these would be printed
    // literally.
    if (g_con.vt_enabled) write_raw(L"\x1b[?25h\x1b[?1049l");

    if (g_con.have_out_mode) SetConsoleMode(g_con.out, g_con.orig_out_mode);
    if (g_con.have_in_mode)  SetConsoleMode(g_con.in,  g_con.orig_in_mode);
    if (g_con.orig_out_cp)   SetConsoleOutputCP(g_con.orig_out_cp);
    if (g_con.orig_in_cp)    SetConsoleCP(g_con.orig_in_cp);

    // Put std::cout back the way we found it. Note: if this runs from
    // ctrl_handler() (Ctrl+C), it races against a main-thread write that may
    // be mid-flight through g_cout_buf -- swapping rdbuf here is not
    // synchronized against that. This is the same accepted, small race
    // window every console-app Ctrl+C handler in this file already lives
    // with (see ctrl_handler()'s comment); the process is torn down by the
    // OS's default handler immediately after we return, so the exposure is
    // brief and leaving UTF-8 output silently reverted to garbage for the
    // rest of a *normal* exit would be worse.
    if (g_cout_orig_buf) {
        std::cout.rdbuf(g_cout_orig_buf);
        g_cout_orig_buf = nullptr;
    }
    delete g_cout_buf;
    g_cout_buf = nullptr;
}

void win_raw_mode_enter() {
    if (!g_con.have_in_mode) return;
    // win_poll_key() reads raw key events via ReadConsoleInputW, which
    // never echoes on its own -- but the console host's line-editing
    // machinery is a separate layer that still echoes and buffers
    // whatever's typed until Enter unless these two modes are off, so
    // that's the only thing that actually has to change here.
    DWORD mode = g_con.orig_in_mode;
    mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
    mode |=  ENABLE_PROCESSED_INPUT;   // keep Ctrl+C working, as ISIG does on POSIX
    SetConsoleMode(g_con.in, mode);
}

void win_raw_mode_exit() {
    if (!g_con.have_in_mode) return;
    SetConsoleMode(g_con.in, g_con.orig_in_mode);
}

namespace {
// Bytes still queued from a previously decoded character: a single
// keypress can produce up to 4 UTF-8 bytes, but win_poll_key()'s contract
// (documented in win_compat.h) is one int per call, so anything past the
// first byte waits here for the next call.
std::string g_pending_key_bytes;
// A high surrogate holds here until the low surrogate that completes it
// arrives in a later event -- needed for any character outside the BMP
// (rare from a keyboard, but IME composition and emoji input can do it).
wchar_t g_pending_high_surrogate = 0;
} // namespace

int win_poll_key() {
    if (!g_pending_key_bytes.empty()) {
        unsigned char b = static_cast<unsigned char>(g_pending_key_bytes.front());
        g_pending_key_bytes.erase(g_pending_key_bytes.begin());
        return b;
    }

    DWORD events = 0;
    if (!GetNumberOfConsoleInputEvents(g_con.in, &events) || events == 0) return 0;

    INPUT_RECORD rec;
    DWORD got = 0;
    if (!ReadConsoleInputW(g_con.in, &rec, 1, &got) || got == 0) return 0;

    if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown) {
        // Key-up, resize, focus-change, mouse (mouse input isn't even
        // enabled) -- every one of these is interleaved with real
        // keystrokes constantly (each keypress has its own key-up event
        // right behind it), so returning 0 here would mean "nothing was
        // pressed" once per actual keystroke. Recurse instead so the
        // caller only ever sees 0 when the input queue is genuinely
        // empty. GetNumberOfConsoleInputEvents above guarantees this
        // terminates rather than spinning: it never recurses past
        // however many events are actually queued.
        return win_poll_key();
    }

    const KEY_EVENT_RECORD& k = rec.Event.KeyEvent;

    // Arrow keys, by virtual-key code rather than guessing a scan code off
    // a raw byte stream -- collapses to the same four letters the POSIX
    // ESC-[-X decoder and the old _getch()-based path both used, so
    // nothing downstream of this function (app.cpp's key handling) has to
    // know or care which platform it's running on.
    switch (k.wVirtualKeyCode) {
        case VK_UP:    return 'A';
        case VK_DOWN:  return 'B';
        case VK_RIGHT: return 'C';
        case VK_LEFT:  return 'D';
        case VK_HOME:  return kKeyHome;
        case VK_DELETE: return kKeyDelete;
        default: break;
    }

    wchar_t wc = k.uChar.UnicodeChar;
    if (wc == 0) return 0; // a bare modifier, function key, Home/End/PgUp/... -- ignored, as on POSIX
    if (wc == 8) return 127; // Backspace -- same normalization the old _getch() path applied
    if (wc < 128) return static_cast<int>(wc); // plain ASCII: Enter (13), Tab (9), Esc (27), space, digits, letters, ...

    // A real non-ASCII character: a German a/o/u-umlaut typed directly off
    // the keyboard, any other accented or non-Latin letter, or (via a
    // surrogate pair spanning two events) something outside the BMP. The
    // OLD path here was _getch(), which reads a single BYTE at a time
    // under whatever the console's legacy input code page happens to be
    // -- not reliably UTF-8 even with SetConsoleCP(CP_UTF8) set, which is
    // exactly why typing an umlaut either vanished (app.cpp's text-entry
    // fields only ever accepted byte values 32-126) or came through as a
    // mangled single byte. ReadConsoleInputW hands over the actual UTF-16
    // code unit the keyboard layout produced, with no code-page ambiguity
    // at all, so this is encoded straight to UTF-8 and queued -- from
    // there it's handled exactly like typing the same character over a
    // UTF-8 POSIX terminal already was.
    if (wc >= 0xD800 && wc <= 0xDBFF) { // high surrogate: not a complete character on its own
        g_pending_high_surrogate = wc;
        return win_poll_key();
    }

    std::wstring utf16;
    if (wc >= 0xDC00 && wc <= 0xDFFF && g_pending_high_surrogate != 0) {
        utf16.push_back(g_pending_high_surrogate);
        utf16.push_back(wc);
        g_pending_high_surrogate = 0;
    } else {
        utf16.push_back(wc);
    }

    std::string utf8 = narrow(utf16);
    if (utf8.empty()) return 0; // conversion failure -- shouldn't happen for a valid code unit
    g_pending_key_bytes.assign(utf8.begin() + 1, utf8.end());
    return static_cast<unsigned char>(utf8[0]);
}

int win_term_rows() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(g_con.out, &info)) {
        // srWindow, not dwSize: dwSize is the scrollback buffer, which is
        // typically 9000 lines tall and would have the UI try to draw a
        // frame far taller than the visible window.
        int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
        if (rows > 0) return rows;
    }
    return 40;
}

int win_term_cols() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(g_con.out, &info)) {
        int cols = info.srWindow.Right - info.srWindow.Left + 1;
        if (cols > 0) return cols;
    }
    return 155;
}

// ---------------------------------------------------------------------------
// Environment / paths
// ---------------------------------------------------------------------------

void win_bootstrap_env() {
    if (const char* existing = std::getenv("HOME")) {
        if (existing[0] != '\0') return;   // respect an explicitly set HOME
    }

    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, static_cast<DWORD>(std::size(buf)));
    std::string home;
    if (n > 0 && n < std::size(buf)) {
        home = narrow(std::wstring(buf, n));
    } else {
        // Fall back to HOMEDRIVE + HOMEPATH, which is what roaming-profile
        // domain machines set when USERPROFILE is absent.
        wchar_t drive[MAX_PATH], path[MAX_PATH];
        DWORD dn = GetEnvironmentVariableW(L"HOMEDRIVE", drive, MAX_PATH);
        DWORD pn = GetEnvironmentVariableW(L"HOMEPATH", path, MAX_PATH);
        if (dn > 0 && dn < MAX_PATH && pn > 0 && pn < MAX_PATH) {
            home = narrow(std::wstring(drive, dn) + std::wstring(path, pn));
        }
    }
    if (home.empty()) return;

    _putenv_s("HOME", home.c_str());
}

std::string win_executable_path() {
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return std::string();
        if (n < buf.size() - 1) return narrow(std::wstring(buf.data(), n));
        buf.resize(buf.size() * 2);   // path longer than MAX_PATH, retry bigger
    }
}

std::string win_find_executable(const std::string& name) {
    std::wstring wname = widen(name);
    if (wname.empty()) return std::string();

    // An explicit path (contains a separator or a drive colon) is not a PATH
    // lookup -- test it directly.
    bool has_sep = name.find('\\') != std::string::npos ||
                   name.find('/')  != std::string::npos ||
                   name.find(':')  != std::string::npos;

    std::vector<std::wstring> exts;
    if (name.find('.') == std::string::npos) {
        wchar_t pathext[4096];
        DWORD n = GetEnvironmentVariableW(L"PATHEXT", pathext, 4096);
        std::wstring raw = (n > 0 && n < 4096) ? std::wstring(pathext, n)
                                               : L".COM;.EXE;.BAT;.CMD";
        size_t start = 0;
        while (start <= raw.size()) {
            size_t semi = raw.find(L';', start);
            std::wstring e = raw.substr(start, semi == std::wstring::npos
                                                   ? std::wstring::npos
                                                   : semi - start);
            if (!e.empty()) exts.push_back(e);
            if (semi == std::wstring::npos) break;
            start = semi + 1;
        }
    }
    exts.insert(exts.begin(), L"");   // try the name verbatim first

    for (const auto& ext : exts) {
        std::wstring candidate = wname + ext;
        if (has_sep) {
            DWORD attrs = GetFileAttributesW(candidate.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
                return narrow(candidate);
            continue;
        }
        wchar_t found[MAX_PATH * 2];
        wchar_t* filepart = nullptr;
        DWORD n = SearchPathW(nullptr, candidate.c_str(), nullptr,
                              static_cast<DWORD>(std::size(found)), found, &filepart);
        if (n > 0 && n < std::size(found)) return narrow(std::wstring(found, n));
    }
    return std::string();
}

const std::string& win_python_command() {
    static std::string cached;
    static bool resolved = false;
    if (resolved) return cached;
    resolved = true;

    // "py -3" first. The Python launcher is the one invocation that is
    // reliably correct regardless of how Python was installed, and it
    // sidesteps the Microsoft Store app-execution-alias stub that ships
    // enabled by default: a zero-length python.exe in WindowsApps that does
    // nothing but open the Store page. Finding that stub and handing it a
    // script would look exactly like "python is installed but the lyrics
    // helper produces no output".
    if (!win_find_executable("py").empty()) {
        cached = "py -3";
        return cached;
    }
    for (const char* cand : {"python3", "python"}) {
        std::string path = win_find_executable(cand);
        if (path.empty()) continue;
        if (path.find("\\WindowsApps\\") != std::string::npos ||
            path.find("\\windowsapps\\") != std::string::npos) {
            continue;   // Store alias stub, not a real interpreter
        }
        cached = path;
        return cached;
    }
    return cached;   // empty -> lyrics_fetcher reports PythonMissing
}

// ---------------------------------------------------------------------------
// POSIX shims
// ---------------------------------------------------------------------------

long long win_pread(int fd, void* buf, std::size_t count, long long offset) {
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) return -1;

    // OVERLAPPED carries the offset, so unlike a seek+read pair this never
    // disturbs the descriptor's own file pointer and needs no locking.
    OVERLAPPED ov{};
    ov.Offset     = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFull);
    ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);

    DWORD read_bytes = 0;
    if (!ReadFile(h, buf, static_cast<DWORD>(count), &read_bytes, &ov)) {
        // Reading at or past EOF is a normal outcome for the header probes
        // in native_duration.cpp, not an error.
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return static_cast<long long>(read_bytes);
}

std::tm win_localtime(std::time_t t) {
    std::tm out{};
    localtime_s(&out, &t);
    return out;
}

// ---------------------------------------------------------------------------
// Column widths
//
// A compact wcwidth. The ranges below follow Markus Kuhn's reference
// implementation and Unicode's East_Asian_Width property closely enough for
// terminal layout: zero for combining marks and format characters, two for
// the East Asian wide and emoji blocks, one for everything else.
//
// Indic scripts never reach here -- terminal_ui.cpp routes those to
// indic_handler.cpp first, which knows about the virama conjunct rules that
// no wcwidth can express.
// ---------------------------------------------------------------------------

namespace {

struct Range { uint32_t lo, hi; };

// Sorted; binary-searched.
constexpr Range kZeroWidth[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0610, 0x061A},
    {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0711, 0x0711}, {0x0730, 0x074A},
    {0x07A6, 0x07B0}, {0x07EB, 0x07F3}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x08E3, 0x0903},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E},
    {0x2060, 0x2064}, {0x206A, 0x206F}, {0x20D0, 0x20F0}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB},
    {0xE0100, 0xE01EF},
};

constexpr Range kWide[] = {
    {0x1100, 0x115F},
    // BMP emoji with Emoji_Presentation=Yes (East Asian Width "W").
    {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
    {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
    {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
    {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF},
    {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60},
    {0xFFE0, 0xFFE6},
    {0x16FE0, 0x16FE4}, {0x17000, 0x18AFF},
    {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F320}, {0x1F32D, 0x1F335},
    {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4},
    {0x1F3F8, 0x1F43E}, {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC},
    {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E},
    // 0x1F550-0x1F64F as one unbroken range (not split at 0x1F567/0x1F568
    // the way upstream emoji-data.txt's Emoji_Presentation property would
    // suggest): several codepoints in that gap -- MAN DANCING (0x1F57A)
    // among them -- have default *text* presentation per Unicode, but this
    // console always renders every codepoint in this block through the
    // double-width emoji font regardless of presentation, so treating the
    // gap as narrow under-measured them by one column and shifted every
    // border to the right of that title.
    {0x1F550, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC},
    {0x1F6F4, 0x1F6FC}, {0x1F7E0, 0x1F7EB},
    {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x3FFFD},
};

bool in_ranges(uint32_t cp, const Range* table, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < table[mid].lo)      hi = mid;
        else if (cp > table[mid].hi) lo = mid + 1;
        else                         return true;
    }
    return false;
}

} // namespace

int win_codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return -1;   // control characters
    if (in_ranges(cp, kZeroWidth, std::size(kZeroWidth))) return 0;
    if (in_ranges(cp, kWide, std::size(kWide))) return 2;
    return 1;
}

} // namespace muisc

#endif // _WIN32
