#include "terminal_ui.h"
#include "indic_handler.h"
#include "utf8_util.h"
#include <algorithm>
#include <cstdint>
#include <iostream>
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <cwchar>
#endif

namespace muisc {

#if !defined(_WIN32)
static struct termios g_orig_termios;
#endif

TerminalIO::TerminalIO() {
#if defined(_WIN32)
    // The console mode/code-page save already happened in main() via
    // win_console_init(); this only flips input into the no-echo,
    // no-line-editing state that the termios branch below sets up.
    win_raw_mode_enter();
#else
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
    raw_mode_active_ = true;
    // Alternate screen buffer: the terminal keeps a second, separate
    // grid (same dimensions as the visible one) while this is active.
    // "\x1b[H" homing the cursor and any accidental scroll it causes
    // both happen *within* that private grid, never touching the user's
    // actual shell scrollback -- and switching back with "\x1b[?1049l"
    // on exit restores exactly whatever was on screen before mousiki
    // started, cleanly, rather than leaving a trail of overwritten
    // frames behind in their scrollback history.
    //
    // To be clear about what this does and doesn't fix: it does NOT by
    // itself prevent the frame-height-exceeds-terminal-rows scrolling
    // bug (the alt-screen grid is still exactly term.rows() tall, and
    // still scrolls internally if you print past its bottom with no
    // clear) -- that's what term_rows_ and clamp_output_rows() in
    // app.cpp actually fix. This is a separate, complementary
    // improvement: even if some future change reintroduces a height
    // miscalculation, the damage is contained to mousiki's own private
    // buffer instead of polluting the terminal the person is actually
    // going to keep using afterwards.
    std::cout << "\x1b[?1049h" << "\x1b[?25l" << std::flush; // enter alt-screen, hide cursor
}

TerminalIO::~TerminalIO() { restore(); }

void TerminalIO::restore() {
    if (raw_mode_active_) {
#if defined(_WIN32)
        win_raw_mode_exit();
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
#endif
        std::cout << "\x1b[?25h" << "\x1b[?1049l" << std::flush; // show cursor, leave alt-screen
        raw_mode_active_ = false;
    }
}

// Subprocesses we spawn are supposed to never touch our stdin at all
// (see process_util.cpp's run_capture() and waveform.cpp's ffmpeg
// fallback — both redirect the child's stdin to /dev/null specifically
// because of this). But that fix lives in the spawn call sites, and
// this is the one place that actually NEEDS raw+non-blocking mode to
// keep working no matter what: re-applying our own termios settings on
// every poll is cheap (one syscall, ~25x/sec) and means that even if
// something unexpected resets the terminal to canonical/line-buffered
// mode, we're never more than one frame away from correcting it,
// instead of the read() call silently becoming blocking and stalling
// the entire render loop until a keypress+Enter happens to satisfy it.
void TerminalIO::reassert_raw_mode() {
    if (!raw_mode_active_) return;
#if defined(_WIN32)
    // Same reasoning, different mechanism: ffmpeg and yt-dlp are spawned with
    // their own console handles (see process_util.cpp), but re-asserting the
    // input mode every frame is one cheap call and makes the render loop
    // self-healing if anything else resets it.
    win_raw_mode_enter();
#else
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
#endif
}

// See last_key_was_arrow() in terminal_ui.h. Only ever written by
// poll_key() below, and only read by whatever handles the key that same
// call just returned -- single-threaded, since the input loop is the only
// thing that polls.
static bool g_last_key_was_arrow = false;

bool last_key_was_arrow() { return g_last_key_was_arrow; }

int TerminalIO::poll_key() {
    reassert_raw_mode();
#if defined(_WIN32)
    int key = win_poll_key();
    g_last_key_was_arrow = win_last_key_was_arrow();
    return key;
#else
    unsigned char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) { g_last_key_was_arrow = false; return 0; }

    if (c == '\x1b') {
        unsigned char seq[2] = {0, 0};
        if (read(STDIN_FILENO, &seq[0], 1) != 1) { g_last_key_was_arrow = false; return 27; }
        if (read(STDIN_FILENO, &seq[1], 1) != 1) { g_last_key_was_arrow = false; return 27; }
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': g_last_key_was_arrow = true; return 'A';
                case 'B': g_last_key_was_arrow = true; return 'B';
                case 'C': g_last_key_was_arrow = true; return 'C';
                case 'D': g_last_key_was_arrow = true; return 'D';
                case 'H': g_last_key_was_arrow = false; return kKeyHome; // most xterm-likes send ESC [ H for Home
                case '3': { // ESC [ 3 ~ -- Delete key
                    unsigned char tail = 0;
                    if (read(STDIN_FILENO, &tail, 1) == 1 && tail == '~') { g_last_key_was_arrow = false; return kKeyDelete; }
                    g_last_key_was_arrow = false;
                    return 27;
                }
            }
        }
        g_last_key_was_arrow = false;
        return 27;
    }
    g_last_key_was_arrow = false;
    return c;
#endif
}

int TerminalIO::rows() const {
#if defined(_WIN32)
    return win_term_rows();
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    return 40;
#endif
}

int TerminalIO::cols() const {
#if defined(_WIN32)
    return win_term_cols();
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    return 155;
#endif
}

// ---------------------------------------------------------------------------
// Emoji replacement
//
// Whether an emoji occupies one cell or two is decided by the *terminal*, not
// by this program, and terminals disagree: conhost, Windows Terminal (which
// changed its rules between releases), the VS Code terminal and ConPTY
// each ship their own width tables, and multi-codepoint emoji (ZWJ sequences,
// flags, skin tones, VS16 "emoji style" forms, keycaps) are measured
// differently again. Every column calculation in this file is only as good as
// our guess at that answer, and a wrong guess by a single cell shifts every
// "|" to the right of the title on that row.
//
// So by default an emoji (a whole cluster, however many codepoints it has) is
// drawn as ONE plain "?" -- a character every terminal agrees is exactly one
// cell wide -- and display_width()/pad_right()/truncate_str()/... all measure
// that "?". The layout is then correct no matter what the terminal thinks of
// emoji. Set  ReplaceEmoji=false  in config.txt to draw the real emoji again
// (alignment then depends on the terminal agreeing with the width table).
//
// Deliberately narrow: only genuine emoji are replaced. Text symbols that
// music titles use all the time (music notes, stars, arrows, dingbat checks)
// are untouched, as are all CJK / Indic / Latin characters.
// ---------------------------------------------------------------------------
static bool g_replace_emoji = true;

void set_emoji_replacement(bool on) { g_replace_emoji = on; }

namespace {

struct CpRange { uint32_t lo, hi; };

// Codepoints below U+1F000 with Emoji_Presentation=Yes, i.e. ones that are
// emoji even without a variation selector (sorted, binary-searched).
constexpr CpRange kBmpEmoji[] = {
    {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
    {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
    {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50},
    {0x2B55, 0x2B55},
};

bool cp_in_table(uint32_t cp, const CpRange* t, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < t[mid].lo)      hi = mid;
        else if (cp > t[mid].hi) lo = mid + 1;
        else                     return true;
    }
    return false;
}

// A codepoint that is an emoji on its own.
bool is_emoji_base(uint32_t cp) {
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return true; // pictographs, flags' regional indicators, emoticons, ...
    return cp_in_table(cp, kBmpEmoji, sizeof(kBmpEmoji) / sizeof(kBmpEmoji[0]));
}

bool is_regional_indicator(uint32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
bool is_emoji_modifier(uint32_t cp)     { return cp >= 0x1F3FB && cp <= 0x1F3FF; } // skin tones
bool is_emoji_tag(uint32_t cp)          { return cp >= 0xE0020 && cp <= 0xE007F; } // flag subdivisions

std::string replace_emoji(const std::string& s) {
    if (!g_replace_emoji) return s;
    // Fast path: every emoji, VS16, ZWJ and keycap is encoded with a lead byte
    // >= 0xE2, so nearly every ordinary title bails out here after one scan.
    bool maybe = false;
    for (unsigned char c : s) { if (c >= 0xE2) { maybe = true; break; } }
    if (!maybe) return s;

    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);

        // A plain character turned into an emoji by a following VS16 (U+FE0F)
        // or keycap (U+20E3): "\u2764\uFE0F", "1\uFE0F\u20E3", ...
        bool forms_emoji = false;
        if (!is_emoji_base(cp) && i < s.size()) {
            size_t j = i;
            uint32_t next = utf8_decode(s, j);
            forms_emoji = (next == 0xFE0F || next == 0x20E3);
        }
        if (!is_emoji_base(cp) && !forms_emoji) {
            out.append(s, start, i - start);
            continue;
        }

        // Swallow the rest of the cluster: modifiers, variation selectors,
        // keycaps, tag sequences, a second regional indicator (flags), and
        // ZWJ-joined follow-up emoji.
        bool first_is_ri = is_regional_indicator(cp);
        while (i < s.size()) {
            size_t j = i;
            uint32_t next = utf8_decode(s, j);
            if (next == 0xFE0F || next == 0xFE0E || next == 0x20E3
                || is_emoji_modifier(next) || is_emoji_tag(next)) {
                i = j;
            } else if (first_is_ri && is_regional_indicator(next)) {
                i = j;
                first_is_ri = false; // a flag is exactly two indicators
            } else if (next == 0x200D && j < s.size()) {
                size_t k = j;
                uint32_t after = utf8_decode(s, k);
                if (is_emoji_base(after)) { i = k; } else { break; }
            } else {
                break;
            }
        }
        out += '?';
    }
    return out;
}

} // namespace

static int codepoint_width(uint32_t cp) {
    if (cp == 0) return 0;
    if (is_indic_codepoint(cp)) return indic_codepoint_width(cp);
#if defined(_WIN32)
    // Not wcwidth(static_cast<wchar_t>(cp)): wchar_t is 16 bits here, so
    // that cast silently mangles every codepoint above U+FFFF.
    int w = win_codepoint_width(cp);
#else
    int w = wcwidth(static_cast<wchar_t>(cp));
#endif
    return w < 0 ? 0 : w;
}
// w
// int display_width(const std::string& s) {
//     std::string clean = sanitize_lyric_text(s);
// 
//     int cols = 0;
//     size_t i = 0;
//     uint32_t prev_cp = 0;
// 
//     while (i < clean.size()) {
//         uint32_t cp = utf8_decode(clean, i);
//         int w = codepoint_width(cp);

int display_width(const std::string& raw) {
    const std::string s = replace_emoji(raw);
    int cols = 0;
    size_t i = 0;
    uint32_t prev_cp = 0;
    while (i < s.size()) {
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        // Virama conjunct subtraction: if previous char was a Virama and current is a consonant (width 1),
        // they form a conjunct ligature that fits in the same cell. Subtract 1 to compensate.
        if (is_indic_codepoint(cp)) {
            if (prev_cp == 0x094D || prev_cp == 0x09CD || prev_cp == 0x0A4D || 
                prev_cp == 0x0ACD || prev_cp == 0x0B4D || prev_cp == 0x0BCD || 
                prev_cp == 0x0C4D || prev_cp == 0x0CCD || prev_cp == 0x0D4D) {
                if (w == 1) {
                    cols -= 1; // Merge with previous cell
                }
            }
        }
        
        cols += w;
        prev_cp = cp;
    }
    return cols;
}

std::string utf8_take(const std::string& raw, int width) {
    const std::string s = replace_emoji(raw);
    std::string out;
    size_t i = 0;
    int used = 0;
    uint32_t prev_cp = 0;
    while (i < s.size()) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        
        if (is_indic_codepoint(cp)) {
            if (prev_cp == 0x094D || prev_cp == 0x09CD || prev_cp == 0x0A4D || 
                prev_cp == 0x0ACD || prev_cp == 0x0B4D || prev_cp == 0x0BCD || 
                prev_cp == 0x0C4D || prev_cp == 0x0CCD || prev_cp == 0x0D4D) {
                if (w == 1) {
                    used -= 1; // Merge with previous cell
                }
            }
        }
        
        if (used + w > width) {
            if (w == 0) {
                out += s.substr(start, i - start);
                used += w;
                prev_cp = cp;
                continue;
            }
            break;
        }
        out += s.substr(start, i - start);
        used += w;
        prev_cp = cp;
    }
    return out;
}

std::string utf8_skip_take(const std::string& raw, int skip_cols, int take_cols) {
    const std::string s = replace_emoji(raw);
    std::string out;
    size_t i = 0;
    int skipped = 0;
    uint32_t prev_cp = 0;
    while (i < s.size() && skipped < skip_cols) {
        uint32_t cp = utf8_decode(s, i);
        skipped += codepoint_width(cp);
        prev_cp = cp;
    }
    (void)prev_cp;
    int taken = 0;
    while (i < s.size() && taken < take_cols) {
        size_t start = i;
        uint32_t cp = utf8_decode(s, i);
        int w = codepoint_width(cp);
        if (taken + w > take_cols) break;
        out += s.substr(start, i - start);
        taken += w;
    }
    return out;
}

std::string pad_right(const std::string& raw, int width) {
    if (width <= 0) return "";
    const std::string s = replace_emoji(raw);
    int w = display_width(s);
    if (w >= width) return utf8_take(s, width);
    return s + std::string(width - w, ' ');
}

std::string pad_left(const std::string& raw, int width) {
    if (width <= 0) return "";
    const std::string s = replace_emoji(raw);
    int w = display_width(s);
    if (w >= width) return utf8_take(s, width);
    return std::string(width - w, ' ') + s;
}

std::string truncate_str(const std::string& raw, int width) {
    if (width <= 0) return "";
    const std::string s = replace_emoji(raw);
    int w = display_width(s);
    if (w <= width) return s;
    if (width <= 3) return utf8_take(s, width);
    return utf8_take(s, width - 3) + "...";
}

std::vector<std::string> wrap_lines(const std::string& raw, int width, int max_lines) {
    const std::string s = replace_emoji(raw);
    std::vector<std::string> out;
    if (width <= 0 || max_lines <= 0) return out;

    // Word-wrap on ASCII spaces (the only inputs are track titles and
    // filesystem paths). The hard case this exists for: a "word" that alone
    // is wider than one whole line -- a long unbroken filename, or a CJK/
    // Thai/etc. title, which has no spaces at all and would otherwise be
    // one giant word -- is sliced into successive width-sized chunks
    // instead of being dumped whole onto an overflowing line or silently
    // cut down to a single line.
    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : s) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }

    size_t wi = 0;
    while (wi < words.size() && static_cast<int>(out.size()) < max_lines) {
        std::string line;
        int line_w = 0;
        bool line_done = false;
        while (wi < words.size() && !line_done) {
            std::string& w = words[wi];
            int ww = display_width(w);
            if (ww > width) {
                // Doesn't fit on a line by itself. If this line already has
                // something on it, close it out so the oversized word gets
                // its own fresh line(s) to be chunked across.
                if (!line.empty()) { line_done = true; break; }
                std::string piece = utf8_take(w, width);
                if (piece.empty()) { ++wi; continue; } // width too small for even one codepoint; skip rather than loop forever
                line = piece;
                line_w = display_width(piece);
                std::string rest = w.substr(piece.size());
                if (rest.empty()) ++wi; else w = rest;
                line_done = true;
                break;
            }
            int add_w = ww + (line.empty() ? 0 : 1);
            if (line_w + add_w > width) { line_done = true; break; }
            if (!line.empty()) { line += ' '; line_w += 1; }
            line += w;
            line_w += ww;
            ++wi;
        }
        out.push_back(line);
    }

    // Words remain but we're out of lines: mark the truncation on the last
    // line actually produced, the same way truncate_str() does for a
    // single line.
    if (wi < words.size() && !out.empty()) {
        std::string& last = out.back();
        last = (width <= 3) ? utf8_take(last, width) : utf8_take(last, std::max(0, width - 3)) + "...";
    }
    return out;
}

} // namespace muisc
