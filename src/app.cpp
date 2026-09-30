#include "app.h"
#include "path_utf8.h"
#include "utf8_util.h"
#include "console_log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>
#if defined(_WIN32)
#include "win_compat.h"
#else
#include <unistd.h>
#include <sys/utsname.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace muisc {

namespace {

// ---------------------------------------------------------------------
// Worker-thread exception guard
//
// An exception that escapes a std::thread's function does NOT propagate to
// the thread that spawned it and does not unwind anywhere useful: the
// standard says it calls std::terminate(), which kills the entire process on
// the spot. On Windows that means the app vanishes with no message at all --
// no stack trace, no console output, nothing in the log. Every background
// task in this file (metadata sweep, decode, lyrics fetch, waveform pass,
// search, bulk queue add) touches the filesystem or spawns subprocesses, and
// any of those can throw.
//
// This wrapper is the process-level safety net: whatever a worker throws is
// turned into a log line, and that task alone fails. The encoding bugs fixed
// alongside it were the cause we know about; this makes sure the next one
// (a disappearing USB drive, a permissions change mid-scan) degrades instead
// of detonating.
template <class F>
void run_guarded(const char* what, F&& body) noexcept {
    try {
        body();
    } catch (const std::exception& e) {
        ConsoleLog::instance().log_basic(std::string("internal error in ") + what + ": " + e.what());
    } catch (...) {
        ConsoleLog::instance().log_basic(std::string("internal error in ") + what + " (unknown exception)");
    }
}

// ASCII-only, deliberately: this folds track titles and artist names, which
// are UTF-8. std::tolower over raw bytes mangles multi-byte sequences under
// any single-byte locale -- see ascii_lower() in utf8_util.h.
// Every text-entry field below (search box, bulk-add link field, color
// hex field, the retry-lyrics title/artist/type fields) used to only
// accept key values 32-126 -- plain printable ASCII. That's not just a
// Windows gap: a typed umlaut, accented letter, or any other non-ASCII
// character arrives as a multi-byte UTF-8 sequence whose individual byte
// values are all >= 0x80 (continuation bytes 0x80-0xBF, lead bytes
// 0xC2-0xF4), every one of which used to fail this check and simply
// never reach the buffer -- on POSIX with a real UTF-8 terminal just as
// much as on Windows. win_poll_key() (Windows) and the POSIX raw-input
// path both hand such a keystroke over one UTF-8 byte per call already;
// this is what actually lets any of those bytes through.
bool is_text_key(int key) {
    return (key >= 32 && key < 127) || (key >= 0x80 && key <= 0xFF);
}

// Removes exactly one full UTF-8 codepoint from the end of a text-entry
// buffer, not just its last byte. A plain pop_back() left a dangling lead
// byte behind for any accented/non-ASCII character (a German umlaut is
// two bytes; CJK is three) -- that lone byte decodes as a replacement
// glyph, and needed a second, confusing Backspace press to actually
// finish clearing what looked like one character.
void pop_utf8_char(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
}

// ---------------------------------------------------------------------------
// Single-line text editing: caret, selection, and how a field is painted
//
// A text field in this app is a plain std::string plus two BYTE offsets (see
// edit_caret_/edit_anchor_ in app.h): the caret, and the other end of the
// selection -- equal to the caret when nothing is marked, which makes
// "caret != anchor" the selection test itself. Offsets are in bytes because
// that is what the buffers hold, but every movement and every deletion is
// quantised to the codepoint boundaries below, so a multi-byte character is
// never split in half.
//
// Two editors share this code and are never active at the same time:
// Mode::ColorEdit (the Settings' in-place value editing) and the meta
// editor's field editor. Both own their offsets and pass them in.
// ---------------------------------------------------------------------------

// Every byte offset at which `s` may be split without breaking UTF-8: 0,
// then the start of each codepoint, then s.size() itself.
static std::vector<size_t> utf8_cuts(const std::string& s) {
    std::vector<size_t> cuts;
    cuts.reserve(s.size() + 1);
    cuts.push_back(0);
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = 1;
        if ((c & 0xE0) == 0xC0) n = 2;
        else if ((c & 0xF0) == 0xE0) n = 3;
        else if ((c & 0xF8) == 0xF0) n = 4;
        i += n;
        if (i > s.size()) i = s.size(); // truncated lead byte: treat the tail as one char
        cuts.push_back(i);
    }
    return cuts;
}

static size_t utf8_prev_cut(const std::string& s, size_t off) {
    if (off == 0) return 0;
    size_t best = 0;
    for (size_t c : utf8_cuts(s)) {
        if (c >= off) break;
        best = c;
    }
    return best;
}

static size_t utf8_next_cut(const std::string& s, size_t off) {
    if (off >= s.size()) return s.size();
    for (size_t c : utf8_cuts(s)) if (c > off) return c;
    return s.size();
}

static void le_clamp(const std::string& s, size_t& caret, size_t& anchor) {
    if (caret > s.size()) caret = s.size();
    if (anchor > s.size()) anchor = s.size();
}

static void le_range(size_t caret, size_t anchor, size_t& a, size_t& b) {
    a = std::min(caret, anchor);
    b = std::max(caret, anchor);
}

static bool le_has_sel(size_t caret, size_t anchor) { return caret != anchor; }

static void le_erase_selection(std::string& s, size_t& caret, size_t& anchor) {
    size_t a = 0, b = 0;
    le_range(caret, anchor, a, b);
    if (a == b) return;
    s.erase(a, b - a);
    caret = anchor = a;
}

// dir: -1 left, +1 right. shift keeps anchor where it was (marking), which
// is exactly the difference between Shift+Left and Left.
static void le_move(std::string& s, size_t& caret, size_t& anchor, int dir, bool shift) {
    le_clamp(s, caret, anchor);
    caret = (dir < 0) ? utf8_prev_cut(s, caret) : utf8_next_cut(s, caret);
    if (!shift) anchor = caret;
}

static void le_backspace(std::string& s, size_t& caret, size_t& anchor) {
    le_clamp(s, caret, anchor);
    if (le_has_sel(caret, anchor)) { le_erase_selection(s, caret, anchor); return; }
    size_t p = utf8_prev_cut(s, caret);
    s.erase(p, caret - p);
    caret = anchor = p;
}

static void le_delete_forward(std::string& s, size_t& caret, size_t& anchor) {
    le_clamp(s, caret, anchor);
    if (le_has_sel(caret, anchor)) { le_erase_selection(s, caret, anchor); return; }
    size_t n = utf8_next_cut(s, caret);
    s.erase(caret, n - caret);
    anchor = caret;
}

// `limit` is the field's maximum byte length -- the same cap the old
// "append at the end" code enforced with its inline size() checks.
static void le_insert(std::string& s, size_t& caret, size_t& anchor, char c, size_t limit) {
    le_clamp(s, caret, anchor);
    le_erase_selection(s, caret, anchor);
    if (s.size() >= limit) return;
    s.insert(caret, 1, c);
    caret = anchor = caret + 1;
}

// Paste. Clipboard text can be multi-line and arbitrary length: control
// characters are dropped (a newline inside a single-line row would corrupt
// the layout) and whatever still doesn't fit is trimmed, codepoint-aligned,
// against the same limit.
static void le_paste(std::string& s, size_t& caret, size_t& anchor, const std::string& text, size_t limit) {
    le_clamp(s, caret, anchor);
    le_erase_selection(s, caret, anchor);
    std::string t;
    t.reserve(text.size());
    for (unsigned char c : text) {
        if (c >= 32 && c != 127) t.push_back(static_cast<char>(c));
    }
    if (t.empty()) return;
    const size_t room = (s.size() >= limit) ? 0 : limit - s.size();
    while (t.size() > room && !t.empty()) t.erase(utf8_prev_cut(t, t.size()), std::string::npos);
    if (t.empty()) return;
    s.insert(caret, t);
    caret = anchor = caret + t.size();
}

// One keystroke, every single-line text field in the app.
//
// Caret movement, Shift+arrows marking, Home/End, Ctrl+C/X/V and typing --
// the same key set the Settings' ColorEdit and the meta editor's field editor
// grew first, factored out here so the four search/filter boxes (the main
// UI's "/", the meta editor's Search line, the playlist editor's name and
// library boxes and the playlist list's own search) can offer exactly the
// same thing instead of a fifth hand-rolled copy.
//
// Returns true only when the TEXT really changed, which is what tells the
// caller to re-run whatever filter the box drives; pure caret/clipboard keys
// return false. `status` (nullable) receives the short feedback these keys
// produce ("COPIED"/"CUT"/"PASTED"), because a box with no status line of
// its own should not go silent on a copy.
static bool edit_text_key(std::string& buf, size_t& caret, size_t& anchor, int key,
                          size_t limit, std::string* status) {
    auto say = [&](const char* s) { if (status) *status = s; };
    // Modifier combinations arrive as their own sentinel values
    // (terminal_ui.h) precisely because the bare ones are already taken: an
    // arrow press collapses to the letter it would otherwise type, and a
    // typed 'c' has to stay a typed 'c' inside a word.
    if (key == kKeyHome) { caret = anchor = 0; return false; }
    if (key == kKeyEnd) { caret = anchor = buf.size(); return false; }
    if (key == kKeyShiftLeft) { le_move(buf, caret, anchor, -1, true); return false; }
    if (key == kKeyShiftRight) { le_move(buf, caret, anchor, +1, true); return false; }
    if (key == kKeyDelete) { le_delete_forward(buf, caret, anchor); return true; }
    if (key == kKeyCtrlC) {
        size_t a = 0, b = 0;
        le_range(caret, anchor, a, b);
        std::string t = (a == b) ? buf : buf.substr(a, b - a);
        clipboard_set(t);
        say(t.empty() ? "nothing selected" : "COPIED");
        return false;
    }
    if (key == kKeyCtrlX) {
        size_t a = 0, b = 0;
        le_range(caret, anchor, a, b);
        if (a == b) return false; // no selection: cut must never empty the field
        clipboard_set(buf.substr(a, b - a));
        le_erase_selection(buf, caret, anchor);
        say("CUT");
        return true;
    }
    if (key == kKeyCtrlV) { le_paste(buf, caret, anchor, clipboard_get(), limit); say("PASTED"); return true; }
    if (key == 127 || key == 8) { le_backspace(buf, caret, anchor); return true; }
    // A genuinely typed A-D goes into the buffer ("C:\" has no way past this
    // check otherwise), while an arrow press moves the caret Left/Right -- and
    // Up/Down is dropped here: every field this is used on is a single line,
    // and the boxes that DO want Up/Down for the list below them claim those
    // keys before calling in.
    if (last_key_was_arrow() && (key == 'A' || key == 'B')) return false;
    if (last_key_was_arrow() && (key == 'C' || key == 'D')) {
        le_move(buf, caret, anchor, key == 'D' ? -1 : +1, false);
        return false;
    }
    if (is_text_key(key)) { le_insert(buf, caret, anchor, static_cast<char>(key), limit); return true; }
    return false;
}

// (App::edit_focus() lives below, outside this anonymous namespace.)

// What one field looks like on screen after all of that.
struct EditPaint {
    std::string s;  // the windowed text, selection wrapped in reverse video
    int cols = 0;   // display columns s really occupies (ANSI escapes not counted)
    int caret = 0;  // 0-based column of the caret inside that window
};

// Paints a field into `w` columns: a window scrolled so the caret stays
// visible (the buffer can be far longer than the field), the selected range
// in reverse video, and -- with `block` -- the caret itself drawn as a █,
// which is how the meta editor shows a cursor. The Settings tabs instead move
// the REAL terminal cursor to `caret` (they already do that today) and pass
// block = false.
//
// `restore` is re-emitted after the selection so the field keeps its own
// colour; it must be exactly the SGR the caller has already put in effect,
// because display_width() cannot see through ANSI escapes -- colouring first
// and measuring afterwards would miscount every row.
static EditPaint paint_edit_field(const std::string& s, size_t caret, size_t anchor, int w,
                                  const std::string& restore, bool block) {
    EditPaint out;
    if (w < 1) w = 1;
    le_clamp(s, caret, anchor);
    if (anchor > caret) std::swap(anchor, caret);

    const std::vector<size_t> cuts = utf8_cuts(s);
    std::vector<int> col(cuts.size(), 0); // col[i] = display column of cuts[i]
    for (size_t i = 1; i < cuts.size(); ++i) {
        col[i] = col[i - 1] + display_width(s.substr(cuts[i - 1], cuts[i] - cuts[i - 1]));
    }
    auto idx_of = [&](size_t off) -> size_t {
        for (size_t i = 0; i < cuts.size(); ++i) if (cuts[i] == off) return i;
        return cuts.size() - 1;
    };
    const size_t caret_i = idx_of(caret);
    const size_t anchor_i = idx_of(anchor);

    // The leftmost window that still keeps the caret inside it: starting any
    // further left would push the caret past the right edge, starting further
    // right would hide text the caret is sitting on.
    const int caret_col = col[caret_i];
    const int target = caret_col - (w - 1);
    size_t lo_i = 0;
    for (size_t i = 0; i <= caret_i; ++i) {
        if (col[i] >= target) { lo_i = i; break; }
        lo_i = caret_i;
    }
    size_t hi_i = lo_i;
    while (hi_i + 1 < cuts.size() && col[hi_i + 1] - col[lo_i] <= w) ++hi_i;
    if (hi_i < caret_i) hi_i = caret_i;

    out.caret = caret_col - col[lo_i];
    if (out.caret < 0) out.caret = 0;
    if (out.caret > w - 1) out.caret = w - 1;

    auto emit = [&](size_t from, size_t to, bool sel, const char* glyph) {
        std::string body = glyph ? std::string(glyph) : s.substr(from, to - from);
        if (body.empty()) return;
        if (sel) out.s += "\x1b[7m" + body + "\x1b[0m" + restore;
        else out.s += body;
    };
    for (size_t i = lo_i; i < hi_i; ++i) {
        const bool sel = (i >= anchor_i && i < caret_i);
        if (block && i == caret_i) emit(cuts[i], cuts[i + 1], sel, "\u2588");
        else emit(cuts[i], cuts[i + 1], sel, nullptr);
    }
    // Caret at (or beyond) the end of the window: there is no character to
    // replace, so the block is appended -- which only fits because the window
    // above reserves the column (col[caret_i] - col[lo_i] <= w-1).
    if (block && caret_i >= hi_i) emit(0, 0, false, "\u2588");

    out.cols = col[hi_i] - col[lo_i];
    if (block && caret_i >= hi_i) out.cols += 1;
    return out;
}

std::string lower(std::string s) {
    return ascii_lower_str(std::move(s));
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

// Lowercases AND folds common word-separator punctuation (hyphen,
// underscore, dot, slash) down to plain spaces. Used only for search
// matching (fuzzy_score below), never for display.
//
// Without this, a tag/filename like "X-Files" is one glued-together
// token "x-files" as far as matching is concerned, while a query typed
// as "X Files" is two separate words ["x", "files"]. Tier 1 (exact
// substring) fails because "x-files" never contains the literal text
// "x files". Tier 2 (per-word fuzzy) also fails: comparing whole word
// "x" against whole word "x-files" gives a huge edit distance (adding
// "-files"), well below the 0.55 quality floor -- even though every
// individual word the user typed is actually present. Folding the
// separator to a space first turns "x-files" into "x files" so both
// tiers line up with "X Files" the same way they already would for a
// title that happens to use a plain space.
std::string normalize_for_search(std::string s) {
    s = lower(std::move(s));
    for (char& c : s) {
        if (c == '-' || c == '_' || c == '.' || c == '/') c = ' ';
    }
    return s;
}

std::vector<std::string> split_words(const std::string& s) {
    std::vector<std::string> words;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) { words.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) words.push_back(cur);
    return words;
}

// Standard edit distance (single-char insert/delete/substitute cost 1).
// Small strings only (track titles/artists/search words) — the O(n*m)
// DP table is negligible at this scale.
int levenshtein(const std::string& a, const std::string& b) {
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 0; i <= n; ++i) dp[i][0] = static_cast<int>(i);
    for (size_t j = 0; j <= m; ++j) dp[0][j] = static_cast<int>(j);
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1, dp[i - 1][j - 1] + cost});
        }
    }
    return dp[n][m];
}

// Typo-tolerant match score: higher is better, negative means "not a
// match at all" (excluded from results). An exact literal substring
// match always outranks every fuzzy match, regardless of how good the
// fuzzy quality is — so "another love" typed exactly always sits above
// a merely-close fuzzy hit. Within the fuzzy tier, each query word must
// find a reasonably close word somewhere in the target (a query word
// that's wildly different from everything in the target means this
// isn't really a match, even if some OTHER query word happens to fit)
// — this is what lets "anogher lobe" (typo'd "another love") still find
// the track: word-level edit distance tolerates the substituted
// characters that a plain substring or subsequence check would miss
// entirely (neither "anogher" nor "lobe" appears anywhere in "another
// love" as literal text).
double fuzzy_score(const std::string& query, const std::string& target) {
    std::string q = normalize_for_search(query), t = normalize_for_search(target);
    if (q.empty()) return 0.0;

    size_t pos = t.find(q);
    if (pos != std::string::npos) {
        return 1000.0 - std::min<double>(static_cast<double>(pos), 900.0); // tier 1: exact substring, earlier position ranks higher
    }

    auto q_words = split_words(q);
    auto t_words = split_words(t);
    if (q_words.empty() || t_words.empty()) return -1.0;

    double total_quality = 0.0;
    for (auto& qw : q_words) {
        double best_quality = -1.0;
        for (auto& tw : t_words) {
            size_t max_len = std::max(qw.size(), tw.size());
            if (max_len == 0) continue;
            double normalized = static_cast<double>(levenshtein(qw, tw)) / static_cast<double>(max_len);
            double quality = 1.0 - normalized;
            if (quality > best_quality) best_quality = quality;
        }
        if (best_quality < 0.55) return -1.0; // this query word doesn't fit anywhere close enough -- not a real match
        total_quality += best_quality;
    }
    return (total_quality / static_cast<double>(q_words.size())) * 100.0; // tier 2: fuzzy, always below tier 1's range
}

std::string fmt_mmss(double seconds) {
    if (seconds < 0) return "--:--";
    int total = static_cast<int>(seconds);
    std::ostringstream oss;
    oss.width(2); oss.fill('0'); oss << (total / 60) << ":";
    oss.width(2); oss.fill('0'); oss << (total % 60);
    return oss.str();
}

// --- shared box-drawing helpers ------------------------------------------
// Every panel is built against `total_width` (the FULL visual width of the
// box, borders included) so two boxes placed side by side on the same row
// always sum to exactly the width the caller asked for, and single-box
// rows line up with everything above/below them. Content is always
// total_width-4 (for the "│ X │" pattern), padded with the codepoint-safe
// helpers from terminal_ui.cpp so multi-byte glyphs can't throw off the
// column count the way byte-length padding did before.

// SGR for a "cursor" (hovering) row. If both the configured foreground and
// background resolve to nothing -- e.g. ColorQueueCursorBg is blank in the
// user's config -- the row would look identical to its neighbours and the
// cursor would be invisible, so fall back to reverse video.
std::string cursor_sgr(const std::string& fg, const std::string& bg) {
    std::string s = ansi_for(fg) + bg_ansi_for(bg);
    return s.empty() ? std::string("\x1b[7m") : s;
}

} // namespace

std::string App::box_top(const std::string& label, int total_width, const std::string& border_ansi) const {
    std::string lbl = label.empty() ? "" : (" " + label + " ");
    std::string prefix = settings_.box_upper_left + settings_.box_horizontal + lbl;
    int used = display_width(prefix);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_upper_right;
    s = pad_right(s, total_width);
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_bottom(int total_width, const std::string& footer, const std::string& border_ansi) const {
    std::string prefix = footer.empty() ? (settings_.box_lower_left + settings_.box_horizontal) : (settings_.box_lower_left + settings_.box_horizontal + " " + footer + " ");
    int used = display_width(prefix);
    int dashes = std::max(0, total_width - used - 1);
    std::string s = prefix;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal;
    s += settings_.box_lower_right;
    s = pad_right(s, total_width);
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

std::string App::box_line(const std::string& content, int total_width, const std::string& border_ansi) const {
    int inner = std::max(0, total_width - 4);
    std::string padded = pad_right(truncate_str(content, inner), inner);
    if (border_ansi.empty()) return settings_.box_vertical + " " + padded + " " + settings_.box_vertical;
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    return bar + " " + padded + " " + bar;
}

// box_top() for a label whose tail is a live text field (caret + marked
// range). `field` is already painted and therefore full of reverse-video
// escapes, which display_width() -- UTF-8 aware, ANSI blind -- would count
// as columns; so the dash fill is computed from `field_cols`, the number of
// columns paint_edit_field() says it really occupies.
std::string App::box_top_field(const std::string& prefix, const std::string& field, int field_cols,
                               int total_width, const std::string& border_ansi) const {
    const int dashes = std::max(0, total_width - display_width(prefix) - field_cols - 1);
    std::string s = prefix + field;
    for (int i = 0; i < dashes; ++i) s += settings_.box_horizontal; // a std::string, not a char
    s += settings_.box_upper_right;
    if (border_ansi.empty()) return s;
    return border_ansi + s + "\x1b[0m";
}

// The same for box_line(): `prefix` plain, `field` painted, padded to
// box_line()'s own inner width (total_width - 4).
std::string App::box_line_field(const std::string& prefix, const std::string& field, int field_cols,
                                int total_width, const std::string& border_ansi) const {
    const int inner = std::max(0, total_width - 4);
    const int fill = std::max(0, inner - display_width(prefix) - field_cols);
    std::string padded = prefix + field + std::string(fill, ' ');
    if (border_ansi.empty()) return settings_.box_vertical + " " + padded + " " + settings_.box_vertical;
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    return bar + " " + padded + " " + bar;
}

// Points the shared caret/selection (edit_caret_/edit_anchor_) at one
// specific field: keeps the position while that same field is being typed
// into continuously, and jumps to the end whenever a different field -- or
// the same one, freshly focused -- takes over. Called once per key by each
// box, right before edit_text_key() runs on it.
void App::edit_focus(const std::string& owner, const std::string& text) {
    if (edit_owner_ != owner) {
        edit_owner_ = owner;
        edit_caret_ = edit_anchor_ = text.size();
    }
    le_clamp(text, edit_caret_, edit_anchor_);
}

namespace {

// Center-aligns plain (no-ANSI) text within `width` visual columns.
std::string center_pad(const std::string& text, int width) {
    std::string t = truncate_str(text, width);
    int pad = std::max(0, width - display_width(t));
    int left = pad / 2;
    int right = pad - left;
    return std::string(left, ' ') + t + std::string(right, ' ');
}

// Visualizer bars use ONLY this glyph set (per explicit instruction) —
// distinct from WaveformQuantizer's 6-level top/mid/bot triples used by
// the progress bar.
const char* fft_glyph(int level) {
    switch (std::clamp(level, 0, 4)) {
        case 0: return " ";
        case 1: return "\u28C0"; // ⣀
        case 2: return "\u28E4"; // ⣤
        case 3: return "\u28F6"; // ⣶
        default: return "\u28FF"; // ⣿
    }
}

// Word-wraps one lyric line to `width` visual columns (breaking on word
// boundaries, never mid-word), center-aligning each resulting row, and
// bakes in ANSI highlighting for words already "sung" (their timestamp
// <= elapsed) when this is the active line. Width/centering math is done
// on PLAIN text first — color codes are spliced in afterwards, since they
// don't occupy display columns but would otherwise confuse the
// codepoint-counting padding helpers.
// Counts UTF-8 codepoints (not bytes) -- used for the letter-by-letter
// lyrics reveal below. terminal_ui.cpp has an equivalent utf8_take(), but
// it's `static` (file-local) so it isn't reachable from here.


std::vector<std::string> render_lyric_line_wrapped(const LyricLine& line, double elapsed, int width,
                                                     bool is_active, const Settings& settings) {
    struct W { std::string text; double t; bool has_ts; };
    std::vector<W> words;
    if (!line.words.empty()) {
        for (const auto& wt : line.words) words.push_back({wt.second, wt.first, true});
    } else {
        std::istringstream iss(line.full_text);
        std::string w;
        while (iss >> w) words.push_back({w, line.start_time, false});
    }
    if (words.empty() || width <= 0) return {std::string(std::max(0, width), ' ')};

    std::vector<std::vector<W>> rows;
    std::vector<W> cur;
    int cur_len = 0;
    for (auto& w : words) {
        std::string remain = w.text;
        while (!remain.empty()) {
            int r_wlen = display_width(remain);
            int available = cur.empty() ? width : (width - cur_len - 1);
            
            if (r_wlen <= available) {
                cur.push_back({remain, w.t, w.has_ts});
                cur_len += (cur.empty() ? r_wlen : 1 + r_wlen);
                break;
            }
            
            if (!cur.empty()) {
                rows.push_back(cur);
                cur.clear();
                cur_len = 0;
                continue; // retry fitting on a new line
            }
            
            // The word exceeds the full width of a line, must split it.
            std::string chunk = utf8_take(remain, width);
            if (chunk.empty()) {
                // Failsafe: width is too small (e.g., 1) to fit a wide character (width 2).
                // Force-take 2 columns so we at least make progress (1 grapheme cluster).
                chunk = utf8_take(remain, 2);
            }
            
            cur.push_back({chunk, w.t, w.has_ts});
            rows.push_back(cur);
            cur.clear();
            cur_len = 0;
            remain = remain.substr(chunk.size());
        }
    }
    if (!cur.empty()) rows.push_back(cur);

    std::vector<std::string> out;
    for (auto& row : rows) {
        std::vector<std::string> mapped_words;
        int plain_len = 0;
        for (size_t i = 0; i < row.size(); ++i) {
            std::string mapped = apply_font_map(row[i].text, settings.font_map);
            mapped_words.push_back(mapped);
            plain_len += display_width(mapped);
            if (i > 0) plain_len += 1;
        }
        int total_pad = std::max(0, width - plain_len);
        int left_pad, right_pad;
        if (settings.lyrics_alignment == 1) { left_pad = 0; right_pad = total_pad; }           // left
        else if (settings.lyrics_alignment == 2) { left_pad = total_pad; right_pad = 0; }       // right
        else { left_pad = total_pad / 2; right_pad = total_pad - left_pad; }                    // center (default)

        // Word-level karaoke highlight: find the word currently being sung
        // (the last word whose timestamp has passed but the *next* one
        // hasn't) and render just that one underlined on top of the
        // normal active-word color -- gives the highlight a moving
        // "leading edge" instead of every already-sung word looking
        // identical. (A continuous per-frame pulse used to live here too,
        // applied to whole line-synced-only lines with no way to turn it
        // off -- removed; that wasn't requested and had no toggle.)
        int currently_singing = -1;
        for (size_t i = 0; i < row.size(); ++i) {
            if (is_active && row[i].has_ts && row[i].t <= elapsed) currently_singing = static_cast<int>(i);
        }
        const char* kUnderline = "\x1b[4m";

        std::string s(left_pad, ' ');
        std::string word_ansi = ansi_for(settings.active_word_color) + bg_ansi_for(settings.active_word_bg_color);
        std::string active_line_ansi = ansi_for(settings.active_line_color) + bg_ansi_for(settings.active_line_bg_color);
        std::string inactive_ansi = ansi_for(settings.inactive_line_color) + bg_ansi_for(settings.inactive_line_bg_color);
        // Word-by-word / letter-by-letter modes hide not-yet-sung words in
        // the active line entirely (blanked to spaces, same width, so the
        // alignment doesn't jump around) instead of showing them in the
        // active-line color right away -- that's the actual "progressive
        // reveal" that was asked for, as opposed to the old always-fully-
        // visible line with just a moving color highlight.
        bool progressive = is_active && (settings.lyrics_animation == 1 || settings.lyrics_animation == 2);
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0) s += ' ';
            bool sung = is_active && row[i].has_ts && row[i].t <= elapsed;
            bool no_word_ts_but_active = is_active && !row[i].has_ts;
            bool is_current = sung && static_cast<int>(i) == currently_singing;
            if (is_current && settings.lyrics_animation == 2) {
                // Letter-by-letter: interpolate how many characters of the
                // CURRENT word are revealed so far. Prefer the next word's
                // timestamp (within this wrapped row) as the end bound; if
                // this is the last word in the row (no next timestamp to
                // interpolate toward), fall back to the previous word's
                // pace instead of just popping the whole word in at once --
                // that "spawns out of nowhere" look was the bug. With no
                // timing context at all (a single-word line), use a
                // reasonable fixed pace.
                std::string full = mapped_words[i];
                int nchars = display_width(full);
                double dur = -1.0;
                if (i + 1 < row.size() && row[i + 1].has_ts && row[i + 1].t > row[i].t) {
                    dur = row[i + 1].t - row[i].t;
                } else if (i > 0 && row[i - 1].has_ts && row[i].t > row[i - 1].t) {
                    dur = row[i].t - row[i - 1].t;
                } else {
                    dur = 0.4;
                }
                double frac = std::clamp((elapsed - row[i].t) / dur, 0.0, 1.0);
                int revealed = static_cast<int>(frac * nchars);
                std::string shown = utf8_take(full, revealed);
                int hidden_cols = display_width(full) - display_width(shown);
                s += word_ansi + kUnderline + shown + "\x1b[0m" + std::string(std::max(0, hidden_cols), ' ');
            } else if (is_current) {
                s += word_ansi + kUnderline + mapped_words[i] + "\x1b[0m";
            } else if (sung) {
                s += word_ansi + mapped_words[i] + "\x1b[0m";
            } else if (progressive && row[i].has_ts) {
                s += std::string(display_width(mapped_words[i]), ' '); // not sung yet -- hidden, not just dimmed
            } else if (no_word_ts_but_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else if (is_active) {
                s += active_line_ansi + mapped_words[i] + "\x1b[0m";
            } else {
                s += inactive_ansi + mapped_words[i] + "\x1b[0m";
            }
        }
        s += std::string(right_pad, ' ');
        out.push_back(s);
    }
    return out;
}

// Resolves a filename under scripts/ next to the running binary. Shared by
// the lyrics helper and the fast-search helper -- same candidate list
// (env override, cwd, exe-relative at one/two/three levels up to cover a
// multi-config MSVC build), only the filename differs.
fs::path find_scripts_file(const std::string& filename) {
    if (const char* env = std::getenv("MOUSIKI_SCRIPTS_DIR")) {
        fs::path p = path_from_utf8(env) / filename;
        if (fs::exists(p)) return p;
    }
    fs::path cwd_candidate = fs::path("scripts") / filename;
    if (fs::exists(cwd_candidate)) return cwd_candidate;

#if defined(__APPLE__)
    char exe_buf[4096];
    uint32_t size = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        std::error_code ec;
        fs::path exe_dir = fs::canonical(fs::path(exe_buf), ec).parent_path();
        if (!ec) {
            fs::path p = exe_dir / "scripts" / filename;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
        }
    }
#elif defined(_WIN32)
    // No /proc on Windows; GetModuleFileNameW is the direct equivalent.
    // Worth noting the lookup one level up matters more here than on Linux:
    // a multi-config MSVC build puts the exe in build\\Release\\, so
    // scripts/ is two levels above it, and the CMake copy step mirrors
    // scripts/ next to the exe to cover that.
    {
        std::string exe = win_executable_path();
        if (!exe.empty()) {
            fs::path exe_dir = path_from_utf8(exe).parent_path();
            fs::path p = exe_dir / "scripts" / filename;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
            p = exe_dir.parent_path().parent_path() / "scripts" / filename;
            if (fs::exists(p)) return p;
        }
    }
#else
    char exe_buf[4096];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        fs::path exe_dir = fs::path(exe_buf).parent_path();
        fs::path p = exe_dir / "scripts" / filename;
        if (fs::exists(p)) return p;
        p = exe_dir.parent_path() / "scripts" / filename;
        if (fs::exists(p)) return p;
    }
#endif
    return cwd_candidate;
}

fs::path find_lyrics_script() { return find_scripts_file("fetch_lyrics.py"); }

// The InnerTube-based search script (see OnlineSource::search()). Unlike
// the lyrics script, its absence is not an error condition anywhere --
// OnlineSource falls back to yt-dlp's own search whenever this path
// doesn't resolve to a real file, so an empty/missing result here is a
// normal, silent path for anyone who only has the core scripts installed.
fs::path find_fast_search_script() { return find_scripts_file("fast_yt_search.py"); }

// fpcalc is a BUILD product: CMake compiles tools/fpcalc.cpp and copies the
// result next to mousiki.exe and into that exe's scripts/, so it exists only
// beside the executable -- never in a checkout's scripts/ folder, and
// find_scripts_file() can very well hand out exactly that one (the working
// directory wins there on purpose, so edited .py files take effect without a
// rebuild). fetch_meta.py resolves fpcalc next to the script IT was given
// (resolve_fpcalc()), so with those two rules combined a fetch started from a
// checkout's root dies with "fpcalc not found" even though this build has a
// perfectly good helper next to its own exe. Handing that path down through
// MOUSIKI_FPCLC -- the script's own documented override -- keeps both rules
// intact, and never overwrites a path the user exported themselves.
void export_fpcalc_to_scripts() {
    if (std::getenv("MOUSIKI_FPCLC")) return; // an explicit override always wins
#if defined(_WIN32)
    const char* name = "fpcalc.exe";
#else
    const char* name = "fpcalc";
#endif
    fs::path exe_dir;
#if defined(__APPLE__)
    char exe_buf[4096];
    uint32_t size = sizeof(exe_buf);
    if (_NSGetExecutablePath(exe_buf, &size) == 0) {
        std::error_code ec;
        fs::path resolved = fs::canonical(fs::path(exe_buf), ec);
        if (!ec) exe_dir = resolved.parent_path();
    }
#elif defined(_WIN32)
    std::string exe = win_executable_path();
    if (!exe.empty()) exe_dir = path_from_utf8(exe).parent_path();
#else
    char exe_buf[4096];
    ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) {
        exe_buf[n] = '\0';
        exe_dir = fs::path(exe_buf).parent_path();
    }
#endif
    if (exe_dir.empty()) return;
    // The same ladder find_scripts_file() walks for scripts/: beside the exe,
    // in its scripts/, then one and two levels up (a multi-config MSVC build
    // puts the exe in build\Release\).
    const fs::path dirs[] = {exe_dir, exe_dir / "scripts",
                             exe_dir.parent_path() / "scripts",
                             exe_dir.parent_path().parent_path() / "scripts"};
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        fs::path candidate = dir / name;
        if (!fs::exists(candidate, ec)) continue;
        std::string value = path_utf8(candidate);
#if defined(_WIN32)
        _putenv_s("MOUSIKI_FPCLC", value.c_str());
#else
        setenv("MOUSIKI_FPCLC", value.c_str(), 1);
#endif
        return;
    }
}

} // namespace

App::App() {
    settings_ = load_settings();
    // Point yt-dlp at the configured folder (if any) before anything can
    // ask where a download would go. Empty config = default cache folder,
    // which is what set_download_dir() restores on its own.
    cache_.set_download_dir(path_from_utf8(settings_.download_folder));
    history_.load(); // listening history: a missing/corrupt file is not fatal (see HistoryStore::load)
    set_emoji_replacement(settings_.replace_emoji);
    player_.set_stereo(settings_.stereo);
    player_.set_normalization(settings_.normalize,
                              static_cast<float>(settings_.normalize_target_lufs),
                              static_cast<float>(settings_.normalize_max_boost_db));
    lyrics_script_ = find_lyrics_script();
    fast_search_script_ = find_fast_search_script();
    meta_script_ = find_scripts_file("fetch_meta.py"); // AcoustID helper for Mode::MetaEdit
    export_fpcalc_to_scripts(); // ...and the fpcalc it runs (see why there)

    // BUGFIX: the cache-dir injection below used to push_back()
    // unconditionally, every single launch -- and since save_settings()
    // (called on every quit) writes settings_.local_music_paths back to
    // config.txt verbatim, *including* this appended entry, the cache
    // directory accumulated one more duplicate line in config.txt every
    // session. Harmless for correctness -- LocalSource::scan()'s
    // dedup-by-canonical-path already prevents the same file being
    // counted twice -- but wasteful: dozens of redundant directory
    // existence checks and walks on every startup, and a config.txt that
    // silently grows without bound over months of use. Deduping the whole
    // list first (in case manual edits introduced other repeats too),
    // order preserved so the saved file doesn't get needlessly reshuffled.
    {
        std::vector<std::string> deduped;
        deduped.reserve(settings_.local_music_paths.size());
        for (const auto& p : settings_.local_music_paths) {
            if (std::find(deduped.begin(), deduped.end(), p) == deduped.end()) deduped.push_back(p);
        }
        settings_.local_music_paths = std::move(deduped);
    }

    // Inject the cache directory into local music paths so streamed songs
    // automatically appear in the local view for seamless offline playback
    // -- only if it isn't already there (see the dedup note just above).
    std::string cache_dir_str = path_utf8(cache_.cache_dir());
    if (std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), cache_dir_str)
        == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(cache_dir_str);
    }
    // And the configured download folder, for the same reason: whatever
    // yt-dlp writes has to be scannable without a second LocalMusicPath
    // line, which is what the DOWNLOAD FOLDER setting promises. Skipped
    // while it is still the default -- that IS the cache dir just above.
    std::string download_dir_str = path_utf8(cache_.download_dir());
    if (download_dir_str != cache_dir_str
        && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), download_dir_str)
               == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(download_dir_str);
    }

    all_local_tracks_ = local_source_.scan(settings_.local_music_paths, &local_scan_diagnostics_);
    local_view_ = all_local_tracks_;
    launch_row_meta_resolver();
    start_device_worker();
}


int App::hotkey_string_to_key(const std::string& s) {
    if (s == "ARROW_KEY_UP") return 'A';
    if (s == "ARROW_KEY_DOWN") return 'B';
    if (s == "ARROW_KEY_RIGHT") return 'C';
    if (s == "ARROW_KEY_LEFT") return 'D';
    if (s == "ENTER") return '\n';
    if (s == "TAB") return 9;
    if (s == "SPACE") return ' ';
    if (s == "ESC") return 27;
    if (s == "BACKSPACE") return 127;
    if (s.size() == 1) return static_cast<int>(s[0]);
    return 0;
}

std::string App::resolve_hotkey_action(int key) const {
    for (const auto& [action, key_str] : settings_.hotkeys) {
        if (hotkey_string_to_key(key_str) == key) return action;
    }
    return "";
}

std::string App::hotkey_conflict(const std::string& key_str, const std::string& except_action) const {
    if (key_str.empty()) return "";
    for (const auto& [action, val] : settings_.hotkeys) {
        if (action == except_action) continue;
        if (val == key_str) return action;
    }
    return "";
}

// Pushes a message into both the one-line status area (existing
// behavior) and the persistent console log (both the in-memory buffer
// the Console overlay reads and, via ConsoleLog, the on-disk
// console.log) -- so events are still visible after they've scrolled
// off the status line, after a terminal-session switch redraw wiped
// the screen, or after the session itself has ended.
void App::log_event(const std::string& msg) {
    status_line_ = msg;
    ConsoleLog::instance().log_basic(msg);
}

// ---------------------------------------------------------------------
// Search / list state
// ---------------------------------------------------------------------

// Shared by refresh_local_view() (final, committed query) and the live
// incremental-search preview (whatever's currently typed, before Enter)
// so both behave identically -- what you see while typing is exactly
// what you'll get if you confirm it.
//
// An empty query returns the library sorted per local_sort_mode_ (see
// apply_local_sort()). A real query filters to fuzzy-matching tracks
// only and sorts PURELY by match quality, highest first -- this
// deliberately throws away the sort order entirely rather than using it
// as a tiebreak, so the best match always sits at the top regardless of
// where it happened to fall alphabetically/by-folder.
//
// Matches against filename-derived title, real artist tag, embedded
// title tag, and album tag -- using whatever's already been resolved
// for that row in row_meta_cache_, falling back to the cheap
// parent-folder guess for artist where the tag hasn't been probed yet.
// This is why matching against embedded metadata gets more complete the
// more of the library you've scrolled past / the longer the one-time
// background sweep (launch_row_meta_resolver) has had to run: each row's
// real tags only become searchable once resolved. A file whose tags
// haven't resolved yet is still findable by filename in the meantime.
// Title shown for `path` in the (search-)lists. Normally that is just the
// filename stem the row was built from; with "Show meta data only"
// (settings_.meta_only) on, the embedded title tag wins as soon as one has
// been resolved for that file, so the row shows metadata rather than a
// filename. Files with no title tag (or tags not probed yet) still fall
// back to the filename -- otherwise turning the toggle on would blank out
// an untagged library instead of just re-describing it.
std::string App::list_row_title(const fs::path& path, const std::string& filename_title) const {
    if (!settings_.meta_only) return filename_title;
    std::lock_guard<std::mutex> lk(row_meta_mutex_);
    auto it = row_meta_cache_.find(path_utf8(path));
    if (it != row_meta_cache_.end() && !it->second.title.empty()) return it->second.title;
    return filename_title;
}

std::string App::playlist_row_label(const fs::path& path, const std::string& filename_title) const {
    std::lock_guard<std::mutex> lk(row_meta_mutex_);
    auto it = row_meta_cache_.find(path_utf8(path));
    if (it != row_meta_cache_.end() && !it->second.title.empty() && it->second.title != filename_title) {
        return filename_title + "  \u2014 " + it->second.title; // em dash separator
    }
    return filename_title;
}

std::string App::marquee_or_truncate(const std::string& text, int width, int row_idx,
                                      int& tracked_idx, std::chrono::steady_clock::time_point& since) const {
    if (row_idx != tracked_idx) {
        tracked_idx = row_idx;
        since = std::chrono::steady_clock::now();
    }
    if (display_width(text) <= width) return pad_right(truncate_str(text, width), width);

    const double hold_secs = 1.2;    // pause on the title's start before scrolling
    const double cols_per_sec = 4.0; // scroll speed
    const std::string gap = "    ";  // seam between one loop and the next
    std::string loop_text = text + gap;
    int period = display_width(loop_text);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    int start_col = 0;
    if (elapsed > hold_secs && period > 0) {
        double scrolled = (elapsed - hold_secs) * cols_per_sec;
        start_col = static_cast<int>(scrolled) % period;
    }
    // Three repeats guarantee a full-width window is always available no
    // matter where start_col lands in the cycle.
    std::string doubled = loop_text + loop_text + loop_text;
    return pad_right(utf8_skip_take(doubled, start_col, width), width);
}

std::vector<LocalTrack> App::filter_and_rank_local(const std::string& query) const {
    if (query.empty()) {
        auto result = all_local_tracks_;
        apply_local_sort(result);
        return result;
    }

    std::vector<std::pair<double, const LocalTrack*>> scored;
    scored.reserve(all_local_tracks_.size());
    for (const auto& t : all_local_tracks_) {
        std::string artist = t.folder_artist;
        std::string tag_title, album;
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            auto it = row_meta_cache_.find(path_utf8(t.path));
            if (it != row_meta_cache_.end()) {
                if (!it->second.artist.empty()) artist = it->second.artist;
                tag_title = it->second.title;
                album = it->second.album;
            }
        }
        double score = fuzzy_score(query, t.title);
        score = std::max(score, fuzzy_score(query, artist));
        if (!tag_title.empty()) score = std::max(score, fuzzy_score(query, tag_title));
        if (!album.empty()) score = std::max(score, fuzzy_score(query, album));
        if (score > 0.0) scored.emplace_back(score, &t);
    }
    std::stable_sort(scored.begin(), scored.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<LocalTrack> result;
    result.reserve(scored.size());
    for (auto& [score, t] : scored) result.push_back(*t);
    return result;
}

// Mode 1 names what it sorts by, which follows Shift+N (see apply_local_sort()):
// the file name normally, the embedded title tag in metadata-only mode.
const char* App::sort_mode_name(int mode, bool meta_only) {
    switch (mode) {
        case 1: return meta_only ? "title A-Z" : "file name A-Z";
        case 2: return "artist A-Z";
        default: return "folder order";
    }
}

// Applied only when browsing with no active search query -- a fuzzy
// search's relevance ranking always wins over the manual sort mode.
// Duration isn't a sort option (yet): most rows only get a real duration
// once they've been lazily probed for display, so sorting by it up front
// would show mostly-unprobed rows in an arbitrary order until the
// background resolver catches up.
void App::apply_local_sort(std::vector<LocalTrack>& tracks) const {
    if (local_sort_mode_ == 1) {
        // Sort by the title the row actually SHOWS: the filename-derived one,
        // or -- with "Show metadata only" (Shift+N) on -- the embedded title
        // tag where one has been resolved (list_row_title() falls back to the
        // filename otherwise, exactly like the row does). Keys are built once
        // up front instead of taking row_meta_mutex_ on every comparison.
        std::vector<std::pair<std::string, size_t>> keyed;
        keyed.reserve(tracks.size());
        for (size_t i = 0; i < tracks.size(); ++i)
            keyed.emplace_back(lower(list_row_title(tracks[i].path, tracks[i].title)), i);
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<LocalTrack> sorted;
        sorted.reserve(tracks.size());
        for (const auto& k : keyed) sorted.push_back(std::move(tracks[k.second]));
        tracks = std::move(sorted);
    } else if (local_sort_mode_ == 2) {
        std::stable_sort(tracks.begin(), tracks.end(), [this](const LocalTrack& a, const LocalTrack& b) {
            auto artist_of = [this](const LocalTrack& t) {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                return (it != row_meta_cache_.end() && !it->second.artist.empty()) ? it->second.artist : t.folder_artist;
            };
            return lower(artist_of(a)) < lower(artist_of(b));
        });
    }
    // mode 0: leave as scanned (folder order) -- no-op
}

// Folder filter (HKeyFilterForFolder) stacks on top of the search/sort
// result rather than replacing it, so filtering-by-folder while a search is
// active narrows to just that folder's matches. Every place that rebuilds
// local_view_ must go through this (or refresh_local_view()): the background
// tag resolver re-filters the list every time new tags land, and a rebuild
// that skipped the folder filter silently undid 'f' a frame or two later.
std::vector<LocalTrack> App::filter_and_rank_local_view(const std::string& query) const {
    std::vector<LocalTrack> view = filter_and_rank_local(query);
    if (folder_filter_.empty()) return view;
    std::vector<LocalTrack> filtered;
    filtered.reserve(view.size());
    for (auto& t : view) {
        if (path_utf8(t.path.parent_path()) == folder_filter_) filtered.push_back(std::move(t));
    }
    return filtered;
}

void App::refresh_local_view() {
    local_view_ = filter_and_rank_local_view(last_local_query_);
    selected_ = 0;
    scroll_ = 0;
}

// Rebuilds the local list after something changed HOW rows are titled or
// ordered (the Shift+N metadata-only toggle), keeping the cursor on the same
// track instead of jumping to the top like refresh_local_view() does.
void App::resort_local_view_keep_selection() {
    if (list_source_ != ListSource::Local) return;
    fs::path prev;
    const bool had = selected_ >= 0 && selected_ < static_cast<int>(local_view_.size());
    if (had) prev = local_view_[static_cast<size_t>(selected_)].path;
    local_view_ = filter_and_rank_local_view(last_local_query_);
    selected_ = 0;
    if (had) {
        for (size_t i = 0; i < local_view_.size(); ++i)
            if (local_view_[i].path == prev) { selected_ = static_cast<int>(i); break; }
    }
    scroll_ = std::max(0, selected_ - list_nav_rows() / 2);
    if (scroll_ > selected_) scroll_ = selected_;
}

// Re-runs the local library scan over whatever settings_.local_music_paths
// holds right now, then rebuilds the view on top of the result. This is
// what makes a path edited in the ON/OFF tab take effect immediately --
// config.txt used to promise "the library is scanned once at startup,
// there's no live rescan", and editing a local path from Settings now IS
// a live rescan (it still needs [S]/quit for the change to be written to
// config.txt, of course).
void App::rescan_library() {
    // Keep the streamed-track cache folder in the list even if the edit
    // happened to drop it, for the same reason the constructor injects it
    // at all: downloaded songs belong in the local view.
    std::string cache_dir_str = path_utf8(cache_.cache_dir());
    if (std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), cache_dir_str)
        == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(cache_dir_str);
    }
    // And the configured download folder, for the same reason: whatever
    // yt-dlp writes has to be scannable without a second LocalMusicPath
    // line, which is what the DOWNLOAD FOLDER setting promises. Skipped
    // while it is still the default -- that IS the cache dir just above.
    std::string download_dir_str = path_utf8(cache_.download_dir());
    if (download_dir_str != cache_dir_str
        && std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), download_dir_str)
               == settings_.local_music_paths.end()) {
        settings_.local_music_paths.push_back(download_dir_str);
    }

    // A local diagnostics vector, not local_scan_diagnostics_ -- that one
    // is read back once at startup by run() (before any rescan can happen)
    // and never again, so per-scan results go straight to the log instead.
    std::vector<std::string> diagnostics;
    all_local_tracks_ = local_source_.scan(settings_.local_music_paths, &diagnostics);
    refresh_local_view(); // applies the active query/sort/filter to the new set
    for (const auto& line : diagnostics) ConsoleLog::instance().log_basic(line);

    // Newly added files need their tags resolved too -- for search and for
    // metadata-only rows. Re-arm the background sweep, which skips every
    // path already present in row_meta_cache_; if a previous sweep is
    // still winding down the two simply overlap, idempotently.
    row_meta_resolver_started_.store(false, std::memory_order_relaxed);
    launch_row_meta_resolver();
}

// Called on every keystroke while typing in the search box, before
// Enter is pressed — this is the "incremental search" behavior: the
// list updates live as you type instead of only after confirming. Local
// queries get filtered+ranked immediately via the same fuzzy logic
// submit_search() will commit on Enter. An "s:" (online search) prefix
// is left alone here — firing a network request on every keystroke
// would be wasteful and slow, so online search still only fires on
// Enter — but the view is reset back to whatever it was before '/' was
// pressed so a stale local preview doesn't linger behind the search box
// while an online query is being typed.
void App::update_live_search_preview() {
    std::string buf = search_buffer_;
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "s:") {
        list_source_ = pre_search_list_source_;
        local_view_ = filter_and_rank_local_view(pre_search_local_query_);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    // "p:" (playlist search) is local like a plain query -- no network
    // call to defer -- so unlike "s:" above, it's fine to actually run
    // the filter live on every keystroke rather than waiting for Enter.
    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "p:") {
        std::string q = buf.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        list_source_ = ListSource::Playlist;
        playlist_view_ = filter_playlists(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    // "f:" (folder search) is local too -- filter the folder list live.
    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "f:") {
        std::string q = buf.substr(2);
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(q);
        selected_ = 0;
        scroll_ = 0;
        return;
    }

    list_source_ = ListSource::Local;
    local_view_ = filter_and_rank_local_view(buf);
    selected_ = 0;
    scroll_ = 0;
}

void App::submit_search() {
    std::string buf = search_buffer_;
    // trim
    while (!buf.empty() && buf.front() == ' ') buf.erase(buf.begin());
    while (!buf.empty() && buf.back() == ' ') buf.pop_back();

    if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "s:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_online_query_ = query;
        list_source_ = ListSource::Online;
        if (search_in_progress_.load()) {
            status_line_ = "still searching, hang on ...";
            return;
        }
        launch_search_async(query.empty() ? "music" : query);
    } else if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "p:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_playlist_query_ = query;
        list_source_ = ListSource::Playlist;
        playlist_view_ = filter_playlists(query);
        selected_ = 0;
        scroll_ = 0;
    } else if (buf.size() >= 2 && lower(buf.substr(0, 2)) == "f:") {
        std::string query = buf.substr(2);
        while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        last_folder_query_ = query;
        list_source_ = ListSource::Folder;
        folder_view_ = filter_folders(query);
        selected_ = 0;
        scroll_ = 0;
    } else {
        last_local_query_ = buf;
        list_source_ = ListSource::Local;
        refresh_local_view();
    }
}

// "/f:" folder search: every folder that directly holds at least one scanned
// track, filtered by the query. Each whitespace-separated word of the query
// must appear (case-insensitively) in "<parent folder> <folder name>", so
// "beatles abbey" finds Music/The Beatles/Abbey Road. An empty query lists
// every folder. Sorted by folder name.
std::vector<FolderSummary> App::filter_folders(const std::string& query) const {
    std::map<std::string, FolderSummary> by_path;
    for (const auto& t : all_local_tracks_) {
        fs::path dir = t.path.parent_path();
        std::string key = path_utf8(dir);
        auto it = by_path.find(key);
        if (it == by_path.end()) {
            FolderSummary f;
            f.path = key;
            f.name = path_utf8(dir.filename());
            if (f.name.empty()) f.name = key; // drive root etc.
            f.parent = path_utf8(dir.parent_path().filename());
            f.track_count = 1;
            by_path.emplace(key, std::move(f));
        } else {
            ++it->second.track_count;
        }
    }

    std::vector<std::string> words;
    {
        std::string cur;
        for (char c : query) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur += c;
        }
        if (!cur.empty()) words.push_back(cur);
    }

    std::vector<FolderSummary> out;
    out.reserve(by_path.size());
    for (auto& [key, f] : by_path) {
        const std::string hay = f.parent + " " + f.name;
        bool ok = true;
        for (const auto& w : words) if (!contains_ci(hay, w)) { ok = false; break; }
        if (ok) out.push_back(f);
    }
    std::stable_sort(out.begin(), out.end(), [](const FolderSummary& a, const FolderSummary& b) {
        return lower(a.name) < lower(b.name);
    });
    return out;
}

// Enter on a row of the "/f:" list: list all files of that folder in the
// LOCAL AUDIO FILES pane. Same mechanism as the 'f' filter (folder_filter_),
// so [c] clears it again and the pane title shows the folder name. Any
// active local query is dropped so really every file of the folder shows up.
void App::open_selected_folder() {
    if (folder_view_.empty() || selected_ < 0 || selected_ >= static_cast<int>(folder_view_.size())) return;
    folder_filter_ = folder_view_[static_cast<size_t>(selected_)].path;
    last_local_query_.clear();
    local_sort_mode_ = 0; // folder order, like 'f'
    list_source_ = ListSource::Local;
    refresh_local_view();
    log_event("filtered: " + path_utf8(path_from_utf8(folder_filter_).filename()));
}

// Local, case-insensitive substring match on playlist name -- cheap
// enough (just a directory scan) to re-run on every keystroke, same as
// the "p:" live preview above does.
std::vector<PlaylistSummary> App::filter_playlists(const std::string& query) const {
    auto all = playlist_summaries();
    if (query.empty()) return all;
    std::vector<PlaylistSummary> out;
    out.reserve(all.size());
    for (auto& p : all) if (contains_ci(p.name, query)) out.push_back(p);
    return out;
}

// The folder NEW playlists are written to and deleted from: the first
// configured PlaylistsPath (settings_.playlists_paths[0] -- config.txt's
// PlaylistsPath= line, editable from the ON/OFF tab's PLAYLIST PATH
// list), otherwise settings_.local_music_paths[0]/playlists (~/Music if
// none configured at all, which by the time this runs may itself have
// become the cache folder -- see load_library()'s cache-dir injection).
// Unrelated to HKeyDownloadStream's folder, which comes from the
// separate DOWNLOAD FOLDER setting (cache_.download_dir()) instead.
// Computed fresh every call, not cached, so it always reflects whatever
// the user currently has set in Settings.
fs::path App::playlists_dir() const {
    if (!settings_.playlists_paths.empty() && !settings_.playlists_paths[0].empty())
        return path_from_utf8(settings_.playlists_paths[0]);
    std::string base;
    if (!settings_.local_music_paths.empty()) {
        base = settings_.local_music_paths[0];
    } else {
        const char* home = std::getenv("HOME");
        base = home ? (std::string(home) + "/Music") : "./Music";
    }
    return path_from_utf8(base) / "playlists";
}

// Every folder playlists are searched in: all of the configured ones, or
// the single folder playlists_dir() resolves to when none is (so every
// caller always has at least one directory to look in). Blanks are
// skipped -- an empty entry is what a "+ new path" line looks like until
// the user has finished typing it -- and duplicates collapse, since
// scanning the same folder twice would just re-yield its playlists.
std::vector<fs::path> App::playlist_dirs() const {
    std::vector<fs::path> out;
    for (const auto& p : settings_.playlists_paths) {
        if (p.empty()) continue;
        fs::path dir = path_from_utf8(p);
        if (std::find(out.begin(), out.end(), dir) == out.end()) out.push_back(dir);
    }
    if (out.empty()) out.push_back(playlists_dir());
    return out;
}

// Playlists across every folder in playlist_dirs(), merged into one
// list: what both the main "/p:" list and the playlist editor's manage
// tab show. De-duplicated by case-insensitive name with the first folder
// containing it winning (so a row always resolves to one concrete file,
// wherever it gets opened from), then re-sorted with exactly the
// case-insensitive order PlaylistManager::list() already guarantees
// within a single folder -- so several configured folders read as one
// alphabetized list rather than as concatenated per-folder blocks.
std::vector<PlaylistSummary> App::playlist_summaries() const {
    std::vector<PlaylistSummary> merged;
    std::unordered_set<std::string> seen;
    for (const fs::path& dir : playlist_dirs()) {
        for (PlaylistSummary& s : PlaylistManager::list(dir)) {
            if (seen.insert(lower(s.name)).second) merged.push_back(std::move(s));
        }
    }
    std::sort(merged.begin(), merged.end(),
              [](const PlaylistSummary& a, const PlaylistSummary& b) { return lower(a.name) < lower(b.name); });
    return merged;
}

// Loads a playlist by name from whichever playlist_dirs() folder has it
// -- first match wins, the same rule playlist_summaries() de-duplicated
// with, so what the list showed always loads. nullopt when no folder
// holds that name (anymore).
std::optional<Playlist> App::load_playlist(const std::string& name) const {
    for (const fs::path& dir : playlist_dirs()) {
        if (auto pl = PlaylistManager::load(dir, name)) return pl;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------
// Playback start
// ---------------------------------------------------------------------

void App::write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                 double t_probe, double t_total, const std::string& error) {
    const char* home = std::getenv("HOME");
    if (!home) return;
    fs::path dir = path_from_utf8(home) / ".cache" / "mousiki";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream log(dir / "load_timing.log", std::ios::app);
    if (!log.is_open()) return;

    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

    log << timebuf << " track=\"" << title << "\" source=" << (is_local ? "local" : "online");
    if (!is_local) log << " resolve=" << t_resolve << "s";
    // This is now "time to first sound", not "time to fully decoded" —
    // decode itself streams in after this point, off the critical path.
    log << " probe=" << t_probe << "s time_to_playback=" << t_total << "s";
    if (!error.empty()) log << " ERROR=\"" << error << "\"";
    log << "\n";
}

void App::launch_load_async(fs::path local_path, std::string title, std::string artist,
                             std::string location_label, bool is_local, std::string video_id) {
    if (load_thread_.joinable()) load_thread_.join(); // previous job already signaled done, safe to reap
    load_in_progress_ = true;
    load_ready_ = false;
    load_stage_ = is_local ? 4 : 1;
    load_started_at_ = std::chrono::steady_clock::now();
    if (!is_local) status_line_ = "resolving \"" + title + "\" ...";

    // This thread ONLY resolves (online) and probes metadata/duration —
    // both fast, no full decode. It publishes a result and returns. Full
    // decode is a SEPARATE, detached thread spawned at the bottom, so
    // this thread (the one the main loop's next launch_load_async call
    // will join()) is never blocked waiting on decode — that's what
    // makes it safe to join from launch_load_async without risking a
    // freeze if the user switches tracks again quickly.
    const bool want_stereo = settings_.stereo; // read here, on the calling thread, rather than from inside the load thread
    load_thread_ = std::thread([this, local_path, title, artist, location_label, is_local, video_id, want_stereo]() {
      run_guarded("track load", [&] {
        using clock = std::chrono::steady_clock;
        auto t_start = clock::now();
        auto elapsed_s = [](clock::time_point from) {
            return std::chrono::duration<double>(clock::now() - from).count();
        };

        PendingLoad pl;
        pl.title = title;
        pl.artist = artist;
        pl.location_label = location_label;
        pl.is_local = is_local;
        pl.video_id = video_id;

        double t_resolve = 0.0, t_probe = 0.0;

        fs::path path = local_path;
        if (!is_local) {
            load_stage_ = 1;
            auto t0 = clock::now();
            std::string err;
            auto resolved = youtube_.resolve_by_id(video_id, title, artist, &err);
            t_resolve = elapsed_s(t0);
            if (!resolved) {
                pl.error = "download failed: " + err;
                write_load_timing_log(title, is_local, t_resolve, 0, elapsed_s(t_start), pl.error);
                std::lock_guard<std::mutex> lk(load_mutex_);
                pending_load_ = std::move(pl);
                load_ready_ = true;
                return;
            }
            path = resolved->cached_path;
            pl.title = resolved->title;
            pl.artist = resolved->artist;
        }
        pl.path = path;

        load_stage_ = 4;
        auto t2 = clock::now();
        pl.metadata = probe_metadata(path, pl.title, pl.artist, pl.location_label);
        
        // Ensure lyrics fetch uses the real metadata tags instead of the filename/folder
        if (is_local) {
            if (!pl.metadata.name.empty() && pl.metadata.name != "-") {
                pl.title = pl.metadata.name;
            }
            if (!pl.metadata.artist.empty() && pl.metadata.artist != "-") {
                pl.artist = pl.metadata.artist;
            }
        }
        
        double duration = probe_duration_seconds(path);
        t_probe = elapsed_s(t2);
        pl.total_sec = duration > 0 ? static_cast<size_t>(duration) : 0;

        pl.pcm = std::make_shared<StreamingPcm>();
        pl.pcm->reserve_for_seconds(duration > 0 ? duration : 300.0, 44100, want_stereo ? 2 : 1);
        pl.success = true;

        write_load_timing_log(pl.title, is_local, t_resolve, t_probe, elapsed_s(t_start), "");

        {
            std::lock_guard<std::mutex> lk(load_mutex_);
            pending_load_ = pl;
            load_ready_ = true;
        }

        // Decode continues independently from here — detached because it
        // may still be running when the user switches to a different
        // track, and the StreamingPcm it's filling stays alive via the
        // shared_ptr captured below (and via Player's own reference, if
        // this track is still the one playing) for exactly as long as it
        // needs to. Known tradeoff: if the app quits while a decode is
        // still in flight, that ffmpeg subprocess can be orphaned rather
        // than cleanly killed — worth fixing with real process-group
        // tracking later, not a correctness or crash risk today.
        std::shared_ptr<StreamingPcm> pcm = pl.pcm;
        fs::path decode_path = path;
        std::string wtitle = pl.title, wartist = pl.artist;
        bool waveform_smooth = settings_.waveform_smooth; // captured by value — see below, avoids a cross-thread read of settings_
        std::thread([this, decode_path, pcm, waveform_smooth]() { run_guarded("track decode", [&] {
            stream_decode_ffmpeg(decode_path, *pcm);

            // Deferred mini-waveform pass — only starts once decode is
            // fully done, never gates playback.
            if (!pcm->decode_failed.load()) {
                // PERF: decode is done — pcm->data is no longer being
                // written to, so we pass it directly as a const-ref
                // instead of making a full snapshot copy.  A 5-minute
                // track at 44100 Hz is ~50 MB; that copy was the single
                // biggest reason the waveform appeared so late after
                // playback started, because it doubled the working-set
                // size and stalled the RMS pass behind a large memcpy.
                auto envelope = WaveformQuantizer::generate_high_res_envelope(pcm->data, 4096, waveform_smooth, pcm->channels);
                std::lock_guard<std::mutex> lk(waveform_mutex_);
                pending_waveform_envelope_ = std::move(envelope);
                waveform_pending_ready_ = true;
            }
        }); }).detach();
      });

      // If the guard above swallowed an exception, nothing published a
      // result -- and poll_pending_load() is the only thing that clears
      // load_in_progress_, so the app would refuse to start any track for
      // the rest of the session ("still loading the previous track ...").
      // Publish a failed load instead so the UI recovers and says why.
      if (!load_ready_.load()) {
          std::lock_guard<std::mutex> lk(load_mutex_);
          PendingLoad failed;
          failed.title = title;
          failed.is_local = is_local;
          failed.success = false;
          failed.error = "couldn't load \"" + title + "\" -- see the console log (t)";
          pending_load_ = std::move(failed);
          load_ready_ = true;
      }
    });
}

// Used to block here waiting for the previous track's fetch thread to
// finish (join()) before starting a new one. That's a real main-thread
// stall: whenever lyrics aren't quickly available (still mid network-call
// chain) and the user skips to another track before it resolves,
// switching songs would hang until the abandoned fetch finished.
// Detaching instead, with an epoch guard so a late-arriving stale result
// just gets discarded rather than clobbering the new/retried track's
// lyrics. Shared by the initial per-track fetch (poll_pending_load) and
// the manual retry hotkey (handle_key's 'l' case).
void App::launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network) {
    lyrics_ready_ = false;
    int my_epoch = ++lyrics_epoch_;
    std::thread([this, title, artist, path, force_network, my_epoch]() { run_guarded("lyrics fetch", [&] {
        LyricsResult r = fetch_synced_lyrics(title, artist, path_utf8(lyrics_script_), path, force_network);
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (my_epoch != lyrics_epoch_.load()) return; // a newer/retried fetch has since started — discard
        lyrics_result_ = std::move(r);
        lyrics_ready_ = true;
    }); }).detach();
}

void App::poll_pending_load() {
    if (!load_ready_.load()) return;
    PendingLoad pl;
    {
        std::lock_guard<std::mutex> lk(load_mutex_);
        // BUG FIX #4: clear the flag while still holding load_mutex_ so
        // the load thread cannot race-write pending_load_ again in the
        // window between the copy and the flag reset.
        if (!load_ready_.load()) return; // re-check under lock (spurious wakeup guard)
        pl = pending_load_;
        load_ready_ = false;
    }
    load_in_progress_ = false;
    load_stage_ = 0;
    const bool was_advancing = advancing_;
    advancing_ = false;

    if (!pl.success) {
        status_line_ = pl.error;
        // An automatic advance whose next track failed to load: the previous
        // track is over, so stop treating it as current (otherwise its
        // finished flag would immediately trigger another advance).
        if (was_advancing) {
            has_track_ = false;
            // ...and the play it left behind is over too -- nothing took its
            // place, so close the record now (it was heard to the end; the
            // next track simply never arrived). A FAILED manual selection is
            // deliberately not recorded: whatever was playing keeps playing
            // and its play must stay live.
            history_end_current_play();
        }
        return;
    }

    // Player::play() stops whatever it was previously playing as its own
    // first step, so no separate explicit stop() call is needed here —
    // and doing it inside play() (below, off the main thread) is what
    // lets this whole switch never touch the main thread.
    current_pcm_ = pl.pcm;
    total_sec_ = pl.total_sec;
    metadata_ = pl.metadata;
    current_path_ = pl.path;
    current_is_local_ = pl.is_local;
    current_video_id_ = pl.video_id;
    has_track_ = true;
    // Listening history, in this exact order: the play that just ended is
    // closed BEFORE clear_finished() (its "was it heard to the end?" verdict
    // reads player_.finished(), which the next line resets) and the new one
    // is opened AFTER current_* have been reassigned (they are what
    // history_begin_current_play() records). No history record exists for a
    // failed load -- see the branch above.
    history_end_current_play();
    player_.clear_finished(); // see clear_finished()'s comment — closes the race that caused the double-skip bug
    history_begin_current_play();
    waveform_envelope_.clear();
    waveform_ready_ = false;
    waveform_pending_ready_ = false;
    ++waveform_epoch_; // BUG FIX #5: invalidate any in-flight waveform from the previous track
    last_lyrics_status_.clear();
    fft_.reset(); // don't let the previous track's spectrum tail linger into this one's first frame

    // "Lyrics Engine" (settings_.element_lyrics) used to only hide the
    // panel -- fetch_synced_lyrics() still ran, still spawned Python, and
    // still hit the network for every single track, whether or not
    // anything was ever shown. Toggling it off now actually turns the
    // feature off, matching what the settings label already claimed.
    if (settings_.element_lyrics) {
        launch_lyrics_fetch(pl.title, pl.artist, pl.path);
    }

    // This is the whole point of the redesign: play() is handed a
    // StreamingPcm that may have zero frames decoded yet. The audio
    // callback plays silence for anything past what's been decoded and
    // self-corrects the instant more arrives — so sound starts the
    // moment decode produces its first chunk, not after the whole track.
    // Dispatched off the main thread — see launch_device_play_async().
    launch_device_play_async();
    status_line_.clear();
}

void App::launch_device_play_async() {
    int my_gen = ++device_gen_;
    // Raise the handoff guard BEFORE the request is visible to the worker:
    // from here until player_.play() returns, finished_ still describes the
    // old track and must not drive advance_track() (see device_play_pending_gen_).
    device_play_pending_gen_.store(my_gen);
    auto pcm = current_pcm_;
    int vol = player_.volume() > 0 ? player_.volume() : 70;
    // One-shot resume position from a restored snapshot -- consumed
    // here exactly once, then zeroed so every subsequent track change
    // (skip, search-and-play, queue advance, ...) starts at 0 like
    // always. Reading+clearing it up front (still on the main thread,
    // before it's handed to the worker) avoids any race with a second
    // restore attempt -- there isn't one, but this keeps that invariant
    // obvious rather than implicit.
    double start_sec = resume_start_sec_;
    resume_start_sec_ = 0.0;

    // Post to device_worker_loop() rather than spawning a thread here.
    // There's only one request slot, not a queue: if the worker is still
    // busy with an older request when a newer one lands, this simply
    // overwrites it in place before the worker ever reads it, so only the
    // latest survives -- the same "a superseded switch is silently
    // dropped" behaviour the old generation-counter design gave, just
    // enforced by construction instead of by a check the stale thread had
    // to remember to perform.
    {
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        device_request_.pcm = std::move(pcm);
        device_request_.volume = vol;
        device_request_.start_sec = start_sec;
        device_request_.generation = my_gen;
        device_request_ready_ = true;
    }
    device_request_cv_.notify_one();
}

void App::start_device_worker() {
    device_worker_thread_ = std::thread(&App::device_worker_loop, this);
}

void App::stop_device_worker() {
    {
        std::lock_guard<std::mutex> lk(device_request_mutex_);
        device_worker_stop_ = true;
    }
    device_request_cv_.notify_one();
}

void App::device_worker_loop() {
    for (;;) {
        DevicePlayRequest req;
        {
            std::unique_lock<std::mutex> lk(device_request_mutex_);
            device_request_cv_.wait(lk, [this] { return device_request_ready_ || device_worker_stop_; });
            if (device_worker_stop_) return;   // quitting -- don't start one more track on the way out
            req = device_request_;
            device_request_ready_ = false;
        }
        // Defensive only: with a single overwritable slot rather than a
        // real queue, req.generation should already equal device_gen_ by
        // construction every time this fires.
        if (req.generation != device_gen_.load()) continue;
        // Guarded like every other worker: this loop lives for the whole
        // session, and an exception escaping it would take the process with
        // it rather than just failing one track.
        run_guarded("audio device start", [&] {
            std::lock_guard<std::mutex> lk(player_mutex_);
            player_.play(req.pcm, req.start_sec, req.volume, &fft_);
        });
        // The new track is in and the old pcm (and its latching finished_
        // flag) is gone, so the main loop may look at finished_ again. Only
        // clear if no newer handoff has been posted meanwhile -- that one is
        // still in flight and owns the guard now.
        int expected = req.generation;
        device_play_pending_gen_.compare_exchange_strong(expected, 0);
    }
}

void App::poll_pending_waveform() {
    if (!waveform_pending_ready_.load()) return;
    std::vector<float> envelope;
    {
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        envelope = std::move(pending_waveform_envelope_);
    }
    waveform_pending_ready_ = false;
    waveform_envelope_ = std::move(envelope);
    waveform_ready_ = true;
    waveform_reveal_start_ = std::chrono::steady_clock::now(); // starts the 700ms left-to-right reveal
}

void App::launch_search_async(const std::string& query) {
    if (search_thread_.joinable()) search_thread_.join();
    search_in_progress_ = true;
    search_ready_ = false;
    status_line_ = "searching online for \"" + query + "\" ...";

    search_thread_ = std::thread([this, query]() { run_guarded("online search", [&] {
        auto results = online_.search(query, /*count=*/15, path_utf8(fast_search_script_));
        std::lock_guard<std::mutex> lk(search_mutex_);
        pending_search_results_ = std::move(results);
    }); 
        // Set outside the guard: poll_pending_search() waits on this flag,
        // so it has to be raised even when the search threw, or the UI sits
        // on "searching ..." forever.
        search_ready_ = true;
    });
}

void App::poll_pending_search() {
    if (!search_ready_.load()) return;
    std::vector<OnlineResult> results;
    {
        std::lock_guard<std::mutex> lk(search_mutex_);
        results = std::move(pending_search_results_);
    }
    search_ready_ = false;
    search_in_progress_ = false;

    online_view_ = std::move(results);
    selected_ = 0;
    scroll_ = 0;
    status_line_ = online_view_.empty() ? "no online results" : "";
}

void App::play_selected() {
    // Playlists aren't "played" directly -- there's no single track to
    // start. Enter on a playlist row queues everything in it instead
    // (see playlist_add_selected_to_queue()), same as the user pressing
    // "a" on it would.
    if (list_source_ == ListSource::Playlist) { playlist_add_selected_to_queue(); return; }
    // Folders aren't played either -- Enter opens the folder's files.
    if (list_source_ == ListSource::Folder) { open_selected_folder(); return; }
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) start_local_track(local_view_[selected_]);
    else start_online_track(online_view_[selected_]);
}

int App::current_track_list_index() const {
    if (!has_track_) return -1;
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return -1; // no "now playing" identity in a list of playlist/folder names
    if (list_source_ == ListSource::Local) {
        if (!current_is_local_) return -1; // playing an online track while browsing the local list
        for (size_t i = 0; i < local_view_.size(); ++i) {
            if (local_view_[i].path == current_path_) return static_cast<int>(i);
        }
        return -1;
    } else {
        if (current_is_local_) return -1; // playing a local track while browsing online results
        for (size_t i = 0; i < online_view_.size(); ++i) {
            if (online_view_[i].video_id == current_video_id_) return static_cast<int>(i);
        }
        return -1;
    }
}

void App::play_relative(int delta) {
    // "next/previous track" has no meaning while browsing a list of
    // playlist names rather than tracks.
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return;
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0) return;
    // Relative to what's actually *playing*, not wherever the hover
    // cursor happens to be sitting -- falls back to the hover cursor
    // only when there's no sensible "current" position in this list
    // (nothing playing yet, or what's playing is from a different
    // source/isn't in this view at all).
    int base = current_track_list_index();
    if (base < 0) base = selected_;
    selected_ = std::clamp(base + delta, 0, static_cast<int>(list_len) - 1);
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::play_relative_random() {
    if (list_source_ == ListSource::Playlist || list_source_ == ListSource::Folder) return;
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0) return;
    if (list_len == 1) { selected_ = 0; play_selected(); return; }
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(list_len) - 1);
    int base = current_track_list_index();
    if (base < 0) base = selected_;
    int next;
    do { next = dist(rng); } while (next == base);
    selected_ = next;
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::play_next_from_queue() {
    // Shuffle: pick a random queue item instead of strictly FIFO order.
    // Repeat Queue: rotate the played item to the back instead of
    // discarding it, so the whole queue loops indefinitely rather than
    // draining to empty. Both apply here (not just to library playback)
    // -- this is exactly the "queue mode won't respect shuffle or
    // repeat" bug: previously advance_track()'s queue branch always did
    // plain FIFO regardless of play_mode.
    int idx = 0;
    if (settings_.play_mode == 2 /*shuffle*/ && queue_.size() > 1) {
        static std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(0, static_cast<int>(queue_.size()) - 1);
        idx = dist(rng);
    }
    QueueItem item = queue_[idx];
    queue_.erase(queue_.begin() + idx);
    if (settings_.play_mode == 4 /*repeat queue*/) {
        queue_.push_back(item); // rotate to the back instead of discarding -- keeps the queue looping
    }
    if (queue_selected_ >= idx && queue_selected_ > 0) --queue_selected_; // index shifted down by the erase
    clamp_queue_selected();
    if (item.is_local) {
        LocalTrack t{path_utf8(item.local_path.stem()), item.local_path, item.artist};
        start_local_track(t);
    } else {
        OnlineResult r{item.video_id, item.title, item.artist};
        start_online_track(r);
    }
}

void App::advance_track() {
    // has_track_ is deliberately NOT cleared up front. Clearing it here made
    // the metadata panel show "no track loaded" for the gap between one track
    // ending and the next one's load finishing (and made play_relative()
    // measure "next" from the hover cursor instead of from what was actually
    // playing). It's only cleared where playback really ends: Stop mode, or
    // when nothing could be started / the load failed (see below and
    // poll_pending_load()).

    // Repeat: keep replaying whatever just finished -- whether it came
    // from the queue or the library -- without touching the queue or
    // advancing through any list at all. Stop: don't auto-advance into
    // anything, queue or not. Both apply uniformly regardless of the
    // queue's state now -- previously these were only ever consulted
    // once the queue was already empty, which was the other half of the
    // "queue mode won't respect repeat" bug.
    if (settings_.play_mode == 1 /*loop*/) {
        launch_device_play_async(); // same track, already fully decoded, no reload needed
        has_track_ = true;
        // One play of it just ended and looping it starts another one -- the
        // record is closed (verdict still readable: clear_finished() is
        // right below) and a fresh one opened, which is what makes a looped
        // track count as a replay instead of one endless play.
        history_end_current_play();
        player_.clear_finished();
        history_begin_current_play();
        return;
    }
    if (settings_.play_mode == 3 /*stop*/) {
        has_track_ = false; // the only mode where "no track loaded" is the right message
        history_end_current_play(); // playback really ends here: nothing follows it
        return; // no auto-advance -- queue or not
    }

    // The queue always takes priority over the library — it's an
    // explicit user-built-up-next list.
    advancing_ = true; // suppress re-entry until poll_pending_load() reports back
    if (!queue_.empty()) {
        play_next_from_queue();
    } else {
        switch (settings_.play_mode) {
            case 2: // shuffle
                play_relative_random();
                break;
            default: // list (sequential) -- also where Repeat Queue (4) lands
                     // once the queue's actually empty; there's nothing left
                     // to "repeat queue" without one, so it just falls back
                     // to normal sequential playback.
                play_relative(1);
                break;
        }
    }
    // Nothing got started (empty list, ...): playback has genuinely ended.
    if (!load_in_progress_.load()) {
        advancing_ = false;
        has_track_ = false;
        history_end_current_play(); // the finished track has no successor: close its record
    }
}

char App::play_mode_letter() const {
    switch (settings_.play_mode) {
        case 1: return 'R';  // repeat (loop current track)
        case 2: return 'S';  // shuffle
        case 3: return 'O';  // stop (play, then stop -- not "S", shuffle already owns that)
        case 4: return 'Q';  // repeat queue
        default: return 'L'; // list (normal sequential)
    }
}

void App::queue_add_selected() {
    if (list_source_ == ListSource::Playlist) { playlist_add_selected_to_queue(); return; }
    if (list_source_ == ListSource::Folder) { status_line_ = "Enter opens the folder -- then add its tracks"; return; }
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) {
        const auto& t = local_view_[selected_];
        queue_.push_back({true, t.title, t.folder_artist, t.path, ""});
    } else {
        const auto& r = online_view_[selected_];
        queue_.push_back({false, r.title, r.uploader, {}, r.video_id});
    }
    clamp_queue_selected();
}

// Main UI: Enter (or "a") on a playlist row while list_source_==Playlist.
// Loads the playlist from disk and queues every track that's still
// present on disk, skipping (and reporting) any that aren't.
void App::playlist_add_selected_to_queue() {
    if (playlist_view_.empty() || selected_ < 0 || selected_ >= static_cast<int>(playlist_view_.size())) return;
    const auto& summary = playlist_view_[selected_];
    auto pl = load_playlist(summary.name);
    if (!pl) { status_line_ = "could not load \"" + summary.name + "\""; return; }

    int added = 0, skipped = 0;
    for (auto& t : pl->tracks) {
        if (t.missing) { ++skipped; continue; }
        queue_.push_back({true, t.title, t.artist, t.path, ""});
        ++added;
    }
    clamp_queue_selected();
    status_line_ = "queued " + std::to_string(added) + " track" + (added == 1 ? "" : "s")
                 + " from \"" + summary.name + "\""
                 + (skipped > 0 ? " (" + std::to_string(skipped) + " missing, skipped)" : "");
}

void App::queue_remove_last() {
    if (!queue_.empty()) queue_.pop_back();
    clamp_queue_selected();
}

void App::clamp_queue_selected() {
    if (queue_.empty()) { queue_selected_ = 0; queue_scroll_ = 0; return; }
    queue_selected_ = std::clamp(queue_selected_, 0, static_cast<int>(queue_.size()) - 1);
    if (queue_selected_ >= queue_scroll_ + list_visible_rows_) queue_scroll_ = queue_selected_ - list_visible_rows_ + 1;
    if (queue_selected_ < queue_scroll_) queue_scroll_ = queue_selected_;
}

void App::queue_remove_hovering() {
    if (queue_.empty() || queue_selected_ < 0 || queue_selected_ >= static_cast<int>(queue_.size())) return;
    queue_.erase(queue_.begin() + queue_selected_);
    clamp_queue_selected();
}

void App::queue_clear() {
    const size_t n = queue_.size();
    queue_.clear();
    clamp_queue_selected(); // empty queue -> cursor and scroll back to 0
    log_event("queue cleared (" + std::to_string(n) + " track" + (n == 1 ? "" : "s") + " removed)");
}

void App::queue_move_hovering(int dir) {
    if (queue_.empty()) return;
    int target = queue_selected_ + dir;
    if (target < 0 || target >= static_cast<int>(queue_.size())) return; // already at an edge
    std::swap(queue_[queue_selected_], queue_[target]);
    queue_selected_ = target;
    clamp_queue_selected();
}

// ---------------------------------------------------------------------
// Autosave / session snapshot
// ---------------------------------------------------------------------

SnapshotData App::build_snapshot() const {
    SnapshotData snap;
    snap.play_mode = settings_.play_mode;
    snap.muted = muted_;
    // Save the *real* volume, not the forced-0 muted value, so unmuting
    // next session restores to what it actually was, not silence.
    snap.volume = muted_ ? pre_mute_volume_ : player_.volume();

    if (has_track_) {
        snap.has_now_playing = true;
        snap.now_playing.is_local = current_is_local_;
        snap.now_playing.path = current_is_local_ ? path_utf8(current_path_) : std::string();
        snap.now_playing.video_id = current_is_local_ ? std::string() : current_video_id_;
        snap.now_playing.title = metadata_.name;
        snap.now_playing.artist = metadata_.artist;
        snap.position_sec = player_.poll_elapsed();
    }

    for (const auto& item : queue_) {
        SnapshotTrack t;
        t.is_local = item.is_local;
        t.path = item.is_local ? path_utf8(item.local_path) : std::string();
        t.video_id = item.is_local ? std::string() : item.video_id;
        t.title = item.title;
        t.artist = item.artist;
        snap.queue.push_back(std::move(t));
    }
    return snap;
}

void App::restore_snapshot(const SnapshotData& snap) {
    settings_.play_mode = std::clamp(snap.play_mode, 0, 4);
    // Apply the saved volume first, then re-apply mute on top of it --
    // mirrors what pressing 'x' does at runtime (force 0, remember the
    // real value), just seeded from the snapshot instead of live state.
    pre_mute_volume_ = std::clamp(snap.volume, 0, 100);
    player_.set_volume(pre_mute_volume_);
    if (snap.muted) {
        player_.set_volume(0);
        muted_ = true;
    }

    queue_.clear();
    for (const auto& t : snap.queue) {
        queue_.push_back({t.is_local, t.title, t.artist, t.is_local ? path_from_utf8(t.path) : fs::path(), t.video_id});
    }
    clamp_queue_selected();

    if (snap.has_now_playing) {
        resume_start_sec_ = std::max(0.0, snap.position_sec);
        if (snap.now_playing.is_local) {
            fs::path p = path_from_utf8(snap.now_playing.path);
            std::error_code ec;
            if (fs::exists(p, ec)) {
                LocalTrack t{path_utf8(p.stem()), p, snap.now_playing.artist};
                start_local_track(t);
                log_event("resuming: " + t.title);
            } else {
                resume_start_sec_ = 0.0; // file's gone -- nothing to resume into
            }
        } else if (!snap.now_playing.video_id.empty()) {
            OnlineResult r{snap.now_playing.video_id, snap.now_playing.title, snap.now_playing.artist};
            start_online_track(r);
        } else {
            resume_start_sec_ = 0.0;
        }
    }
}

void App::maybe_autosave() {
    if (!settings_.autosave_enabled) return;
    double delay = std::max(1, settings_.autosave_delay_sec);
    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - last_autosave_at_).count();
    if (elapsed < delay) return;

    last_autosave_at_ = std::chrono::steady_clock::now();
    save_snapshot(build_snapshot());
    autosave_pulse_active_ = true;
    autosave_pulse_started_at_ = last_autosave_at_;
    ConsoleLog::instance().log_basic("autosaved session snapshot");
}

std::string App::autosave_indicator_glyph() const {
    if (!settings_.autosave_enabled || !settings_.autosave_indicator) return "";

    double t = autosave_pulse_active_
             ? std::chrono::duration<double>(std::chrono::steady_clock::now() - autosave_pulse_started_at_).count()
             : kAutosavePulseSeconds + 1.0; // idle -- well past the pulse window

    std::string glyph = settings_.autosave_chr.empty() ? "\u2022" : settings_.autosave_chr;

    if (settings_.autosave_indicator_type == 0) {
        // blink: appear/disappear twice (4 half-cycles) right after a
        // save, then settle back to hidden until the next one.
        if (t >= kAutosavePulseSeconds) return ""; // idle: hidden between saves
        int half_cycle = static_cast<int>(t / (kAutosavePulseSeconds / 4.0));
        bool visible = (half_cycle % 2) == 0;
        return visible ? glyph : " ";
    }

    // color: always visible, pulses C1 -> C2 -> C1 (heartbeat) right
    // after a save, then settles to a steady C1.
    std::string c1 = ansi_for(settings_.autosave_c1.empty() ? settings_.border_color : settings_.autosave_c1, false);
    std::string c2 = ansi_for(settings_.autosave_c2.empty() ? settings_.visualizer_color : settings_.autosave_c2, false);
    if (t >= kAutosavePulseSeconds) return c1 + glyph + "\x1b[0m"; // idle: steady C1
    // Two full heartbeats across the pulse window: C1->C2->C1->C2->C1.
    double phase = std::fmod(t, kAutosavePulseSeconds / 2.0) / (kAutosavePulseSeconds / 2.0); // 0..1 within each half-beat
    bool towards_c2 = phase < 0.5;
    return (towards_c2 ? c2 : c1) + glyph + "\x1b[0m";
}

// ---------------------------------------------------------------------
// Bulk add (paste a YouTube playlist link while Queue is focused)
// ---------------------------------------------------------------------

void App::launch_bulk_add_async(const std::string& url) {
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();
    bulk_add_in_progress_ = true;
    bulk_add_ready_ = false;
    {
        // Clear the slot before the worker starts: if the fetch throws, the
        // guard below leaves pending_bulk_add_ untouched, and a stale
        // success from a previous playlist would otherwise be re-committed.
        std::lock_guard<std::mutex> lk(bulk_add_mutex_);
        pending_bulk_add_ = BulkAddResult{};
    }
    bulk_add_thread_ = std::thread([this, url]() { run_guarded("playlist add", [&] {
        BulkAddResult res;
        res.items = online_.list_playlist(url, &res.error);
        res.success = res.error.empty() && !res.items.empty();
        std::lock_guard<std::mutex> lk(bulk_add_mutex_);
        pending_bulk_add_ = std::move(res);
    }); 
        // Same reasoning as launch_search_async(): the poll loop is gated on
        // this flag, so it is raised whether or not the work succeeded.
        bulk_add_ready_ = true;
    });
}

void App::poll_pending_bulk_add() {
    if (!bulk_add_ready_.load()) return;
    BulkAddResult res;
    {
        std::lock_guard<std::mutex> lk(bulk_add_mutex_);
        if (!bulk_add_ready_.load()) return;
        res = std::move(pending_bulk_add_);
        bulk_add_ready_ = false;
    }
    bulk_add_in_progress_ = false;
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();

    if (!res.success) {
        status_line_ = res.error.empty() ? "couldn't load that playlist" : res.error;
        return; // stay in Mode::BulkAdd, phase 1 -- let the person edit the link and retry
    }

    // Enter phase 2: show the checklist rather than committing
    // immediately. Starts fully de-selected -- "SELECT" is meant for
    // picking your own favorites out of the playlist, so nothing is
    // pre-starred; "ALL" (the "a" key) still adds every fetched track
    // regardless of star state, unaffected by this default.
    pending_bulk_add_ = std::move(res);
    bulk_add_selected_.assign(pending_bulk_add_.items.size(), false);
    bulk_add_cursor_ = 0;
    bulk_add_scroll_ = 0;
    bulk_add_results_ready_ = true;
    status_line_.clear();
}

void App::commit_bulk_add(bool all) {
    int added = 0;
    for (size_t i = 0; i < pending_bulk_add_.items.size(); ++i) {
        if (!all && (i >= bulk_add_selected_.size() || !bulk_add_selected_[i])) continue;
        const auto& item = pending_bulk_add_.items[i];
        queue_.push_back({false, item.title, item.uploader, {}, item.video_id});
        ++added;
    }
    clamp_queue_selected();
    log_event("added " + std::to_string(added) + " track" + (added == 1 ? "" : "s") + " to queue");

    mode_ = Mode::Browse;
    bulk_add_results_ready_ = false;
    bulk_add_buffer_.clear();
    pending_bulk_add_ = BulkAddResult{};
    bulk_add_selected_.clear();
    bulk_add_cursor_ = 0;
    bulk_add_scroll_ = 0;
}

// Tab layout: 0=Colors, 1=On/Off, 2=Animation, 3=Reference, 4=About App.
//
// The Reference tab opens with the editable LOUDNESS NORMALIZATION rows
// (kNormLabels), then a one-line read-only note pointing at the cheat sheet
// ('?') for the full command list, then every rebindable hotkey (kRefRows),
// grouped into categories via the optional `header` field -- set only on a
// category's first row, and rendered as a section title above it -- and
// finally the read-only font-mapping table loaded from config.txt. This tab
// deliberately does NOT also list the app's literal/non-rebindable key
// commands (ESC, Y/N, the playlist and meta editors' own fixed navigation
// and text-editing keys, and so on): there is nothing to configure for
// those here, and keeping a second copy of them just meant this list and
// the cheat sheet's could quietly drift apart. The cheat sheet ('?') is the
// single, authoritative list of every command, rebindable or not -- hence
// the note. All sections scroll together as one list; see ref_display_row()
// below for how a selectable row (settings_row_) maps to the row it's
// actually drawn on, once the section headers/dividers are accounted for.
//
// IMPORTANT: kRefRows[row].action is looked up in settings_.hotkeys (a
// plain string->string map), so reordering/recategorizing rows here is
// always safe -- rebinding still keys off the action name, never off the
// row's position.
struct RefHotkeyRow { const char* header; const char* action; const char* label; };
static const RefHotkeyRow kRefRows[] = {
    // --- Playback ---
    {"PLAYBACK", "HKeyPlay", "Play Selected"},
    {nullptr, "HKeyTogglePlayPause", "Toggle Play / Pause"}, // was missing from this tab entirely
    {nullptr, "HKeyPlayNextSong", "Next Track"},
    {nullptr, "HKeyPlayPreviousSong", "Prev Track"},
    {nullptr, "HKeyShuffleNext", "Shuffle Next"},
    {nullptr, "HKeyCyclePlayMode", "Cycle Play Mode"},
    {nullptr, "HKeySeekForward", "Seek Forward"},
    {nullptr, "HKeySeekBackward", "Seek Backward"},
    {nullptr, "HKeyIncreaseVolume", "Volume Up"},
    {nullptr, "HKeyDecreaseVolume", "Volume Down"},
    {nullptr, "HKeyToggleMute", "Toggle Mute"},
    {nullptr, "HKeyToggleNormalize", "Toggle Normalize"},
    // --- Navigation & View ---
    {"NAVIGATION & VIEW", "HKeyNavigateUp", "Navigate Up"},
    {nullptr, "HKeyNavigateDown", "Navigate Down"},
    {nullptr, "HKeySwitchBetweenCards", "Switch Cards"},
    {nullptr, "HKeyFilterForFolder", "Filter By Folder"},
    {nullptr, "HKeyClearFilter", "Clear Filter"},
    {nullptr, "HKeyCycleSortMode", "Cycle Sort Mode"},
    {nullptr, "HKeyRefreshUi", "Refresh UI"},
    {nullptr, "HKeyToggleWaveform", "Toggle Waveform"},
    {nullptr, "HKeyToggleLyrics", "Toggle Lyrics"},
    {nullptr, "HKeyToggleMetaOnly", "Show Metadata Only"}, // list rows: metadata instead of filename
    {nullptr, "HKeyRetryLyrics", "Retry Lyrics"},
    {nullptr, "HKeyListOverlay", "Big List Overlay"}, // larger LOCAL AUDIO FILES pane floated over the main UI
    // --- Search ---
    {"SEARCH", "HKeySearch", "Search Local"},
    {nullptr, "HKeySearchOnline", "Search Online"},
    {nullptr, "HKeySearchPlaylist", "Search Playlists"},
    {nullptr, "HKeySearchFolder", "Search Folders"},
    // --- Queue ---
    {"QUEUE", "HKeyAddHoveringSongToQueue", "Add To Queue"},
    {nullptr, "HKeyRemoveHoveringSongFromQueue", "Remove From Queue"},
    {nullptr, "HKeyQueueMoveUp", "Queue Move Up"},
    {nullptr, "HKeyQueueMoveDown", "Queue Move Down"},
    {nullptr, "HKeyClearQueue", "Clear Queue"},
    // --- Playlists ---
    {"PLAYLISTS", "HKeyPlaylist", "Open Playlist Editor"}, // opens the playlist create/manage screen
    // --- Meta editor ---
    {"META EDITOR", "HKeyMetaEditor", "Open Meta Editor"}, // edit file names/tags, AcoustID fetch
    // --- Listening history ---
    {"HISTORY", "HKeyHistory", "Open Listening History"}, // HISTORY / TOP TRACKS / HABITS overlay
    // --- Downloads ---
    {"DOWNLOADS", "HKeyDownloadStream", "Download Stream"},
    // --- System ---
    {"SYSTEM", "HKeySetting", "Open Settings"},
    {nullptr, "HKeyConsole", "Console / Logs"},
    {nullptr, "HKeyCheatsheet", "Cheatsheet"},
    {nullptr, "HKeyQuit", "Quit Application"},
};
static constexpr int kRefRowCount = sizeof(kRefRows) / sizeof(kRefRows[0]);

// The one-line, read-only note drawn right after the LOUDNESS NORMALIZATION
// section (with a blank spacer line above it, same as a header) and before
// the hotkeys start. Not selectable -- it costs a display line, same as a
// header, but no entry in the row-index space below.
static const char* const kRefNote = "SEE CHEAT SHEET FOR FULL LIST OF COMMANDS, `?`";

// ---------------------------------------------------------------------------
// LOUDNESS NORMALIZATION -- the two parameters behind the "Normalize Volume"
// toggle, editable on this tab (rather than buried in config.txt, where
// they used to be the only way to change them). This section is first on
// the tab, ahead of the hotkeys, so it -- and the toggle right above it on
// the ON/OFF tab -- are the first thing this tab shows. Their selectable
// row range sits ahead of the rebindable hotkeys and the read-only font-map
// section:
//
//     kNormStart .. kNormEnd-1        these three rows (editable)
//     kRefStart .. kRefEnd-1          rebindable hotkeys (kRefRows)
//     kRefEnd ..                      font-map rows (read-only)
//
// Every row-index translation on this tab (ref_display_row(), the scroll
// window, settings_max_row(), the Enter-to-edit guard) has to go through
// these four constants, which is exactly where they all used to assume a
// single hard/soft split at kRefRowCount.
// ---------------------------------------------------------------------------
static const char* const kNormLabels[] = {
    "Normalize Volume",    // bool, same setting as the ON/OFF tab's row
    "Target Level (LUFS)", // -40..0, what every track is measured to
    "Max Boost (dB)",      // 0..24, how much a quiet track may be lifted
};
static constexpr int kNormRowCount =
    static_cast<int>(sizeof(kNormLabels) / sizeof(kNormLabels[0]));
static constexpr int kNormStart = 0;
static constexpr int kNormEnd = kNormStart + kNormRowCount;
static constexpr int kRefStart = kNormEnd;
static constexpr int kRefEnd = kRefStart + kRefRowCount;

// "-16" instead of "-16.000000". These are the only two fractional values
// on the tab, and settings_options_for() has to render its option list with
// exactly this formatter -- settings_cycle() finds the current value by
// string equality, so a mismatch would silently jump to the first option.
static std::string fmt_loudness(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    std::string s(buf);
    // Drop the ".0" of a whole number -- the '.' as well, not just the zero:
    // a leftover "-16." is not what settings_options_for() offers, so
    // settings_cycle() would never find it and every Left/Right would snap
    // back to the first option instead of stepping from the current one.
    if (s.size() > 2 && s.back() == '0' && s[s.size() - 2] == '.') {
        s.pop_back(); // the zero
        s.pop_back(); // ...and the point that only existed to show it
    }
    return s;
}

// The label column of every row on this tab is 25 cells wide, with the
// ":" barrier sitting right behind it -- every kRefRows/kNormLabels label
// is short enough to fit inside that without wrapping.
static constexpr int kRefLabelW = 25;

// Maps a selectable row index -- kNormStart..kNormEnd-1 for the loudness
// parameters, kRefStart..kRefEnd-1 for the hotkeys, then the font-map
// letters -- to the row it's actually drawn on, once the section
// header/divider lines inserted along the way (one above the loudness
// section, one for the read-only note right after it, one above each hotkey
// category, one above the font map) are accounted for. Used by both the
// render block and the ColorEdit cursor placement below, so the two always
// agree on where a given row lands.
static int ref_display_row(int selectable_row) {
    int headers = 0;
    for (int i = 0; i <= selectable_row; ++i) {
        if (i < kNormEnd) {
            if (i == kNormStart) headers += 2;   // "LOUDNESS NORMALIZATION"
        } else if (i < kRefEnd) {
            if (i == kRefStart) headers += 2;    // the read-only note (blank + text), just before the hotkeys
            if (kRefRows[i - kRefStart].header) headers += 2;
        } else {
            if (i == kRefEnd) headers += 2;      // "FONT / CHARACTER MAP"
        }
    }
    return selectable_row + headers;
}

// The ON/OFF tab's toggle rows, in paint order -- and that order IS the
// tab's selectable row index (settings_row_) for its toggle part: both
// settings_get_value()/settings_commit_edit() and the renderer key off
// it, so appending a line here is all it takes to add another toggle.
static const char* const kOnOffToggles[] = {
    "Eliment Disk", "Dummy Buttons", "Queue Display", "WaveForm",
    "Lyrics Engine", "Lyric Ball", "Visualizer", "Stereo Sound",
    "Normalize Volume", "Show meta data only",
};
static constexpr int kOnOffToggleCount =
    static_cast<int>(sizeof(kOnOffToggles) / sizeof(kOnOffToggles[0]));

// Rebuilds the ON/OFF tab's full paint order: every toggle, then a
// "LOCAL PATH" header + one row per configured local music path + a
// "+ new path" row, then the same three for the playlist paths. Headers
// are display-only (sel stays -1); everything else gets the next
// selectable index in order.
//
// A section whose vector is still empty still shows one (empty) row: an
// unset path has to be an editable blank field, not a missing one,
// otherwise there'd be nowhere to type the very first path.
std::vector<App::OnOffRow> App::build_onoff_rows() const {
    std::vector<OnOffRow> rows;
    rows.reserve(kOnOffToggleCount + 8);

    int sel = 0;
    for (int i = 0; i < kOnOffToggleCount; ++i) {
        OnOffRow r;
        r.kind = OnOffRow::Kind::Toggle;
        r.sel = sel++;
        r.label = kOnOffToggles[i];
        rows.push_back(r);
    }

    auto add_path_section = [&](const char* header, bool playlist) {
        OnOffRow h;
        h.kind = OnOffRow::Kind::Header;
        h.label = header;
        rows.push_back(h);

        const std::vector<std::string>& paths = playlist ? settings_.playlists_paths
                                                         : settings_.local_music_paths;
        int count = std::max(1, static_cast<int>(paths.size()));
        for (int i = 0; i < count; ++i) {
            OnOffRow p;
            p.kind = OnOffRow::Kind::Path;
            p.sel = sel++;
            p.path_index = i;
            p.playlist_path = playlist;
            rows.push_back(p);
        }

        OnOffRow add;
        add.kind = OnOffRow::Kind::AddPath;
        add.sel = sel++;
        add.playlist_path = playlist;
        add.label = "+ new path";
        rows.push_back(add);
    };

    add_path_section("LOCAL PATH", false);

    // DOWNLOAD FOLDER: yt-dlp's single output folder, deliberately NOT run
    // through add_path_section() -- there is only ever one place downloads
    // land, so there is nothing to list and no "+ new path" to add. Reads
    // and writes settings_.download_folder (see settings_get_value() /
    // settings_commit_edit()) and is auto-injected into the local paths at
    // load/rescan, so setting it is all it takes for downloads to appear
    // in the library.
    {
        OnOffRow h;
        h.kind = OnOffRow::Kind::Header;
        h.label = "DOWNLOAD FOLDER";
        rows.push_back(h);

        OnOffRow d;
        d.kind = OnOffRow::Kind::Path;
        d.sel = sel++;
        d.path_index = -1; // no vector index: d.download_folder says who owns it
        d.download_folder = true;
        rows.push_back(d);
    }

    add_path_section("PLAYLIST PATH", true);

    return rows;
}

// Selectable row -> the display row it's painted on. Every header
// contributes two display rows (a blank spacer above it plus the title
// itself, exactly like the Reference tab's category headers), so this
// walks the layout and counts rather than just indexing -- the same
// relationship ref_display_row() provides over on the Reference tab,
// shared by the renderer and the ColorEdit cursor placement below so the
// two can never disagree. -1 if there is no such selectable row.
int App::onoff_display_row(int selectable_row) const {
    int disp = 0;
    for (const OnOffRow& r : build_onoff_rows()) {
        if (r.kind == OnOffRow::Kind::Header) { disp += 2; continue; }
        if (r.sel == selectable_row) return disp;
        disp++;
    }
    return -1;
}

App::OnOffRow App::onoff_row(int selectable_row) const {
    OnOffRow none; // sel == -1 == "no such row"
    if (selectable_row < 0) return none;
    for (const OnOffRow& r : build_onoff_rows()) if (r.sel == selectable_row) return r;
    return none;
}

bool App::onoff_row_is_path(int selectable_row) const {
    OnOffRow r = onoff_row(selectable_row);
    return r.sel >= 0 && r.kind == OnOffRow::Kind::Path;
}

std::string* App::color_field_ptr(int row, int col) {
    switch (row) {
        case 0: return col == 0 ? &settings_.border_color : &settings_.border_color_bottom;
        case 1: return col == 0 ? &settings_.disk_color : &settings_.disk_color_end;
        case 2: return col == 0 ? &settings_.meta_key_color : &settings_.meta_val_color;
        case 3: return col == 0 ? &settings_.visualizer_color : &settings_.visualizer_color_end;
        case 4: return col == 0 ? &settings_.progress_played_color : &settings_.progress_remaining_color;
        case 5: return col == 0 ? &settings_.list_color : &settings_.list_inactive_bg_color;
        case 6: return col == 0 ? &settings_.list_playing_color : &settings_.list_playing_bg_color;
        case 7: return col == 0 ? &settings_.list_cursor_color : &settings_.list_cursor_bg_color;
        case 8: return col == 0 ? &settings_.queue_color : &settings_.queue_inactive_bg_color;
        case 9: return col == 0 ? &settings_.queue_playing_color : &settings_.queue_playing_bg_color;
        case 10: return col == 0 ? &settings_.queue_cursor_color : &settings_.queue_cursor_bg_color;
        case 11: return col == 0 ? &settings_.inactive_line_color : &settings_.inactive_line_bg_color;
        case 12: return col == 0 ? &settings_.active_line_color : &settings_.active_line_bg_color;
        case 13: return col == 0 ? &settings_.active_word_color : &settings_.active_word_bg_color;
        case 14: return col == 0 ? &settings_.header_color : nullptr; // HEADER: text color only, no background cell
        default: return nullptr;
    }
}

int App::settings_max_row() const {
    // Matches get_max_row(): SCHEMA.size() - 1 for each tab, extended for
    // the Reference tab's appended font-map rows and the About tab's
    // scrollable text (both computed dynamically, not hardcoded, so they
    // track the actual font_map/about_app_lines content).
    switch (settings_tab_) {
        case 0: return 14; // COLOR_SCHEMA: 15 rows (the extra one is HEADER)
        case 1: {          // ON/OFF: the toggles plus the LOCAL PATH /
            // PLAYLIST PATH lists -- computed from the layout instead of
            // hardcoded, since it now depends on how many paths are
            // configured. Headers are not selectable and don't count.
            int last = -1;
            for (const auto& r : build_onoff_rows()) last = std::max(last, r.sel);
            return last;
        }
        case 2: return 7;  // ANIM_SCHEMA: 8 rows
        case 3: {
            int letters = 0;
            for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) ++letters;
            return kRefEnd + letters - 1; // loudness rows + hotkeys + N font-map rows
        }
        case 4: {
            int MAX_Y = std::max(term_rows_ - 2, 10);
            int visible = std::max(1, MAX_Y - 3);
            int total = static_cast<int>(settings_.about_app_lines.size());
            return std::max(0, total - visible); // scroll range, not a field cursor
        }
        default: return 0;
    }
}

std::string App::settings_get_value(int row, int col) const {
    // Matches getVal(k): config value if set, else the hardcoded default
    // for the 7 keys that have one, else "___" (unset). Colors format
    // their own "___"-equivalent as "none" at render time instead.
    if (settings_tab_ == 0) {
        std::string* p = const_cast<App*>(this)->color_field_ptr(row, col);
        return p ? *p : "";
    }
    if (settings_tab_ == 1) {
        // The toggle rows read straight off their bool; the path rows
        // (added below the toggles -- see build_onoff_rows()) read the
        // string at their index in the owning vector, which is "" both
        // for a not-yet-set path and for a placeholder row shown while
        // the vector is still empty.
        OnOffRow r = onoff_row(row);
        if (r.sel < 0) return "";
        if (r.kind == OnOffRow::Kind::Path) {
            if (r.download_folder) {
                // Show what is actually in effect: while nothing has been
                // configured that is the default cache folder, and the point
                // of this row is that you can see -- and change -- where
                // downloads land, not that you face a blank field.
                return settings_.download_folder.empty() ? path_utf8(cache_.cache_dir())
                                                         : settings_.download_folder;
            }
            const std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                                    : settings_.local_music_paths;
            if (r.path_index >= 0 && r.path_index < static_cast<int>(paths.size()))
                return paths[r.path_index];
            return "";
        }
        if (r.kind == OnOffRow::Kind::AddPath) return ""; // the row IS the button, it has no value
        bool v = false;
        switch (row) {
            case 0: v = settings_.element_disk; break;
            case 1: v = settings_.element_dummy_buttons; break;
            case 2: v = settings_.element_queue; break;
            case 3: v = settings_.element_waveform; break;
            case 4: v = settings_.element_lyrics; break;
            case 5: v = settings_.element_lyrics_placeholder_ball; break;
            case 6: v = settings_.element_visualizer; break;
            case 7: v = settings_.stereo; break;
            case 8: v = settings_.normalize; break;
            case 9: v = settings_.meta_only; break;
        }
        return v ? "true" : "false";
    }
    if (settings_tab_ == 2) {
        switch (row) {
            case 0: return std::to_string(settings_.visualizer_fluidity);
            case 1: return settings_.waveform_smooth ? "smooth" : "raw";
            case 2: return std::to_string(settings_.disk_rotation_speed).substr(0, 4);
            case 3: {
                static const char* names[] = {"list", "loop", "shuffle", "stop", "repeat queue"};
                return names[std::clamp(settings_.play_mode, 0, 4)];
            }
            case 4: return std::to_string(settings_.visualizer_degradation_speed);
            case 5: return std::to_string(settings_.visualizer_viscosity);
            case 6: return settings_.lyrics_alignment == 1 ? "left" : settings_.lyrics_alignment == 2 ? "right" : "center";
            case 7: {
                static const char* names[] = {"full", "word by word", "letter by letter", "active line only", "active word only", "line by line"};
                return names[std::clamp(settings_.lyrics_animation, 0, 5)];
            }
        }
    }
    if (settings_tab_ == 3 && row >= kRefStart && row < kRefEnd) {
        auto it = settings_.hotkeys.find(kRefRows[row - kRefStart].action);
        return it != settings_.hotkeys.end() ? it->second : "";
    }
    // LOUDNESS NORMALIZATION rows: same three values the ON/OFF tab's
    // "Normalize Volume" toggle and config.txt expose -- the point of
    // putting them here is that the numbers are finally reachable without
    // editing a file, and fmt_loudness() is what keeps them looking like
    // "-16" rather than "-16.000000".
    if (settings_tab_ == 3 && row >= kNormStart && row < kNormEnd) {
        switch (row - kNormStart) {
            case 0: return settings_.normalize ? "true" : "false";
            case 1: return fmt_loudness(settings_.normalize_target_lufs);
            case 2: return fmt_loudness(settings_.normalize_max_boost_db);
        }
    }
    return "";
}

// Matches g_options: the fixed value lists that Left/Right cycles
// through. Empty return = not cyclable (Colors and Reference rows,
// exactly like the reference's g_options map has no entries for those).
std::vector<std::string> App::settings_options_for(int tab, int row) const {
    if (tab == 1) {
        // Only the bool toggles cycle through a fixed value list. Path
        // fields are free text (a folder to type in), and "+ new path" is
        // an action row rather than a value -- neither has anything for
        // Left/Right to walk, and letting it pretend otherwise would feed
        // a "true"/"false" into a commit that then had nowhere to put it.
        OnOffRow r = onoff_row(row);
        if (r.sel < 0 || r.kind != OnOffRow::Kind::Toggle) return {};
        return {"true", "false"};
    }
    if (tab == 2) {
        switch (row) {
            case 0: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 1: return {"raw", "smooth"};
            case 2: return {"0.01", "0.05", "0.10", "0.17", "0.25", "0.50", "0.75", "1.00"};
            case 3: return {"list", "loop", "shuffle", "stop", "repeat queue"};
            case 4: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 5: return {"1", "2", "3", "4", "5", "6", "7", "8", "9", "10"};
            case 6: return {"left", "center", "right"};
            case 7: return {"full", "word by word", "line by line", "letter by letter", "active line only", "active word only"};
        }
    }
    if (tab == 3 && row >= kNormStart && row < kNormEnd) {
        // The loudness rows are the one non-bool cyclable thing on the
        // Reference tab: whole numbers only, rendered through the very same
        // formatter settings_get_value() uses so the current value is
        // found by settings_cycle()'s string compare instead of jumping
        // back to the first option.
        switch (row - kNormStart) {
            case 0: return {"true", "false"};
            case 1: {
                std::vector<std::string> v;
                for (int i = -40; i <= 0; ++i) v.push_back(std::to_string(i)); // LUFS target
                return v;
            }
            case 2: {
                std::vector<std::string> v;
                for (int i = 0; i <= 24; ++i) v.push_back(std::to_string(i)); // max boost in dB
                return v;
            }
        }
    }
    return {};
}

// Matches g_config[k] = edit_buffer -- stored close to verbatim, no
// clamping. A bool/enum field that doesn't recognize the typed text
// just leaves the setting unchanged, since there's no way to store
// arbitrary text in a typed field the way the reference's string map can.
void App::settings_commit_edit() {
    const std::string& buf = color_edit_buffer_;
    auto to_lower = [](std::string v) { for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return v; };

    if (settings_tab_ == 0) {
        std::string* p = color_field_ptr(settings_row_, settings_col_);
        if (p) *p = buf;
    } else if (settings_tab_ == 1) {
        OnOffRow r = onoff_row(settings_row_);
        if (r.kind == OnOffRow::Kind::Path) {
            // A path row commits a string instead of a bool. Trim the stray
            // spaces a careless Enter can leave behind (a trailing one would
            // otherwise point at a directory that doesn't exist) and expand
            // a leading ~ exactly the way load_settings() does, so
            // "~/Music" means the same thing whether it was typed into this
            // field or written into config.txt by hand. Committing an empty
            // value deletes the line rather than storing a blank
            // LocalMusicPath=/PlaylistsPath= -- which is also what keeps
            // the placeholder row a still-empty list shows harmless.
            std::string p = buf;
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
            while (!p.empty() && (p.back() == ' ' || p.back() == '\t')) p.pop_back();
            if (!p.empty() && p[0] == '~') {
                const char* home = std::getenv("HOME");
                if (home) p = std::string(home) + p.substr(1);
            }
            if (r.download_folder) {
                // One folder, never a list: store it (an emptied field goes
                // back to the default cache folder), point yt-dlp at it right
                // away, and -- when one is set -- add it to the local paths so
                // the rescan below picks it up. That injection is the whole
                // "no extra LocalMusicPath line needed" half of this setting.
                settings_.download_folder = p;
                cache_.set_download_dir(path_from_utf8(p));
                if (p.empty()) {
                    status_line_ = "download folder: default (cache dir)";
                } else {
                    if (std::find(settings_.local_music_paths.begin(), settings_.local_music_paths.end(), p)
                        == settings_.local_music_paths.end())
                        settings_.local_music_paths.push_back(p);
                    status_line_ = "download folder: " + p;
                }
                rescan_library(); // a path row that does not rescan is a path row you have to restart for
                return;
            }
            std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                              : settings_.local_music_paths;
            if (static_cast<int>(paths.size()) <= r.path_index) paths.resize(r.path_index + 1);
            if (p.empty()) {
                paths.erase(paths.begin() + r.path_index);
                status_line_ = r.playlist_path ? "playlist path removed" : "local path removed";
            } else {
                paths[r.path_index] = p;
                status_line_ = std::string(r.playlist_path ? "playlist path: " : "local path: ") + p;
            }
            if (!r.playlist_path) rescan_library(); // the whole point: paths apply without a restart
            settings_row_ = std::min(settings_row_, settings_max_row()); // the list may have shrunk by one
            return;
        }
        std::string v = to_lower(buf);
        bool is_true = (v == "true"), is_false = (v == "false");
        if (!is_true && !is_false) return;
        switch (settings_row_) {
            case 0: settings_.element_disk = is_true; break;
            case 1: settings_.element_dummy_buttons = is_true; break;
            case 2: settings_.element_queue = is_true; break;
            case 3: settings_.element_waveform = is_true; break;
            case 4: settings_.element_lyrics = is_true; break;
            case 5: settings_.element_lyrics_placeholder_ball = is_true; break;
            case 6: settings_.element_visualizer = is_true; break;
            case 7:
                settings_.stereo = is_true;
                player_.set_stereo(is_true); // audible immediately for a stereo-decoded track
                break;
            case 8:
                settings_.normalize = is_true;
                player_.set_normalization(settings_.normalize,
                                          static_cast<float>(settings_.normalize_target_lufs),
                                          static_cast<float>(settings_.normalize_max_boost_db));
                break;
            case 9: settings_.meta_only = is_true; break;
        }
    } else if (settings_tab_ == 2) {
        std::string v = to_lower(buf);
        switch (settings_row_) {
            case 0: try { settings_.visualizer_fluidity = std::stoi(buf); } catch (...) {} break;
            case 1: settings_.waveform_smooth = (v == "smooth"); break;
            case 2: try { settings_.disk_rotation_speed = std::stod(buf); } catch (...) {} break;
            case 3: settings_.play_mode = (v == "loop") ? 1 : (v == "shuffle") ? 2 : (v == "stop") ? 3 : (v == "repeat queue") ? 4 : 0; break;
            case 4: try { settings_.visualizer_degradation_speed = std::stoi(buf); } catch (...) {} break;
            case 5: try { settings_.visualizer_viscosity = std::stoi(buf); } catch (...) {} break;
            case 6: settings_.lyrics_alignment = (v == "left") ? 1 : (v == "right") ? 2 : 0; break;
            case 7:
                if (v == "word by word") settings_.lyrics_animation = 1;
                else if (v == "letter by letter") settings_.lyrics_animation = 2;
                else if (v == "active line only") settings_.lyrics_animation = 3;
                else if (v == "active word only") settings_.lyrics_animation = 4;
                else if (v == "line by line") settings_.lyrics_animation = 5;
                else settings_.lyrics_animation = 0;
                break;
        }
    } else if (settings_tab_ == 3 && settings_row_ >= kRefStart && settings_row_ < kRefEnd) {
        settings_.hotkeys[kRefRows[settings_row_ - kRefStart].action] = buf;
    } else if (settings_tab_ == 3 && settings_row_ >= kNormStart && settings_row_ < kNormEnd) {
        // LOUDNESS NORMALIZATION: clamped to the same ranges the option
        // lists advertise (typing a stray letter must not derail a value),
        // then pushed into the player immediately, exactly like the ON/OFF
        // tab's "Normalize Volume" toggle does above -- otherwise the
        // numbers would only take effect at the next restart.
        switch (settings_row_ - kNormStart) {
            case 0: {
                std::string v = to_lower(buf);
                if (v == "true") settings_.normalize = true;
                else if (v == "false") settings_.normalize = false;
                else return;
                break;
            }
            case 1:
                try { settings_.normalize_target_lufs = std::clamp(std::stod(buf), -40.0, 0.0); }
                catch (...) { return; }
                break;
            case 2:
                try { settings_.normalize_max_boost_db = std::clamp(std::stod(buf), 0.0, 24.0); }
                catch (...) { return; }
                break;
            default: return;
        }
        player_.set_normalization(settings_.normalize,
                                  static_cast<float>(settings_.normalize_target_lufs),
                                  static_cast<float>(settings_.normalize_max_boost_db));
        status_line_ = std::string("loudness: ") + (settings_.normalize ? "on, target " : "off, target ")
                     + fmt_loudness(settings_.normalize_target_lufs) + " LUFS, boost max "
                     + fmt_loudness(settings_.normalize_max_boost_db) + " dB";
    }
}

// Matches cycle_option(): find the current value's index in its options
// list and step by `dir`, wrapping. No-op if this row has no options
// list at all.
void App::settings_cycle(int dir) {
    auto opts = settings_options_for(settings_tab_, settings_row_);
    if (opts.empty()) return;
    std::string cur = settings_get_value(settings_row_, 0);
    int idx = -1;
    for (size_t i = 0; i < opts.size(); ++i) if (opts[i] == cur) { idx = static_cast<int>(i); break; }
    idx = (idx == -1) ? 0 : (idx + dir + static_cast<int>(opts.size())) % static_cast<int>(opts.size());
    color_edit_buffer_ = opts[idx];
    edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); // the buffer was replaced, not edited
    settings_commit_edit();
    status_line_ = "TOGGLED -> " + opts[idx];
    if (settings_tab_ == 2 && settings_row_ == 1) recompute_waveform_for_current_track();
    if (settings_tab_ == 1 && settings_row_ == 7) {
        // A track that was decoded as mono can't become stereo without being
        // decoded again, so switching stereo ON only applies from the next
        // track. Switching it OFF is immediate.
        if (settings_.stereo && has_track_ && current_pcm_ && current_pcm_->channels == 1)
            status_line_ = "stereo: on -- applies from the next track";
        else
            status_line_ = settings_.stereo ? "stereo: on" : "stereo: off";
    }
    if (settings_tab_ == 1 && settings_row_ == 8) {
        status_line_ = settings_.normalize ? "normalize: on" : "normalize: off";
    }
    if (settings_tab_ == 1 && settings_row_ == 9) {
        status_line_ = settings_.meta_only ? "lists: metadata only (no filename)"
                                           : "lists: filename + metadata";
        resort_local_view_keep_selection();
    }
}

void App::handle_settings_key(int key) {
    if (mode_ == Mode::ColorEdit) {
        // The caret can be stale by one keypress after the buffer was
        // replaced wholesale (an option cycle, a rejected hotkey) -- clamp
        // before anything reads it, exactly like the meta editor does.
        le_clamp(color_edit_buffer_, edit_caret_, edit_anchor_);
        if (key == 27) { mode_ = Mode::Settings; return; } // cancel, discard buffer
        if (key == '\r' || key == '\n') {
            std::string key_name = (settings_tab_ == 3 && settings_row_ >= kRefStart && settings_row_ < kRefEnd)
                                  ? kRefRows[settings_row_ - kRefStart].action : "";
            // Hotkey overlap fix: if this is a Reference-tab hotkey being
            // rebound and the typed key is already owned by a different
            // action, reject the commit instead of silently creating a
            // collision where two actions fire on the same key. Clear the
            // buffer, force a redraw, and stay in ColorEdit mode so the
            // user can just repeat entry with a different key -- same
            // flow as a normal edit, just not accepted yet.
            if (!key_name.empty()) {
                std::string conflict = hotkey_conflict(color_edit_buffer_, key_name);
                if (!conflict.empty()) {
                    status_line_ = "KEY \"" + color_edit_buffer_ + "\" ALREADY USED BY " + conflict + " -- try another key";
                    color_edit_buffer_.clear();
                    edit_caret_ = edit_anchor_ = 0;
                    force_redraw_ = true;
                    return; // stay in ColorEdit: redraw and repeat
                }
            }
            // Path commits write their own status (which path changed, that
            // the library was rescanned) -- don't bury it under the generic
            // "UPDATED".
            OnOffRow commit_row = onoff_row(settings_row_);
            bool wrote_own_status = (settings_tab_ == 1 &&
                                     commit_row.kind == OnOffRow::Kind::Path);
            settings_commit_edit();
            if (!wrote_own_status) status_line_ = key_name.empty() ? "UPDATED" : ("UPDATED " + key_name);
            mode_ = Mode::Settings;
            return;
        }
        // --- caret, selection, clipboard ---------------------------------
        // Shift+arrows mark, Ctrl+C/X/V copy/cut/paste, Home/End jump. These
        // are modifier combinations that arrive as their own sentinel values
        // (terminal_ui.h) precisely because the bare ones are already taken:
        // an arrow press collapses to the letter it would otherwise type, and
        // a typed 'c' has to stay a typed 'c'.
        if (key == kKeyHome) { edit_caret_ = edit_anchor_ = 0; return; }
        if (key == kKeyEnd) { edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); return; }
        if (key == kKeyShiftLeft) { le_move(color_edit_buffer_, edit_caret_, edit_anchor_, -1, true); return; }
        if (key == kKeyShiftRight) { le_move(color_edit_buffer_, edit_caret_, edit_anchor_, +1, true); return; }
        if (key == kKeyDelete) { le_delete_forward(color_edit_buffer_, edit_caret_, edit_anchor_); return; }
        if (key == kKeyCtrlC) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            std::string t = (a == b) ? color_edit_buffer_ : color_edit_buffer_.substr(a, b - a);
            clipboard_set(t);
            status_line_ = t.empty() ? "nothing selected" : "COPIED";
            return;
        }
        if (key == kKeyCtrlX) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            if (a == b) return; // no selection: cut must never empty the field
            clipboard_set(color_edit_buffer_.substr(a, b - a));
            le_erase_selection(color_edit_buffer_, edit_caret_, edit_anchor_);
            status_line_ = "CUT";
            return;
        }
        if (key == kKeyCtrlV) {
            // A path gets far more room than a color/hotkey field does --
            // the same cap typing enforces below.
            const int limit = (settings_tab_ == 1 && onoff_row_is_path(settings_row_)) ? 240 : 18;
            le_paste(color_edit_buffer_, edit_caret_, edit_anchor_, clipboard_get(), static_cast<size_t>(limit));
            status_line_ = "PASTED";
            return;
        }
        if (key == 127 || key == 8) { le_backspace(color_edit_buffer_, edit_caret_, edit_anchor_); return; }
        // Arrow keys collapse to the letters 'A'-'D' (same collision as
        // Mode::Search below), which sit inside 32-126 and would otherwise
        // be typed as literal letters -- for a hotkey rebind that would
        // mean an arrow press could silently become the new binding. The
        // value on its own can't tell a real capital A from an Up press,
        // so ask the input layer instead: a genuinely typed A-D goes into
        // the buffer now (without it a Windows path could never be
        // entered -- "C:\" has no way past this check), while an arrow
        // press moves the caret instead (Left/Right) or is dropped (Up/Down
        // -- a single line has nowhere to go vertically).
        if (last_key_was_arrow() && (key == 'A' || key == 'B')) return;
        if (last_key_was_arrow() && (key == 'C' || key == 'D')) {
            le_move(color_edit_buffer_, edit_caret_, edit_anchor_, key == 'D' ? -1 : +1, false);
            return;
        }
        // A path needs far more room than a color/hotkey field does.
        const int edit_limit = (settings_tab_ == 1 && onoff_row_is_path(settings_row_)) ? 240 : 18;
        if (is_text_key(key)) {
            le_insert(color_edit_buffer_, edit_caret_, edit_anchor_, static_cast<char>(key),
                      static_cast<size_t>(edit_limit));
        }
        return;
    }

    if (key == 9) { // Tab
        settings_tab_ = (settings_tab_ + 1) % kSettingsTabCount;
        settings_row_ = 0;
        settings_col_ = 0;
        return;
    }
    // Explicit spec from the user, overriding the reference's own
    // key semantics for this exact case (reference's 's' saves without
    // closing; here 's' saves AND exits, Esc/q just exits without saving).
    if (key == 's' || key == 'S') { save_settings(settings_); status_line_ = "SAVED"; mode_ = Mode::Browse; return; }
    if (key == 27 || key == 'q' || key == 'Q') {
        mode_ = Mode::Browse;
        return;
    }
    if (settings_tab_ == 4) {
        // About App: no fields to edit, but Up/Down still scroll the text.
        if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
        if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
        return;
    }

    if (key == '\r' || key == '\n') {
        // Reference tab: the loudness rows (kNormStart..kNormEnd-1) and the
        // hotkeys (kRefStart..kRefEnd-1) are editable and sit back-to-back;
        // the note between them and the font-map rows after them are
        // read-only display and never enter ColorEdit at all.
        if (settings_tab_ == 3 && settings_row_ >= kRefEnd) {
            return;
        }
        if (settings_tab_ == 1) {
            OnOffRow r = onoff_row(settings_row_);
            if (r.kind == OnOffRow::Kind::AddPath) {
                // "+ new path": append an empty line to the list this row
                // belongs to and go straight into editing it. No row
                // bookkeeping needed -- build_onoff_rows() gives the new
                // entry exactly the selectable index this AddPath row just
                // vacated (every row after it shifts up by one), so
                // settings_row_ already points at the new empty field.
                std::vector<std::string>& paths = r.playlist_path ? settings_.playlists_paths
                                                                  : settings_.local_music_paths;
                paths.push_back("");
                color_edit_buffer_.clear();
                edit_caret_ = edit_anchor_ = 0;
                edit_owner_ = "settings"; // claim the shared caret -- see edit_focus()
                mode_ = Mode::ColorEdit;
                status_line_ = r.playlist_path
                    ? "new playlist path -- type it, ENTER to confirm"
                    : "new local music path -- type it, ENTER to confirm";
                force_redraw_ = true;
                return;
            }
        }
        color_edit_buffer_ = settings_get_value(settings_row_, settings_col_);
        edit_caret_ = edit_anchor_ = color_edit_buffer_.size(); // caret starts at the end, as usual
        edit_owner_ = "settings"; // claim the shared caret -- see edit_focus()
        mode_ = Mode::ColorEdit;
        return;
    }
    if (key == 'A') { if (settings_row_ > 0) --settings_row_; return; }
    if (key == 'B') { if (settings_row_ < settings_max_row()) ++settings_row_; return; }
    if (key == 'C') { // right
        if (settings_tab_ == 0) { if (color_field_ptr(settings_row_, 1)) settings_col_ = 1; }
        else settings_cycle(1);
        return;
    }
    if (key == 'D') { // left
        if (settings_tab_ == 0) settings_col_ = 0;
        else settings_cycle(-1);
        return;
    }
}

void App::start_local_track(const LocalTrack& track) {
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    fs::path parent = track.path.parent_path().filename();
    // folder_artist is only a cheap guess for the browse-list Artist column
    // (see LocalTrack's comment in local_source.h) -- it's the same string
    // as the Location field below (both are just the parent folder name),
    // so feeding it in here as the fallback artist made an untagged file's
    // metadata panel show the folder path twice, once as "Artist" and once
    // as "Location". Pass "" instead: probe_metadata() already falls back
    // to "-" when there's no real artist tag, same as start_online_track
    // does for YouTube tracks below.
    launch_load_async(track.path, track.title, "",
                       path_utf8(parent) + "/", /*is_local=*/true, /*video_id=*/"");
}

void App::start_online_track(const OnlineResult& result) {
    if (load_in_progress_.load()) { status_line_ = "still loading the previous track ..."; return; }
    // Pass "" for artist so the lyrics search queries just the YouTube video title
    // (which usually contains "Artist - Song Name" perfectly), instead of appending the channel name.
    launch_load_async({}, result.title, "", "youtube", /*is_local=*/false, result.video_id);
}

// ---------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------

void App::handle_key(int key) {
    if (key == 0) return;

    // A meta-editor confirmation -- Shift+B's AcoustID disclaimer (raised
    // here in Browse) or Ctrl+Shift+S/D raised from the menu -- swallows every
    // key until it's answered, so nothing underneath it can be triggered by
    // accident. Checked before the mode dispatch on purpose: the prompt can
    // outlive the key that raised it in either of two modes.
    if (handle_meta_prompt_key(key)) return;

    if (mode_ == Mode::ColorEdit || mode_ == Mode::Settings) {
        handle_settings_key(key);
        return;
    }

    if (mode_ == Mode::Console) {
        // ESC, or the console hotkey again, closes it. Anything else is
        // ignored -- this is a read-only log view.
        if (key == 27 || key == 't' || key == 'T') mode_ = Mode::Browse;
        return;
    }

    if (mode_ == Mode::Playlist) {
        handle_playlist_key(key);
        return;
    }

    if (mode_ == Mode::MetaEdit) {
        handle_meta_key(key);
        return;
    }

    if (mode_ == Mode::History) {
        handle_history_key(key);
        return;
    }

    if (mode_ == Mode::ClearQueue) {
        // "Want to clear queue?" -- Left/Right/TAB move between Yes and No,
        // Enter takes the highlighted one, y / n answer directly, ESC is No.
        // Any OTHER key also cancels (nothing is cleared): this mode must never
        // be a place where keys vanish silently, or a popup that is hard to
        // see would look like a frozen UI. Cancelling is always the safe side.
        const bool arrow = last_key_was_arrow();
        if (arrow && (key == 'C' || key == 'D')) { clear_queue_choice_ = (key == 'D') ? 0 : 1; return; } // Left=Yes, Right=No
        if (!arrow && key == 9) { clear_queue_choice_ ^= 1; return; }
        if (!arrow && (key == 'y' || key == 'Y')) { mode_ = Mode::Browse; queue_clear(); return; }
        if (!arrow && (key == '\r' || key == '\n')) {
            mode_ = Mode::Browse;
            if (clear_queue_choice_ == 0) queue_clear();
            return;
        }
        mode_ = Mode::Browse; // n / N / ESC / anything else: No
        status_line_.clear();
        return;
    }

    if (mode_ == Mode::Cheatsheet) {
        if (key == 27 || key == '?') mode_ = Mode::Browse;
        else if (key == 'A') --cheatsheet_scroll_; // up -- the table is longer than the screen
        else if (key == 'B') ++cheatsheet_scroll_; // down (clamped while rendering)
        return;
    }

    if (mode_ == Mode::BulkAdd) {
        if (key == 27) { // cancel entirely -- discard buffer, results, and selection state
            mode_ = Mode::Browse;
            status_line_.clear();
            bulk_add_buffer_.clear();
            bulk_add_results_ready_ = false;
            pending_bulk_add_ = BulkAddResult{};
            bulk_add_selected_.clear();
            return;
        }

        if (bulk_add_results_ready_) {
            // --- Phase 2: navigate/star the fetched checklist. ---
            int total = static_cast<int>(pending_bulk_add_.items.size());
            if (key == 'A' && total > 0) { // up
                bulk_add_cursor_ = std::max(0, bulk_add_cursor_ - 1);
                if (bulk_add_cursor_ < bulk_add_scroll_) bulk_add_scroll_ = bulk_add_cursor_;
                return;
            }
            if (key == 'B' && total > 0) { // down
                bulk_add_cursor_ = std::min(total - 1, bulk_add_cursor_ + 1);
                if (bulk_add_cursor_ >= bulk_add_scroll_ + kBulkAddVisibleRows) bulk_add_scroll_ = bulk_add_cursor_ - kBulkAddVisibleRows + 1;
                return;
            }
            if (key == ' ' && total > 0) { // toggle star on the hovered row
                if (bulk_add_cursor_ < static_cast<int>(bulk_add_selected_.size())) {
                    bulk_add_selected_[bulk_add_cursor_] = !bulk_add_selected_[bulk_add_cursor_];
                }
                return;
            }
            if (key == 'a') { commit_bulk_add(/*all=*/true); return; }        // "ALL"
            if (key == '\r' || key == '\n') { commit_bulk_add(/*all=*/false); return; } // "[SELECT]"
            return;
        }

        // --- Phase 1: typing the link. ---
        if (key == '\r' || key == '\n') {
            if (!bulk_add_buffer_.empty() && !bulk_add_in_progress_.load()) {
                status_line_ = "fetching playlist ...";
                launch_bulk_add_async(bulk_add_buffer_);
            }
            return; // stays open -- poll_pending_bulk_add() moves to phase 2 once the fetch resolves
        }
        if (key == 127 || key == 8) { pop_utf8_char(bulk_add_buffer_); return; }
        // Same bug as Mode::Search below: arrow keys collapse to 'A'-'D',
        // which sit inside 32-126 and would otherwise get typed as literal
        // letters into the link being entered.
        if ((key == 'A' || key == 'B' || key == 'C' || key == 'D')) return;
        if (is_text_key(key) && bulk_add_buffer_.size() < 200) bulk_add_buffer_ += static_cast<char>(key);
        return;
    }

    if (mode_ == Mode::RetryLyrics) {
        if (key == 27) { mode_ = Mode::Browse; return; } // cancel entirely, nothing submitted

        std::vector<RLField> fields = rl_visible_fields();
        auto cur_pos = std::find(fields.begin(), fields.end(), rl_focus_);
        int idx = (cur_pos != fields.end()) ? static_cast<int>(cur_pos - fields.begin()) : 0;

        if (key == 9 || key == 'B') { // Tab / down -- next field
            idx = (idx + 1) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            return;
        }
        if (key == 'A') { // up -- previous field
            idx = (idx - 1 + static_cast<int>(fields.size())) % static_cast<int>(fields.size());
            rl_focus_ = fields[idx];
            return;
        }
        if (key == '\r' || key == '\n') { rl_submit(); return; } // "enter to fetch", works from any field

        if (bool* b = rl_bool_ptr(rl_focus_)) {
            if (key == ' ') { // toggle -- checkboxes are the only thing Space does anything to
                bool new_val = !*b;
                // Enforce the radio group: turning one of
                // slowed/ultra-slowed/spedup on clears the other two.
                // Reverb/remix/other stay independent of this and of
                // each other.
                if (rl_focus_ == RLField::TypeSlowed || rl_focus_ == RLField::TypeUltraSlowed || rl_focus_ == RLField::TypeSpedup) {
                    rl_slowed_ = rl_ultra_slowed_ = rl_spedup_ = false;
                }
                *b = new_val;
                // Turning RemixText/OtherText off (unchecking) drops
                // them from rl_visible_fields() -- if focus was sitting
                // on one of those now-hidden text fields, land back on
                // the checkbox that just hid it instead of a field that
                // no longer exists in the nav order.
                return;
            }
            return; // typing/backspace do nothing on a checkbox field
        }

        if (std::string* t = rl_text_ptr(rl_focus_)) {
            if (key == 127 || key == 8) { pop_utf8_char(*t); return; }
            // 'A'/'B' (up/down) are already intercepted above for field
            // navigation, but 'C'/'D' (right/left) fall through to here
            // uncaught -- same bug as Mode::Search below, where an arrow
            // code inside the printable range gets typed as a literal
            // letter instead of being recognized as an arrow key.
            if (key == 'C' || key == 'D') return;
            if (is_text_key(key) && t->size() < 200) *t += static_cast<char>(key);
            return;
        }
        return;
    }

    if (mode_ == Mode::Search) {
        if (key == 27) {
            // Cancel: put the view back exactly as it was before '/' was
            // pressed, discarding whatever the live preview below was
            // showing.
            list_source_ = pre_search_list_source_;
            last_local_query_ = pre_search_local_query_;
            refresh_local_view();
            mode_ = Mode::Browse;
            return;
        }
        if (key == '\r' || key == '\n') { submit_search(); mode_ = Mode::Browse; return; }
        // Up/Down navigate the live preview below the box -- claimed before
        // the caret keys because they are exactly the two arrow letters
        // edit_text_key() would otherwise drop ("nowhere to go vertically"),
        // and navigating the list you are filtering is worth more here than
        // a second way to do nothing. Left/Right used to seek by 5 seconds
        // from inside this box too; they are the caret keys now, since a
        // text field needs them more and seek still works from Browse.
        if (last_key_was_arrow() && (key == 'A' || key == 'B')) { // up / down
            // SHIFT+Up/Down: whole page at a time while the big list overlay is open.
            if (list_overlay_active() && last_key_was_shifted()) { list_overlay_page(key == 'A' ? -1 : 1); return; }
            if (key == 'A') {
                if (selected_ > 0) --selected_;
                if (selected_ < scroll_) scroll_ = selected_;
                return;
            }
            // While actively typing "p:...", the live preview below is
            // already showing playlist_view_ (see update_live_search_preview())
            // -- navigate that instead of local_view_ in that case, same
            // as Enter/submit_search() would commit to.
            size_t list_len = (list_source_ == ListSource::Playlist) ? playlist_view_.size()
                            : (list_source_ == ListSource::Folder) ? folder_view_.size()
                            : local_view_.size();
            if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
            if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
            return;
        }
        edit_focus("browse-search", search_buffer_);
        const bool search_changed = edit_text_key(search_buffer_, edit_caret_, edit_anchor_, key, 120, &status_line_);
        if (search_changed) update_live_search_preview();
        return;
    }

    // Mode::Browse
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size()
                     : (list_source_ == ListSource::Online) ? online_view_.size()
                     : (list_source_ == ListSource::Folder) ? folder_view_.size()
                     : playlist_view_.size();

    // Big list overlay (SHIFT+L): SHIFT+Up/Down turn a whole page, ESC closes
    // it. Matched on the raw arrow codes + the shift flag, like SHIFT+B below
    // -- the input layer reports Shift+Up/Down as a plain 'A'/'B' arrow with
    // last_key_was_shifted() set, so there is no hotkey string for them. With
    // the overlay closed both keys fall through and behave exactly like plain
    // Up/Down, as they always did.
    if (list_overlay_active()) {
        if (last_key_was_arrow() && last_key_was_shifted() && (key == 'A' || key == 'B')) {
            list_overlay_page(key == 'A' ? -1 : 1);
            return;
        }
        if (key == 27) { list_overlay_close(); return; }
    }

    // SHIFT+B -- AcoustID lookup of the hovered title (by audio fingerprint).
    //
    // Deliberately matched on the raw key + last_key_was_arrow() instead of
    // going through settings_.hotkeys: the four arrow keys collapse to the
    // letters 'A'..'D', so a hotkey string of "B" would be VALUE-identical
    // to the Down arrow and would race HKeyNavigateDown inside
    // resolve_hotkey_action()'s unordered_map scan (whichever the hash
    // happened to visit first would win -- a nondeterministic bug), while
    // the lowercase-fallback at the bottom of this chain would otherwise
    // turn Shift+B into "play previous track" ('b'). last_key_was_arrow() is
    // the only thing that can tell "the B key with Shift held" from "Down".
    if (key == 'B' && !last_key_was_arrow()) {
        std::string path;
        if (queue_focus_) {
            if (queue_selected_ >= 0 && queue_selected_ < static_cast<int>(queue_.size()) && queue_[queue_selected_].is_local) {
                path = path_utf8(queue_[queue_selected_].local_path);
            }
        } else if (list_source_ == ListSource::Local && !local_view_.empty() &&
                   selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
            path = path_utf8(local_view_[selected_].path);
        }
        if (path.empty()) {
            status_line_ = "SHIFT+B: only a local file can be fingerprinted";
        } else {
            meta_prompt_single_fetch(path); // asks the AcoustID disclaimer first
        }
        return;
    }

    // Hotkeys are resolved to an action name via settings_.hotkeys /
    // resolve_hotkey_action() instead of switching on the raw key
    // directly, so a rebinding in Settings > Reference (or config.txt)
    // actually changes what a keypress does. This closes a gap that
    // exists in the *original* Linux/macOS codebase too, not something
    // the Windows port introduced: resolve_hotkey_action() was already
    // there, fully implemented, but nothing ever called it -- the
    // switch below was hardcoded on literal characters no matter what
    // config.txt or the Settings UI said. Two small normalizations keep
    // every default binding behaving exactly as it did before this
    // change: CR (13, what Windows' _getch() actually sends for Enter)
    // is treated as LF (10, what hotkey_string_to_key("ENTER") maps to)
    // so Enter keeps working regardless of which one a given terminal
    // reports; and a letter that doesn't resolve on its own also tries
    // its opposite case, so rebinding an action to "n" still fires on
    // Shift+N the same way the old hardcoded "case \'n\': case \'N\':"
    // pairs always did, without having to special-case every letter
    // action individually.
    int lookup_key = (key == '\r') ? '\n' : key;
    std::string action = resolve_hotkey_action(lookup_key);
    if (action.empty() && lookup_key >= 'a' && lookup_key <= 'z') action = resolve_hotkey_action(lookup_key - 32);
    if (action.empty() && lookup_key >= 'A' && lookup_key <= 'Z') action = resolve_hotkey_action(lookup_key + 32);

    if (action == "HKeySetting") {
        mode_ = Mode::Settings;
        settings_tab_ = 0;
        settings_row_ = 0;
        settings_col_ = 0;
    } else if (action == "HKeyPlaylist") {
        playlist_open_editor();
    } else if (action == "HKeyMetaEditor") { // SHIFT+M: edit file names / tags, fetch metadata
        meta_open();
    } else if (action == "HKeyHistory") { // SHIFT+H: listening history (HISTORY / TOP TRACKS / HABITS)
        history_open();
    } else if (action == "HKeyListOverlay") { // SHIFT+L: big list overlay on/off
        if (list_overlay_open_) list_overlay_close(); else list_overlay_open();
    } else if (action == "HKeySwitchBetweenCards") { // Tab: toggle Up/Down + reorder focus between the list and the queue
        // The queue pane is hidden behind the big list overlay, so there is
        // nothing to switch to while it is up.
        if (!list_overlay_active()) queue_focus_ = !queue_focus_;
    } else if (action == "HKeyNavigateUp") {
        if (queue_focus_) {
            if (queue_selected_ > 0) --queue_selected_;
            clamp_queue_selected();
        } else {
            if (selected_ > 0) --selected_;
            if (selected_ < scroll_) scroll_ = selected_;
        }
    } else if (action == "HKeyNavigateDown") {
        if (queue_focus_) {
            if (!queue_.empty() && queue_selected_ < static_cast<int>(queue_.size()) - 1) ++queue_selected_;
            clamp_queue_selected();
        } else {
            if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
            // BUGFIX: selection could move past the visible window without
            // the window ever following it, leaving the highlighted row
            // invisible below row 8 instead of the list scrolling up.
            if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
        }
    } else if (action == "HKeySeekForward") {
        if (has_track_) player_.seek_relative(5.0);
    } else if (action == "HKeySeekBackward") {
        if (has_track_) player_.seek_relative(-5.0);
    } else if (action == "HKeyQueueMoveUp") { // move the hovering queue item up (only meaningful once you've Tab'd into the queue)
        queue_move_hovering(-1);
    } else if (action == "HKeyQueueMoveDown") { // move the hovering queue item down (only meaningful once you've Tab'd into the queue)
        queue_move_hovering(1);
    } else if (action == "HKeyTogglePlayPause") {
        if (has_track_) { if (player_.is_paused()) player_.resume(); else player_.pause(); }
    } else if (action == "HKeyIncreaseVolume") {
        if (has_track_) player_.set_volume(std::min(100, player_.volume() + 5));
    } else if (action == "HKeyDecreaseVolume") {
        if (has_track_) player_.set_volume(std::max(0, player_.volume() - 5));
    } else if (action == "HKeyPlayNextSong") {
        // The queue (if any) takes priority, same as auto-advance-on-
        // finish does, and respects Shuffle/Repeat Queue via
        // play_next_from_queue() -- a manual skip still always actually
        // skips, though: Repeat/Stop only govern *automatic* advance,
        // not an explicit "next" press.
        if (!queue_.empty()) play_next_from_queue();
        else play_relative(1);
    } else if (action == "HKeyPlayPreviousSong") {
        // Relative to what's actually playing (see
        // current_track_list_index()), not the hover cursor. No queue
        // equivalent: a FIFO queue has no well-defined "previous" once
        // an item's been consumed.
        play_relative(-1);
    } else if (action == "HKeyShuffleNext") {
        // Shuffle to a random next track in the current list -- a
        // manual one-off jump, independent of Play Mode
        // (settings_.play_mode). Reuses the same play_relative_random()
        // the automatic Shuffle play mode already calls on auto-advance;
        // unlike PlayNextSong, this does not consult the queue at all --
        // shuffling picks from the browse list on purpose, since the
        // queue is a deliberately ordered, user-built list and jumping
        // it around at random would defeat the point of it.
        play_relative_random();
    } else if (action == "HKeyToggleLyrics") {
        // Toggle the Lyrics Engine without going through Settings >
        // On/Off. Turning it off needs nothing extra -- the panel
        // already checks element_lyrics every frame and falls back to
        // the sphere on its own. Turning it *on* mid-track does need a
        // nudge though: the only other place that starts a fetch is
        // track load (poll_pending_load), so without this, flipping it
        // on here would just sit showing the sphere with no caption
        // until the next track change.
        settings_.element_lyrics = !settings_.element_lyrics;
        if (settings_.element_lyrics && has_track_) {
            std::string artist = (metadata_.artist == "-") ? "" : metadata_.artist;
            last_lyrics_status_.clear(); // fresh 1.75s caption window, not a leftover from before it was off
            launch_lyrics_fetch(metadata_.name, artist, current_path_);
        }
        status_line_ = settings_.element_lyrics ? "lyrics engine: on" : "lyrics engine: off";
    } else if (action == "HKeyAddHoveringSongToQueue") {
        // Add hovering song to queue (List focus) -- or, when the Queue
        // panel itself is focused, there's nothing hovering-in-the-list
        // to add, so it opens the bulk-add panel instead (paste a
        // YouTube playlist link, queue everything in it).
        if (queue_focus_) {
            mode_ = Mode::BulkAdd;
            bulk_add_buffer_.clear();
            bulk_add_results_ready_ = false;
            pending_bulk_add_ = BulkAddResult{};
            bulk_add_selected_.clear();
            bulk_add_cursor_ = 0;
            bulk_add_scroll_ = 0;
            status_line_.clear();
        } else {
            queue_add_selected();
            log_event("added to queue");
        }
    } else if (action == "HKeyClearQueue") { // SHIFT+X: clear the whole queue, after a Yes/No confirmation
        if (queue_.empty()) {
            status_line_ = "queue is already empty";
        } else {
            clear_queue_choice_ = 1; // default to No so a stray Enter never wipes the queue
            mode_ = Mode::ClearQueue;
        }
    } else if (action == "HKeyRemoveHoveringSongFromQueue") {
        queue_remove_hovering();
        log_event("removed from queue");
    } else if (action == "HKeyCyclePlayMode") {
        // Cycle play mode: list -> repeat -> shuffle -> repeat queue ->
        // stop -> list -- one key for all five instead of a separate
        // toggle per mode.
        settings_.play_mode = (settings_.play_mode + 1) % 5;
        {
            // Indexed 0=list,1=repeat,2=shuffle,3=stop,4=repeat queue,
            // matching play_mode's own numbering (not cycle order).
            static const char* mode_names[] = {"list", "repeat", "shuffle", "stop", "repeat queue"};
            log_event(std::string("play mode: ") + mode_names[settings_.play_mode]);
        }
    } else if (action == "HKeyRefreshUi") {
        // Force a full redraw, for when a resize or terminal-session
        // switch raced the render loop and left a torn/stale frame on
        // screen. hard_clear is normally only set on a detected width or
        // mode change; this forces it once unconditionally on the very
        // next frame.
        force_redraw_ = true;
        log_event("ui refreshed");
    } else if (action == "HKeyConsole") {
        mode_ = Mode::Console;
    } else if (action == "HKeyToggleNormalize") { // loudness normalisation on/off, for A/B-ing it by ear
        settings_.normalize = !settings_.normalize;
        player_.set_normalization(settings_.normalize,
                                  static_cast<float>(settings_.normalize_target_lufs),
                                  static_cast<float>(settings_.normalize_max_boost_db));
        std::string msg = settings_.normalize ? "normalize: on" : "normalize: off";
        const float lufs = player_.track_lufs();
        if (settings_.normalize && has_track_ && !std::isnan(lufs)) {
            char buf[96];
            std::snprintf(buf, sizeof buf, " (track %.1f LUFS -> target %.0f, %+.1f dB)",
                          lufs, settings_.normalize_target_lufs,
                          static_cast<double>(settings_.normalize_target_lufs) - lufs);
            msg += buf;
        }
        status_line_ = msg;
        log_event(msg);
    } else if (action == "HKeyToggleMetaOnly") { // SHIFT+N: swap the lists between filename+metadata and metadata-only rows
        settings_.meta_only = !settings_.meta_only;
        std::string msg = settings_.meta_only ? "lists: metadata only (no filename)"
                                              : "lists: filename + metadata";
        status_line_ = msg;
        log_event(msg);
        resort_local_view_keep_selection(); // "title A-Z" follows the displayed title
        force_redraw_ = true;
    } else if (action == "HKeyToggleMute") { // force volume to 0 without touching pause state
        if (!muted_) {
            pre_mute_volume_ = player_.volume();
            player_.set_volume(0);
            muted_ = true;
            log_event("muted");
        } else {
            player_.set_volume(pre_mute_volume_);
            muted_ = false;
            log_event("unmuted");
        }
    } else if (action == "HKeyCheatsheet") {
        mode_ = Mode::Cheatsheet;
    } else if (action == "HKeyFilterForFolder") {
        if (list_source_ == ListSource::Local && !local_view_.empty() &&
            selected_ >= 0 && selected_ < static_cast<int>(local_view_.size())) {
            folder_filter_ = path_utf8(local_view_[selected_].path.parent_path());
            // Filtering by folder also drops back to folder order (T cycles
            // away from it again), so the pane title reads "sort: folder
            // order" and the files sit in the order they are on disk.
            local_sort_mode_ = 0;
            refresh_local_view();
            log_event("filtered: " + path_utf8(path_from_utf8(folder_filter_).filename()));
        }
    } else if (action == "HKeyClearFilter") {
        if (!folder_filter_.empty()) {
            folder_filter_.clear();
            refresh_local_view();
            log_event("filter cleared");
        }
    } else if (action == "HKeyRetryLyrics") { // opens the manual title/artist override form
        if (!settings_.element_lyrics) {
            status_line_ = "lyrics are turned off (Settings > Lyrics Engine)";
        } else if (has_track_) {
            rl_open_from_current_track();
            mode_ = Mode::RetryLyrics;
        }
    } else if (action == "HKeyToggleWaveform") { // toggle waveform style (raw/smooth) directly, without going into Settings
        settings_.waveform_smooth = !settings_.waveform_smooth;
        recompute_waveform_for_current_track();
        log_event(settings_.waveform_smooth ? "waveform: smooth" : "waveform: raw");
    } else if (action == "HKeyDownloadStream") { // save cached stream to the configured download folder
        if (has_track_) {
            if (path_utf8(current_path_).find(".cache") != std::string::npos || metadata_.location == "youtube") {
                // Same folder the DOWNLOAD FOLDER setting promises everywhere
                // else (see load_library()'s cache-dir injection and the
                // Settings > ON/OFF tab's Download Folder field): the
                // configured path, or ~/.cache/mousiki when none is set --
                // never settings_.local_music_paths[0]/$HOME/Music, which
                // this used to fall back to and had nothing to do with the
                // folder the user actually configured for downloads.
                std::string dest_dir = path_utf8(cache_.download_dir());
                std::error_code ec;
                fs::create_directories(path_from_utf8(dest_dir), ec);

                std::string safe_name = metadata_.name;
                for (char& c : safe_name) if (c == '/' || c == '\\') c = '_';
                std::string safe_artist = (metadata_.artist == "-" ? "" : metadata_.artist);
                for (char& c : safe_artist) if (c == '/' || c == '\\') c = '_';

                std::string filename = safe_artist.empty() ? safe_name : safe_name + " - " + safe_artist;
                filename += path_utf8(current_path_.extension());

                fs::path dest_path = path_from_utf8(dest_dir) / path_from_utf8(filename);
                if (fs::exists(dest_path, ec)) {
                    status_line_ = "already saved: " + path_utf8(dest_path.filename());
                } else {
                    fs::copy_file(current_path_, dest_path, fs::copy_options::overwrite_existing, ec);
                    if (!ec) {
                        fs::remove(current_path_, ec);
                        current_path_ = dest_path; // update so sidecar lyrics go to the new folder
                        metadata_.location = dest_dir;
                        status_line_ = "saved to " + path_utf8(dest_path);
                        refresh_local_view();
                    } else {
                        status_line_ = "failed to save: " + ec.message();
                    }
                }
            } else {
                status_line_ = "not a cached stream";
            }
        }
    } else if (action == "HKeyCycleSortMode") { // cycle local-list sort mode (folder order -> title A-Z -> artist A-Z)
        local_sort_mode_ = (local_sort_mode_ + 1) % 3;
        refresh_local_view();
        log_event(std::string("sort: ") + sort_mode_name(local_sort_mode_, settings_.meta_only));
    } else if (action == "HKeyPlay") {
        play_selected();
    } else if (action == "HKeySearch") {
        mode_ = Mode::Search;
        search_buffer_.clear();
        pre_search_list_source_ = list_source_;
        pre_search_local_query_ = last_local_query_;
    } else if (action == "HKeyQuit") {
        quit_ = true;
    } else if (key == 27) {
        // ESC -- back to the home view: full local library, no filter,
        // from the top. Same destination regardless of how buried you
        // are (mid search results, viewing online results, scrolled
        // deep into the list). Not in settings_.hotkeys / kRefRows at all,
        // on purpose: this mirrors the original, which likewise has no
        // HKeyEsc entry -- ESC is a fixed shortcut, not something meant
        // to be rebound (see the SYSTEM (MAIN UI) section of
        // the cheat sheet, '?', which is where fixed keys like this one
        // are documented -- the Reference tab only lists rebindable ones).
        list_source_ = ListSource::Local;
        last_local_query_.clear();
        folder_filter_.clear();
        refresh_local_view();
        status_line_.clear();
    }
}

// ---------------------------------------------------------------------
// Lazy metadata probing for whatever's currently visible in the list
// ---------------------------------------------------------------------

// Tries to fully resolve a row -- duration AND tags -- using nothing but
// in-process header parsing (native_duration.h): no subprocess, safe to
// call from the render thread. For a well-formed tag in a format this
// covers (MP3/ID3v2, FLAC/Ogg/Opus's Vorbis comments, or M4A/MP4/AAC's
// iTunes-style atoms -- covers every format probe_duration_native()
// already parses for duration) this is a complete answer on its own. For
// anything it can't handle (an unsupported/unusual tag layout, or a
// container this doesn't recognize at all), it comes back with whatever
// duration native parsing found, tags_resolved left false -- callers fall
// back to ffprobe for tags in that case.
//
// This is what turns "resolving an 800-track library's metadata" from
// hundreds of ffprobe subprocess spawns (the actual bottleneck -- each one
// costs tens to hundreds of milliseconds, worse under antivirus real-time
// scanning on Windows) into a few pread() syscalls per file for the
// overwhelming majority of a typical library, whatever mix of formats it's in.
RowMeta try_native_row_meta(const fs::path& path) {
    RowMeta rm;
    uint32_t dur = probe_duration_native(path);
    if (dur > 0) rm.duration_sec = static_cast<double>(dur);

    std::string ext = lower(path_utf8(path.extension()));
    NativeId3Tags tags;
    if (ext == ".mp3") tags = probe_id3v2_native(path);
    else if (ext == ".flac") tags = probe_flac_native(path);
    else if (ext == ".ogg" || ext == ".opus") tags = probe_ogg_native(path);
    else if (ext == ".m4a" || ext == ".mp4" || ext == ".aac") tags = probe_mp4_native(path);

    if (tags.resolved) {
        rm.title = tags.title;
        rm.artist = tags.artist;
        rm.album = tags.album;
        rm.year = tags.year; // TYER/TDRC (MP3), DATE (Vorbis), ©day (MP4)
        rm.tags_resolved = true; // fully resolved without ffprobe
    }
    return rm;
}

// This used to call probe_row_meta() here directly — which spawns a real
// ffprobe SUBPROCESS, synchronously, on the render thread, once per
// newly-visible row, every single frame a new row scrolled into view.
// Scroll through a big library quickly (or hit one slow/hanging file —
// weird encode, flaky storage) and the whole UI thread — rendering AND
// input — blocks for however long those subprocess calls take, with no
// way to even press 'q' to get out of it. That's what was crashing the
// whole player.
//
// Fix: use the native in-process binary-header parser (native_duration.h
// — pure pread() syscalls, no subprocess, effectively can't hang) for
// duration AND (for MP3) tags here, directly on the render thread —
// genuinely safe now. Anything that parser can't handle (unsupported
// format, corrupt file, a tag layout probe_id3v2_native() declines) gets
// picked up by the background sweep (launch_row_meta_resolver, started
// once after the initial scan) that walks the whole library and falls
// back to ffprobe there — off the main thread entirely, never gating
// rendering or input.
void App::ensure_visible_row_meta() {
    if (list_source_ != ListSource::Local) return;
    for (int i = scroll_; i < std::min(static_cast<int>(local_view_.size()), scroll_ + list_nav_rows()); ++i) {
        const auto& t = local_view_[i];
        std::string key = path_utf8(t.path);
        {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            if (row_meta_cache_.count(key)) continue; // already resolved (native path or background sweep)
        }
        RowMeta rm = try_native_row_meta(t.path);
        if (rm.duration_sec > 0 || rm.tags_resolved) {
            std::lock_guard<std::mutex> lk(row_meta_mutex_);
            // BUG FIX #1: cap the cache so it can't grow proportionally
            // to an arbitrarily large library (a 50k-track collection
            // would otherwise accumulate tens of MB that are never freed).
            // BUG FIX #4: the cap must only ever block a brand-new key --
            // `size()` doesn't change when overwriting a key already in the
            // map, so once the map filled up to exactly the cap, the old
            // "size() < cap" check also silently blocked ever promoting an
            // existing duration-only entry to include real tags, for any
            // file whose key already happened to be present. That made
            // metadata search permanently stop updating past whatever
            // point the cache first filled up.
            if (row_meta_cache_.count(key) || row_meta_cache_.size() < 4096)
                row_meta_cache_[key] = rm;
        }
        // A visible row whose tags resolved right here (native, no ffprobe
        // needed) still needs the version bump: this row scrolling into
        // view generally happens well before the background sweep ever
        // gets to it, so without this the same "search doesn't notice
        // metadata that arrived after the filter ran" gap would just
        // reappear for the fast native path instead of the slow ffprobe
        // one. See poll_pending_row_meta_tags().
        if (rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
        // else: leave unresolved — native parsing is cheap enough to just
        // retry next frame, and the background sweep will fill it in via
        // ffprobe regardless, so there's no real cost to not caching a miss.
    }
}

// Re-runs quantization on the already-decoded PCM for whatever's
// currently playing — used when the smooth/raw toggle changes mid-track,
// so the effect shows up right away instead of only on the next track.
// No re-decode needed: current_pcm_ already holds everything decoded so
// far (streaming decode may still be filling it in, hence the acquire
// load of `available` rather than assuming it's complete).
void App::recompute_waveform_for_current_track() {
    if (!has_track_ || !current_pcm_) return;
    std::shared_ptr<StreamingPcm> pcm = current_pcm_;
    bool smooth = settings_.waveform_smooth;
    // BUG FIX #5: increment epoch before spawning — any already-running
    // waveform thread will see its own epoch is stale and discard its
    // result instead of racing to overwrite pending_waveform_envelope_.
    int my_epoch = ++waveform_epoch_;
    std::thread([this, pcm, smooth, my_epoch]() { run_guarded("waveform pass", [&] {
        size_t n = pcm->available.load(std::memory_order_acquire);
        if (n == 0) return;
        // `n` counts frames; the buffer is interleaved, so copy n * channels floats.
        std::vector<float> snapshot(pcm->data.begin(),
                                    pcm->data.begin() + static_cast<long>(n * static_cast<size_t>(pcm->channels)));
        auto envelope = WaveformQuantizer::generate_high_res_envelope(snapshot, 4096, smooth, pcm->channels);
        std::lock_guard<std::mutex> lk(waveform_mutex_);
        if (my_epoch != waveform_epoch_.load()) return; // superseded — discard
        pending_waveform_envelope_ = std::move(envelope);
        waveform_pending_ready_ = true;
    }); }).detach();
}

void App::launch_row_meta_resolver() {
    // One sweep per session -- except that rescan_library() clears the
    // flag when the configured path set changes, which is the one
    // situation where files this sweep has already passed (or never saw)
    // can appear.
    if (row_meta_resolver_started_.exchange(true)) return;
    auto paths = std::make_shared<std::vector<fs::path>>();
    paths->reserve(all_local_tracks_.size());
    for (auto& t : all_local_tracks_) paths->push_back(t.path);

    // BUG FIX #5: this used to be a single thread working through the
    // whole library one ffprobe subprocess at a time. Each call is a real
    // process spawn -- slow, and especially so on Windows (process
    // creation there routinely runs tens of milliseconds even before
    // antivirus real-time scanning gets a look at ffprobe.exe, which adds
    // more on top). Sequentially, that means a library of even a few
    // hundred tracks can take a long time to fully resolve, and a file
    // that only happens to sit later in scan order is simply unresolved
    // -- and therefore not yet findable by its tags -- for that entire
    // stretch. A small fixed pool of worker threads pulling from a shared
    // index lets several ffprobe calls be in flight at once (this is
    // I/O/process-spawn-bound work, not CPU-bound, so a handful of
    // threads is a real speedup and not just contention), cutting the
    // time-to-fully-searchable by roughly the pool size with no change to
    // per-file behavior.
    constexpr int kResolverWorkers = 4;
    auto next_index = std::make_shared<std::atomic<size_t>>(0);

    for (int worker = 0; worker < kResolverWorkers; ++worker) {
        std::thread([this, paths, next_index]() { run_guarded("library metadata sweep", [&] {
            for (;;) {
                size_t i = next_index->fetch_add(1, std::memory_order_relaxed);
                if (i >= paths->size()) return;
                const fs::path& p = (*paths)[i];
                std::string key = path_utf8(p);
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(key);
                    // BUG FIX #2: a cache hit here used to be treated as "this
                    // file is done", which was true back when the cache only
                    // ever held a duration. Now ensure_visible_row_meta() can
                    // populate an entry with JUST duration_sec (from the cheap
                    // native header parser, no tags) before this sweep ever
                    // reaches the file -- for any file with a well-formed
                    // MP3/FLAC/etc. header, that's the common case, since the
                    // render thread visits it first. Skipping here on presence
                    // alone meant that entry's artist/title/album tags -- the
                    // ones search actually needs -- never got fetched, because
                    // this is the only place that runs the ffprobe tag lookup.
                    // Only a real prior probe_row_meta() result (tags_resolved)
                    // means there's genuinely nothing left to fetch.
                    if (it != row_meta_cache_.end() && it->second.tags_resolved) continue;
                }
                RowMeta rm = try_native_row_meta(p);
                // BUG FIX #6 / perf: try the native, no-subprocess ID3v2
                // reader first. For a well-formed MP3 tag -- the common
                // case in any real library -- this resolves the file with
                // a few pread() calls instead of a whole ffprobe process
                // spawn, which is what actually made an 800-track library
                // take minutes: ffprobe's per-call overhead (worse under
                // Windows antivirus real-time scanning) dominates
                // completely once you're spawning hundreds of them. Only
                // fall back to ffprobe for whatever native declined --
                // a non-MP3 format, or an MP3 tag laid out in a way the
                // lightweight reader won't guess at (see native_duration.h).
                if (!rm.tags_resolved) {
                    RowMeta ff = probe_row_meta(p);
                    if (rm.duration_sec <= 0) rm.duration_sec = ff.duration_sec; // keep native's duration if we already had it
                    rm.title = ff.title;
                    rm.artist = ff.artist;
                    rm.album = ff.album;
                    rm.tags_resolved = ff.tags_resolved;
                }
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    // BUG FIX #4: only a brand-new key is subject to the cap --
                    // overwriting a key already present doesn't grow the map,
                    // so it must never be blocked by "already at the cap", or
                    // no file already in the cache could ever be promoted from
                    // a duration-only entry to one with real tags once the
                    // cache first filled up.
                    if (row_meta_cache_.count(key) || row_meta_cache_.size() < 4096) // BUG FIX #1
                        row_meta_cache_[key] = rm;
                }
                // BUG FIX #3: tell the main thread real tags for a file just
                // landed. Without this, a search typed (or already showing)
                // before this sweep reached the file stays frozen on whatever
                // it found at the time -- the file becomes searchable by its
                // metadata from this point on, but nothing ever re-runs the
                // filter to notice, so it looks like metadata search "doesn't
                // work" for exactly the files whose tags resolve after the
                // fact. poll_pending_row_meta_tags() (called every frame,
                // same as the other poll_pending_* functions) picks this up.
                if (rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
            }
        }); }).detach();
    }
}

// Every frame's counterpart to launch_row_meta_resolver(): re-filters the
// currently-shown local list once new tags have arrived in the background,
// so a metadata match (like an artist tag) shows up on its own instead of
// requiring the user to retype the query after the sweep happens to catch
// up. A no-op the vast majority of frames (the version check is a single
// relaxed atomic load), so this is cheap to call unconditionally.
void App::poll_pending_row_meta_tags() {
    uint64_t v = row_meta_tags_version_.load(std::memory_order_relaxed);
    if (v == row_meta_tags_seen_) return;
    row_meta_tags_seen_ = v;

    if (list_source_ != ListSource::Local) return; // nothing local is even on screen right now

    // Mirror update_live_search_preview()'s "which query is live right now"
    // logic: mid-typing uses search_buffer_, otherwise the last committed
    // query. Either way, leave "s:"/"p:" alone -- those aren't local
    // filters and don't read row_meta_cache_ at all.
    std::string q = (mode_ == Mode::Search) ? search_buffer_ : last_local_query_;
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    while (!q.empty() && q.back() == ' ') q.pop_back();
    if (q.size() >= 2) {
        std::string prefix = lower(q.substr(0, 2));
        if (prefix == "s:" || prefix == "p:") return;
    }

    // Re-filtering can reorder/shrink the list (a track that just became a
    // metadata match can appear anywhere by rank), so re-anchor on the
    // previously-selected track's identity rather than leaving `selected_`
    // pointing at whatever index now happens to sit there.
    fs::path prev_selected_path;
    bool had_selection = selected_ >= 0 && selected_ < static_cast<int>(local_view_.size());
    if (had_selection) prev_selected_path = local_view_[static_cast<size_t>(selected_)].path;

    local_view_ = filter_and_rank_local_view(q);

    if (had_selection) {
        selected_ = 0;
        for (size_t i = 0; i < local_view_.size(); ++i) {
            if (local_view_[i].path == prev_selected_path) { selected_ = static_cast<int>(i); break; }
        }
    }
    if (local_view_.empty()) selected_ = 0;
    else if (selected_ >= static_cast<int>(local_view_.size())) selected_ = static_cast<int>(local_view_.size()) - 1;
    if (scroll_ > selected_) scroll_ = selected_;
    if (selected_ >= scroll_ + list_nav_rows()) scroll_ = selected_ - list_nav_rows() + 1;
}

// ---------------------------------------------------------------------
// Idle-state cassette picture
// ---------------------------------------------------------------------
// Braille cassette shown in the metadata panel's lyrics/sphere column
// whenever no track is loaded (never started, playback stopped in "stop"
// mode, or the file that was playing got deleted). Byte-for-byte copy of
// tape_ascii.txt: 15 rows of 30 braille cells -- deliberately the same
// geometry as the rotating disk (disk_art.cpp), so it fills that column at
// exactly the panel's size and can be tinted with the same top-to-bottom
// disk gradient. See the !has_track_ branch of build_metadata_panel().
//
// The characters are written as \uXXXX escapes so this file stays pure
// ASCII: no editor, copy or checkout can re-encode them, while /utf-8
// still compiles them into exactly the same UTF-8 bytes.
static constexpr const char* kTapeArt[] = {
    "\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2880\u2864\u28c4\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2880\u28c0\u28e4\u28c0\u28c0\u2800\u2800\u2880\u2840\u2814\u2801\u2800\u28b8\u2803\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2820\u2802\u2809\u2809\u2809\u2811\u28b2\u28fe\u2849\u2800\u2800\u2800\u2800\u2808\u2801\u2800\u2800\u28c0\u2814\u2802\u2801\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2847\u2800\u2800\u2800\u2800\u2800\u28b8\u2800\u2809\u28a6\u2800\u2800\u2800\u2800\u2800\u2800\u28b0\u2801\u2800\u2800\u2800\u2880\u28c0\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2810\u28c4\u2800\u2800\u2800\u2800\u2808\u2823\u2804\u280a\u2800\u28c0\u28c0\u28e4\u28e4\u28f4\u28fe\u28f6\u28ff\u28ff\u28ff\u28ff\u28ff\u2847\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2808\u28f3\u28c4\u28e0\u28e4\u28f4\u28f6\u28f6\u28ff\u28ff\u28bf\u28df\u28ef\u28bf\u28dd\u28af\u2877\u28fb\u28be\u28df\u28ff\u28fb\u28ff\u2800\u2800\u2800\u2800",
    "\u28e4\u28f6\u28f6\u28fe\u28ff\u28bf\u28fb\u289f\u28ef\u28bf\u2875\u28fb\u28ae\u283f\u28ee\u28b7\u28ef\u28fb\u28fd\u286b\u28bf\u289d\u286b\u285b\u28cd\u28bf\u2846\u2800\u2800\u2800",
    "\u28bb\u28fe\u28ff\u28ff\u28ef\u28b7\u28ef\u287b\u28f7\u28eb\u285f\u287d\u28cf\u287b\u2873\u286d\u286a\u28d6\u289c\u28ce\u28b3\u289d\u2854\u286e\u28d5\u283d\u28f7\u2800\u2800\u2800",
    "\u2818\u28ff\u283f\u28de\u289b\u2873\u286d\u28f9\u28a2\u28a7\u2879\u28ea\u28ce\u28de\u283c\u282e\u28be\u287e\u28de\u285b\u281a\u2897\u28dd\u28ae\u286a\u2873\u28fb\u2844\u2800\u2800",
    "\u2800\u28bf\u2857\u2875\u28d9\u288e\u285e\u289c\u2837\u28bb\u28ef\u287b\u28de\u28b7\u28a9\u28a3\u28b9\u287e\u28ef\u2866\u2820\u28ed\u2897\u28b5\u2839\u285c\u287d\u28e7\u2800\u2800",
    "\u2800\u2838\u28df\u28f2\u2809\u28de\u28ba\u28eb\u2840\u28c0\u28bf\u28dd\u28de\u287f\u2874\u2875\u2873\u283b\u280d\u281b\u2899\u2801\u2829\u2800\u2804\u2800\u28b9\u28bf\u2840\u2800",
    "\u2800\u2800\u28ff\u28d5\u289d\u2855\u28c7\u283b\u2832\u281a\u280b\u288a\u2808\u28ec\u28c0\u28e4\u2802\u2880\u2801\u2800\u2804\u2800\u28c2\u28c0\u28ec\u28e4\u28fc\u285d\u2847\u2800",
    "\u2800\u2800\u28b8\u28ef\u280e\u2880\u2800\u28b2\u2866\u2815\u2800\u2804\u2800\u28c3\u28c8\u28ed\u28e4\u28f4\u28f6\u28fe\u28ff\u283f\u283f\u281f\u281b\u280b\u2809\u2809\u2800\u2800",
    "\u2800\u2800\u2800\u28ff\u28f3\u28c0\u28ec\u28e4\u28f4\u28f6\u28fe\u28ff\u283f\u283f\u281f\u281b\u280b\u2809\u2809\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
    "\u2800\u2800\u2800\u2839\u283d\u281f\u281b\u280b\u2809\u2809\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800\u2800",
};

static constexpr int kTapeArtRows = 15;   // == rows in tape_ascii.txt
static constexpr int kTapeArtWidth = 30;  // == cells per row == disk width

// ---------------------------------------------------------------------
// Panel builders
// ---------------------------------------------------------------------

std::vector<std::string> App::build_metadata_panel(int total_width) const {
    const int inner = total_width - 4;
    const int disk_w = settings_.element_disk ? disk_.width() : 0;
    const int panel_h = disk_.height();
    
    std::string sep = "  " + settings_.meta_separator + "  ";
    int sep_w = display_width(sep);
    const int fixed_extra = settings_.element_disk ? sep_w : 2;

    int avail = std::max(10, inner - disk_w - fixed_extra);
    // Previously this split only happened when settings_.element_lyrics was
    // true; with it off, lyrics_w stayed 0 and meta_w took the whole
    // panel, so turning the Lyrics Engine off silently also gave up the
    // sphere visualization that normally fills this column while nothing
    // is playing lyrics -- the panel just went from "sphere" to "wide
    // plain metadata" instead of staying visually alive. The split is now
    // unconditional; settings_.element_lyrics only gates whether lyrics
    // are fetched/shown as text (below), not whether this column exists.
    int meta_w = std::min(42, std::max(10, avail - 10));
    meta_w = std::min(meta_w, avail);
    int lyrics_w = std::max(0, avail - meta_w);

    std::vector<std::string> disk_frame;
    if (settings_.element_disk) {
        disk_frame = disk_.frame(angle_);
        while (static_cast<int>(disk_frame.size()) < panel_h) disk_frame.emplace_back(std::string(disk_w, ' '));
        for (size_t row_i = 0; row_i < disk_frame.size(); ++row_i) {
            float t = disk_frame.size() > 1 ? static_cast<float>(row_i) / static_cast<float>(disk_frame.size() - 1) : 0.0f;
            std::string disk_end = settings_.disk_color_end;
            std::string row_ansi = gradient_ansi(settings_.disk_color, disk_end, t);
            disk_frame[row_i] = row_ansi + disk_frame[row_i] + "\x1b[0m";
        }
    }

    double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    fft_.set_fluidity(settings_.visualizer_fluidity);
    fft_.set_degradation_speed(settings_.visualizer_degradation_speed);
    fft_.set_viscosity(settings_.visualizer_viscosity);

    // meta content rows
    std::vector<std::string> meta_rows(panel_h, std::string());
    bool viz_rows_colored = false;
    std::vector<int> bars; // computed once below, reused by the sphere visualizer fallback further down
    if (has_track_) {
        int meta_row_cursor_ = 1; // row 0 stays blank: the metadata block starts below the panel's top edge (the idle message is centred in the column instead)
        std::string k_col = settings_.meta_key_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_key_color);
        std::string v_col = settings_.meta_val_color.empty() ? ansi_for(settings_.list_color) : ansi_for(settings_.meta_val_color);
        auto kv = [&](const std::string& label, const std::string& value, int max_lines = 1) {
            std::string mapped_label = apply_font_map(label, settings_.font_map);
            std::string mapped_val = apply_font_map(value, settings_.font_map);
            int avail_v = std::max(0, meta_w - 12);
            std::string l_pad = pad_right(mapped_label, 10);

            // How many rows are actually free before the visualizer's
            // fixed bottom two rows -- so a long Name/Artist/Location
            // can spill into the panel's spare rows without ever
            // overwriting the spectrum, however many fields are above it.
            int room = std::max(1, (panel_h - 2) - meta_row_cursor_);
            std::vector<std::string> value_lines = wrap_lines(mapped_val, avail_v, std::min(max_lines, room));
            if (value_lines.empty()) value_lines.push_back(std::string());

            for (size_t li = 0; li < value_lines.size(); ++li) {
                if (meta_row_cursor_ >= panel_h) break; // no room left at all; drop silently rather than corrupt later rows
                const std::string& v_tr = value_lines[li];
                bool first = (li == 0);
                // Continuation lines repeat the label column as blank
                // space (not the colon) so the wrapped text lines up
                // directly under where the value on line one starts.
                std::string label_col = first ? l_pad : std::string(10, ' ');
                std::string sep_txt = first ? ": " : "  ";
                std::string plain = label_col + sep_txt + v_tr;
                std::string ansi = first
                    ? (k_col + label_col + "\x1b[0m" + sep_txt + v_col + v_tr + "\x1b[0m")
                    : (label_col + sep_txt + v_col + v_tr + "\x1b[0m");
                ansi += std::string(std::max(0, meta_w - display_width(plain)), ' ');
                meta_rows[meta_row_cursor_++] = ansi;
            }
        };
        // Name and Location are the two fields most likely to overrun a
        // single line (long track titles; deep folder paths); Artist can
        // too for multi-artist collabs. Everything else is short enough
        // in practice that one line is always enough.
        kv("Name", metadata_.name, 3);
        kv("Artist", metadata_.artist, 2);
        kv("Year", metadata_.year);
        kv("Sampling", metadata_.sampling);
        kv("Type", metadata_.type);
        kv("Format", metadata_.format);
        kv("File size", metadata_.file_size);
        kv("Location", metadata_.location, 2);
        if (!metadata_.extra_label.empty()) kv(metadata_.extra_label, metadata_.extra_value);

        // Real spectrum visualizer (KISS FFT), not a copy of the progress
        // bar's RMS envelope. Two rows: bottom row is the base level
        // (0-4), top row is whatever's left over above that (0-4) so
        // taller peaks build upward — attached to the panel's bottom row
        // per instruction, with the second row directly above it.
        if (settings_.element_visualizer) {
            int viz_w = std::min(meta_w, 48);
            bars = fft_.compute_bars(viz_w, viz_dt_);
            std::string viz_top, viz_bottom;
            int nbars = static_cast<int>(bars.size());
            for (int i = 0; i < nbars; ++i) {
                int level = bars[i];
                float t = nbars > 1 ? static_cast<float>(i) / static_cast<float>(nbars - 1) : 0.0f;
                std::string bar_ansi;
                if (!settings_.viz_center_color.empty()) {
                    bar_ansi = multi_stop_gradient_ansi(settings_.viz_left_color, settings_.viz_center_color, settings_.viz_right_color, t);
                } else {
                    bar_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, t);
                }
                viz_bottom += bar_ansi;
                viz_bottom += fft_glyph(std::min(level, 4));
                viz_top += bar_ansi;
                viz_top += fft_glyph(std::max(0, level - 4));
            }
            viz_top += "\x1b[0m";
            viz_bottom += "\x1b[0m";
            if (viz_w < meta_w) {
                std::string tail(meta_w - viz_w, ' ');
                viz_top += tail;
                viz_bottom += tail;
            }
            meta_rows[panel_h - 2] = viz_top;
            meta_rows[panel_h - 1] = viz_bottom;
            viz_rows_colored = true;
        } else {
            bars = fft_.compute_bars(48, viz_dt_);
        }
    } else {
        // Nothing loaded -- never started, playback stopped with no track,
        // or the file that was playing got deleted. The message sits centred
        // (both ways) in the metadata/visualizer column instead of being
        // glued to its first row, where it used to hide behind the disk.
        meta_rows[std::max(0, panel_h / 2)] = center_pad("Currently No Track Loaded", meta_w);
    }
    for (int i = 0; i < static_cast<int>(meta_rows.size()); ++i) {
        if (meta_rows[i].empty()) {
            meta_rows[i] = std::string(meta_w, ' ');
        }
        // Every other row (kv data, visualizer, and the centred idle message
        // above -- center_pad() already pads that one to meta_w) was padded
        // by its own builder, and padding it again would miscount its ANSI
        // color escapes as visible columns, truncating it.
    }

    // lyrics window: word-wrapped, center-aligned, word-level highlight on
    // the active line — windowed so the active line's wrapped block is
    // always vertically centered, blank-padded at the edges.
    std::vector<std::string> lyric_rows(panel_h, std::string(lyrics_w, ' '));
    bool lyrics_avail = false;
    std::vector<LyricLine> lines_copy;
    std::string lyrics_status;
    {
        std::lock_guard<std::mutex> lock(lyrics_mutex_);
        if (settings_.element_lyrics && lyrics_ready_) {
            lines_copy = lyrics_result_.lines;
            lyrics_status = lyrics_result_.message;
            lyrics_avail = !lines_copy.empty();
        } else if (settings_.element_lyrics && has_track_) {
            lyrics_status = "fetching lyrics ...";
        }
        // else: Lyrics Engine is off -- no fetch ever ran, so there's
        // nothing to report. Leaving lyrics_status empty means the sphere
        // below renders with no caption at all, rather than a stale or
        // misleading status line.
    }

    if (!has_track_) {
        // Nothing is playing, so there is no sphere and no lyric line to draw
        // and this column would just be empty. Fill it with the braille
        // cassette picture (kTapeArt above -- 15 rows of 30 cells, the disk's
        // own dimensions), centred in the column and tinted with the SAME
        // top-to-bottom disk gradient the rotating disk gets, so the idle
        // panel keeps the colour it has while playing.
        int start = std::max(0, (panel_h - kTapeArtRows) / 2);
        for (int i = 0; i < kTapeArtRows && start + i < panel_h; ++i) {
            float t = panel_h > 1 ? static_cast<float>(start + i) / (panel_h - 1) : 0.0f;
            int left = (lyrics_w - kTapeArtWidth) / 2;
            std::string body = (left >= 0)
                ? std::string(left, ' ') + kTapeArt[i]
                  + std::string(lyrics_w - left - kTapeArtWidth, ' ')
                : utf8_skip_take(kTapeArt[i], -left, lyrics_w); // column narrower than the art: keep its middle, never wrap braille cells
            lyric_rows[start + i] =
                gradient_ansi(settings_.disk_color, settings_.disk_color_end, t) + body + "\x1b[0m";
        }
    } else if (!lyrics_avail) {
        // A status message ("fetching...", "no lyrics found", etc.) is
        // only shown for the first 1.75s after it appears -- after that
        // the sphere gets the whole panel to itself instead of a
        // permanently stuck caption line. Each distinct message content
        // gets its own fresh window (so "fetching..." showing, then
        // later "no lyrics found", each get their moment) rather than
        // one timer for the whole track.
        if (lyrics_status != last_lyrics_status_) {
            last_lyrics_status_ = lyrics_status;
            lyrics_status_shown_at_ = std::chrono::steady_clock::now();
        }
        double status_age = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - lyrics_status_shown_at_).count();
        bool show_caption = status_age < 1.75 && !lyrics_status.empty();

        if (has_track_ && lyrics_w >= 6 && panel_h >= 3 && settings_.element_lyrics_placeholder_ball) {
            // Fill the panel with the audio-reactive sphere instead of
            // leaving it blank — reuses `bars` (already computed for the
            // main spectrum strip above, same frame) rather than running
            // a second independent audio analysis.
            int sphere_rows_h = show_caption ? panel_h - 1 : panel_h;
            auto sphere_rows = sphere_.render(lyrics_w, sphere_rows_h, bars, viz_dt_);
            std::string sphere_ansi = gradient_ansi(settings_.visualizer_color, settings_.visualizer_color_end, 0.5f);
            for (int i = 0; i < static_cast<int>(sphere_rows.size()) && i < panel_h; ++i) {
                lyric_rows[i] = sphere_ansi + pad_right(sphere_rows[i], lyrics_w) + "\x1b[0m";
            }
            if (show_caption) {
                int pad = std::max(0, (lyrics_w - display_width(lyrics_status)) / 2);
                lyric_rows[panel_h - 1] = pad_right(std::string(pad, ' ') + lyrics_status, lyrics_w);
            }
        } else if (show_caption) {
            int pad = std::max(0, (lyrics_w - display_width(lyrics_status)) / 2);
            lyric_rows[0] = pad_right(std::string(pad, ' ') + lyrics_status, lyrics_w);
        }
    } else {
        int active = 0;
        for (size_t i = 0; i < lines_copy.size(); ++i) {
            if (lines_copy[i].start_time <= elapsed) active = static_cast<int>(i);
            else break;
        }

        if (settings_.lyrics_animation == 4) {
            // Only active word: show nothing but whichever single word is
            // currently being sung, centered alone in the panel. Falls
            // back to the whole line if this song has no word-level sync
            // data at all (nothing finer to show).
            const LyricLine& al = lines_copy[active];
            std::string word = al.full_text;
            if (!al.words.empty()) {
                word = al.words.front().second;
                for (const auto& wt : al.words) if (wt.first <= elapsed) word = wt.second; // last one <= elapsed
            }
            word = apply_font_map(word, settings_.font_map);
            int pad = std::max(0, (lyrics_w - display_width(word)) / 2);
            std::string line = std::string(pad, ' ') + word;
            std::string colored = ansi_for(settings_.active_word_color) + pad_right(truncate_str(line, lyrics_w), lyrics_w) + "\x1b[0m";
            lyric_rows[panel_h / 2] = colored;
        } else if (settings_.lyrics_animation == 3) {
            // Only active line: same per-word rendering as the default
            // view, just without the scrolling context lines around it.
            auto wrapped = render_lyric_line_wrapped(lines_copy[active], elapsed, lyrics_w, true, settings_);
            int start_row = std::max(0, (panel_h - static_cast<int>(wrapped.size())) / 2);
            for (size_t i = 0; i < wrapped.size() && start_row + static_cast<int>(i) < panel_h; ++i) {
                lyric_rows[start_row + i] = wrapped[i];
            }
        } else {
            // Full (default) and Word-by-word/Letter-by-letter (which only
            // change render_lyric_line_wrapped's *content*, not this
            // scrolling layout) all share the same multi-line context view.
            //
            // Only wrap lines actually near the visible window — wrapping
            // the whole song every frame would be wasted work.
            int context = 6;
            int lo = std::max(0, active - context);
            int hi = std::min(static_cast<int>(lines_copy.size()) - 1, active + context);

            std::vector<std::string> flat_rows;
            int active_row_start = 0, active_row_count = 1;
            for (int li = lo; li <= hi; ++li) {
                auto wrapped = render_lyric_line_wrapped(lines_copy[li], elapsed, lyrics_w, li == active, settings_);
                if (li == active) {
                    active_row_start = static_cast<int>(flat_rows.size());
                    active_row_count = static_cast<int>(wrapped.size());
                }
                for (auto& r : wrapped) flat_rows.push_back(std::move(r));
            }

            int active_mid = active_row_start + active_row_count / 2;
            int start = active_mid - panel_h / 2;
            for (int row = 0; row < panel_h; ++row) {
                int idx = start + row;
                if (idx >= 0 && idx < static_cast<int>(flat_rows.size())) lyric_rows[row] = flat_rows[idx];
            }
        }
    }

    // --- assemble bordered block ---
    // NOTE: lyric_rows may contain ANSI color codes (word-highlighting),
    // so this assembles rows by direct concatenation of pre-padded pieces
    // rather than routing through box_line()/pad_right() — those count
    // UTF-8 codepoints for width, and ANSI escape bytes would be
    // miscounted as visible columns, throwing off alignment. Every piece
    // here (disk_frame/meta_rows/lyric_rows) is already padded to its own
    // exact width, so the concatenation is guaranteed to equal `inner`.
    std::vector<std::string> out;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    out.push_back(box_top("", total_width, border_ansi));

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::string sep_ansi = ansi_for(settings_.border_color, false) + sep + "\x1b[0m";
    for (int row = 0; row < panel_h; ++row) {
        std::string content = "";
        if (settings_.element_disk) {
            content += disk_frame[row] + sep_ansi;
        } else {
            content += "  ";
        }
        content += meta_rows[row];
        // lyrics_w is now always reserved (see the split above), and
        // lyric_rows is always fully padded to it -- whether that's
        // actual synced lyrics, a status caption, or just the sphere --
        // so this no longer needs to be conditional on element_lyrics.
        content += lyric_rows[row];
        out.push_back(bar + " " + content + " " + bar);
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_progress_panel(int total_width) const {
    const int button_content_w = 9;
    const int button_total_w = button_content_w + 4; // "│ X │"
    int side_panel_w = settings_.element_dummy_buttons ? (button_total_w * 3) : 38;
    int main_total_w = std::max(24, total_width - side_panel_w);
    int wave_w = main_total_w - 4;

    double elapsed = has_track_ ? player_.poll_elapsed() : 0.0;
    int active_cols = (total_sec_ > 0) ? static_cast<int>((elapsed / static_cast<double>(total_sec_)) * wave_w) : 0;
    active_cols = std::clamp(active_cols, 0, wave_w);

    int reveal_cols = wave_w;
    if (waveform_ready_) {
        double reveal_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - waveform_reveal_start_).count();
        if (reveal_sec < 0.7) {
            reveal_cols = static_cast<int>((reveal_sec / 0.7) * wave_w);
        }
    } else {
        reveal_cols = 0;
    }

    std::string color_played = ansi_for(settings_.progress_played_color);
    std::string color_unplayed = ansi_for(settings_.progress_remaining_color);
    std::string color_reset = "\x1b[0m";

    std::string top_wave, mid_wave, bot_wave;
    std::vector<int> waveform_levels;
    if (!waveform_envelope_.empty()) {
        waveform_levels = WaveformQuantizer::resample_for_ui(waveform_envelope_, wave_w);
    } else {
        waveform_levels.assign(wave_w, 0);
    }

    for (int i = 0; i < wave_w; ++i) {
        bool revealed = i < reveal_cols;
        int level = (revealed && i < static_cast<int>(waveform_levels.size())) ? waveform_levels[i] : 0;
        BrailleColumn col = WaveformQuantizer::get_column(level);
        if (i == 0) {
            std::string c = (i < active_cols) ? color_played : color_unplayed;
            top_wave += c; mid_wave += c; bot_wave += c;
        } else if (i == active_cols) {
            top_wave += color_unplayed; mid_wave += color_unplayed; bot_wave += color_unplayed;
        }
        top_wave += col.top; mid_wave += col.mid; bot_wave += col.bot;
    }
    top_wave += color_reset; mid_wave += color_reset; bot_wave += color_reset;

    // Generic fallback bar for when the waveform element is switched
    // off: a plain "[#####-------]" fill on the middle row, blank above
    // and below it, instead of leaving the panel showing a stray braille
    // waveform on two of its three rows (top_wave/mid_wave used to render
    // unconditionally regardless of this setting -- only the bottom row
    // respected the toggle, which is the "turning off waveform doesn't
    // actually turn it off" bug).
    std::string generic_blank(wave_w, ' ');
    std::string generic_bar;
    if (!settings_.element_waveform) {
        int inner_w = std::max(0, wave_w - 2); // account for the '[' and ']'
        int filled = (total_sec_ > 0) ? static_cast<int>((elapsed / static_cast<double>(total_sec_)) * inner_w) : 0;
        filled = std::clamp(filled, 0, inner_w);
        generic_bar = "[" + color_played + std::string(filled, '#') + color_reset
                     + color_unplayed + std::string(inner_w - filled, '-') + color_reset + "]";
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    // Buttons (<<< PLAY >>>) and the volume bar now match the border
    // color rather than the separate (and, for these two elements,
    // effectively unused/inert) button_color field.
    std::string button_ansi = border_ansi;

    auto button_mid = [&](const std::string& text) {
        std::string centered = button_ansi + center_pad(text, button_content_w) + "\x1b[0m";
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        return bar + " " + centered + " " + bar;
    };
    std::string play_label = (has_track_ && player_.is_paused()) ? "PLAY" : (has_track_ ? "PAUSE" : "PLAY");

    std::vector<std::string> out;
    if (settings_.element_dummy_buttons) {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi) + box_top("", button_total_w, border_ansi)
                      + box_top("", button_total_w, border_ansi));
    } else {
        out.push_back(box_top("PROGRESS BAR", main_total_w, border_ansi));
    }
    
    std::string row1_content = settings_.element_waveform ? top_wave : generic_blank;
    std::string row2_content = settings_.element_waveform ? mid_wave : generic_bar;
    std::string row3_content = settings_.element_waveform ? bot_wave : generic_blank;

    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    if (settings_.element_dummy_buttons) {
        out.push_back(bar + " " + row1_content + " " + bar
                      + button_mid("<<<") + button_mid(play_label) + button_mid(">>>"));
        out.push_back(bar + " " + row2_content + " " + bar
                      + box_bottom(button_total_w, "", border_ansi_bottom) + box_bottom(button_total_w, "", border_ansi_bottom)
                      + box_bottom(button_total_w, "", border_ansi_bottom));
    } else {
        out.push_back(bar + " " + row1_content + " " + bar);
        out.push_back(bar + " " + row2_content + " " + bar);
    }

    int vol = player_.volume();
    int vol_hashes = (vol * 20) / 100;
    // "#" (filled) matches the waveform's played color, "-" (empty)
    // matches its unplayed/remaining color -- color_played/color_unplayed
    // are already computed above from progress_played_color/
    // progress_remaining_color for the waveform itself, reused here so
    // the volume bar visually reads as the same kind of fill. The
    // "VOLUME BAR:[...]" label and brackets use the border color, same
    // as the buttons above.
    std::string vol_bar_colored = color_played + std::string(vol_hashes, '#') + "\x1b[0m"
                                 + color_unplayed + std::string(20 - vol_hashes, '-') + "\x1b[0m";
    std::string vol_bar_text = std::string(vol_hashes, '#') + std::string(20 - vol_hashes, '-'); // plain, for width math only
    std::string vol_tail = border_ansi + "VOLUME BAR:[" + "\x1b[0m" + vol_bar_colored
                          + border_ansi + "]" + "\x1b[0m" + " " + std::to_string(vol) + "%";
    int side_w = total_width - main_total_w;
    // pad_left counts raw bytes, so it can't be used once vol_tail carries
    // ANSI bytes -- pad by the *visible* width instead (the volume-bar
    // color fix below is what introduced the mismatch).
    int visible_w = display_width("VOLUME BAR:[" + vol_bar_text + "] " + std::to_string(vol) + "%");
    int gap = std::max(0, side_w - visible_w);

    if (!settings_.autosave_enabled) {
        // AutoSave fully off: no indicator, and the blank gap moves to
        // the *right* of the text instead of sitting between the
        // border and it -- "│VOLUME BAR" touching the border directly,
        // same total line width either way so nothing else has to
        // change to stay aligned.
        if (gap > 0) vol_tail = vol_tail + std::string(gap, ' ');
    } else {
        // AutoSave on: keep the existing gap (│  VOLUME BAR), but let
        // the indicator glyph occupy the first character of it when
        // enabled -- │• VOLUME BAR with room to spare, or │•VOLUME BAR
        // once the gap is down to exactly one column.
        std::string pad(gap, ' ');
        if (gap >= 1) {
            std::string glyph = autosave_indicator_glyph();
            if (!glyph.empty()) pad = glyph + pad.substr(1);
        }
        vol_tail = pad + vol_tail;
    }

    std::string time_plain = "[ " + fmt_mmss(elapsed) + " ]" + settings_.box_horizontal + "[ " + fmt_mmss(static_cast<double>(total_sec_)) + " ]";
    // Manual box-bottom construction (rather than the shared box_bottom()
    // helper) so the timestamp text can carry its own color: box_bottom()
    // measures/pads the footer as plain text, and embedding ANSI bytes
    // into that path would get miscounted as visible columns.
    std::string ts_ansi = ansi_for(settings_.progress_timestamp_color.empty() ? settings_.border_color : settings_.progress_timestamp_color, false);
    std::string time_colored = ts_ansi + time_plain + "\x1b[0m";
    std::string prefix_plain = settings_.box_lower_left + settings_.box_horizontal + " " + time_plain + " ";
    int used = display_width(prefix_plain);
    int dashes_n = std::max(0, main_total_w - used - 1);
    std::string bottom_line = border_ansi_bottom + settings_.box_lower_left + settings_.box_horizontal + " "
                             + time_colored + border_ansi_bottom + " "; // re-apply border color -- time_colored's own reset above would otherwise leave the rest of this line uncolored
    for (int i = 0; i < dashes_n; ++i) bottom_line += settings_.box_horizontal;
    bottom_line += settings_.box_lower_right;
    bottom_line += "\x1b[0m";
    // This row is only main_total_w wide, but the row above it runs the
    // full width (volume bar occupies the right-hand side). Frames are
    // drawn without a screen clear (cursor-home only), so any cell this
    // row does not write keeps whatever was there before -- e.g. the edge
    // of a floating panel (Retry Lyrics / Bulk Add / Clear Queue) that was
    // stamped over it, which then stayed on screen under the volume bar
    // after the panel closed. Pad to the full width so it is overwritten.
    if (side_w > 0) bottom_line += std::string(side_w, ' ');
    out.push_back(bar + " " + row3_content + " " + bar + vol_tail);
    out.push_back(bottom_line);
    return out;
}
std::vector<std::string> App::build_search_bar(int total_width) const {
    std::string label = (list_source_ == ListSource::Online) ? "SEARCH ONLINE"
                       : (list_source_ == ListSource::Playlist) ? "SEARCH PLAYLISTS"
                       : (list_source_ == ListSource::Folder) ? "SEARCH FOLDERS"
                       : "SEARCH LOCAL";

    std::string content;
    if (mode_ == Mode::Search) {
        content.clear(); // caret/selection box built lower down
    } else if (list_source_ == ListSource::Online) {
        content = "/s:" + last_online_query_;
    } else if (list_source_ == ListSource::Playlist) {
        content = "/p:" + last_playlist_query_;
    } else if (list_source_ == ListSource::Folder) {
        content = "/f:" + last_folder_query_;
    } else {
        content = "/l:" + last_local_query_;
    }

    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::vector<std::string> out;
    int search_w = total_width - 5;
    out.push_back(box_top(label, search_w, border_ansi) + border_ansi + "╭───╮\x1b[0m");
    // Play-mode indicator: L=list, R=repeat, S=shuffle, Q=repeat queue,
    // O=stop -- one letter for whichever of the five settings_.play_mode
    // states is active, cycled with a single "m" press
    // (HKeyCyclePlayMode) rather than a separate toggle per mode.
    std::string mode_letter(1, play_mode_letter());
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::string line;
    if (mode_ == Mode::Search) {
        // Caret + selection + block cursor, where this used to be the query
        // with a hard-coded block appended. This one row cannot go through
        // box_line(): that pads with display_width(), which counts the
        // reverse-video escapes of a marked range as columns, so every
        // highlighted character would over-pad the line -- pad against
        // paint_edit_field column count instead (it returns exactly what
        // those escapes are worth).
        const int inner = std::max(1, search_w - 4); // box_line inner width
        EditPaint p = paint_edit_field(search_buffer_, edit_caret_, edit_anchor_, inner - 1, "", true);
        std::string body = "/" + p.s;
        const int fill = std::max(0, inner - (1 + p.cols));
        std::string pad(static_cast<size_t>(fill), ' ');
        line = bar + " " + body + pad + " " + bar;
    } else {
        line = box_line(content, search_w, border_ansi);
    }
    out.push_back(line + border_ansi + settings_.box_vertical
                  + " " + mode_letter + " " + settings_.box_vertical + "\x1b[0m");
    out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    return out;
}

std::vector<std::string> App::build_list_panel(int total_width, int height) const {
    bool online = (list_source_ == ListSource::Online);
    bool playlists_mode = (list_source_ == ListSource::Playlist);
    bool folders_mode = (list_source_ == ListSource::Folder);
    std::string label = online ? "ONLINE RESULTS"
                       : playlists_mode ? "SAVED PLAYLISTS (Enter: queue all)"
                       : folders_mode ? "LOCAL AUDIO FOLDERS (Enter: open folder)"
                       : "LOCAL AUDIO FILES (sort: " + std::string(sort_mode_name(local_sort_mode_, settings_.meta_only))
                         + (folder_filter_.empty() ? std::string()
                            : ", folder: " + path_utf8(path_from_utf8(folder_filter_).filename()) + " [c] clear")
                         + ")";
    size_t total = online ? online_view_.size() : playlists_mode ? playlist_view_.size()
                 : folders_mode ? folder_view_.size() : local_view_.size();
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);

    std::vector<std::string> out;
    out.push_back(box_top(label, total_width, border_ansi));

    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll_ + row;
        std::string content;
        if (idx < static_cast<int>(total)) {
            if (online) {
                const auto& r = online_view_[idx];
                const int uploader_w = 18;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - uploader_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(r.title, settings_.font_map);
                std::string t_uploader = apply_font_map(r.uploader, settings_.font_map);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_title, title_w), title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_uploader, uploader_w), uploader_w);
            } else if (playlists_mode) {
                const auto& p = playlist_view_[idx];
                const int count_w = 10;
                int name_w = std::max(5, inner - idx_w - 2 - 2 - count_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_name = apply_font_map(p.name, settings_.font_map);
                std::string t_count = std::to_string(p.track_count) + (p.track_count == 1 ? " track" : " tracks");
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_name, name_w), name_w) + settings_.list_separator + " "
                        + pad_right(t_count, count_w);
            } else if (folders_mode) {
                const auto& f = folder_view_[idx];
                const int parent_w = 16;
                const int count_w = 10;
                int name_w = std::max(5, inner - idx_w - 2 - 2 - parent_w - 2 - count_w);
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_name = apply_font_map(f.name, settings_.font_map);
                std::string t_parent = apply_font_map(f.parent, settings_.font_map);
                std::string t_count = std::to_string(f.track_count) + (f.track_count == 1 ? " track" : " tracks");
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_name, name_w), name_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_parent, parent_w), parent_w) + settings_.list_separator + " "
                        + pad_right(t_count, count_w);
            } else {
                const auto& t = local_view_[idx];
                const int artist_w = 16;
                const int dur_w = 5;
                int title_w = std::max(5, inner - idx_w - 2 - 2 - artist_w - 2 - dur_w);
                double dur = -1;
                // BUGFIX: this was always the parent-folder name, even
                // though the metadata panel already reads the real ffprobe
                // artist tag for the loaded track — now the list uses that
                // same real tag (probed lazily for visible rows), falling
                // back to the folder guess only until it's been probed.
                std::string artist = t.folder_artist;
                {
                    std::lock_guard<std::mutex> lk(row_meta_mutex_);
                    auto it = row_meta_cache_.find(path_utf8(t.path));
                    if (it != row_meta_cache_.end()) {
                        dur = it->second.duration_sec;
                        if (!it->second.artist.empty()) {
                            artist = it->second.artist;
                        } else if (it->second.tags_resolved) {
                            // A real probe already ran and confirmed this file
                            // has no artist tag -- same distinction the
                            // metadata panel now makes (see start_local_track).
                            // Without this, an untagged file kept showing the
                            // folder-name guess forever instead of "-", which
                            // just relocated the original bug into this list.
                            artist = "-";
                        }
                    }
                }
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(list_row_title(t.path, t.title), settings_.font_map);
                std::string t_artist = apply_font_map(artist, settings_.font_map);
                std::string t_dur = apply_font_map(fmt_mmss(dur), settings_.font_map);

                // Only the hovered row animates, and only when its title
                // is actually too long to fit -- every other row still
                // gets the same static truncate_str() as before, so
                // nothing about the rest of the list changes.
                std::string title_shown = (idx == selected_)
                    ? marquee_or_truncate(t_title, title_w, idx, marquee_row_idx_, marquee_since_)
                    : pad_right(truncate_str(t_title, title_w), title_w);
                content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                        + pad_right(title_shown, title_w) + settings_.list_separator + " "
                        + pad_right(truncate_str(t_artist, artist_w), artist_w) + settings_.list_separator + " "
                        + t_dur;
            }
        }
        bool sel = (idx == selected_) && idx < static_cast<int>(total);
        bool is_playing_row = has_track_ && list_source_ == ListSource::Local && idx < static_cast<int>(total)
                               && local_view_[idx].path == current_path_;
        std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else if (is_playing_row) {
            std::string playing_ansi = ansi_for(settings_.list_playing_color) + bg_ansi_for(settings_.list_playing_bg_color);
            out.push_back(bar + " " + playing_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }

    std::string footer;
    int remaining = static_cast<int>(total) - (scroll_ + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_queue_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;
    std::string title = queue_focus_ ? "QUEUE (focused)" : "QUEUE";
    out.push_back(box_top(title, total_width, border_ansi));

    if (queue_.empty()) {
        int mid_row = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid_row) {
                std::string text = apply_font_map("ADD TRACKS TO QUEUE", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string queue_ansi = ansi_for(settings_.queue_color, false);
            out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
        }
    } else {
        for (int row = 0; row < height; ++row) {
            int idx = queue_scroll_ + row;
            std::string content;
            bool is_row_playing = false;
            bool is_row_hovering = false;
            if (idx < static_cast<int>(queue_.size())) {
                const auto& q = queue_[idx];
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(q.title, settings_.font_map);
                content = pad_right(t_idx, 3) + settings_.list_separator + " " + t_title;
                is_row_playing = has_track_ && q.is_local && q.local_path == current_path_;
                is_row_hovering = queue_focus_ && (idx == queue_selected_);
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string color_ansi;
            if (is_row_hovering) color_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
            else if (is_row_playing) color_ansi = ansi_for(settings_.queue_playing_color, true) + bg_ansi_for(settings_.queue_playing_bg_color);
            else color_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
            out.push_back(bar + " " + color_ansi + padded + "\x1b[0m " + bar);
        }
    }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

// ---------------------------------------------------------------------
// Playlist editor overlay (Mode::Playlist, HKeyPlaylist)
// ---------------------------------------------------------------------

void App::playlist_refresh_lib_view() {
    playlist_edit_lib_view_ = filter_and_rank_local(playlist_edit_lib_query_);
    playlist_edit_lib_selected_ = std::clamp(playlist_edit_lib_selected_, 0,
        std::max(0, static_cast<int>(playlist_edit_lib_view_.size()) - 1));
}

void App::playlist_refresh_manage_view() {
    playlist_manage_view_ = filter_playlists(playlist_manage_query_);
    playlist_manage_selected_ = std::clamp(playlist_manage_selected_, 0,
        std::max(0, static_cast<int>(playlist_manage_view_.size()) - 1));
}

// HKeyPlaylist entry point -- always starts a fresh, blank playlist on
// tab 0 (name field focused, ready to type). The only way to bring an
// *existing* playlist back into the editor is explicit: tab 1, Enter on
// it (see playlist_load_into_editor()) -- that way reopening this
// overlay never silently discards an unsaved in-progress playlist by
// accident.
void App::playlist_open_editor() {
    mode_ = Mode::Playlist;
    playlist_tab_ = 0;
    playlist_edit_focus_ = 0;
    playlist_edit_name_.clear();
    playlist_edit_tracks_.clear();
    playlist_edit_lib_query_.clear();
    playlist_edit_lib_selected_ = 0;
    playlist_edit_track_selected_ = 0;
    playlist_status_.clear();
    playlist_edit_dirty_ = false;
    playlist_confirm_exit_ = false;
    playlist_confirm_delete_ = false;
    playlist_manage_query_.clear();
    playlist_manage_focus_ = 1; // land in the list: Enter/DEL work immediately; Tab goes to the search box
    playlist_refresh_lib_view();
    playlist_refresh_manage_view();
}

void App::playlist_load_into_editor(const std::string& name) {
    auto pl = load_playlist(name);
    if (!pl) { playlist_status_ = "could not load \"" + name + "\""; return; }
    playlist_edit_name_ = pl->name;
    playlist_edit_tracks_ = pl->tracks;
    playlist_edit_track_selected_ = 0;
    playlist_tab_ = 0;
    playlist_edit_focus_ = 1;
    playlist_edit_dirty_ = false; // freshly loaded from disk -- matches what's saved, nothing to lose yet
    playlist_status_ = "editing \"" + pl->name + "\" (" + std::to_string(pl->tracks.size()) + " tracks)";
}

void App::playlist_add_hovering_to_edit() {
    if (playlist_edit_lib_selected_ < 0
        || playlist_edit_lib_selected_ >= static_cast<int>(playlist_edit_lib_view_.size())) return;
    const auto& t = playlist_edit_lib_view_[playlist_edit_lib_selected_];
    for (auto& existing : playlist_edit_tracks_) {
        if (existing.path == t.path) { playlist_status_ = "already in the playlist"; return; }
    }
    PlaylistTrack pt;
    pt.title = t.title;
    pt.artist = t.folder_artist;
    pt.path = t.path;
    pt.missing = false;
    playlist_edit_tracks_.push_back(std::move(pt));
    playlist_edit_dirty_ = true;
    playlist_status_.clear();
}

void App::playlist_remove_hovering_track() {
    if (playlist_edit_track_selected_ < 0
        || playlist_edit_track_selected_ >= static_cast<int>(playlist_edit_tracks_.size())) return;
    const std::string removed_title = playlist_edit_tracks_[playlist_edit_track_selected_].title;
    playlist_edit_tracks_.erase(playlist_edit_tracks_.begin() + playlist_edit_track_selected_);
    playlist_status_ = "removed \"" + removed_title + "\"";
    playlist_edit_track_selected_ = std::clamp(playlist_edit_track_selected_, 0,
        std::max(0, static_cast<int>(playlist_edit_tracks_.size()) - 1));
    playlist_edit_dirty_ = true;
}

// Keys 4/5 in the track list (tab 0, playlist_edit_focus_==2) -- same
// swap-with-neighbor approach as queue_move_hovering(), just against
// playlist_edit_tracks_ instead of queue_. Reordering counts as a
// change like add/remove, so it sets the dirty flag too.
void App::playlist_move_hovering_track(int dir) {
    if (playlist_edit_tracks_.empty()) return;
    int target = playlist_edit_track_selected_ + dir;
    if (target < 0 || target >= static_cast<int>(playlist_edit_tracks_.size())) return; // already at an edge
    std::swap(playlist_edit_tracks_[playlist_edit_track_selected_], playlist_edit_tracks_[target]);
    playlist_edit_track_selected_ = target;
    playlist_edit_dirty_ = true;
}

// Tab 1's DEL, fired only after playlist_confirm_delete_ has been
// confirmed with Y -- see handle_playlist_key().
void App::playlist_delete_selected() {
    if (playlist_manage_selected_ < 0
        || playlist_manage_selected_ >= static_cast<int>(playlist_manage_view_.size())) return;
    std::string name = playlist_manage_view_[playlist_manage_selected_].name;
    std::string error;
    if (!PlaylistManager::remove(playlists_dir(), name, &error)) {
        playlist_status_ = "delete failed: " + error;
        return;
    }
    playlist_status_ = "deleted \"" + name + "\"";
    playlist_refresh_manage_view();
    // Same reasoning as playlist_save_current(): keep the main UI's "/p:"
    // browse view in sync if it's currently showing playlists.
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);
}

// HOME -- saves and exits back to Browse. Deliberately not a plain
// letter (an earlier version used "S", which meant typing an "s" into
// the name field or the library search saved and kicked you out
// mid-keystroke); HOME can never appear inside typed text.
void App::playlist_save_current() {
    std::string name = playlist_edit_name_;
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) {
        playlist_status_ = "enter a name first";
        playlist_edit_focus_ = 0;
        return;
    }
    Playlist pl;
    pl.name = name;
    pl.tracks = playlist_edit_tracks_;
    std::string error;
    if (!PlaylistManager::save(playlists_dir(), pl, &error)) {
        playlist_status_ = "save failed: " + error;
        return;
    }
    status_line_ = "saved playlist \"" + name + "\" (" + std::to_string(pl.tracks.size()) + " tracks)";
    // If the main UI is currently browsing "/p:" results, refresh them
    // so a newly-saved (or renamed) playlist shows up immediately.
    if (list_source_ == ListSource::Playlist) playlist_view_ = filter_playlists(last_playlist_query_);
    playlist_edit_dirty_ = false;
    mode_ = Mode::Browse;
}

void App::handle_playlist_key(int key) {
    // "Save before exiting?" prompt -- shown instead of the hint line
    // when ESC is pressed on tab 0 with unsaved changes (see below).
    // Swallows every key except the three it cares about so nothing
    // gets edited underneath it by accident.
    if (playlist_confirm_exit_) {
        if (key == 'y' || key == 'Y') { playlist_confirm_exit_ = false; playlist_save_current(); return; }
        if (key == 'n' || key == 'N') { playlist_confirm_exit_ = false; mode_ = Mode::Browse; return; }
        if (key == 27) { playlist_confirm_exit_ = false; } // cancel the prompt, keep editing
        return;
    }

    // "Really delete this playlist?" prompt -- shown instead of the hint
    // line when DEL is pressed on tab 1 (see below). Same swallow-every-
    // other-key shape as playlist_confirm_exit_ above, so nothing on tab
    // 1 can be triggered by accident while it's up.
    if (playlist_confirm_delete_) {
        if (key == 'y' || key == 'Y') { playlist_confirm_delete_ = false; playlist_delete_selected(); return; }
        if (key == 'n' || key == 'N') { playlist_confirm_delete_ = false; return; }
        if (key == 27) { playlist_confirm_delete_ = false; } // cancel the prompt
        return;
    }

    if (key == 27) { // ESC
        if (playlist_tab_ == 0 && playlist_edit_dirty_) { playlist_confirm_exit_ = true; return; }
        mode_ = Mode::Browse;
        return;
    }
    if (key == kKeyHome) {
        if (playlist_tab_ == 0) playlist_save_current();
        return;
    }
    // Arrows collapse to 'A'..'D' app-wide; last_key_was_arrow() is what
    // tells a real arrow from a typed capital.
    const bool arrow = last_key_was_arrow();
    // Alt+Left/Right switch the two top-level tabs. This used to be plain
    // Left/Right, but this screen starts in the name field (focus 0) where
    // Left/Right has to stay the caret key -- and since the only way to
    // move focus off the name field is Tab, which cycles within a tab
    // rather than switching one, a plain arrow could only ever reach tab 1
    // from the track list pane. Alt+Arrow is a modifier combination none
    // of this screen's fields claims for anything (Shift+Left/Right marks
    // text instead), so it now switches tabs the same way from every pane.
    // (Ctrl+Arrow was tried first, but several terminals intercept
    // Ctrl+Left/Right for their own shortcuts before the app ever sees it.)
    if (key == kKeyAltLeft || key == kKeyAltRight) {
        playlist_tab_ = (playlist_tab_ + 1) % 2;
        if (playlist_tab_ == 1) playlist_refresh_manage_view();
        return;
    }

    if (playlist_tab_ == 1) {
        // --- Tab 1: browse/manage saved playlists ---
        int total = static_cast<int>(playlist_manage_view_.size());
        if (key == 9) { // Tab -- search box <-> list
            playlist_manage_focus_ = (playlist_manage_focus_ == 0) ? 1 : 0;
            return;
        }
        if (playlist_manage_focus_ == 0) { // search box -- filter, caret, marking, clipboard
            // Up/Down keep walking the filtered list while the box is
            // focused, same as the main UI's "/" search does.
            if (arrow && (key == 'A' || key == 'B')) {
                if (key == 'A') {
                    if (playlist_manage_selected_ > 0) --playlist_manage_selected_;
                    return;
                }
                if (total > 0 && playlist_manage_selected_ < total - 1) ++playlist_manage_selected_;
                return;
            }
            if (key == '\r' || key == '\n') { playlist_manage_focus_ = 1; return; } // Enter: leave the box, take the list
            edit_focus("playlist-manage-search", playlist_manage_query_);
            if (edit_text_key(playlist_manage_query_, edit_caret_, edit_anchor_, key, 80, &playlist_status_))
                playlist_refresh_manage_view();
            return;
        }
        if (arrow && key == 'A') { // up
            if (playlist_manage_selected_ > 0) --playlist_manage_selected_;
            return;
        }
        if (arrow && key == 'B') { // down
            if (total > 0 && playlist_manage_selected_ < total - 1) ++playlist_manage_selected_;
            return;
        }
        if (key == '\r' || key == '\n') {
            if (total > 0 && playlist_manage_selected_ < total) {
                playlist_load_into_editor(playlist_manage_view_[playlist_manage_selected_].name);
            }
            return;
        }
        if (key == kKeyDelete && total > 0 && playlist_manage_selected_ < total) {
            playlist_confirm_delete_ = true;
            return;
        }
        return;
    }

    // --- Tab 0: create/edit ---
    if (key == 9) { // Tab -- cycle focus: name field -> library picker -> track list -> ...
        playlist_edit_focus_ = (playlist_edit_focus_ + 1) % 3;
        return;
    }

    if (playlist_edit_focus_ == 0) { // name field
        if (key == '\r' || key == '\n') { playlist_edit_focus_ = 1; return; } // confirm name, jump to picking tracks
        edit_focus("playlist-name", playlist_edit_name_);
        // Caret, marking, clipboard and typing in one call -- Up/Down
        // arrows included (edit_text_key drops them, this box has no
        // vertical anything to navigate), where the old code had to
        // blacklist 'A'/'B' so they could not be typed either.
        if (edit_text_key(playlist_edit_name_, edit_caret_, edit_anchor_, key, 80, &playlist_status_))
            playlist_edit_dirty_ = true;
        return;
    }

    if (playlist_edit_focus_ == 1) { // library picker -- typing filters live, same as the main search box
        int total = static_cast<int>(playlist_edit_lib_view_.size());
        if (arrow && key == 'A') {
            if (playlist_edit_lib_selected_ > 0) --playlist_edit_lib_selected_;
            return;
        }
        if (arrow && key == 'B') {
            if (total > 0 && playlist_edit_lib_selected_ < total - 1) ++playlist_edit_lib_selected_;
            return;
        }
        if (key == '\r' || key == '\n') { playlist_add_hovering_to_edit(); return; }
        edit_focus("playlist-lib-search", playlist_edit_lib_query_);
        if (edit_text_key(playlist_edit_lib_query_, edit_caret_, edit_anchor_, key, 120, &playlist_status_))
            playlist_refresh_lib_view();
        return;
    }

    // playlist_edit_focus_ == 2: the in-progress playlist's track list
    {
        int total = static_cast<int>(playlist_edit_tracks_.size());
        if (arrow && key == 'A') {
            if (playlist_edit_track_selected_ > 0) --playlist_edit_track_selected_;
            return;
        }
        if (arrow && key == 'B') {
            if (total > 0 && playlist_edit_track_selected_ < total - 1) ++playlist_edit_track_selected_;
            return;
        }
        // DEL, Backspace, or 'd' (lowercase only -- a typed capital C/D
        // used to be intercepted above as the Left/Right arrow and now
        // only reaches here as a genuine arrow, which does nothing).
        if (key == kKeyDelete || key == 127 || key == 8 || key == 'd') { playlist_remove_hovering_track(); return; }
        // 4/5 move the hovering track up/down -- same keys as the main
        // queue's HKeyQueueMoveUp/Down, kept as literal codes (like the
        // rest of this function) rather than routed through
        // resolve_hotkey_action() since this whole handler already
        // works in raw arrow-collapsed key codes, not configurable
        // hotkeys.
        if (key == '4') { playlist_move_hovering_track(-1); return; }
        if (key == '5') { playlist_move_hovering_track(1); return; }
        return;
    }
}

static std::string header_sgr(const Settings& s); // defined further down (Settings panel section)

std::vector<std::string> App::build_playlist_library_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    {
        // The query lives in the title row, with caret/selection while the
        // library pane has focus -- built by hand, box_top() measures with
        // display_width() and cannot see through the selection escapes.
        const std::string prefix = settings_.box_upper_left + settings_.box_horizontal + " LIBRARY  /";
        const int prefix_w = static_cast<int>(display_width(prefix));
        std::string field;
        int field_cols = 0;
        if (playlist_edit_focus_ == 1) {
            EditPaint p = paint_edit_field(playlist_edit_lib_query_, edit_caret_, edit_anchor_,
                                           std::max(1, total_width - prefix_w - 1), "", true);
            field = p.s;
            field_cols = p.cols;
        } else {
            field = playlist_edit_lib_query_;
            field_cols = static_cast<int>(display_width(field));
        }
        out.push_back(box_top_field(prefix, field, field_cols, total_width, border_ansi));
    }

    // Three equal columns -- Filename | Title | Artist -- separated by
    // " | ", with a header row of their own right under the title row.
    // Filename is the file's stem, Title/Artist come from the resolved
    // tags (row_meta_cache_) with the same folder-name fallback the main
    // list uses for Artist; an untagged Title shows "-". No index column:
    // the header promises exactly these three and each gets a third.
    const std::string col_sep = " | ";
    const int sep_w = static_cast<int>(col_sep.size());
    const int avail = std::max(3, inner - 2 * sep_w);
    const int col1_w = avail / 3;
    const int col2_w = avail / 3;
    const int col3_w = avail - col1_w - col2_w;
    {
        std::string header = pad_right(truncate_str("Filename", col1_w), col1_w) + col_sep
                           + pad_right(truncate_str("Title", col2_w), col2_w) + col_sep
                           + pad_right(truncate_str("Artist", col3_w), col3_w);
        out.push_back(bar + " " + header_sgr(settings_)
                      + pad_right(truncate_str(header, inner), inner) + "\x1b[0m " + bar);
    }

    int total = static_cast<int>(playlist_edit_lib_view_.size());
    int scroll = std::clamp(playlist_edit_lib_selected_ - height / 2, 0, std::max(0, total - height));
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& t = playlist_edit_lib_view_[idx];
            std::string tag_title, tag_artist;
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                if (it != row_meta_cache_.end()) {
                    tag_title = it->second.title;
                    tag_artist = it->second.artist;
                }
            }
            if (tag_title.empty()) tag_title = "-";
            if (tag_artist.empty()) tag_artist = t.folder_artist.empty() ? std::string("-") : t.folder_artist;
            std::string c_file = apply_font_map(t.title, settings_.font_map);
            std::string c_title = apply_font_map(tag_title, settings_.font_map);
            std::string c_artist = apply_font_map(tag_artist, settings_.font_map);
            // Every column padded to its own fixed width *before*
            // concatenating (rather than truncating the assembled whole
            // afterward) -- matches build_list_panel()'s row construction,
            // so one title's display_width() landing a column short can't
            // shift every border to its right. The hovering row's
            // filename scrolls (marquee) when it doesn't fit.
            bool row_focused_sel = (playlist_edit_focus_ == 1) && (idx == playlist_edit_lib_selected_);
            std::string file_shown = row_focused_sel
                ? marquee_or_truncate(c_file, col1_w, idx, marquee_pl_lib_row_idx_, marquee_pl_lib_since_)
                : pad_right(truncate_str(c_file, col1_w), col1_w);
            content = file_shown + col_sep
                    + pad_right(truncate_str(c_title, col2_w), col2_w) + col_sep
                    + pad_right(truncate_str(c_artist, col3_w), col3_w);
        }
        bool sel = (playlist_edit_focus_ == 1) && (idx == playlist_edit_lib_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_playlist_tracks_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // When focused, also show WHICH row the cursor is on ("3/12"), so the
    // selection is readable even on a terminal/colour scheme where the
    // highlighted row is hard to see.
    std::string sel_pos;
    if (playlist_edit_focus_ == 2 && !playlist_edit_tracks_.empty()) {
        sel_pos = "  " + std::to_string(playlist_edit_track_selected_ + 1) + "/"
                + std::to_string(playlist_edit_tracks_.size());
    }
    std::string label = "TRACKS (" + std::to_string(playlist_edit_tracks_.size()) + ")" + sel_pos
                       + (playlist_edit_focus_ == 2 ? " \u25c0" : ""); // filled triangle: focus indicator,
                                                                        // same purpose as LIBRARY's "\u2588" text
                                                                        // cursor but this panel has no text field
                                                                        // of its own to blink a cursor in
    out.push_back(box_top(label, total_width, border_ansi));

    int total = static_cast<int>(playlist_edit_tracks_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map("ENTER ON A LIBRARY TRACK TO ADD IT", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            // Highlighted even with nothing to select, same cursor-color
            // treatment a real row gets below -- otherwise an empty,
            // focused panel is visually identical to an unfocused one.
            if (playlist_edit_focus_ == 2) {
                std::string cursor_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
                out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
            } else {
                std::string queue_ansi = ansi_for(settings_.queue_color, false);
                out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
            }
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    // Same three equal columns as the LIBRARY pane above (Filename | Title |
    // Artist, identical widths so they line up under its header) -- but no
    // header row of its own, and no index column.
    const std::string col_sep = " | ";
    const int sep_w = static_cast<int>(col_sep.size());
    const int avail = std::max(3, inner - 2 * sep_w);
    const int col1_w = avail / 3;
    const int col2_w = avail / 3;
    const int col3_w = avail - col1_w - col2_w;

    int scroll = std::clamp(playlist_edit_track_selected_ - height / 2, 0, std::max(0, total - height));
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& t = playlist_edit_tracks_[idx];
            std::string tag_title, tag_artist;
            {
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                auto it = row_meta_cache_.find(path_utf8(t.path));
                if (it != row_meta_cache_.end()) {
                    tag_title = it->second.title;
                    tag_artist = it->second.artist;
                }
            }
            if (tag_title.empty()) tag_title = "-";
            if (tag_artist.empty()) {
                std::string folder = path_utf8(t.path.parent_path().filename());
                tag_artist = folder.empty() ? std::string("-") : folder;
            }
            std::string file_txt = t.missing ? (t.title + " [missing]") : t.title;
            std::string c_file = apply_font_map(file_txt, settings_.font_map);
            std::string c_title = apply_font_map(tag_title, settings_.font_map);
            std::string c_artist = apply_font_map(tag_artist, settings_.font_map);
            bool row_focused_sel = (playlist_edit_focus_ == 2) && (idx == playlist_edit_track_selected_);
            std::string file_shown = row_focused_sel
                ? marquee_or_truncate(c_file, col1_w, idx, marquee_pl_track_row_idx_, marquee_pl_track_since_)
                : pad_right(truncate_str(c_file, col1_w), col1_w);
            content = file_shown + col_sep
                    + pad_right(truncate_str(c_title, col2_w), col2_w) + col_sep
                    + pad_right(truncate_str(c_artist, col3_w), col3_w);
        }
        bool sel = (playlist_edit_focus_ == 2) && (idx == playlist_edit_track_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.queue_cursor_color, settings_.queue_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string queue_ansi = ansi_for(settings_.queue_color, false) + bg_ansi_for(settings_.queue_inactive_bg_color);
            out.push_back(bar + " " + queue_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_playlist_manage_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;
    out.push_back(box_top("SAVED PLAYLISTS", total_width, border_ansi));

    {
        // Search field, on its own row now -- it used to be crammed into
        // the title row above ("SAVED PLAYLISTS  / <query>"), which left
        // barely any width to actually see what had been typed once that
        // label had eaten most of the box. A dedicated line (the same
        // box_line_field() shape the meta editor's "Search: " row uses)
        // gives it real room; the list below gives up exactly one row to
        // it (see list_height) so the panel's total height -- and the
        // whole playlist screen's -- doesn't change.
        const std::string prefix = "Search: ";
        std::string field;
        int field_cols = 0;
        if (playlist_manage_focus_ == 0) { // focus is in the box: caret + block cursor
            EditPaint p = paint_edit_field(playlist_manage_query_, edit_caret_, edit_anchor_,
                                           std::max(1, total_width - 4 - static_cast<int>(prefix.size())), "", true);
            field = p.s;
            field_cols = p.cols;
        } else { // focus elsewhere: plain text, with the placeholder hint
            field = playlist_manage_query_.empty() ? std::string("(type to filter)") : playlist_manage_query_;
            field_cols = static_cast<int>(display_width(field));
        }
        out.push_back(box_line_field(prefix, field, field_cols, total_width, border_ansi));
    }

    const int list_height = std::max(1, height - 1);

    int total = static_cast<int>(playlist_manage_view_.size());
    if (total == 0) {
        int mid = list_height / 2;
        for (int row = 0; row < list_height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map(playlist_manage_query_.empty() ? "NO SAVED PLAYLISTS YET" : "NO MATCHING PLAYLISTS", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string list_ansi = ansi_for(settings_.list_color, false);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(playlist_manage_selected_ - list_height / 2, 0, std::max(0, total - list_height));
    const int idx_w = 3;
    for (int row = 0; row < list_height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& p = playlist_manage_view_[idx];
            const int count_w = 10;
            int name_w = std::max(5, inner - idx_w - 2 - 2 - count_w);
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_name = apply_font_map(p.name, settings_.font_map);
            std::string t_count = std::to_string(p.track_count) + (p.track_count == 1 ? " track" : " tracks");
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_name, name_w), name_w) + settings_.list_separator + " "
                    + pad_right(t_count, count_w);
        }
        bool sel = (idx == playlist_manage_selected_) && idx < total;
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + list_height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// Assembles the full-screen playlist editor overlay. Structured like the
// Browse view's own stack of boxed panels (a header box, then side-by-
// side boxed sub-panels, then a plain hint/status line) rather than
// Settings' absolute-positioned single mega-box -- same box-drawing
// vocabulary (box_top/box_line/box_bottom), simpler composition.
void App::build_playlist_screen(std::ostringstream& frame, int W, int target_height) const {
    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    frame << box_top("PLAYLISTS", W, border) << "\n";

    // Tab strip -- built manually rather than via box_line(): box_line()
    // measures/pads its content by (UTF-8-aware, but not ANSI-aware)
    // display width, so embedding the reverse-video highlight before
    // padding would miscount and corrupt the row. Same reasoning as
    // build_list_panel()'s row construction: build plain text, measure
    // that, then wrap the already-fixed-width segments in color.
    {
        std::string bar = border + settings_.box_vertical + R;
        std::string plain0 = " 1: CREATE / EDIT ";
        std::string plain1 = " 2: SAVED PLAYLISTS ";
        std::string gap = "   ";
        int inner = W - 4;
        std::string plain_row = plain0 + gap + plain1;
        std::string seg0 = (playlist_tab_ == 0) ? (HI + plain0 + R) : plain0;
        std::string seg1 = (playlist_tab_ == 1) ? (HI + plain1 + R) : plain1;
        std::string colored_row = seg0 + gap + seg1;
        int pad_n = std::max(0, inner - display_width(plain_row));
        frame << bar << " " << colored_row << std::string(pad_n, ' ') << " " << bar << "\n";
    }

    int fixed_rows = 3; // top border + tab strip + bottom border
    if (playlist_tab_ == 0) {
        // Same hand-built row as the meta editor Search line.
        const std::string prefix = "Name: ";
        const std::string dirty_mark = playlist_edit_dirty_ ? " *" : "";
        const int mark_w = static_cast<int>(dirty_mark.size());
        std::string field;
        int field_cols = 0;
        if (playlist_edit_focus_ == 0) {
            EditPaint p = paint_edit_field(playlist_edit_name_, edit_caret_, edit_anchor_,
                                           std::max(1, W - 4 - static_cast<int>(prefix.size()) - mark_w), "", true);
            field = p.s;
            field_cols = p.cols;
        } else {
            field = playlist_edit_name_.empty() ? std::string("(untitled)") : playlist_edit_name_;
            field_cols = static_cast<int>(display_width(field));
        }
        frame << box_line_field(prefix, field + dirty_mark, field_cols + mark_w, W, border) << "\n";
        fixed_rows += 1;
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    // Deliberately NOT sized to fill whatever room player_view_height()
    // happens to have (that made the list feel oddly tall/short
    // depending on terminal size) -- clamped to a fixed, comfortable
    // range instead so up to 22 tracks are visible regardless of
    // terminal height (fewer on a short terminal, down to the 8-row
    // floor).
    // Sized against BOTH bounds that matter (same reasoning as
    // build_meta_screen()): target_height is player_view_height(), but
    // clamp_output_rows() keeps term_rows_ - 1 lines and a full-width
    // prompt here can wrap, so never lay out taller than the screen.
    int budget = std::min(target_height, term_rows_ - 1);
    // -5: the panel box's own top+bottom border rows (added by
    // build_playlist_*_panel(), which draw a box around the panel_h rows
    // they're handed) plus the TWO legend lines + status line below it. The
    // old "-2" forgot the border rows, which made the frame 1-2 lines too
    // tall and chopped the green status line -- "removed ...", "deleted ...",
    // "added ..." -- off every single frame. Keeping that sum exact is also
    // what stops the second legend row (the text-field keys) from pushing
    // the frame past term_rows_ - 1 and scrolling the terminal.
    int panel_h = std::clamp(budget - fixed_rows - 5, 8, 22);
    if (playlist_tab_ == 0) {
        // Stacked, full width: playlist box (above), LIBRARY, then TRACKS.
        // The two panes together take exactly the lines the old side-by-
        // side pair did (panel_h rows + 2 borders), so the whole screen
        // keeps its height. LIBRARY spends 3 lines on chrome (top border,
        // Filename|Title|Artist header, bottom border), TRACKS 2, which
        // leaves panel_h - 3 data rows to split roughly 1:3.
        int data_rows = std::max(5, panel_h - 3);
        int lib_rows = std::max(2, data_rows / 4);
        int track_rows = data_rows - lib_rows;
        for (auto& l : build_playlist_library_panel(W, lib_rows)) frame << l << "\n";
        for (auto& l : build_playlist_tracks_panel(W, track_rows)) frame << l << "\n";
    } else {
        for (auto& l : build_playlist_manage_panel(W, panel_h)) frame << l << "\n";
    }

    // Footer: a full key legend (gray, "\x1b[90m") plus a status line
    // (green, "\x1b[32m") below it -- exactly the colors and layout
    // Settings' own footer uses (see build_settings_screen()'s
    // "[TAB] Switch | ... " line and its status_line_ line just below).
    if (playlist_confirm_exit_) {
        std::string prompt = "Save changes to \"" + (playlist_edit_name_.empty() ? std::string("(untitled)") : playlist_edit_name_)
                            + "\" before exiting?   [Y]es   [N]o   [ESC] cancel";
        frame << "\x1b[43;30m " << prompt << " \x1b[0m\n";
        frame << "\n";
    } else if (playlist_confirm_delete_) {
        std::string name = (playlist_manage_selected_ >= 0
                          && playlist_manage_selected_ < static_cast<int>(playlist_manage_view_.size()))
                          ? playlist_manage_view_[playlist_manage_selected_].name : std::string();
        std::string prompt = "Delete playlist \"" + name + "\"? This can't be undone.   [Y]es   [N]o   [ESC] cancel";
        frame << "\x1b[41;97m " << prompt << " \x1b[0m\n";
        frame << "\n";
    } else {
        std::string hint = "[Alt+\u2190\u2192] Switch Tab | [TAB] Focus | [\u2191\u2193] Navi. | [ENTER] Add/Load | "
                            "[DEL] Remove | [4/5] Move \u2191\u2193 | [HOME] Save";
        frame << "\x1b[90m" << hint << "\x1b[0m\n";
        // Row 2: the text-field keys, plus Exit. Kept off row 1 so each row
        // fits comfortably inside a 120-column terminal without wrapping
        // (the meta editor splits its legend the same way) -- panel_h's
        // budget above already accounts for exactly these two rows. [ESC]
        // Exit used to sit at the end of row 1, but that pushed row 1 past
        // the wrap width and cost a spurious third line; it lives here now.
        frame << "\x1b[90m[SHIFT+←→] Mark | [Ctrl+C/X/V] Copy/Cut/Paste | [ESC] Exit\x1b[0m\n";
        if (!playlist_status_.empty()) frame << "\x1b[32m" << playlist_status_ << "\x1b[0m\n";
        else frame << "\n";
    }
}

// ---------------------------------------------------------------------
// Settings panel
// ---------------------------------------------------------------------

// main_frame_height() used to live here -- it estimated the Browse view's
// total line count (from the fixed kListVisibleRows constant, among
// other things) so the Settings/Console/Cheatsheet/BulkAdd overlays
// could size themselves to "roughly the same height as the player
// view". That was never actually tied to the real terminal size, which
// is exactly what let all of those overlays overflow a short terminal
// and scroll-duplicate just like the Browse view did (see term_rows_'s
// comment in app.h). Every caller now sizes directly off term_rows_
// (the real, current ioctl-reported row count) instead, so this
// function no longer has a reason to exist.

// SGR prefix for the section titles drawn by the Settings panel itself:
// bold plus settings_.header_color -- the Colors tab's HEADER row, a
// palette index of the 256 by default (10, which is exactly the color
// the REFERENCE tab's category titles have always been drawn in, and
// what the ON/OFF tab's LOCAL PATH / PLAYLIST PATH titles borrow). An
// explicit 0/empty means "no color" here like it does everywhere else,
// which degrades these to plain bold rather than to a stray escape.
static std::string header_sgr(const Settings& s) {
    std::string params = sgr_params_for(s.header_color);
    return params.empty() ? "\x1b[1m" : "\x1b[1;" + params + "m";
}

// ---------------------------------------------------------------------
// Meta/tag editor overlay (Mode::MetaEdit, HKeyMetaEditor = Shift+M)
//
// A second full-screen overlay modelled on the playlist editor further up --
// same tab strip, same boxed side-by-side panels, same hint/status footer,
// same Y/N prompts in place of the hint line -- but instead of building a
// playlist it edits the FILES themselves: their names, and their
// artist/title/album/year tags.
//
// Three rules shape the whole thing:
//
//  1. Edits are session state, never a side effect. Everything typed or
//     fetched goes into meta_session_ and is written to an autosave backup
//     (meta_editor.h, under ~/.cache/mousiki/meta_session/) after every
//     single change. The audio files stay untouched until Ctrl+Shift+S
//     explicitly applies the session.
//  2. Leaving is always safe. ESC, quitting, crashing between two edits --
//     none of that loses work: the backup is reloaded next run by
//     meta_ensure_session_loaded(), and it is only ever deleted when the
//     session is applied or explicitly discarded with Ctrl+Shift+X.
//  3. What changed stays visible. A field that was touched -- typed OR
//     filled in by AcoustID -- is drawn in header_sgr(), the same bold
//     header colour the Settings section titles use, so "what will
//     Ctrl+Shift+S write?" is answerable at a glance.
// ---------------------------------------------------------------------

// The AcoustID disclaimer, verbatim, shown by Shift+B's confirmation in
// Browse (in the status line) and by the menu's batch fetch (in the footer).
static const char* kMetaFetchDisclaimer =
    "Fetching meta data via AcoustID; not always accurate and previous "
    "meta data will be overwritten. Continue?";

void App::meta_open() {
    meta_status_.clear();          // before the loader below, which may set it
    meta_prompt_ = MetaPrompt::None;
    meta_prompt_paths_.clear();
    meta_ensure_session_loaded();  // reopening never loses what's already pending
    mode_ = Mode::MetaEdit;
    meta_tab_ = 0;
    meta_focus_ = 0;
    meta_query_.clear();
    meta_filter_ = 0; // a fresh open shows the whole library again
    meta_lib_selected_ = 0;
    meta_field_ = 0;
    meta_fetch_selected_ = 0;
    meta_refresh_lib_view();
}

// Loads the autosaved backup exactly once per run. Called from meta_open()
// AND from every session-touching action, because Shift+B reaches the same
// session from Browse mode without ever opening the menu.
void App::meta_ensure_session_loaded() {
    if (meta_session_loaded_) return;
    meta_session_loaded_ = true;
    std::vector<MetaEditEntry> entries;
    std::vector<std::string> fetch_list;
    if (load_meta_session(entries, fetch_list)) {
        meta_session_ = std::move(entries);
        meta_fetch_list_ = std::move(fetch_list);
        meta_status_ = "restored autosaved session: " + std::to_string(meta_session_.size())
                     + " file" + (meta_session_.size() == 1 ? "" : "s") + " with pending edits";
        log_event("restored meta edit session (" + std::to_string(meta_session_.size()) + " files)");
    }
    meta_refresh_lib_view();
}

void App::meta_refresh_lib_view() {
    // With the 'r' resort on, this rebuild re-orders the pane underneath the
    // cursor, so remember the hovered row by PATH first: clamping the old
    // INDEX would silently point the picker -- and with it the fields panel,
    // which is what the user is editing -- at a different track.
    std::string keep;
    if (meta_resort_edited_ && !meta_lib_view_.empty()) keep = meta_hovering_path();

    meta_lib_view_ = filter_and_rank_local(meta_query_);

    // Missing-tag filter ('x' / Shift+T/A/Y). meta_row_meta() is a native,
    // in-process tag read cached in row_meta_cache_, so the first pass costs
    // one header parse per file and everything after that is a lookup --
    // no ffprobe, no subprocess. A pending session edit counts as the value
    // the file WOULD have, so filtering right after typing (or deleting) a
    // title reflects what is on screen instead of what is on disk.
    if (meta_filter_ != 0) {
        auto value_of = [this](const LocalTrack& t, int field) -> std::string {
            const std::string p = path_utf8(t.path);
            const MetaEditEntry* e = meta_entry(p);
            if (e && e->edited[field] && !e->value[field].empty()) return e->value[field];
            RowMeta rm = meta_row_meta(t.path);
            switch (field) {
                case 1: return rm.artist;
                case 2: return rm.title;
                case 3: return rm.album;
                case 4: return rm.year;
                default: return t.title;
            }
        };
        auto matches = [&](const LocalTrack& t) {
            if (meta_filter_ == 1) { // "no meta data" = every tag field empty
                for (int f = 1; f < kMetaFieldCount; ++f)
                    if (!value_of(t, f).empty()) return false;
                return true;
            }
            const int field = (meta_filter_ == 3) ? 1   // ARTIST
                            : (meta_filter_ == 2) ? 2   // TITLE
                            : 4;                        // YEAR
            return value_of(t, field).empty();
        };
        meta_lib_view_.erase(std::remove_if(meta_lib_view_.begin(), meta_lib_view_.end(),
                                            [&](const LocalTrack& t) { return !matches(t); }),
                             meta_lib_view_.end());
    }

    if (meta_resort_edited_) {
        // Strict "edited before not edited" on a stable sort is a stable
        // partition: every file with a pending edit moves into one block at
        // the top, and both blocks keep the exact order the pane already had
        // -- the normal alphabetical/scan order, or the fuzzy ranking while
        // a search is active (so a query's relevance order is never thrown
        // away). Toggling 'r' off simply rebuilds without this step, i.e.
        // straight back to that order.
        std::stable_sort(meta_lib_view_.begin(), meta_lib_view_.end(),
                         [this](const LocalTrack& a, const LocalTrack& b) {
                             auto edited = [this](const LocalTrack& t) {
                                 const MetaEditEntry* e = meta_entry(path_utf8(t.path));
                                 return e && e->any_edited();
                             };
                             return edited(a) && !edited(b);
                         });
        if (!keep.empty()) {
            for (size_t i = 0; i < meta_lib_view_.size(); ++i) {
                if (path_utf8(meta_lib_view_[i].path) == keep) {
                    meta_lib_selected_ = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    meta_lib_selected_ = std::clamp(meta_lib_selected_, 0,
        std::max(0, static_cast<int>(meta_lib_view_.size()) - 1));
    meta_fetch_selected_ = std::clamp(meta_fetch_selected_, 0,
        std::max(0, static_cast<int>(meta_fetch_list_.size()) - 1));
}

// 'r': flip the library pane between its normal order and "every file with
// pending edits on top". Nothing else about the list changes -- same rows,
// same filter, same selection (meta_refresh_lib_view() follows it by path).
void App::meta_toggle_resort() {
    meta_resort_edited_ = !meta_resort_edited_;
    meta_refresh_lib_view();
    if (meta_resort_edited_) {
        int n = 0;
        for (const auto& e : meta_session_) if (e.any_edited()) ++n;
        meta_status_ = "library resorted: " + std::to_string(n) + " edited file"
                     + (n == 1 ? "" : "s") + " on top";
    } else {
        meta_status_ = "library order: alphabetical again";
    }
}

const char* App::meta_filter_label() const {
    switch (meta_filter_) {
        case 1: return "NO META DATA";
        case 2: return "MISSING TITLE";
        case 3: return "MISSING ARTIST";
        case 4: return "MISSING YEAR";
        default: return "";
    }
}

// 'x' / Shift+T / Shift+A / Shift+Y: narrow the library pane to files a
// given tag is (or, for 'x', every tag is) missing. The same key clears it,
// which is what keeps four keys enough for five states, and the status line
// always says which filter is up and how much of the library survived it --
// the pane's title carries the label too, so an active filter is never
// something you have to remember.
void App::meta_toggle_filter(int filter) {
    meta_filter_ = (meta_filter_ == filter) ? 0 : filter;
    meta_refresh_lib_view();
    const std::string shown = std::to_string(meta_lib_view_.size());
    if (meta_filter_ == 0) meta_status_ = "filter cleared - " + shown + " files";
    else meta_status_ = std::string("filter: ") + meta_filter_label() + " - " + shown + " files (press again to clear)";
}

// The autosave: save whenever there is something to restore, remove the file
// when there isn't. Deliberately NOT tied to any "was it applied" flag --
// leaving the app always ends with whatever the user last edited still on
// disk as a backup, which is the whole point of the session file.
void App::meta_persist() {
    if (!meta_session_loaded_) return;
    if (meta_session_.empty() && meta_fetch_list_.empty()) delete_meta_session();
    else save_meta_session(meta_session_, meta_fetch_list_);
}

const MetaEditEntry* App::meta_entry(const std::string& path) const {
    for (const auto& e : meta_session_) if (e.path == path) return &e;
    return nullptr;
}

MetaEditEntry& App::meta_touch_entry(const std::string& path) {
    for (auto& e : meta_session_) if (e.path == path) return e;
    MetaEditEntry e;
    e.path = path;
    meta_session_.push_back(std::move(e));
    return meta_session_.back();
}

void App::meta_set_field(const std::string& path, int field, const std::string& value) {
    meta_ensure_session_loaded();
    MetaEditEntry& e = meta_touch_entry(path);
    e.edited[field] = true;
    e.value[field] = value;
    meta_persist(); // autosave the backup after every keystroke -- it's a ~2KB file
}

// Current tags of one file for the editor. Uses the same cache the main list
// uses, and the same in-process header parser as a render-thread-safe first
// pass (no subprocess on the frame loop -- see ensure_visible_row_meta()'s
// comment for why a stray ffprobe here used to freeze the whole UI). Files
// that parser declines are finished off by the background sweep's ffprobe
// pass, which now also reads the date tag, so YEAR arrives there too.
RowMeta App::meta_row_meta(const fs::path& path) {
    std::string key = path_utf8(path);
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        auto it = row_meta_cache_.find(key);
        if (it != row_meta_cache_.end() && it->second.tags_resolved) return it->second;
    }
    RowMeta rm = try_native_row_meta(path);
    bool usable = (rm.duration_sec > 0 || rm.tags_resolved);
    bool promote = usable;
    {
        std::lock_guard<std::mutex> lk(row_meta_mutex_);
        auto it = row_meta_cache_.find(key);
        if (it != row_meta_cache_.end()) {
            if (it->second.tags_resolved) { promote = false; rm = it->second; } // a better answer is already cached
            else if (!usable) { rm = it->second; }
        } else if (usable && row_meta_cache_.size() >= 4096) {
            promote = false; // cache cap: same rule as ensure_visible_row_meta()
        }
        if (promote) row_meta_cache_[key] = rm;
    }
    if (promote && rm.tags_resolved) row_meta_tags_version_.fetch_add(1, std::memory_order_relaxed);
    return rm;
}

std::string App::meta_hovering_path() const {
    if (meta_tab_ == 1) { // the fetch list's rows ARE the titles
        if (meta_fetch_list_.empty()) return {};
        int i = std::clamp(meta_fetch_selected_, 0, static_cast<int>(meta_fetch_list_.size()) - 1);
        return meta_fetch_list_[i];
    }
    if (meta_lib_view_.empty()) return {};
    int i = std::clamp(meta_lib_selected_, 0, static_cast<int>(meta_lib_view_.size()) - 1);
    return path_utf8(meta_lib_view_[i].path);
}

// What the field editor shows for one field: the pending value if the field
// has been edited (that's what the highlight keys off), otherwise whatever
// the file already carries.
std::string App::meta_display_value(const std::string& path, int field, const MetaEditEntry* e) {
    if (e && e->edited[field]) return e->value[field];
    if (field == static_cast<int>(MetaField::FileName)) {
        // The STEM, not the whole name: typing is meant to feel like editing
        // "Alpha Song" rather than "Alpha Song.mp3", and apply_meta_entry()
        // puts the original extension back on (meta_editor.cpp's
        // normalize_file_name()), so a rename can never change the format.
        return path_utf8(path_from_utf8(path).stem());
    }
    RowMeta rm = meta_row_meta(path_from_utf8(path));
    switch (static_cast<MetaField>(field)) {
        case MetaField::Artist: return rm.artist;
        case MetaField::Title:  return rm.title;
        case MetaField::Album:  return rm.album;
        case MetaField::Year:   return rm.year;
        default: return {};
    }
}

// Precomputes the five displayed values for whatever row is hovered, once
// per frame, because resolving them needs the mutable tag cache while
// build_meta_screen() itself is const. Called from render_frame().
void App::meta_refresh_hover_values() {
    meta_hover_values_.fill(std::string());
    meta_hover_resolved_ = false;
    if (mode_ != Mode::MetaEdit) return;
    std::string path = meta_hovering_path();
    if (path.empty()) return;
    const MetaEditEntry* e = meta_entry(path);
    meta_hover_resolved_ = meta_row_meta(path_from_utf8(path)).tags_resolved;
    for (int i = 0; i < kMetaFieldCount; ++i) meta_hover_values_[i] = meta_display_value(path, i, e);
}

void App::meta_add_hovering_to_fetch() {
    meta_ensure_session_loaded();
    std::string path = meta_hovering_path();
    if (path.empty()) { meta_status_ = "no title selected"; return; }
    for (const auto& p : meta_fetch_list_) {
        if (p == path) {
            meta_status_ = "already on the fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
            return;
        }
    }
    meta_fetch_list_.push_back(path);
    meta_fetch_selected_ = static_cast<int>(meta_fetch_list_.size()) - 1;
    meta_persist();
    meta_status_ = "added to fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
}

// DEL / 'd' -- same "unqueue" gesture as the main list's removal key.
void App::meta_remove_hovering() {
    meta_ensure_session_loaded();
    std::string path = meta_hovering_path();
    if (path.empty()) return;
    for (size_t i = 0; i < meta_fetch_list_.size(); ++i) {
        if (meta_fetch_list_[i] == path) {
            meta_fetch_list_.erase(meta_fetch_list_.begin() + static_cast<long>(i));
            meta_refresh_lib_view();
            meta_persist();
            meta_status_ = "removed from fetch list (" + std::to_string(meta_fetch_list_.size()) + " queued)";
            return;
        }
    }
    meta_status_ = "not on the fetch list";
}

void App::meta_prompt_single_fetch(const std::string& path) {
    if (path.empty()) return;
    meta_ensure_session_loaded();
    meta_prompt_ = MetaPrompt::Fetch;
    meta_prompt_paths_.assign(1, path);
    // In the menu the footer renders the disclaimer; in Browse there is no
    // footer, so it goes through the status line instead.
    if (mode_ == Mode::MetaEdit) meta_status_.clear();
    else status_line_ = kMetaFetchDisclaimer;
}

void App::meta_prompt_list_fetch() {
    meta_ensure_session_loaded();
    if (meta_fetch_list_.empty()) {
        meta_status_ = "fetch list is empty -- press 'a' on a track to queue it";
        return;
    }
    meta_prompt_ = MetaPrompt::Fetch;
    meta_prompt_paths_ = meta_fetch_list_;
}

void App::meta_start_fetch() {
    meta_ensure_session_loaded();
    if (meta_prompt_paths_.empty()) return;
    if (meta_fetch_running_.exchange(true)) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "an AcoustID fetch is already running";
        return;
    }

    std::vector<MetaFetchRequest> reqs;
    reqs.reserve(meta_prompt_paths_.size());
    for (const auto& path : meta_prompt_paths_) {
        MetaFetchRequest r;
        r.path = path;
        const MetaEditEntry* e = meta_entry(path);
        RowMeta rm = meta_row_meta(path_from_utf8(path));
        r.title = (e && e->edited[static_cast<int>(MetaField::Title)])
                      ? e->value[static_cast<int>(MetaField::Title)]
                      : rm.title;
        if (r.title.empty()) r.title = path_utf8(path_from_utf8(path).stem()); // last resort: the file name
        r.artist = (e && e->edited[static_cast<int>(MetaField::Artist)])
                       ? e->value[static_cast<int>(MetaField::Artist)]
                       : rm.artist;
        reqs.push_back(std::move(r));
    }
    meta_prompt_paths_.clear();
    meta_fetch_ok_paths_.clear();
    {
        std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
        meta_fetch_results_.clear();
        meta_fetch_progress_ = "0/" + std::to_string(reqs.size());
        meta_fetch_done_ = false;
        meta_fetch_outcome_ = MetaFetchOutcome{};
    }
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) =
        "AcoustID: fetching " + std::to_string(reqs.size()) + " title" + (reqs.size() == 1 ? "" : "s") + " ...";

    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join(); // previous batch already finished (running_ was false)
    meta_fetch_thread_ = std::thread([this, reqs]() {
        run_guarded("acoustid fetch", [&] {
            int total = static_cast<int>(reqs.size());
            int done = 0;
            // One python process fingerprints the whole batch (fpcalc) and
            // paces itself to AcoustID's 3-requests-per-second limit, printing
            // a JSON line per lookup, so results are applied to the session as
            // they land instead of only after the last one.
            MetaFetchOutcome out = run_acoustid_fetch(reqs, meta_script_,
                [this, total, &done](const MetaFetchResult& r) {
                    std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
                    meta_fetch_results_.push_back(r);
                    ++done;
                    meta_fetch_progress_ = std::to_string(done) + "/" + std::to_string(total);
                });
            std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
            meta_fetch_outcome_ = out;
            meta_fetch_done_ = true;
        });
    });
}

// Called from the render loop: drains whatever the batch produced since the
// last frame (each result becomes pending edits, highlighted exactly like
// typed ones) and finalises it once the worker reports done.
void App::poll_pending_meta_fetch() {
    std::vector<MetaFetchResult> incoming;
    std::string progress;
    bool done = false;
    MetaFetchOutcome outcome;
    {
        std::lock_guard<std::mutex> lk(meta_fetch_mutex_);
        // All pushes happen under this same lock, so reading `done` in the
        // same critical section as the swap means a true `done` implies no
        // further results are coming.
        incoming.swap(meta_fetch_results_);
        progress = meta_fetch_progress_;
        done = meta_fetch_done_;
        if (done) { outcome = meta_fetch_outcome_; meta_fetch_done_ = false; }
    }

    if (!incoming.empty()) {
        int applied = 0;
        for (const auto& r : incoming) {
            if (!r.ok) continue;
            MetaEditEntry& e = meta_touch_entry(r.path);
            bool any = false;
            for (int i = 1; i < kMetaFieldCount; ++i) { // 0 = file name: AcoustID never renames anything
                if (r.set[i]) { e.edited[i] = true; e.value[i] = r.value[i]; any = true; }
            }
            if (any) { ++applied; meta_fetch_ok_paths_.push_back(r.path); }
        }
        if (applied) { meta_persist(); meta_refresh_lib_view(); }
        if (!progress.empty()) {
            (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "AcoustID: " + progress + " ...";
        }
    }
    if (!done) return;

    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join();
    meta_fetch_running_ = false;

    // Everything that came back is now in the session, so drop it from the
    // fetch list -- what remains is exactly what still needs another try.
    if (!meta_fetch_ok_paths_.empty() && !meta_fetch_list_.empty()) {
        std::vector<std::string> keep;
        for (const auto& p : meta_fetch_list_) {
            if (std::find(meta_fetch_ok_paths_.begin(), meta_fetch_ok_paths_.end(), p) == meta_fetch_ok_paths_.end()) {
                keep.push_back(p);
            }
        }
        meta_fetch_list_ = std::move(keep);
        meta_refresh_lib_view();
    }
    meta_fetch_ok_paths_.clear();

    std::string msg;
    if (outcome.python_missing || outcome.script_missing) {
        msg = "AcoustID: " + outcome.error;
    } else if (outcome.ok_count > 0 || outcome.fail_count > 0) {
        msg = "AcoustID: " + std::to_string(outcome.ok_count) + " ok";
        if (outcome.fail_count > 0) {
            msg += ", " + std::to_string(outcome.fail_count) + " failed";
            if (!outcome.error.empty()) msg += " (" + outcome.error + ")";
        }
    } else {
        msg = "AcoustID: " + (outcome.error.empty() ? std::string("no result") : outcome.error);
    }
    meta_persist();
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

// Ctrl+Shift+S -> Y. Writes every pending edit to disk, keeps the failures
// in the session so they can be retried, and only clears the backup when
// nothing is left over.
void App::meta_apply_session() {
    meta_ensure_session_loaded();
    if (meta_session_.empty()) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = "nothing to save -- no pending edits";
        return;
    }
    int ok = 0, failed = 0;
    std::string first_error;
    std::vector<MetaEditEntry> remaining;
    remaining.reserve(meta_session_.size());
    for (const auto& e : meta_session_) {
        std::string err, new_path;
        if (apply_meta_entry(e, &err, &new_path)) {
            ++ok;
            {
                // The tags on disk just changed, but row_meta_cache_ is
                // keyed by path and only ever filled in once per path (see
                // meta_row_meta() and rescan_library()'s background sweep,
                // which explicitly SKIPS any path already cached). Left
                // alone, a title-only edit -- same path, so rescan_library()
                // never re-resolves it -- would keep showing the pre-edit
                // tags (including "missing title") forever, even though the
                // file itself now has the new title. Dropping the cache
                // entry here (old path, and the new one if renamed) is what
                // makes the next read actually hit the file again.
                std::lock_guard<std::mutex> lk(row_meta_mutex_);
                row_meta_cache_.erase(e.path);
                if (!new_path.empty() && new_path != e.path) row_meta_cache_.erase(new_path);
            }
            if (!new_path.empty() && new_path != e.path) {
                // The file was renamed: keep every in-app reference pointed
                // at the new name so playback/queue/lyrics don't break.
                if (path_utf8(current_path_) == e.path) current_path_ = path_from_utf8(new_path);
                for (auto& q : queue_) {
                    if (q.is_local && path_utf8(q.local_path) == e.path) q.local_path = path_from_utf8(new_path);
                }
                for (auto& p : meta_fetch_list_) if (p == e.path) p = new_path;
            }
        } else {
            ++failed;
            if (first_error.empty()) first_error = err;
            remaining.push_back(e);
        }
    }
    meta_session_ = std::move(remaining);
    meta_persist();
    if (ok > 0) {
        // Names and tags changed -- rebuild the lists. rescan_library() only
        // refreshes the app's own views, so the meta menu's LIBRARY panel has
        // to be rebuilt as well: after a rename it would otherwise keep
        // showing (and keep handing out, via meta_hovering_path()) the old
        // path, and every edit made against it from there on would be filed
        // under a file that no longer exists.
        rescan_library();
        meta_refresh_lib_view();
    }
    std::string msg;
    if (failed == 0) {
        msg = "applied " + std::to_string(ok) + " pending edit" + (ok == 1 ? "" : "s") + " to disk";
    } else {
        msg = "applied " + std::to_string(ok) + ", " + std::to_string(failed) + " failed: " + first_error;
    }
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

// Ctrl+Shift+X -> Y. The audio files were never written to by an edit, so
// discarding only means throwing the pending values (and their backup file)
// away.
void App::meta_discard_session() {
    meta_ensure_session_loaded();
    int had = static_cast<int>(meta_session_.size());
    meta_session_.clear();
    meta_fetch_list_.clear();
    meta_fetch_ok_paths_.clear();
    meta_persist(); // empty -> the autosave backup file goes away
    std::string msg = "editing session discarded";
    if (had > 0) msg += " (" + std::to_string(had) + " file" + (had == 1 ? "" : "s") + " had pending edits)";
    (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = msg;
    log_event(msg);
}

// Shared Y/N/ESC handler for the three confirmations. Returns true whenever
// it consumed the key -- including the "a prompt is up, swallow everything
// else" case -- and false only when there is no prompt at all, which is what
// lets handle_key() call it unconditionally before its mode dispatch.
bool App::handle_meta_prompt_key(int key) {
    if (meta_prompt_ == MetaPrompt::None) return false;
    auto say = [this](const std::string& s) {
        (mode_ == Mode::MetaEdit ? meta_status_ : status_line_) = s;
    };
    if (key == 'y' || key == 'Y') {
        MetaPrompt p = meta_prompt_;
        meta_prompt_ = MetaPrompt::None;
        // Careful with meta_prompt_paths_: the fetch batch is built FROM it,
        // so it may only be cleared by the actions that don't read it -- an
        // eager clear here made meta_start_fetch() see an empty list and
        // return without fetching anything (silently: the confirmation had
        // already been dismissed).
        if (p == MetaPrompt::Save) {
            meta_prompt_paths_.clear();
            meta_apply_session();
        } else if (p == MetaPrompt::Discard) {
            meta_prompt_paths_.clear();
            meta_discard_session();
        } else {
            meta_start_fetch(); // clears meta_prompt_paths_ itself
        }
        return true;
    }
    if (key == 'n' || key == 'N' || key == 27) {
        meta_prompt_ = MetaPrompt::None;
        meta_prompt_paths_.clear();
        say("cancelled");
        return true;
    }
    return true; // swallow everything else while a confirmation is up
}

void App::handle_meta_key(int key) {
    if (handle_meta_prompt_key(key)) return;

    if (key == 27) { // ESC -- leaving always just keeps the autosave backup
        mode_ = Mode::Browse;
        return;
    }
    if (key == kKeyCtrlShiftS) {
        if (meta_session_.empty()) { meta_status_ = "nothing to save -- no pending edits"; return; }
        meta_prompt_ = MetaPrompt::Save;
        return;
    }
    if (key == kKeyCtrlShiftX) {
        if (meta_session_.empty() && meta_fetch_list_.empty()) { meta_status_ = "nothing to discard"; return; }
        meta_prompt_ = MetaPrompt::Discard;
        return;
    }
    // SHIFT+B for the hovered title -- raw 'B' with no arrow behind it (see
    // handle_key()'s Browse branch for why this can't be a normal hotkey).
    // Only fires while the current focus has no caret of its own to type
    // into: the library picker (focus 1) and the whole of tab 1's fetch
    // list, neither of which is a text field. On tab 0 with the search box
    // (focus 0) or the field editor (focus 2) focused, a capital B must
    // still be typable, so this hotkey is skipped there and 'B' falls
    // through to the normal text-entry handling below instead.
    if (key == 'B' && !last_key_was_arrow() && (meta_tab_ == 1 || meta_focus_ == 1)) {
        std::string path = meta_hovering_path();
        if (path.empty()) meta_status_ = "no title selected";
        else meta_prompt_single_fetch(path);
        return;
    }

    // Arrows collapse to 'A'..'D' app-wide; last_key_was_arrow() is what
    // tells a real arrow from a typed capital. Unlike the playlist editor
    // (where the name field only ever sees casual typing) these fields hold
    // real words, so capitals MUST stay typable here -- which is also why
    // the tab switch below only fires for the arrow itself. It is skipped
    // entirely while a field is being edited: Left/Right are the caret keys
    // there (see the field editor at the bottom), and bouncing to the fetch
    // list on every arrow press is exactly what made marking text
    // impossible.
    bool arrow = last_key_was_arrow();

    if (arrow && (key == 'C' || key == 'D') && !(meta_tab_ == 0 && meta_focus_ == 2)) { // left/right: the only 2 tabs
        meta_tab_ = (meta_tab_ + 1) % 2;
        meta_focus_ = 0;
        meta_refresh_lib_view();
        return;
    }
    if (key == 9) { // Tab: search field -> library picker -> field editor -> ...
        if (meta_tab_ == 0) meta_focus_ = (meta_focus_ + 1) % 3;
        return;
    }

    if (meta_tab_ == 1) { // --- fetch list ---
        int total = static_cast<int>(meta_fetch_list_.size());
        if (arrow && key == 'A') { if (meta_fetch_selected_ > 0) --meta_fetch_selected_; return; }
        if (arrow && key == 'B') { if (total > 0 && meta_fetch_selected_ < total - 1) ++meta_fetch_selected_; return; }
        if (key == '\r' || key == '\n') { meta_prompt_list_fetch(); return; } // "fetch metadata" over the whole list
        if (key == kKeyDelete || key == 127 || key == 'd') { meta_remove_hovering(); return; }
        return;
    }

    if (meta_focus_ == 0) { // --- search field ---
        // Up/Down still walk the library while the box has focus (same
        // convention as the main UI's "/" search); Left/Right are the caret
        // now, which is what makes marking -- and therefore copy/paste --
        // possible in here at all.
        if (arrow && key == 'A') { if (meta_lib_selected_ > 0) --meta_lib_selected_; return; }
        if (arrow && key == 'B') { if (meta_lib_selected_ + 1 < static_cast<int>(meta_lib_view_.size())) ++meta_lib_selected_; return; }
        if (key == '\r' || key == '\n') { meta_focus_ = 1; return; } // confirm the filter, jump to the list
        edit_focus("meta-search", meta_query_);
        if (edit_text_key(meta_query_, edit_caret_, edit_anchor_, key, 120, &meta_status_))
            meta_refresh_lib_view();
        return;
    }

    if (meta_focus_ == 1) { // --- library picker ---
        int total = static_cast<int>(meta_lib_view_.size());
        if (arrow && key == 'A') { if (meta_lib_selected_ > 0) --meta_lib_selected_; return; }
        if (arrow && key == 'B') { if (total > 0 && meta_lib_selected_ < total - 1) ++meta_lib_selected_; return; }
        if (key == '\r' || key == '\n') {
            if (total == 0) { meta_status_ = "no track selected"; return; }
            meta_focus_ = 2; // start editing the hovered title's fields
            return;
        }
        if (key == 'a') { meta_add_hovering_to_fetch(); return; } // queue it for AcoustID, like the main queue's 'a'
        if (key == 'r') { meta_toggle_resort(); return; } // toggle: edited files on top vs. alphabetical order
        // Missing-tag filters. 'T'/'Y' have no other meaning here; 'A' is
        // only the Up ARROW together with last_key_was_arrow(), which the
        // two navigation keys above already claimed -- so a plain Shift+A
        // typed here reaches this line.
        if (key == 'x') { meta_toggle_filter(1); return; } // only files with no metadata at all
        if (key == 'T') { meta_toggle_filter(2); return; } // SHIFT+T: missing title
        if (key == 'A') { meta_toggle_filter(3); return; } // SHIFT+A: missing artist
        if (key == 'Y') { meta_toggle_filter(4); return; } // SHIFT+Y: missing year
        if (key == kKeyDelete || key == 127 || key == 'd') { meta_remove_hovering(); return; }
        return; // every other printable key would be search input, and search lives in focus 0
    }

    // --- field editor: type directly into the selected field ---
    {
        std::string path = meta_hovering_path();
        if (path.empty()) { meta_focus_ = 1; return; }

        // The caret belongs to one specific (file, field) pair. If either
        // changed since the last keypress -- a field move, another row, the
        // 'r' resort reordering the list -- the offsets now point at a
        // different string, so retarget them to the end of this one instead
        // of letting them be clamped against the wrong text.
        const MetaEditEntry* e = meta_entry(path);
        std::string cur = meta_display_value(path, meta_field_, e);
        const std::string owner = path + "#" + std::to_string(meta_field_);
        if (edit_owner_ != owner) {
            edit_owner_ = owner;
            edit_caret_ = edit_anchor_ = cur.size();
        }
        le_clamp(cur, edit_caret_, edit_anchor_);
        // meta_set_field() marks the file as edited and rewrites the
        // autosave unconditionally, so it is only ever reached when the
        // bytes really changed -- moving the caret must not make a file
        // count as edited.
        const std::string orig = cur;
        auto commit = [&]() { if (cur != orig) meta_set_field(path, meta_field_, cur); };
        const size_t limit = (meta_field_ == static_cast<int>(MetaField::FileName)) ? 200 : 160;

        if (arrow && key == 'A') { if (meta_field_ > 0) --meta_field_; return; }
        if (arrow && key == 'B') { if (meta_field_ + 1 < kMetaFieldCount) ++meta_field_; return; }
        if (key == '\r' || key == '\n') {
            meta_focus_ = 1; // "done with this field"
            // With the 'r' resort on, the file just edited has (re)joined the
            // edited block: move it up now, while the cursor is still on it --
            // meta_refresh_lib_view() follows the row by path.
            if (meta_resort_edited_ && !meta_lib_view_.empty()) meta_refresh_lib_view();
            return;
        }
        // --- caret, selection, clipboard ---------------------------------
        // Shift+arrows mark, Ctrl+C/X/V copy/cut/paste, Home/End jump. They
        // arrive as their own sentinel values (terminal_ui.h) because the
        // bare ones are already taken here: a plain arrow press collapses to
        // the letter it would otherwise type, and a typed 'c' has to stay a
        // typed 'c' inside a word.
        if (key == kKeyHome) { edit_caret_ = edit_anchor_ = 0; return; }
        if (key == kKeyEnd) { edit_caret_ = edit_anchor_ = cur.size(); return; }
        if (key == kKeyShiftLeft) { le_move(cur, edit_caret_, edit_anchor_, -1, true); return; }
        if (key == kKeyShiftRight) { le_move(cur, edit_caret_, edit_anchor_, +1, true); return; }
        if (key == kKeyCtrlC) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            std::string t = (a == b) ? cur : cur.substr(a, b - a);
            clipboard_set(t);
            meta_status_ = t.empty() ? "nothing selected" : "COPIED";
            return;
        }
        if (key == kKeyCtrlX) {
            size_t a = 0, b = 0;
            le_range(edit_caret_, edit_anchor_, a, b);
            if (a == b) return; // never empty the field by accident
            clipboard_set(cur.substr(a, b - a));
            le_erase_selection(cur, edit_caret_, edit_anchor_);
            commit();
            meta_status_ = "CUT";
            return;
        }
        if (key == kKeyCtrlV) {
            le_paste(cur, edit_caret_, edit_anchor_, clipboard_get(), limit);
            commit();
            meta_status_ = "PASTED";
            return;
        }
        if (key == kKeyDelete) { le_delete_forward(cur, edit_caret_, edit_anchor_); commit(); return; }
        if (key == 127 || key == 8) { le_backspace(cur, edit_caret_, edit_anchor_); commit(); return; }
        if (arrow && (key == 'C' || key == 'D')) { // plain Left/Right: move the caret, leave the tab alone
            le_move(cur, edit_caret_, edit_anchor_, key == 'D' ? -1 : +1, false);
            return;
        }
        if (is_text_key(key)) {
            le_insert(cur, edit_caret_, edit_anchor_, static_cast<char>(key), limit);
            commit();
        }
        return;
    }
}

std::vector<std::string> App::build_meta_library_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    // The pane title carries the active missing-tag filter, so a list
    // narrowed by 'x' or Shift+T/A/Y is never mistaken for the whole
    // library (the status line repeats it with a count).
    std::string lib_label = "LIBRARY";
    if (meta_filter_ != 0) lib_label += std::string(" [") + meta_filter_label() + "]";
    if (meta_focus_ == 1) lib_label += " \u25c0";
    out.push_back(box_top(lib_label, total_width, border_ansi));

    int total = static_cast<int>(meta_lib_view_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string plain = (meta_filter_ != 0) ? "NO TRACKS MATCH THE FILTER"
                                    : meta_query_.empty() ? "NO TRACKS FOUND"
                                    : "NO MATCHING TRACK";
                std::string text = apply_font_map(plain, settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            std::string list_ansi = ansi_for(settings_.list_color, false);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(meta_lib_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        bool edited = false;
        if (idx < total) {
            const auto& t = meta_lib_view_[idx];
            int title_w = std::max(5, inner - idx_w - 2);
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_title = apply_font_map(list_row_title(t.path, t.title), settings_.font_map);
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_title, title_w), title_w);
            // Files carrying pending edits are painted in the header colour,
            // the same bold highlight an edited field gets in the FIELDS
            // panel -- so the list answers "which files is that panel talking
            // about?" without having to walk over them one by one.
            const MetaEditEntry* e = meta_entry(path_utf8(t.path));
            edited = e && e->any_edited();
        }
        // The selected row stays highlighted in EVERY focus, not just while
        // the pane itself owns focus: arrows scroll the library from the
        // search field too (see handle_meta_key(), focus 0), and without the
        // highlight there was nothing on screen saying which row those arrows
        // were moving. Matches build_list_panel() in Browse, which never gates
        // its cursor on focus either -- the LIBRARY box's "◀" stays the focus
        // marker, so there is still exactly one thing naming the focused pane.
        bool sel = (idx == meta_lib_selected_);
        std::string padded = pad_right(truncate_str(content, inner), inner);
        std::string base;
        if (sel) {
            base = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
        } else {
            base = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
        }
        // Colour AFTER padding: display_width() is UTF-8-aware but not
        // ANSI-aware, so a coloured row would otherwise measure wrong.
        if (edited) {
            out.push_back(bar + " " + base + header_sgr(settings_) + padded
                          + "\x1b[0m" + base + "\x1b[0m " + bar);
        } else {
            out.push_back(bar + " " + base + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    // One number for the highlight above: how many files in the session have
    // pending edits (meta_session_ only ever holds those).
    if (!meta_session_.empty()) {
        std::string tag = "[ " + std::to_string(meta_session_.size()) + " edited ]";
        footer += footer.empty() ? tag : "  " + tag;
    }
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// The five editable fields. This is where the header-colour highlight and
// the block cursor live, and both are built the same way build_list_panel()
// builds a row: plain segments are measured first, then the already-sized
// pieces get wrapped in colour -- display_width() is UTF-8-aware but NOT
// ANSI-aware, so colouring before padding would miscount the row.
std::vector<std::string> App::build_meta_fields_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    std::string path = meta_hovering_path();
    std::string name = path.empty() ? std::string() : path_utf8(path_from_utf8(path).filename());
    std::string sel_pos;
    if (!meta_lib_view_.empty()) {
        sel_pos = "  " + std::to_string(meta_lib_selected_ + 1) + "/" + std::to_string(meta_lib_view_.size());
    }
    std::string label = "EDIT" + sel_pos + (name.empty() ? std::string() : " " + truncate_str(name, 40))
                      + (meta_focus_ == 2 ? " \u25c0" : std::string());
    out.push_back(box_top(label, total_width, border_ansi));

    if (path.empty()) {
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == height / 2) {
                std::string text = apply_font_map("NO TRACK SELECTED", settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    const MetaEditEntry* entry = meta_entry(path);
    const int label_w = 8;
    for (int row = 0; row < height; ++row) {
        if (row >= kMetaFieldCount) { // unused rows stay blank, so the panel height never changes
            std::string padded = pad_right(std::string(), inner);
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
            continue;
        }
        int i = row;
        std::string lab = pad_right(meta_field_label(i), label_w);
        std::string val_raw = meta_hover_values_[i];
        if (val_raw.empty()) {
            val_raw = (i == 0) ? std::string("-")
                    : (meta_hover_resolved_ ? "(untagged)" : "(reading ...)");
        }
        bool sel = (meta_focus_ == 2) && (i == meta_field_);
        bool edited = entry && entry->edited[i];
        const int val_w = std::max(4, inner - label_w);

        std::string base = sel ? cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color)
                               : (ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color));

        std::string val;
        int used = 0;
        if (sel) {
            // The field being typed into is painted by paint_edit_field():
            // the window scrolls so the caret stays inside the panel, the
            // marked range is drawn in reverse video, and the caret itself
            // is the block glyph this panel uses for a cursor (it renders
            // whole frames, so there is no real terminal cursor to move --
            // which is also why an arrow press used to be free to switch
            // tabs while the text still had nowhere to go).
            // The caret only belongs to this row if the offsets were last
            // set against exactly this (file, field) pair -- see the field
            // editor in handle_meta_key().
            const std::string owner = path + "#" + std::to_string(i);
            size_t c = val_raw.size(), a = val_raw.size();
            if (edit_owner_ == owner) { c = edit_caret_; a = edit_anchor_; }
            const std::string restore = base + (edited ? header_sgr(settings_) : std::string());
            EditPaint p = paint_edit_field(val_raw, c, a, val_w, restore, /*block=*/true);
            val = p.s;
            used = display_width(lab) + p.cols; // p.cols already ignores the ANSI codes
        } else {
            val = truncate_str(val_raw, val_w);
            used = display_width(lab) + display_width(val);
        }
        std::string fill(std::max(0, inner - used), ' ');

        std::string row_ansi = base + lab;
        if (edited) row_ansi += header_sgr(settings_); // bold + header colour: this field has been edited
        row_ansi += val;
        if (edited) row_ansi += "\x1b[0m" + base; // back to the row's own colours for the padding
        row_ansi += fill + "\x1b[0m";
        out.push_back(bar + " " + row_ansi + " " + bar);
    }

    int n_edited = 0;
    if (entry) for (bool b : entry->edited) if (b) ++n_edited;
    std::string footer = n_edited ? ("( " + std::to_string(n_edited) + " edited, pending )") : "";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

std::vector<std::string> App::build_meta_fetch_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    out.push_back(box_top("FETCH LIST (" + std::to_string(meta_fetch_list_.size()) + ") \u25c0",
                          total_width, border_ansi));

    int total = static_cast<int>(meta_fetch_list_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map("NOTHING QUEUED -- PRESS 'a' ON A TRACK IN THE EDIT TAB",
                                                  settings_.font_map);
                int left = std::max(0, (inner - display_width(text)) / 2);
                content = std::string(left, ' ') + text;
            }
            std::string padded = pad_right(truncate_str(content, inner), inner);
            out.push_back(bar + " " + ansi_for(settings_.list_color, false) + padded + "\x1b[0m " + bar);
        }
        out.push_back(box_bottom(total_width, "", border_ansi_bottom));
        return out;
    }

    int scroll = std::clamp(meta_fetch_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const std::string& p = meta_fetch_list_[idx];
            int title_w = std::max(5, inner - idx_w - 2);
            const MetaEditEntry* e = meta_entry(p);
            std::string base = list_row_title(path_from_utf8(p), path_utf8(path_from_utf8(p).stem()));
            std::string shown = (e && e->any_edited()) ? (base + " [edited]") : base;
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_title = apply_font_map(shown, settings_.font_map);
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_title, title_w), title_w);
        }
        bool sel = (idx == meta_fetch_selected_);
        std::string padded = pad_right(truncate_str(content, inner), inner);
        if (sel) {
            std::string cursor_ansi = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
            out.push_back(bar + " " + cursor_ansi + padded + "\x1b[0m " + bar);
        } else {
            std::string list_ansi = ansi_for(settings_.list_color, false) + bg_ansi_for(settings_.list_inactive_bg_color);
            out.push_back(bar + " " + list_ansi + padded + "\x1b[0m " + bar);
        }
    }
    std::string footer;
    int remaining = total - (scroll + height);
    if (remaining > 0) footer = "( " + std::to_string(remaining) + " more )";
    out.push_back(box_bottom(total_width, footer, border_ansi_bottom));
    return out;
}

// Full-screen meta editor, assembled exactly like build_playlist_screen():
// header box, tab strip, optional search row, boxed panels, then a footer
// that is ALWAYS three lines (prompt padded to two + status, or legend's
// two rows + status) so switching states never makes the layout jump.
void App::build_meta_screen(std::ostringstream& frame, int W, int target_height) const {
    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    frame << box_top("META EDITOR", W, border) << "\n";

    // Tab strip, hand-built like the playlist editor's (box_line() can't
    // measure the reverse-video highlight before padding it).
    {
        std::string bar = border + settings_.box_vertical + R;
        std::string plain0 = " 1: EDIT ";
        std::string plain1 = " 2: FETCH LIST ";
        std::string gap = "   ";
        int inner = W - 4;
        std::string plain_row = plain0 + gap + plain1;
        std::string seg0 = (meta_tab_ == 0) ? (HI + plain0 + R) : plain0;
        std::string seg1 = (meta_tab_ == 1) ? (HI + plain1 + R) : plain1;
        int pad_n = std::max(0, inner - display_width(plain_row));
        frame << bar << " " << seg0 + gap + seg1 << std::string(pad_n, ' ') << " " << bar << "\n";
    }

    int fixed_rows = 3; // top border + tab strip + bottom border
    if (meta_tab_ == 0) {
        // Painted by hand: a marked range carries reverse-video escapes,
        // and box_line() pads with display_width(), which counts escape
        // bytes as columns. box_line_field() pads against what the field
        // is really worth instead.
        const std::string prefix = "Search: ";
        std::string field;
        int field_cols = 0;
        if (meta_focus_ == 0) { // focus is in the box: caret + block cursor
            EditPaint p = paint_edit_field(meta_query_, edit_caret_, edit_anchor_,
                                           std::max(1, W - 4 - static_cast<int>(prefix.size())), "", true);
            field = p.s;
            field_cols = p.cols;
        } else { // focus elsewhere: plain text, with the placeholder hint
            field = meta_query_.empty() ? std::string("(type to filter)") : meta_query_;
            field_cols = static_cast<int>(display_width(field));
        }
        frame << box_line_field(prefix, field, field_cols, W, border) << "\n";
        fixed_rows += 1;
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    // Sized against BOTH bounds that matter: term_rows_ - 1 is what
    // clamp_output_rows() will actually keep (a footer line chopped off
    // every frame is a status message or half a confirmation prompt gone),
    // while player_view_height() is what the other overlays are sized to --
    // except on a short terminal, where the Browse view's metadata/progress/
    // search panels don't shrink and that value is several rows taller than
    // the screen. total = fixed_rows + (panel_h + 2) + 3 must fit the
    // smaller of the two: 2 for the panel's own border rows, 3 for the
    // footer (two legend rows + status -- see where it's written below).
    int budget = std::min(target_height, term_rows_ - 1);
    int panel_h = std::clamp(budget - fixed_rows - 5, 6, 22);
    if (meta_tab_ == 0) {
        int left_w = W / 2;
        int right_w = W - left_w;
        auto left_lines = build_meta_library_panel(left_w, panel_h);
        auto right_lines = build_meta_fields_panel(right_w, panel_h);
        size_t rows = std::max(left_lines.size(), right_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < left_lines.size()) ? left_lines[i] : std::string(left_w, ' ');
            std::string r = (i < right_lines.size()) ? right_lines[i] : std::string(right_w, ' ');
            frame << l << r << "\n";
        }
    } else {
        for (auto& l : build_meta_fetch_panel(W, panel_h)) frame << l << "\n";
    }

    if (meta_prompt_ != MetaPrompt::None) {
        // Wrapped to at most 2 lines: the AcoustID disclaimer is longer
        // than a narrow terminal, and an unwrapped bar would wrap into a
        // third line and push the whole overlay past its own height.
        std::string text;
        std::string colors;
        if (meta_prompt_ == MetaPrompt::Fetch) {
            text = std::string(kMetaFetchDisclaimer) + "   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[43;30m"; // yellow, same as the playlist editor's save prompt
        } else if (meta_prompt_ == MetaPrompt::Save) {
            text = "Want to save? Applies every pending edit to the files.   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[43;30m";
        } else {
            text = "Want to discard changes? The files themselves were never touched.   [Y]es   [N]o   [ESC] cancel";
            colors = "\x1b[41;97m"; // red, same as its delete prompt
        }
        auto lines = wrap_lines(text, std::max(10, W - 2), 2);
        // Padded to the same three rows the legend footer always uses, so a
        // prompt appearing (or going away) can never change the height of
        // what sits above it.
        while (lines.size() < 3) lines.push_back(std::string());
        for (const auto& l : lines) {
            // The trailing row is reserved (so the footer never changes
            // height between a prompt and a hint/status), but a reserved
            // row stays bare: a full-width empty color bar reads as a
            // rendering glitch, not as an empty status line.
            if (l.empty()) frame << "\n";
            else frame << colors << " " << l << " \x1b[0m\n";
        }
    } else {
        std::string hint = (meta_tab_ == 0)
            ? "[\u2190\u2192] Tab | [TAB] Focus | [\u2191\u2193] Navi. | [ENTER] Edit | [SHIFT+←→] Mark | [Ctrl+C/X/V] Copy/Cut/Paste | [a] Fetch list | "
              "[SHIFT+B] Fetch | [r] Edited first | [x/T/A/Y] Missing meta | [CTRL+SHIFT+S] Save | [CTRL+SHIFT+X] Discard | [ESC] Exit"
            : "[\u2190\u2192] Tab | [ENTER] Fetch all | [SHIFT+B] Fetch this | [DEL] Remove | "
              "[CTRL+SHIFT+S] Save | [CTRL+SHIFT+X] Discard | [ESC] Exit";
        // The legend is wider than the screen (tab 0: ~210 columns once the
        // selection/clipboard commands are in it, tab 1: 116), and
        // truncate_str(hint, W) used to cut it mid-command at 120 -- the
        // [CTRL+SHIFT+X] Discard half of it was simply never shown. Split it
        // back into its " | "-separated entries and pack them greedily
        // instead: an entry only moves to the second legend row when the row
        // it would join is already full, so at 120 columns every command
        // stays readable -- never cut through the middle of a key name.
        // TWO rows is a hard limit, not just a preference: build_meta_screen()
        // sizes the panels from "two legend rows + one status row", so a
        // third row would make the overlay one line taller than the terminal
        // budget and bring the scrollbar back. Anything that still doesn't
        // fit is therefore joined onto row two and truncated there by the
        // writer below; at the 120-column size this app targets nothing gets
        // dropped (~114 + ~94 columns), the cap only guards narrower
        // terminals, where the alternative used to be a growing footer. Both
        // tabs are padded to exactly two legend rows (+ the status row), so
        // the footer keeps one constant height: switching tabs, or a status
        // message appearing, can't jump the layout.
        std::vector<std::string> hint_lines(1);
        size_t pos = 0;
        for (;;) {
            size_t sep = hint.find(" | ", pos);
            std::string entry = (sep == std::string::npos) ? hint.substr(pos)
                                                           : hint.substr(pos, sep - pos);
            std::string& cur = hint_lines.back();
            std::string joined = cur.empty() ? entry : cur + " | " + entry;
            if (cur.empty() || display_width(joined) <= W || hint_lines.size() >= 2) cur = joined;
            else hint_lines.push_back(entry);
            if (sep == std::string::npos) break;
            pos = sep + 3;
        }
        while (hint_lines.size() < 2) hint_lines.push_back(std::string());
        for (const auto& l : hint_lines) {
            // An empty row is written bare: a full-width colour bar over an
            // empty line reads as a rendering glitch, not as a blank row
            // (same reasoning as in the prompt branch above).
            if (l.empty()) frame << "\n";
            else frame << "\x1b[90m" << truncate_str(l, W) << "\x1b[0m\n";
        }
        if (!meta_status_.empty()) frame << "\x1b[32m" << truncate_str(meta_status_, W) << "\x1b[0m\n";
        else frame << "\n";
    }
}

// ---------------------------------------------------------------------------
// Listening history overlay (Mode::History, HKeyHistory = Shift+H)
//
// Modelled on the two overlays above: same tab strip, same boxed panel, same
// "two legend rows + one status row" footer, so its height never depends on
// what it happens to be showing. Three tabs:
//
//   1 HISTORY     the last 100 plays, newest first, each with a PLAY / done /
//                 skip marker (the live play is the top row while music runs)
//   2 TOP TRACKS  one row per title -- length and play count -- sorted by play
//                 count; 'r' flips between most- and least-played first
//   3 HABITS      session / time-per-day / play-behaviour aggregates, every
//                 category under a header_sgr() header, exactly like the
//                 Settings section titles and the cheat sheet's categories
//
// It is a pure viewer: no key in here edits anything, and 'r' only flips an
// in-memory sort order. The data is maintained outside this overlay -- see
// history_end_current_play()/history_begin_current_play() below (called on
// every playback hand-over) and the frame loop's history_.add_listened().
// ---------------------------------------------------------------------------

void App::history_open() {
    mode_ = Mode::History;
    history_tab_ = 0;
    history_selected_ = 0;
    history_scroll_ = 0;
    history_pane_ = 0;
    history_add_sel_ = 0;
    history_status_.clear();
    if (history_.plays().empty())
        history_status_ = "nothing played yet in this installation";
    history_refresh_top();
    force_redraw_ = true; // full-screen takeover, same rule as entering Settings
}

void App::history_refresh_top() {
    history_top_view_ = history_top(history_.plays(), history_most_first_);
}

// Closes the record of whatever is playing right now. Called on every
// hand-over (a new track replacing it, playback running out, the app
// quitting), never from anywhere that could see a record twice -- the store
// itself refuses to close one that isn't live.
void App::history_end_current_play() {
    if (!history_.live()) return;
    const HistoryPlay* p = history_.live_play();
    const double len = p ? p->len_sec : 0.0;
    const double heard = p ? p->listened_sec : 0.0;
    // "Heard to the end" = the player ran out of track, or the listener got
    // through 90% of it (which also covers seeking close to the end and then
    // letting it run out). Everything else -- a skip, an app closed
    // mid-track -- is not a completion, and that is what the Habits tab's
    // completion rate is built on.
    const bool completed = player_.finished() || (len > 0.0 && heard >= 0.9 * len);
    history_.finish_live(completed);
    history_.save(); // one small JSON write per finished track
}

// Opens a record for current_path_/current_video_id_/metadata_. Must only be
// called once those three already describe the NEW track.
void App::history_begin_current_play() {
    if (!has_track_) return;
    HistoryPlay p;
    p.id = history_track_id(current_is_local_, path_utf8(current_path_), current_video_id_);
    if (p.id.empty()) return; // nothing to aggregate by: record nothing rather than a phantom title
    p.title = metadata_.name;
    if (p.title.empty() || p.title == "-")
        p.title = path_utf8(current_path_.stem()); // untagged local file
    p.artist = (metadata_.artist == "-") ? std::string() : metadata_.artist;
    p.len_sec = static_cast<double>(total_sec_);
    p.started_at = static_cast<long long>(std::time(nullptr));
    history_.begin_play(p);
}

void App::history_add_top_to_queue(int n) {
    // Ranked independently of the list above (which 'r' can flip to
    // least-played first): "top N" always means the N most-played titles.
    const std::vector<HistoryTopRow> top = history_top(history_.plays(), /*most_first=*/true);
    if (top.empty()) {
        history_status_ = "nothing played yet -- nothing to queue";
        return;
    }
    const int take = std::min(n, static_cast<int>(top.size()));
    int added = 0, missing = 0;
    for (int i = 0; i < take; ++i) {
        const HistoryTopRow& r = top[static_cast<size_t>(i)];
        // The record only keeps the title; the artist comes from the newest
        // play of the same track (plays_ is newest-first).
        std::string artist;
        for (const HistoryPlay& p : history_.plays()) {
            if (p.id == r.id) { artist = p.artist; break; }
        }
        if (r.id.compare(0, 3, "yt:") == 0) {
            queue_.push_back({false, r.title, artist, {}, r.id.substr(3)});
            ++added;
        } else {
            fs::path path = path_from_utf8(r.id);
            std::error_code ec;
            if (!fs::exists(path, ec)) { ++missing; continue; } // moved/deleted since it was played
            queue_.push_back({true, r.title, artist, path, ""});
            ++added;
        }
    }
    clamp_queue_selected();
    std::string msg = "queued " + std::to_string(added) + " of top " + std::to_string(n) + " tracks";
    if (take < n) msg += " (only " + std::to_string(take) + " played so far)";
    if (missing > 0) msg += " (" + std::to_string(missing) + " missing, skipped)";
    history_status_ = msg;
    log_event(msg);
}

void App::handle_history_key(int key) {
    if (key == 0) return;
    const bool arrow = last_key_was_arrow();

    // Leaving: ESC, or SHIFT+H again -- the very key that opened this
    // (nothing here is a text field, so an uppercase 'H' is always that key).
    if (key == 27 || (key == 'H' && !arrow)) { mode_ = Mode::Browse; return; }

    // Top Tracks tab: TAB does not cycle the tab strip there, it switches
    // between the two panes (the track list and ADD TOP TRACKS TO QUEUE).
    // Left/Right and 1/2/3 still move between tabs.
    if (key == 9 && history_tab_ == 1) {
        history_pane_ ^= 1;
        history_status_.clear();
        return;
    }

    // Tab strip: Left/Right or TAB cycles, 1/2/3 jumps outright.
    if ((arrow && (key == 'C' || key == 'D')) || key == 9) {
        // 'D' is Left, 'C' is Right -- the same convention the seek keys and
        // the caret movement above use (see handle_key()'s "key == 'D' //
        // left = seek back"). Swapped here, every arrow press walked the tab
        // strip backwards, which on a 3-tab strip looks like jumping 1->3.
        const int dir = (key == 'D') ? -1 : 1;
        history_tab_ = (history_tab_ + dir + 3) % 3;
        history_selected_ = 0;
        history_scroll_ = 0;
        history_pane_ = 0;
        history_status_.clear();
        return;
    }
    if (key == '1' || key == '2' || key == '3') {
        history_tab_ = key - '1';
        history_selected_ = 0;
        history_scroll_ = 0;
        history_pane_ = 0;
        history_status_.clear();
        return;
    }

    // Top Tracks tab, lower pane focused: Up/Down pick Top 10/25/50/100 and
    // Enter queues that many. (Enter does nothing while the list is focused.)
    if (history_tab_ == 1 && history_pane_ == 1) {
        if (arrow && key == 'A') { if (history_add_sel_ > 0) --history_add_sel_; return; }
        if (arrow && key == 'B') { if (history_add_sel_ < 3) ++history_add_sel_; return; }
        if (key == '\r' || key == '\n') { history_add_top_to_queue(kHistoryAddCounts[history_add_sel_]); return; }
    }

    // Up/Down: a cursor on the two list tabs, plain scrolling on Habits
    // (whose content has no cursor and is taller than the panel).
    const int total = (history_tab_ == 0)
        ? static_cast<int>(std::min<size_t>(100, history_.plays().size()))
        : static_cast<int>(history_top_view_.size());
    if (arrow && key == 'A') {
        if (history_tab_ == 2) { if (history_scroll_ > 0) --history_scroll_; }
        else if (history_selected_ > 0) --history_selected_;
        return;
    }
    if (arrow && key == 'B') {
        if (history_tab_ == 2) ++history_scroll_; // clamped against the real content while drawing
        else if (history_selected_ + 1 < total) ++history_selected_;
        return;
    }

    // 'r' flips the Top Tracks order: most played on top (the default) and
    // least played on top. Nowhere else to do it, so it is a no-op there
    // rather than a surprise.
    if (key == 'r' || key == 'R') {
        if (history_tab_ != 1) return;
        history_most_first_ = !history_most_first_;
        history_refresh_top();
        if (history_selected_ >= static_cast<int>(history_top_view_.size()))
            history_selected_ = std::max(0, static_cast<int>(history_top_view_.size()) - 1);
        history_status_ = history_most_first_ ? "TOP TRACKS: most played first"
                                              : "TOP TRACKS: least played first";
        return;
    }
    if (key == 'q' || key == 'Q') mode_ = Mode::Browse;
}

void App::build_history_screen(std::ostringstream& frame, int W, int target_height) {
    // Rebuilt every frame: a track can finish (and another start) while this
    // overlay is on screen -- playback never pauses for it.
    history_refresh_top();

    if (W < 60) W = 60;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string HI = "\x1b[7m", R = "\x1b[0m";

    frame << box_top("LISTENING HISTORY", W, border) << "\n";

    // Tab strip, hand-built like the two overlays above (box_line() can't
    // measure the reverse-video highlight before padding it).
    {
        std::string bar = border + settings_.box_vertical + R;
        std::string p0 = " 1: HISTORY ", p1 = " 2: TOP TRACKS ", p2 = " 3: HABITS ";
        std::string gap = "   ";
        int inner = W - 4;
        std::string plain_row = p0 + gap + p1 + gap + p2;
        std::string seg0 = (history_tab_ == 0) ? (HI + p0 + R) : p0;
        std::string seg1 = (history_tab_ == 1) ? (HI + p1 + R) : p1;
        std::string seg2 = (history_tab_ == 2) ? (HI + p2 + R) : p2;
        int pad_n = std::max(0, inner - display_width(plain_row));
        frame << bar << " " << seg0 + gap + seg1 + gap + seg2 << std::string(pad_n, ' ') << " " << bar << "\n";
    }

    int fixed_rows = 3; // top border + tab strip + bottom border
    frame << box_bottom(W, "", border_bottom) << "\n";

    // Same two bounds build_meta_screen() sizes against: term_rows_ - 1 is
    // what clamp_output_rows() keeps, target_height is what the other
    // overlays use. 5 = the panel's own two border rows + the three footer rows.
    int budget = std::min(target_height, term_rows_ - 1);
    // Top Tracks carries a second pane (ADD TOP TRACKS TO QUEUE: four option
    // rows + its own two border rows) below the list, so the list gives those
    // rows up -- the whole tab still adds up to `budget` lines (29 on a
    // 30-line terminal).
    const bool top_tab = (history_tab_ == 1);
    const int add_body = 4;                       // Top 10 / 25 / 50 / 100
    const int add_rows = top_tab ? add_body + 2 : 0;
    int panel_h = std::clamp(budget - fixed_rows - 5 - add_rows, top_tab ? 3 : 6, 22);
    for (const auto& l : build_history_panel(W, panel_h)) frame << l << "\n";
    if (top_tab)
        for (const auto& l : build_history_add_panel(W, add_body)) frame << l << "\n";

    // Footer: two legend rows + one status row, always exactly three, so a
    // message appearing can never change the height of what sits above it.
    const std::string hint = (history_tab_ == 1)
        ? "[\u2190\u2192] Tab | [1/2/3] Tab | [TAB] Switch pane | [\u2191\u2193] Move | [ENTER] Add to queue | [r] Flip sort | [ESC] Exit"
        : "[\u2190\u2192/TAB] Tab | [1/2/3] Tab | [\u2191\u2193] Move | [r] Flip sort | [ESC] Exit";
    std::vector<std::string> hint_lines(1);
    size_t pos = 0;
    for (;;) {
        size_t sep = hint.find(" | ", pos);
        std::string entry = (sep == std::string::npos) ? hint.substr(pos) : hint.substr(pos, sep - pos);
        std::string& cur = hint_lines.back();
        std::string joined = cur.empty() ? entry : cur + " | " + entry;
        if (cur.empty() || display_width(joined) <= W || hint_lines.size() >= 2) cur = joined;
        else hint_lines.push_back(entry);
        if (sep == std::string::npos) break;
        pos = sep + 3;
    }
    while (hint_lines.size() < 2) hint_lines.push_back(std::string());
    for (const auto& l : hint_lines) {
        if (l.empty()) frame << "\n";
        else frame << "\x1b[90m" << truncate_str(l, W) << "\x1b[0m\n";
    }
    if (!history_status_.empty()) frame << "\x1b[32m" << truncate_str(history_status_, W) << "\x1b[0m\n";
    else frame << "\n";
}

std::vector<std::string> App::build_history_panel(int total_width, int height) {
    const int inner = std::max(20, total_width - 4);
    const int body = std::max(1, height); // rows between box_top() and box_bottom()
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;

    static const char* kTabTitle[3] = {"HISTORY", "TOP TRACKS", "HABITS"};
    std::string title = kTabTitle[history_tab_];
    if (history_tab_ == 1)
        title += history_most_first_ ? "  --  most played first" : "  --  least played first";
    out.push_back(box_top(title, total_width, border_ansi));

    // Column helpers. Both are only ever fed PLAIN text: display_width()
    // counts escape bytes as columns, so a decorated string must be padded
    // first and coloured after (same rule every other panel here follows).
    auto L = [](const std::string& s, int n) { // right-align an ASCII value
        if (static_cast<int>(s.size()) >= n) return s.substr(0, static_cast<size_t>(n));
        return std::string(static_cast<size_t>(n - s.size()), ' ') + s;
    };
    auto T = [&](const std::string& s, int n) { // left-align, truncated at display width
        std::string t = truncate_str(s, n);
        const int fill = n - display_width(t);
        return t + (fill > 0 ? std::string(static_cast<size_t>(fill), ' ') : std::string());
    };
    auto row = [&](const std::string& plain, const std::string& sgr) {
        out.push_back(bar + " " + sgr + pad_right(truncate_str(plain, inner), inner) + R + " " + bar);
    };
    // A section header: only the title carries the colour, like every other
    // header in the app -- a full-width bar would read as a selected row.
    auto head = [&](const std::string& plain) {
        std::string t = truncate_str(plain, inner);
        out.push_back(bar + " " + header_sgr(settings_) + t + R +
                      std::string(static_cast<size_t>(std::max(0, inner - display_width(t))), ' ') + " " + bar);
    };
    auto blank = [&]() { out.push_back(bar + " " + std::string(static_cast<size_t>(inner), ' ') + " " + bar); };

    // text + style: 0 plain, 1 section header, 2 cursor row
    std::vector<std::pair<std::string, int>> lines;
    int cursor = -1; // index of the selected line, for the two list tabs

    if (history_tab_ == 0) {
        const auto& plays = history_.plays();
        const int total = static_cast<int>(std::min<size_t>(100, plays.size()));
        const int when_w = 11, len_w = 7, state_w = 5;
        const int title_w = std::max(8, inner - when_w - 2 - len_w - 2 - state_w);
        lines.push_back({T("WHEN", when_w) + "  " + T("TITLE", title_w) + L("LEN", len_w) + "  " + T("STATE", state_w), 1});
        if (total == 0) {
            lines.push_back({"nothing played yet -- plays show up here as soon as music runs", 0});
        } else {
            for (int i = 0; i < total; ++i) {
                const HistoryPlay& p = plays[i];
                std::string t = (p.artist.empty() || p.artist == "-") ? p.title
                                                                     : p.artist + " - " + p.title;
                if (t.empty() || t == "-") t = p.id;
                const bool live = (i == 0 && history_.live());
                lines.push_back({T(format_when(p.started_at), when_w) + "  " + T(t, title_w) +
                                 L(format_mmss(p.len_sec), len_w) + "  " +
                                 T(live ? "PLAY" : (p.finished ? "done" : "skip"), state_w),
                                 0});
            }
            cursor = history_selected_ + 1; // +1 for the column header above the rows
        }
    } else if (history_tab_ == 1) {
        const int len_w = 7, plays_w = 5;
        const int title_w = std::max(8, inner - len_w - 2 - plays_w);
        lines.push_back({T("TITLE", title_w) + L("LEN", len_w) + "  " + L("PLAYS", plays_w), 1});
        if (history_top_view_.empty()) {
            lines.push_back({"nothing played yet -- plays show up here as soon as music runs", 0});
        } else {
            for (int i = 0; i < static_cast<int>(history_top_view_.size()); ++i) {
                const HistoryTopRow& r = history_top_view_[static_cast<size_t>(i)];
                std::string t = r.title.empty() ? r.id : r.title;
                lines.push_back({T(t, title_w) + L(format_mmss(r.len_sec), len_w) + "  " +
                                 L(std::to_string(r.plays), plays_w), 0});
            }
            cursor = history_selected_ + 1; // +1 for the column header above the rows
        }
    } else {
        // --- Habits: three categories, each under a highlighted header ---
        const HistoryStats s = history_stats(history_.plays());
        const int lbl_w = std::max(20, inner - 20);
        auto num1 = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return std::string(b); };
        auto pct = [](double v) { char b[32]; std::snprintf(b, sizeof b, "%.0f", v * 100.0); return std::string(b); };
        auto cat = [&](const char* name) { lines.push_back({std::string(), 0}); lines.push_back({std::string(name), 1}); };
        auto item = [&](const std::string& label, const std::string& value) {
            lines.push_back({T(label, lbl_w) + value, 0});
        };
        cat("SESSIONS");
        item("Sessions", std::to_string(s.sessions));
        item("Average session length", format_len(s.avg_session_sec));
        item("Tracks per session", num1(s.tracks_per_session));
        cat("TIME PLAYED PER DAY");
        item("Played today", format_len(s.today_sec));
        item("Average day with music", format_len(s.avg_day_sec));
        item("Days with music", std::to_string(s.days));
        item("Total listened", format_len(s.total_sec));
        cat("PLAY BEHAVIOUR");
        item("Tracks played", std::to_string(s.plays));
        item("Finished (played to the end)", std::to_string(s.finished));
        item("Skipped", std::to_string(s.skipped));
        item("Replays", std::to_string(s.replays));
        item("Completion rate", pct(s.completion) + "%");
        cursor = -1; // no cursor: this tab scrolls
    }

    // Window over the content: around the cursor on the two list tabs,
    // from history_scroll_ on Habits (whose scroll offset is clamped here,
    // where the real content height is finally known).
    int start = 0;
    if (cursor >= 0) {
        start = std::clamp(cursor - (body - 1) / 2, 0, std::max(0, static_cast<int>(lines.size()) - body));
    } else {
        const int max_scroll = std::max(0, static_cast<int>(lines.size()) - body);
        history_scroll_ = std::clamp(history_scroll_, 0, max_scroll);
        start = std::min(history_scroll_, max_scroll);
    }

    int shown = 0;
    for (int k = start; k < static_cast<int>(lines.size()) && shown < body; ++k, ++shown) {
        const std::string& text = lines[static_cast<size_t>(k)].first;
        const int style = lines[static_cast<size_t>(k)].second;
        if (style == 1) head(text);
        else if (style == 2 || (k == cursor && !(history_tab_ == 1 && history_pane_ == 1)))
            row(text, cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color));
        else row(text, "");
    }
    while (shown < body) { blank(); ++shown; }

    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

// Top Tracks tab, second pane: one row per selectable count. The cursor is
// only drawn while this pane has focus (history_pane_ == 1).
std::vector<std::string> App::build_history_add_panel(int total_width, int height) const {
    const int inner = std::max(20, total_width - 4);
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    const std::string R = "\x1b[0m";
    std::vector<std::string> out;
    out.push_back(box_top("ADD TOP TRACKS TO QUEUE", total_width, border_ansi));

    const int played = static_cast<int>(history_top_view_.size()); // distinct titles, whatever the sort
    const bool focused = (history_pane_ == 1);
    for (int i = 0; i < std::max(1, height); ++i) {
        std::string text, sgr;
        if (i < 4) {
            const int n = kHistoryAddCounts[i];
            text = " Top " + std::to_string(n) + " tracks";
            if (played == 0) text += "  (nothing played yet)";
            else if (played < n) text += "  (only " + std::to_string(played) + " played so far)";
            if (focused && i == history_add_sel_)
                sgr = cursor_sgr(settings_.list_cursor_color, settings_.list_cursor_bg_color);
        }
        out.push_back(bar + " " + sgr + pad_right(truncate_str(text, inner), inner) + R + " " + bar);
    }
    out.push_back(box_bottom(total_width, "", border_ansi_bottom));
    return out;
}

void App::build_settings_screen(std::ostringstream& frame, int W, int player_h) const {
    // Literal port of the reference SettingsEngine::render() -- same
    // columns, same labels, same schema text, same group-blanking, same
    // divider, same tab-wrap algorithm, same gradient border (applied to
    // this panel's own chrome too, not just the preview swatches), same
    // preview formulas, same bottom hint/status lines, same cursor
    // placement formula. Deliberately not "improved" or restructured.
    if (W < 80) W = 80;
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    // MAX_Y is the row of this panel's own bottom border; the hint line
    // and status line render below it (rows MAX_Y+1, MAX_Y+2), so the
    // total screen rows used here is MAX_Y+2 -- set to exactly match
    // player_h (total rows the Browse-mode view renders), never taller,
    // never shorter, per explicit requirement. Floored modestly so a
    // pathologically small player view still leaves room to render the
    // tab bar/hint/status at all.
    int MAX_Y = std::max(player_h - 2, 10);
    auto B = [&](int y) {
        return gradient_ansi(settings_.border_color, settings_.border_color_bottom,
                              (MAX_Y > 1) ? static_cast<float>(y - 1) / (MAX_Y - 1) : 0.0f, false);
    };
    auto pos = [&](int y, int x, const std::string& s) { frame << "\x1b[" << y << ";" << x << "H" << s; };
    auto repeat = [](const std::string& s, int n) {
        std::string r; r.reserve(s.size() * static_cast<size_t>(std::max(0, n)));
        for (int i = 0; i < n; ++i) r += s;
        return r;
    };
    // Matches pad(): no truncation if s is already >= width, just like
    // the reference -- a longer-than-expected value overflows into the
    // next column rather than getting cut off. Values here are always
    // short in practice (numbers, true/false, single-char hotkeys).
    auto pad = [](const std::string& s, int width, bool left_align = true) {
        int ulen = display_width(s);
        if (ulen >= width) return s;
        std::string spaces(width - ulen, ' ');
        return left_align ? (s + spaces) : (spaces + s);
    };

    // --- in-place value editing (mode_ == Mode::ColorEdit) -----------------
    // Width of the value field on the row currently being edited. The Colors
    // and non-path rows keep their fixed 7/20 columns; a path row is 20
    // columns minimum and grows with what has actually been typed, never
    // past the panel's right edge -- it used to be painted at the full width
    // all the time, so entering one drew a red bar to the far right even
    // when it was still empty.
    auto edit_field_width = [&]() -> int {
        if (settings_tab_ == 0) return 7;
        if (settings_tab_ == 1 && onoff_row_is_path(settings_row_)) {
            const int edge = std::max(10, W - 36);
            const int want = display_width(color_edit_buffer_) + 1;
            return std::min(edge, std::max(20, want));
        }
        return 20;
    };
    // The painted value itself: selection reversed, window scrolled so the
    // caret stays inside the field, then padded to the full field width.
    // pad() cannot do this -- it measures with display_width(), which does
    // not see through the ANSI codes paint_edit_field() emits, so the column
    // count comes back with them included.
    auto edit_paint = [&](int width) {
        size_t c = std::min(edit_caret_, color_edit_buffer_.size());
        size_t a = std::min(edit_anchor_, color_edit_buffer_.size());
        EditPaint p = paint_edit_field(color_edit_buffer_, c, a, width, "\x1b[41;37m", false);
        p.s += std::string(std::max(0, width - p.cols), ' ');
        return p;
    };

    static const char* kTabNames[] = {"COLORS", "ON/OFF", "ANIMATION", "REFERENCE", "ABOUT APP"};

    // 1. Tab-wrap algorithm.
    std::vector<int> top_tabs, bot_tabs;
    int w_track = 22;
    for (int i = 0; i < 5; ++i) {
        int t_len = static_cast<int>(std::string(kTabNames[i]).size()) + 10;
        if (w_track + t_len < W - 2) { top_tabs.push_back(i); w_track += t_len; }
        else bot_tabs.push_back(i);
    }
    int w = 0;
    std::string l1, l2;
    auto add = [&](const std::string& t, const std::string& b, int width) { l1 += t; l2 += b; w += width; };
    add("\u250c\u2500 SETTINGS \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510", "\u2502                    \u2514", 22);
    for (size_t idx = 0; idx < top_tabs.size(); ++idx) {
        int i = top_tabs[idx];
        std::string lab = (i == settings_tab_) ? ("[" + std::string(kTabNames[i]) + "]") : kTabNames[i];
        int lab_len = static_cast<int>(lab.size());
        bool is_last = (idx == top_tabs.size() - 1);
        add("  " + lab + "  \u250c", repeat("\u2500", 4 + lab_len) + "\u2518", 5 + lab_len);
        if (is_last) add("\u2500", " ", 1);
        else add("\u2500\u2500\u2510", "  \u2514", 3);
    }
    if (w < W - 1) add(repeat("\u2500", W - 1 - w), repeat(" ", W - 1 - w), W - 1 - w);
    add("\u2510", "\u2502", 1);
    pos(1, 1, B(1) + l1 + R);
    pos(2, 1, B(2) + l2 + R);

    // 2. Content.
    int y = 3;
    if (settings_tab_ == 0) {
        static const char* grp[15]  = {"BORDER_COLOR", "DISK", "METADATA", "VIZ", "PROGRESS_BAR",
                                        "LIST", "", "", "QUEUE", "", "", "LYRICS", "", "", "HEADER"};
        static const char* l1n[15]  = {"TOP", "TOP", "KEY", "LEFT", "PLAYED",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "ACTIVE L  FG", "ACTIVE W  FG",
                                        "TEXT"};
        static const char* l2n[15]  = {"BOTTOM", "BOTTOM", "VAL", "RIGHT", "PENDING",
                                        "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG",
                                        ""}; // HEADER is a foreground-only field: no background cell at all
        for (int i = 0; i < 15; ++i) {
            if (i == 5) { pos(y, 1, B(y) + "\u251c" + repeat("\u2500", W - 2) + "\u2524" + R); y++; }
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);

            pos(y, 3, pad(grp[i], 15)); pos(y, 18, ":");

            pos(y, 20, pad(l1n[i], 14, false)); pos(y, 35, ":");
            {
                bool sel = (i == settings_row_ && settings_col_ == 0 && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && settings_col_ == 0 && mode_ == Mode::ColorEdit);
                std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(i, 0), 7);
                pos(y, 38, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            }

            // Second (background) column: the HEADER row has no background
            // cell at all, so it renders neither a label nor a ":" here --
            // without this guard there'd be a dangling colon at col 56 with
            // nothing between it and the preview.
            if (l2n[i][0]) {
                pos(y, 48, pad(l2n[i], 7, false)); pos(y, 56, ":");
                bool sel = (i == settings_row_ && settings_col_ == 1 && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && settings_col_ == 1 && mode_ == Mode::ColorEdit);
                std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(i, 1), 7);
                pos(y, 59, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            }

            std::string valA = settings_get_value(i, 0), valB = settings_get_value(i, 1);
            std::string rt;
            if (i == 0) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 1) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 2) rt = ansi_for(valA, false) + "NAME : " + R + ansi_for(valB, false) + "SONG.MP3" + R;
            else if (i == 3) rt = gradient_preview_bar(valA, valB, 23);
            else if (i == 4) rt = ansi_for(valA, false) + "[###########" + R + ansi_for(valB, false) + "----------]" + R;
            else if (i >= 5 && i <= 12) rt = bg_ansi_for(valB) + ansi_for(valA, false) + "THIS IS AN EXAMPLE TEXT" + R;
            else if (i == 13) {
                std::string aL_F = ansi_for(settings_.active_line_color, false), aL_B = bg_ansi_for(settings_.active_line_bg_color);
                std::string aW_F = ansi_for(valA, false), aW_B = bg_ansi_for(valB);
                std::string iN_F = ansi_for(settings_.inactive_line_color, false), iN_B = bg_ansi_for(settings_.inactive_line_bg_color);
                rt = aL_B + aL_F + "THIS IS " + R + aW_B + aW_F + "AN " + R + iN_B + iN_F + "EXAMPLE TEXT" + R;
            } else if (i == 14) {
                // Same bold + color construction header_sgr() draws the real
                // section titles with, so the swatch shows the exact result.
                std::string params = sgr_params_for(valA);
                rt = (params.empty() ? std::string("\x1b[1m") : "\x1b[1;" + params + "m") + "SECTION HEADER" + R;
            }
            if (!rt.empty()) pos(y, 72, rt);
            y++;
        }
    } else if (settings_tab_ == 1) {
        // ON/OFF: the toggles, then a LOCAL PATH list and a PLAYLIST PATH
        // list, each ending in its own "+ new path" row -- an editable
        // list rather than one fixed path, since neither count is pinned
        // to a single entry. Two display rows per section header (a blank
        // spacer plus the title, same rhythm as the Reference tab) and a
        // viewport centered on settings_row_ keep the whole thing inside
        // the panel no matter how many paths have been added, which is
        // what makes this scrollable instead of growing the frame. Order
        // comes from build_onoff_rows(), the row-to-line mapping from
        // onoff_display_row() -- the renderer and the ColorEdit cursor
        // placement below both use them, so they cannot disagree.
        std::vector<OnOffRow> rows = build_onoff_rows();
        int display_count = 0;
        for (const OnOffRow& r : rows) display_count += (r.kind == OnOffRow::Kind::Header) ? 2 : 1;
        int visible = std::max(1, MAX_Y - 3);
        int cur_display = std::max(0, onoff_display_row(settings_row_));
        int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));

        int disp = 0;
        auto in_view = [&]() { return disp >= scroll && y < MAX_Y; };
        auto side_borders = [&]() {
            pos(y, 1, B(y) + "\u2502" + R);
            pos(y, W, B(y) + "\u2502" + R);
        };
        for (const OnOffRow& r : rows) {
            if (r.kind == OnOffRow::Kind::Header) {
                if (in_view()) { side_borders(); y++; } // blank spacer above the title
                disp++;
                if (in_view()) { side_borders(); pos(y, 6, header_sgr(settings_) + r.label + R); y++; }
                disp++;
                continue;
            }
            if (in_view()) {
                side_borders();
                const bool sel = (r.sel == settings_row_ && mode_ != Mode::ColorEdit);
                const bool ed = (r.sel == settings_row_ && mode_ == Mode::ColorEdit);
                if (r.kind == OnOffRow::Kind::Path) {
                    std::string lab = r.download_folder ? "Download Folder"
                                      : std::string(r.playlist_path ? "Playlist Path " : "Local Path ")
                                        + std::to_string(r.path_index + 1);
                    pos(y, 6, pad(lab, 25)); pos(y, 32, ":");
                    // The value column runs from col 35 right up to the border --
                    // a folder path is far longer than a toggle's 20 columns, and
                    // nothing may spill over the panel edge. While editing, the
                    // tail is what's on screen (typing always happens at the end),
                    // with the cursor below clamped to the same width.
                    int val_w = std::max(10, W - 36);
                    std::string v;
                    if (ed) {
                        // While editing the field is only as wide as what has
                        // been typed (20 minimum, growing with the text, never
                        // past the panel edge -- edit_field_width()), so an
                        // empty "+ new path" line no longer draws a red bar all
                        // the way to the right border. The window inside it
                        // follows the caret, not the tail, so a selection
                        // anywhere in a long path stays visible.
                        v = edit_paint(edit_field_width()).s;
                    } else {
                        // The cursor highlight covers the entry, not the whole
                        // column: a path is highlighted exactly as wide as what
                        // is typed into it, the way a hotkey field highlights
                        // its own key. An empty row -- a not-yet-typed new path
                        // -- keeps the full-width bar instead, so there is a
                        // visible place to start typing.
                        std::string raw = settings_get_value(r.sel, 0);
                        int w = raw.empty()
                                ? val_w
                                : std::clamp(static_cast<int>(display_width(truncate_str(raw, val_w))), 1, val_w);
                        v = pad(truncate_str(raw, w), w);
                    }
                    pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
                } else if (r.kind == OnOffRow::Kind::AddPath) {
                    pos(y, 6, pad(r.label, 25)); pos(y, 32, ":");
                    pos(y, 35, (sel ? HI : "") + std::string(20, ' ') + R);
                    if (sel) pos(y, 57, "\x1b[90m[ENTER] add a line\x1b[0m");
                } else { // Toggle
                    pos(y, 6, pad(r.label, 25)); pos(y, 32, ":");
                    std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(r.sel, 0), 20);
                    pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
                    if (sel && !settings_options_for(settings_tab_, r.sel).empty())
                        pos(y, 57, "\x1b[90m< \u2194 >\x1b[0m");
                }
                y++;
            }
            disp++;
        }
    } else if (settings_tab_ == 2) {
        static const char* anim_l[8] = {"Vis. Fluidity", "Waveform Style", "Disk Speed", "Playback Mode",
                                         "Vis. Degradation", "Vis. Viscosity", "Lyrics Alignment", "Lyrics Animation"};
        for (int i = 0; i < 8; ++i) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, pad(anim_l[i], 25)); pos(y, 32, ":");

            bool sel = (i == settings_row_ && mode_ != Mode::ColorEdit);
            bool ed = (i == settings_row_ && mode_ == Mode::ColorEdit);
            std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(i, 0), 20);
            pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);

            if (sel && !settings_options_for(settings_tab_, i).empty()) pos(y, 57, "\x1b[90m< \u2194 >\x1b[0m");
            y++;
        }
    } else if (settings_tab_ == 3) {
        // Reference tab: the editable LOUDNESS NORMALIZATION rows
        // (kNormLabels) first, then a one-line read-only note pointing at
        // the cheat sheet, then the rebindable hotkeys grouped under
        // category headers (kRefRows), then a read-only display of the
        // font-mapping table (section 4 of the config, "A={A,a}" style)
        // loaded from config.txt -- as "A = A, a" rows. Combined they're
        // usually taller than the player view, so this scrolls as one list
        // (viewport follows settings_row_, centered) rather than ever
        // growing the panel past player_h. See ref_display_row() for how a
        // selectable row maps to the row it's drawn on.
        std::vector<char> letters;
        for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
        int total_selectable = kRefEnd + static_cast<int>(letters.size());
        int display_count = ref_display_row(total_selectable - 1) + 1;
        int visible = std::max(1, MAX_Y - 3);
        int cur_display = ref_display_row(settings_row_);
        int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));

        // Walk every display line (headers + rows) in order, only
        // actually drawing (and advancing y) once we're inside the
        // visible scroll window -- same "disp/scroll/visible" shape the
        // rest of this file's scrolling panels use.
        int disp = 0;
        auto in_view = [&]() { return disp >= scroll && y < MAX_Y; };
        auto draw_header = [&](const char* text) {
            // empty spacer row (just the side borders)
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                y++;
            }
            disp++;

            // the header itself, in the configurable Header color (bold;
            // palette index 10 by default, exactly what it always was)
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, header_sgr(settings_) + text + R);
                y++;
            }
            disp++;
        };
        auto draw_hotkey_row = [&](int selectable_row) {
            const RefHotkeyRow& row = kRefRows[selectable_row - kRefStart];
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, pad(row.label, 25)); pos(y, 32, ":");
                bool sel = (selectable_row == settings_row_ && mode_ != Mode::ColorEdit);
                bool ed = (selectable_row == settings_row_ && mode_ == Mode::ColorEdit);
                std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(selectable_row, 0), 20);
                pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
                y++;
            }
            disp++;
        };
        auto draw_note = [&](const char* text) {
            // blank spacer line, then the read-only note in dim grey.
            // Two display lines, matching the `headers += 2` that
            // ref_display_row() adds at kRefStart.
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                y++;
            }
            disp++;
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, std::string("\x1b[90m") + text + R);
                y++;
            }
            disp++;
        };
        auto draw_norm_row = [&](int i) {
            // LOUDNESS NORMALIZATION: same shape as a hotkey row, but the
            // value is editable -- a bool for the first row, a formatted
            // number for the other two -- with the "< <-> >" hint wherever
            // Left/Right has a list to walk (settings_options_for()).
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, pad(kNormLabels[i - kNormStart], kRefLabelW)); pos(y, 32, ":");
                bool sel = (i == settings_row_ && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && mode_ == Mode::ColorEdit);
                std::string v = ed ? edit_paint(edit_field_width()).s : pad(settings_get_value(i, 0), 20);
                pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
                if (sel && !settings_options_for(settings_tab_, i).empty())
                    pos(y, 57, "\x1b[90m< \u2194 >\x1b[0m");
                y++;
            }
            disp++;
        };
        auto draw_font_row = [&](char c, int selectable_row) {
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                const auto& pair = settings_.font_map.at(c);
                bool sel = (selectable_row == settings_row_);
                std::string line = std::string(1, c) + " = " + pair.first + ", " + pair.second;
                pos(y, 6, (sel ? HI : "") + line + R);
                y++;
            }
            disp++;
        };

        if (y < MAX_Y) draw_header("LOUDNESS NORMALIZATION");
        for (int i = kNormStart; i < kNormEnd && y < MAX_Y; ++i) draw_norm_row(i);
        if (y < MAX_Y) draw_note(kRefNote);
        for (int i = 0; i < kRefRowCount && y < MAX_Y; ++i) {
            if (kRefRows[i].header) draw_header(kRefRows[i].header);
            if (y < MAX_Y) draw_hotkey_row(kRefStart + i);
        }
        if (y < MAX_Y) draw_header("FONT / CHARACTER MAP");
        for (size_t li = 0; li < letters.size() && y < MAX_Y; ++li)
            draw_font_row(letters[li], kRefEnd + static_cast<int>(li));
    } else if (settings_tab_ == 4) {
        // About App: shows settings_.about_app_lines (loaded verbatim
        // from config.txt's trailing ClassTextAboutApp={...}; block, not
        // a hardcoded string), scrolled so it never exceeds player_h.
        int visible = std::max(1, MAX_Y - 3);
        int total = static_cast<int>(settings_.about_app_lines.size());
        int scroll = std::clamp(settings_row_, 0, std::max(0, total - visible));
        for (int r = 0; r < visible; ++r) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            int idx = scroll + r;
            if (idx < total) pos(y, 6, settings_.about_app_lines[idx]);
            y++;
        }
    }

    while (y < MAX_Y) { pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R); y++; }

    // 3. Bottom frame & overflow tabs.
    if (bot_tabs.empty()) {
        pos(y, 1, B(y) + "\u2514" + repeat("\u2500", W - 2) + "\u2518" + R);
    } else {
        std::string bot = "\u2514\u2500";
        for (int i : bot_tabs) bot += (i == settings_tab_ ? " [" + std::string(kTabNames[i]) + "] \u2500" : "  " + std::string(kTabNames[i]) + "  \u2500");
        int rem_bot = W - static_cast<int>(bot.size()); if (rem_bot < 1) rem_bot = 1;
        pos(y, 1, B(y) + bot + repeat("\u2500", rem_bot - 1) + "\u2518" + R);
    }
    y++;

    pos(y, 1, "\x1b[90m[TAB] Switch | [\u2191\u2193\u2190\u2192] Navigate/Cycle | [ENTER] Edit | [S] Save | [Q] Quit\x1b[0m");
    y++;
    // The log/status line lives here now -- render_frame() deliberately no
    // longer prints it under the Browse list (an untruncated message there
    // could exceed the terminal width, wrap, and push a full-height frame
    // into a scroll, which is what made the terminal's own scrollbar appear
    // and shift the whole UI left by a column). This row sits on the panel's
    // own last screen line, so it is truncated to the panel width: a long
    // path or error message just ends early instead of wrapping.
    if (!status_line_.empty()) pos(y, 1, "\x1b[32m" + truncate_str(status_line_, W - 2) + "\x1b[0m");

    // 4. In-place text editing cursor placement.
    if (mode_ == Mode::ColorEdit) {
        // The caret column comes from the same window the renderer painted
        // (edit_field_width() + paint_edit_field()), so it cannot disagree
        // with what is on screen: byte length could -- a multi-byte
        // character is one column but many bytes, and a path longer than the
        // field is scrolled to the caret rather than showing its tail.
        const size_t ec = std::min(edit_caret_, color_edit_buffer_.size());
        const size_t ea = std::min(edit_anchor_, color_edit_buffer_.size());
        const int caret_col = paint_edit_field(color_edit_buffer_, ec, ea, edit_field_width(), "", false).caret;
        if (settings_tab_ == 0) {
            int cy = 3 + settings_row_ + (settings_row_ >= 5 ? 1 : 0);
            int cx = ((settings_col_ == 0) ? 38 : 59) + caret_col;
            frame << "\x1b[" << cy << ";" << cx << "H\x1b[?25h";
        } else {
            int cy = 3 + settings_row_;
            int cx = 35 + caret_col;
            if (settings_tab_ == 1) {
                // The ON/OFF tab scrolls and its section headers aren't
                // 1:1 with settings_row_, so 3 + row would put the cursor
                // on the wrong line (or off the panel entirely) once the
                // path lists push it down. Recompute the exact scroll the
                // renderer used -- same build_onoff_rows()/onoff_display_row()
                // pair -- and place the cursor on the line that row was
                // actually painted on. cx already accounts for a path row's
                // caret-anchored window above.
                int visible = std::max(1, MAX_Y - 3);
                int display_count = 0;
                for (const OnOffRow& r : build_onoff_rows())
                    display_count += (r.kind == OnOffRow::Kind::Header) ? 2 : 1;
                int cur_display = std::max(0, onoff_display_row(settings_row_));
                int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));
                cy = 3 + (cur_display - scroll);
            } else if (settings_tab_ == 3) {
                // Reference tab scrolls once its row list exceeds the
                // visible window -- the common case, since it holds every
                // rebindable hotkey plus the loudness-normalization rows
                // and any font-map rows.
                // Recompute the same scroll offset used when rendering (see
                // the settings_tab_==3 branch above, and ref_display_row())
                // so the text cursor lands on the row actually drawn there
                // instead of one that's already scrolled off-screen. Reached
                // only for the loudness rows and the rebindable hotkeys --
                // the Enter handler blocks ColorEdit for the read-only
                // font-map rows.
                int visible = std::max(1, MAX_Y - 3);
                int cur_display = ref_display_row(settings_row_);
                std::vector<char> letters;
                for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
                int total_selectable = kRefEnd + static_cast<int>(letters.size());
                int display_count = ref_display_row(total_selectable - 1) + 1;
                int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));
                cy = 3 + (cur_display - scroll);
            }
            frame << "\x1b[" << cy << ";" << cx << "H\x1b[?25h";
        }
    }
}
// ---------------------------------------------------------------------
// Console / log overlay (HKeyConsole)
// ---------------------------------------------------------------------

void App::build_console_screen(std::ostringstream& frame, int W, int target_height) const {
    std::string border = ansi_for(settings_.border_color, false);
    frame << box_top("CONSOLE / LOGS", W, border) << "\n";

    std::vector<std::string> log_lines = ConsoleLog::instance().lines();
    // Must always equal the player view's own height (target_height,
    // computed by player_view_height() -- see its comment in app.h),
    // never just "whatever the terminal happens to fit". A terminal much
    // taller than the actual Browse-mode view would otherwise leave this
    // overlay awkwardly mismatched from the view it's standing in for.
    int visible = std::max(1, target_height - 2); // minus this overlay's own top/bottom border rows
    int total = static_cast<int>(log_lines.size());
    int start = std::max(0, total - visible); // always shows the tail, newest at the bottom

    for (int r = 0; r < visible; ++r) {
        int idx = start + r;
        std::string line = (idx < total) ? log_lines[idx] : "";
        frame << box_line(line, W, border) << "\n";
    }
    frame << box_bottom(W, "[t / ESC] close", border) << "\n";
}

// ---------------------------------------------------------------------
// Cheatsheet overlay (HKeyCheatsheet)
// ---------------------------------------------------------------------

void App::build_cheatsheet_screen(std::ostringstream& frame, int W) const {
    std::string border = ansi_for(settings_.border_color, false);
    frame << box_top("CHEATSHEET", W, border) << "\n";

    // Similar shape to the Reference tab of Settings (kRefRows) for the
    // rebindable hotkeys, but this screen is the ONE place that also lists
    // every literal, non-rebindable key command -- the Reference tab no
    // longer carries its own copy of those (see its own comment for why),
    // so this table's rows are this app's entire command list, full stop.
    // `header` is set only on a category's first row and is drawn as a
    // section title in the Header colour above a blank spacer row. `action`
    // is either
    //   * a plain action name, looked up in settings_.hotkeys, so the key
    //     shown is whatever the user actually has bound (config.txt /
    //     rebound in Settings), never a hardcoded assumption; or
    //   * '#'-prefixed: a literal key label. SHIFT+B, Ctrl+Shift+S/X, ESC,
    //     the playlist/meta editors' own fixed navigation and text-editing
    //     keys, and friends are checked as raw key codes in handle_*_key()
    //     rather than looked up in settings_.hotkeys, so there is no hotkey
    //     entry for them to reference.
    struct CheatRow { const char* header; const char* action; const char* desc; };
    static const CheatRow rows[] = {
        // --- System -- listed first. Besides the rebindable system hotkeys
        // this also carries the literal keys used across multiple overlays
        // (not tied to one editor's own legend below). ---
        {"SYSTEM (MAIN UI)", "HKeySetting", "Open Settings panel, hit again to save and quit"},
        {nullptr, "HKeyConsole", "Console / logs"},
        {nullptr, "HKeyCheatsheet", "This cheatsheet"},
        {nullptr, "HKeyQuit", "Quit"},
        {nullptr, "#ESC", "Close setting / overlay / menu"},
        {nullptr, "#ENTER", "Confirm / select"},
        {nullptr, "#ARROW KEYS", "Navigate (context-dependent)"},
        {nullptr, "#Y / N", "Confirm or cancel a prompt"},
        // --- Playback ---
        {"PLAYBACK (MAIN UI)", "HKeyPlay", "Play the selected track"},
        {nullptr, "HKeyTogglePlayPause", "Play / pause"},
        {nullptr, "HKeyPlayNextSong", "Play next in list/queue"},
        {nullptr, "HKeyPlayPreviousSong", "Play previous in list"},
        {nullptr, "HKeyShuffleNext", "Shuffle to a random next track"},
        {nullptr, "HKeyCyclePlayMode", "Cycle play mode (list/repeat/shuffle/repeat queue/stop)"},
        {nullptr, "HKeySeekForward", "Seek forward 5s"},
        {nullptr, "HKeySeekBackward", "Seek backward 5s"},
        {nullptr, "HKeyIncreaseVolume", "Volume up"},
        {nullptr, "HKeyDecreaseVolume", "Volume down"},
        {nullptr, "HKeyToggleMute", "Mute (without pausing)"},
        {nullptr, "HKeyToggleNormalize", "Toggle loudness normalization"},
        // --- Navigation & View ---
        {"NAVIGATION & VIEW (MAIN UI)", "HKeyNavigateUp", "Explore list (up)"},
        {nullptr, "HKeyNavigateDown", "Explore list (down)"},
        {nullptr, "HKeySwitchBetweenCards", "Switch between panels"},
        {nullptr, "HKeyFilterForFolder", "Filter by folder"},
        {nullptr, "HKeyClearFilter", "Clear filter"},
        {nullptr, "HKeyCycleSortMode", "Cycle local list sort mode"},
        {nullptr, "HKeyRefreshUi", "Refresh UI (redraw)"},
        {nullptr, "HKeyToggleWaveform", "Toggle waveform style (raw/smooth)"},
        {nullptr, "HKeyToggleLyrics", "Toggle lyrics on/off"},
        {nullptr, "HKeyToggleMetaOnly", "Toggle metadata-only track list (no filename)"},
        {nullptr, "HKeyRetryLyrics", "Retry lyrics"},
        {nullptr, "HKeyListOverlay", "Big list overlay: larger LOCAL AUDIO FILES pane (toggle)"},
        {nullptr, "#SHIFT+UP/DOWN", "Big list overlay: previous / next page (faster scrolling)"},
        {nullptr, "#ESC", "Big list overlay: close (play, sort, filter, search, queue keep working)"},
        // --- Search ---
        {"SEARCH (MAIN UI)", "HKeySearch", "Search local folder"},
        {nullptr, "HKeySearchOnline", "Search online (YouTube)"},
        {nullptr, "HKeySearchPlaylist", "Search saved playlists (type /p:query)"},
        {nullptr, "HKeySearchFolder", "Search folders (type /f:query), ENTER lists all files of that folder"},
        // --- Queue ---
        {"QUEUE (MAIN UI)", "HKeyAddHoveringSongToQueue", "Add hovering track to queue"},
        {nullptr, "HKeyRemoveHoveringSongFromQueue", "Remove hovering track from queue"},
        {nullptr, "HKeyQueueMoveUp", "Move hovering queue item up"},
        {nullptr, "HKeyQueueMoveDown", "Move hovering queue item down"},
        {nullptr, "HKeyClearQueue", "Clear the whole queue (asks Yes / No first)"},
        // --- Playlists -- HKeyPlaylist opens the overlay; every other row
        // is the playlist editor's own fixed legend (build_playlist_screen()'s
        // footer), none of which is a rebindable hotkey.
        {"PLAYLISTS", "HKeyPlaylist", "Open Playlists (create / manage)"},
        {nullptr, "#ALT+LEFT/RIGHT", "Switch tab (Create/Edit vs Saved Playlists)"},
        {nullptr, "#TAB", "Cycle focus (name field / library picker / track list)"},
        {nullptr, "#UP/DOWN", "Navigate the focused list/picker (fixed arrow keys)"},
        {nullptr, "#ENTER", "Add hovering track to playlist / load selected playlist"},
        {nullptr, "#4 / 5", "Move the hovering track up/down"},
        {nullptr, "#D / DEL / BACKSPACE", "Remove hovering track / delete selected playlist"},
        {nullptr, "#HOME", "Save the playlist"},
        {nullptr, "#SHIFT+LEFT/RIGHT", "Mark text (name / search fields)"},
        {nullptr, "#CTRL+C/X/V", "Copy / cut / paste text"},
        // --- Meta editor -- same deal: HKeyMetaEditor opens it, everything
        // else is build_meta_screen()'s own fixed legend (both tabs).
        {"META EDITOR", "HKeyMetaEditor", "Meta editor: edit file name / artist / title / album / year"},
        {nullptr, "#LEFT/RIGHT", "Switch tab (Edit vs Fetch List)"},
        {nullptr, "#TAB", "Meta editor: cycle panels (search / library / fields)"},
        {nullptr, "#UP/DOWN", "Navigate the focused list/picker (fixed arrow keys)"},
        {nullptr, "#ENTER", "Edit the hovering field (Fetch List tab: fetch the whole list)"},
        {nullptr, "#SHIFT+LEFT/RIGHT", "Mark text (field editor)"},
        {nullptr, "#CTRL+C/X/V", "Copy / cut / paste text (field editor)"},
        {nullptr, "#a", "Meta editor: add the hovering file to the fetch list"},
        {nullptr, "#r", "Meta editor: toggle edited files on top of the library pane"},
        {nullptr, "#x / SHIFT+T / SHIFT+A / SHIFT+Y", "Filter library: missing any / title / artist / year"},
        {nullptr, "#SHIFT+B", "Fetch metadata for the hovered title (AcoustID)"},
        {nullptr, "#DEL / d", "Remove hovering track from the fetch list"},
        {nullptr, "#CTRL+SHIFT+S", "Apply the meta editor's pending edits to the files"},
        {nullptr, "#CTRL+SHIFT+X", "Discard the meta editor's pending edits"},
        // --- Listening history ---
        {"HISTORY", "HKeyHistory", "Listening history: last plays, top tracks, habits"},
        {nullptr, "#1 / 2 / 3", "History overlay: switch tab"},
        {nullptr, "#ARROWS", "History overlay: move the cursor / scroll Habits"},
        {nullptr, "#r", "History overlay: most-played first <-> least-played first"},
        {nullptr, "#TAB / ENTER", "Top Tracks tab: switch pane / add top 10-25-50-100 to queue"},
        // --- Downloads ---
        {"DOWNLOADS", "HKeyDownloadStream", "Save stream to the download folder (Settings > Download Folder, else .cache/mousiki)"},
    };

    int height = std::max(term_rows_ - 4, 8); // real terminal height, minus this overlay's own top/bottom border rows
    int visible = std::max(1, height - 2);
    // Counted in DISPLAY lines rather than rows: a category costs three
    // (blank spacer + title + the row the title sits on) exactly like
    // ref_display_row() counts them on the Reference tab, and
    // cheatsheet_scroll_ is kept in the same unit -- so a header scrolls
    // like any other line.
    int total = 0;
    for (const auto& r : rows) total += r.header ? 3 : 1;
    // Scrollable, same as Settings' Reference tab: this table is longer than
    // what a 32-row terminal can show at once (it already was, before the
    // meta editor's rows were added to it), and silently dropping the tail
    // would make entries unreachable rather than just off-screen.
    int max_scroll = std::max(0, total - visible);
    cheatsheet_scroll_ = std::clamp(cheatsheet_scroll_, 0, max_scroll);

    // A coloured line can't go through box_line(): that pads by counting
    // bytes, so it would treat the escape codes as columns and truncate the
    // text away. Pad the plain text first, then wrap the padded result.
    const std::string R = "\x1b[0m";
    const std::string bar = border + settings_.box_vertical + R;
    const int inner = std::max(0, W - 4);
    int disp = 0;  // running index over the whole table's display lines
    int shown = 0; // how many of them are actually printed (a window of `visible`)
    auto emit = [&](const std::string& plain, const std::string& sgr) {
        bool in_window = disp >= cheatsheet_scroll_ && shown < visible;
        ++disp;
        if (!in_window) return;
        ++shown;
        frame << bar << " ";
        if (!sgr.empty()) frame << sgr;
        frame << pad_right(plain, inner);
        if (!sgr.empty()) frame << R;
        frame << " " << bar << "\n";
    };
    for (const auto& r : rows) {
        if (r.header) {
            emit(std::string(), "");                       // blank spacer row...
            emit(r.header, header_sgr(settings_));         // ...then the title, Header colour + bold
            // ...and then the row itself, which is where the header lives
            // (same as kRefRows: a category's title sits on its FIRST entry,
            // not on a line of its own) -- falling through, not continue.
        }
        std::string key;
        if (r.action[0] == '#') {
            key = r.action + 1;                            // literal key label, no hotkey to look up
        } else {
            auto it = settings_.hotkeys.find(r.action);
            key = (it != settings_.hotkeys.end() && !it->second.empty()) ? it->second : "-";
        }
        // 20 columns, wide enough for the widest literal key ("D / DEL /
        // BACKSPACE" is 19): pad_right() truncates instead of leaving a gap,
        // which used to glue "ARROW_KEY_RIGH" / "D / DEL / BACKSP" straight
        // onto their descriptions. A user binding even wider than that keeps
        // its full text with two spaces behind it rather than being cut.
        std::string key_col = (display_width(key) > 19) ? key + "  " : pad_right(key, 20);
        emit(key_col + r.desc, "");
    }
    while (shown < visible) { frame << box_line("", W, border) << "\n"; ++shown; }

    std::string bottom = "[? / ESC] close";
    if (max_scroll > 0) bottom += "  [\u2191\u2193] scroll " + std::to_string(cheatsheet_scroll_ + 1) + "/"
                                + std::to_string(total);
    frame << box_bottom(W, bottom, border) << "\n";
}

// ---------------------------------------------------------------------
// Bulk add overlay (paste-a-playlist-link panel, "a" while Queue focused)
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Bulk add overlay (paste-a-playlist-link panel, "a" while Queue focused)
// ---------------------------------------------------------------------
// Deliberately NOT sized like Console/Settings/Cheatsheet -- those match
// the player view or the terminal on purpose (real overlays meant to
// take over the screen). This one is a small floating panel that only
// takes as many rows as it actually has content for: an input row while
// typing, then a short starred checklist once results come in. It never
// grows to fill the terminal.

// ---------------------------------------------------------------------
// Big list overlay (HKeyListOverlay, SHIFT+L) -- see app.h for the design.
// ---------------------------------------------------------------------

namespace {
inline int list_total_for(ListSource src, size_t local, size_t online, size_t playlists, size_t folders) {
    return static_cast<int>(src == ListSource::Local ? local : src == ListSource::Online ? online
                          : src == ListSource::Folder ? folders : playlists);
}
} // namespace

// The overlay is only shown (and only steers scrolling) in the modes that
// have the main UI underneath it. In the full-screen takeovers (Settings,
// Playlist, ...) the flag simply stays set and the overlay is back when
// they close.
bool App::list_overlay_active() const {
    if (!list_overlay_open_) return false;
    switch (mode_) {
        case Mode::Browse: case Mode::Search: case Mode::BulkAdd:
        case Mode::RetryLyrics: case Mode::ClearQueue: return true;
        default: return false;
    }
}

void App::list_overlay_geometry(int W, int& panel_w, int& list_rows) const {
    panel_w = std::min(W, std::max(40, std::min(W - 6, 160)));
    // Rows 2 .. term_rows_-1 belong to the panel: search bar (3) + list box
    // border (2) + the list rows themselves.
    list_rows = std::max(1, term_rows_ - 2 - kListOverlayChromeRows);
}

// Keeps selected_ inside a window of `rows` rows and the window inside the
// list (no empty rows after the last entry when the list is long enough).
void App::list_overlay_fit_scroll(int rows) {
    rows = std::max(1, rows);
    const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
    if (total <= 0) { selected_ = 0; scroll_ = 0; return; }
    selected_ = std::clamp(selected_, 0, total - 1);
    scroll_ = std::clamp(scroll_, 0, std::max(0, total - rows));
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + rows) scroll_ = selected_ - rows + 1;
}

void App::list_overlay_open() {
    list_overlay_open_ = true;
    queue_focus_ = false; // the queue pane is hidden while the overlay is up
    list_overlay_fit_scroll(overlay_list_rows_);
}

void App::list_overlay_close() {
    list_overlay_open_ = false;
    // Both panes share selected_/scroll_: pull the window back so the cursor
    // is inside the small pane's (much shorter) view again.
    list_overlay_fit_scroll(list_visible_rows_);
}

// One page up/down. The cursor keeps its row within the window, so the eye
// stays in the same place while the list flips underneath it. At either end
// (window can't move any further) the cursor jumps to the first/last entry.
void App::list_overlay_page(int dir) {
    const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
    if (total <= 0) return;
    const int rows = std::max(1, overlay_list_rows_);
    const int max_scroll = std::max(0, total - rows);
    const int rel = std::clamp(selected_ - scroll_, 0, rows - 1);
    const int new_scroll = std::clamp(scroll_ + dir * rows, 0, max_scroll);
    if (new_scroll == scroll_) {
        selected_ = (dir > 0) ? total - 1 : 0;
    } else {
        scroll_ = new_scroll;
        selected_ = std::clamp(new_scroll + rel, 0, total - 1);
    }
    list_overlay_fit_scroll(rows);
}

// The search bar and the list pane at overlay size. The list itself is the
// normal build_list_panel() (same rows, colours, sort/folder title), just
// given the wider/taller box -- only its bottom border is replaced, to carry
// the page counter, the queue size (the queue pane is hidden, so this is the
// only feedback for "add to queue") and the key hints.
std::vector<std::string> App::build_list_overlay_panel(int panel_w, int list_rows) const {
    std::vector<std::string> out = build_search_bar(panel_w);
    std::vector<std::string> list = build_list_panel(panel_w, list_rows);
    if (!list.empty()) {
        const int total = list_total_for(list_source_, local_view_.size(), online_view_.size(), playlist_view_.size(), folder_view_.size());
        const int rows = std::max(1, list_rows);
        const int pages = std::max(1, (total + rows - 1) / rows);
        const int page = std::clamp((scroll_ + rows / 2) / rows + 1, 1, pages);
        const int remaining = total - (scroll_ + rows);

        std::vector<std::string> parts; // dropped from the back until it fits
        if (remaining > 0) parts.push_back("( " + std::to_string(remaining) + " more )");
        parts.push_back("page " + std::to_string(page) + "/" + std::to_string(pages));
        parts.push_back("queue: " + std::to_string(queue_.size()));
        parts.push_back("[SHIFT+UP/DOWN] page  [ESC] close");
        std::string footer;
        for (;;) {
            footer.clear();
            for (size_t i = 0; i < parts.size(); ++i) footer += (i ? "  " : "") + parts[i];
            if (parts.empty() || display_width(footer) + 5 <= panel_w) break;
            parts.pop_back();
        }
        if (parts.empty()) footer.clear();
        list.back() = box_bottom(panel_w, footer, ansi_for(settings_.border_color_bottom, false));
    }
    out.insert(out.end(), list.begin(), list.end());
    return out;
}

// ---------------------------------------------------------------------
// Floating panels (Bulk Add, Retry Lyrics) -- see app.h's comment on
// draw_floating_panel() for why these don't clear the screen.
// ---------------------------------------------------------------------

void App::draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines, int panel_w, int W) const {
    int panel_h = static_cast<int>(lines.size());
    int start_col = 1 + std::max(0, (W - panel_w) / 2);
    int start_row = 1 + std::max(0, (term_rows_ - panel_h) / 2 - kFloatingPanelUpShift);
    start_row = std::clamp(start_row, 1, std::max(1, term_rows_ - panel_h));
    for (int i = 0; i < panel_h; ++i) {
        frame << "\x1b[" << (start_row + i) << ";" << start_col << "H" << lines[i];
    }
}

std::vector<std::string> App::build_bulk_add_panel() const {
    const int W = kBulkAddPanelWidth;   // 62, matches the reference design
    const int inner_w = W - 2;          // 60 -- nested box width, flush against the outer border (no gap)
    std::string border = ansi_for(settings_.border_color, false);
    std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    auto wrap = [&](const std::string& inner_line) { return obar + inner_line + obar; };

    std::vector<std::string> lines;
    lines.push_back(box_top("BULK ADD", W, border));

    // --- input box (nested) ---
    lines.push_back(wrap(box_top("", inner_w, border)));
    std::string cursor_line = bulk_add_buffer_.empty()
        ? "//:paste yt playlist link here"
        : bulk_add_buffer_ + "\u2588"; // block cursor once typing starts, matches the Search bar's style
    lines.push_back(wrap(box_line(cursor_line, inner_w, border)));
    lines.push_back(wrap(box_bottom(inner_w, "", border)));

    // --- results box (nested) -- always present at a fixed row count so
    // the panel's total footprint never changes between phase 1 (typing)
    // and phase 2 (results in) -- unused rows are just blank, not
    // omitted, which is what keeps draw_floating_panel()'s fixed-
    // rectangle overwrite artifact-free without ever needing a clear. ---
    lines.push_back(wrap(box_top("", inner_w, border)));

    const auto& items = pending_bulk_add_.items;
    int total = static_cast<int>(items.size());
    int show = bulk_add_results_ready_ ? std::min(total, kBulkAddVisibleRows) : 0;
    int scroll = bulk_add_results_ready_ ? std::clamp(bulk_add_scroll_, 0, std::max(0, total - show)) : 0;

    // Column widths measured directly from the reference design at its
    // 56-column content width (inner_w - 4): title=27, channel=15, the
    // rest is mark + " | " separators + the unpadded time text.
    const int title_w = 27, chan_w = 15;

    for (int r = 0; r < kBulkAddVisibleRows; ++r) {
        if (r >= show) { lines.push_back(wrap(box_line("", inner_w, border))); continue; }
        int idx = scroll + r;
        const auto& it = items[idx];
        bool starred = idx < static_cast<int>(bulk_add_selected_.size()) && bulk_add_selected_[idx];
        bool hovering = idx == bulk_add_cursor_;

        std::string mark = starred ? "*" : " ";
        std::string title = truncate_str(it.title, title_w);
        std::string chan = truncate_str(it.uploader.empty() ? "-" : it.uploader, chan_w);
        std::string time_str = "-";
        if (it.duration_sec >= 0) {
            int secs = static_cast<int>(it.duration_sec);
            time_str = std::to_string(secs / 60) + ":" + (secs % 60 < 10 ? "0" : "") + std::to_string(secs % 60);
        }

        std::string line = mark + " | " + pad_right(title, title_w) + " | " + pad_right(chan, chan_w) + " | " + time_str;
        // Built plain first, then box_line() pads/truncates it (its
        // truncate_str/pad_right count raw bytes, not display columns --
        // they don't skip ANSI escapes), and only *after* that do we
        // splice the reverse-video hover highlight into the already-
        // finished bar+content+bar string. Wrapping `line` in "\x1b[7m"
        // before handing it to box_line() would get its own length
        // miscounted against the ANSI bytes and risk truncate_str
        // slicing straight through the trailing "\x1b[0m" reset,
        // leaking reverse-video onto every line after it -- exactly the
        // bug build_list_panel avoids by coloring after padding, not
        // before (see its own hover/cursor rendering).
        std::string boxed = box_line(line, inner_w, border);
        if (hovering) {
            std::string vbar_len = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
            size_t start = vbar_len.size() + 1; // past "bar + ' '"
            size_t end = boxed.size() - vbar_len.size() - 1; // before "' ' + bar"
            boxed = boxed.substr(0, start) + "\x1b[7m" + boxed.substr(start, end - start) + "\x1b[0m" + boxed.substr(end);
        }
        lines.push_back(wrap(boxed));
    }

    // Bottom border of the results box carries two plain-text labels --
    // "[ N more ]" on the left (only once there's more than fits), "ALL"
    // and "SELECT" on the right as the two commit actions. Just text, no
    // button/tab border art.
    int more = bulk_add_results_ready_ ? (total - show) : 0;
    std::string left_label = more > 0 ? ("[ " + std::to_string(more) + " more ]") : "";
    std::string right_label = bulk_add_results_ready_ ? "ALL   SELECT" : "";
    std::string prefix = settings_.box_lower_left + settings_.box_horizontal;
    if (!left_label.empty()) prefix += " " + left_label + " ";
    std::string suffix = right_label.empty() ? "" : (" " + right_label + " ");
    suffix += settings_.box_lower_right;
    int used = display_width(prefix) + display_width(suffix);
    int dashes = std::max(0, inner_w - used);
    std::string bottom = prefix;
    for (int i = 0; i < dashes; ++i) bottom += settings_.box_horizontal;
    bottom += suffix;
    bottom = pad_right(bottom, inner_w);
    lines.push_back(wrap(border.empty() ? bottom : (border + bottom + "\x1b[0m")));

    // Outer box's own bottom border carries "[ESC] cancel" directly.
    lines.push_back(box_bottom(W, "[ESC] cancel", border));
    return lines;
}

// ---------------------------------------------------------------------
// Retry Lyrics (HKeyRetryLyrics, 'l') -- manual title/artist override
// ---------------------------------------------------------------------

std::vector<App::RLField> App::rl_visible_fields() const {
    std::vector<RLField> f = {
        RLField::Title, RLField::Artist, RLField::Ft,
        RLField::TypeReverb, RLField::TypeSlowed, RLField::TypeUltraSlowed,
        RLField::TypeSpedup, RLField::TypeRemix, RLField::TypeOther,
    };
    if (rl_remix_) f.push_back(RLField::RemixText);
    if (rl_other_) f.push_back(RLField::OtherText);
    return f;
}

bool* App::rl_bool_ptr(RLField f) {
    switch (f) {
        case RLField::TypeReverb: return &rl_reverb_;
        case RLField::TypeSlowed: return &rl_slowed_;
        case RLField::TypeUltraSlowed: return &rl_ultra_slowed_;
        case RLField::TypeSpedup: return &rl_spedup_;
        case RLField::TypeRemix: return &rl_remix_;
        case RLField::TypeOther: return &rl_other_;
        default: return nullptr;
    }
}

std::string* App::rl_text_ptr(RLField f) {
    switch (f) {
        case RLField::Title: return &rl_title_;
        case RLField::Artist: return &rl_artist_;
        case RLField::Ft: return &rl_ft_;
        case RLField::RemixText: return &rl_remix_text_;
        case RLField::OtherText: return &rl_other_text_;
        default: return nullptr;
    }
}

// Pre-fills from the current track's metadata_ rather than opening
// blank, and resets every other bit of state -- so reopening after a
// previous override (or a previous cancel) never leaks stale values
// from last time.
void App::rl_open_from_current_track() {
    rl_title_ = metadata_.name;
    rl_artist_ = (metadata_.artist == "-") ? "" : metadata_.artist;
    rl_ft_.clear();

    // Best-effort ft./feat. split: if the title itself carries a
    // "feat."/"ft." tag, pull it out into its own field rather than
    // leaving it embedded (so it doesn't get double-appended once the
    // TYPE tags get tacked on after the title at submit time).
    static const std::vector<std::string> markers = {" feat. ", " feat ", " ft. ", " ft "};
    std::string lower_title = ascii_lower_str(metadata_.name);
    for (const auto& marker : markers) {
        size_t pos = lower_title.find(marker);
        if (pos != std::string::npos) {
            rl_title_ = metadata_.name.substr(0, pos);
            rl_ft_ = metadata_.name.substr(pos + marker.size());
            // trim a trailing ')' if the split landed inside "(feat. X)"
            if (!rl_ft_.empty() && rl_ft_.back() == ')') rl_ft_.pop_back();
            while (!rl_title_.empty() && (rl_title_.back() == ' ' || rl_title_.back() == '(')) rl_title_.pop_back();
            break;
        }
    }

    rl_remix_text_.clear();
    rl_other_text_.clear();
    rl_reverb_ = rl_slowed_ = rl_ultra_slowed_ = rl_spedup_ = rl_remix_ = rl_other_ = false;
    rl_focus_ = RLField::Title;
}

// Builds "title [ft. X] [tags...]" and launches the override fetch --
// this is the actual point of the whole form: feeding a corrected
// query into fetch_synced_lyrics() instead of the track's real
// metadata, for tracks whose auto-fetched lyrics are wrong/missing.
void App::rl_submit() {
    std::string query = rl_title_;
    if (!rl_ft_.empty()) query += " ft. " + rl_ft_;

    // Tags go after the title (per your note: "you usually need to place
    // them after title in url"). Slowed/Ultra Slowed/Spedup are mutually
    // exclusive so at most one of these three contributes; Reverb/Remix/
    // Other are independent and can stack with it and each other.
    if (rl_slowed_) query += " slowed";
    else if (rl_ultra_slowed_) query += " ultra slowed";
    else if (rl_spedup_) query += " sped up";
    if (rl_reverb_) query += " reverb";
    if (rl_remix_) query += rl_remix_text_.empty() ? " remix" : (" " + rl_remix_text_ + " remix");
    if (rl_other_ && !rl_other_text_.empty()) query += " " + rl_other_text_;

    launch_lyrics_fetch(query, rl_artist_, current_path_, /*force_network=*/true);
    log_event("retrying lyrics: \"" + query + "\"");

    mode_ = Mode::Browse;
}

// "Want to clear queue?" -- small Yes/No confirmation, stamped over the live
// Browse view by draw_floating_panel() like Bulk Add / Retry Lyrics. Fixed
// size (8 rows x kClearQueuePanelWidth) so it overwrites cleanly without a clear.
std::vector<std::string> App::build_clear_queue_panel() const {
    const int W = kClearQueuePanelWidth;
    const int inner = W - 4;
    std::string border = ansi_for(settings_.border_color, false);
    std::string border_bottom = ansi_for(settings_.border_color_bottom, false);
    const std::string R = "\x1b[0m", HI = "\x1b[7m";
    const std::string bar = border + settings_.box_vertical + R;

    // Centred plain-text row. Only ever fed plain text (display_width() counts
    // escape bytes as columns), so the highlighted buttons are built below.
    auto centred = [&](const std::string& plain) {
        const int fill = std::max(0, inner - display_width(plain));
        const int left = fill / 2;
        return bar + " " + std::string(static_cast<size_t>(left), ' ') + plain +
               std::string(static_cast<size_t>(fill - left), ' ') + " " + bar;
    };

    const size_t n = queue_.size();
    const std::string yes = "  Yes  ", no = "  No  ", gap = "    ";
    const int btn_w = display_width(yes) + display_width(gap) + display_width(no);
    const int fill = std::max(0, inner - btn_w);
    const int left = fill / 2;
    const std::string buttons =
        bar + " " + std::string(static_cast<size_t>(left), ' ') +
        (clear_queue_choice_ == 0 ? HI + yes + R : yes) + gap +
        (clear_queue_choice_ == 1 ? HI + no + R : no) +
        std::string(static_cast<size_t>(fill - left), ' ') + " " + bar;

    std::vector<std::string> lines;
    lines.push_back(box_top("Clear Queue", W, border));
    lines.push_back(centred(""));
    lines.push_back(centred("Want to clear queue?"));
    lines.push_back(centred(std::to_string(n) + " track" + (n == 1 ? "" : "s") + " will be removed"));
    lines.push_back(centred(""));
    lines.push_back(buttons);
    lines.push_back(centred(""));
    lines.push_back(box_bottom(W, "[y / n]  [ENTER] confirm", border_bottom));
    return lines;
}

std::vector<std::string> App::build_retry_lyrics_panel() const {
    const int W = kRetryLyricsPanelWidth; // 62, matches the reference design
    const int label_w = 14;               // left label column, blank on box top/bottom rows
    const int box_w = W - 2 - label_w;    // 46 -- nested input box width
    std::string border = ansi_for(settings_.border_color, false);
    std::string obar = border.empty() ? settings_.box_vertical : (border + settings_.box_vertical + "\x1b[0m");
    auto wrap = [&](const std::string& left, const std::string& right) { return obar + left + right + obar; };
    auto label = [&](const std::string& text) { return pad_left(text, label_w - 2) + " :"; };
    auto blank_label = [&] { return std::string(label_w, ' '); };

    // A text field: label appears only on the middle (content) row, the
    // nested box's own top/bottom rows get a blank label column.
    // `placeholder` shows only when the field is both empty AND not
    // currently focused -- e.g. REMIX/OTHER's "NOT SELECTED". While
    // actively editing an empty field, show just the cursor rather than
    // the placeholder text glued to it (which would otherwise look like
    // "NOT SELECTED" was real, already-typed content).
    auto text_field = [&](RLField f, const std::string& lbl, const std::string& value,
                           const std::string& placeholder, std::vector<std::string>& out) {
        out.push_back(wrap(blank_label(), box_top("", box_w, border)));
        bool focused = (rl_focus_ == f);
        std::string shown = (value.empty() && !focused) ? placeholder : value;
        if (focused) shown += "\u2588"; // block cursor, same convention as Search/Bulk Add
        out.push_back(wrap(label(lbl), box_line(shown, box_w, border)));
        out.push_back(wrap(blank_label(), box_bottom(box_w, "", border)));
    };

    std::vector<std::string> lines;
    lines.push_back(box_top("Retry Lyrics", W, border));

    text_field(RLField::Title, "SONG TITLE", rl_title_, "", lines);
    text_field(RLField::Artist, "ARTIST NAME", rl_artist_, "", lines);
    text_field(RLField::Ft, "FT  ( opt )", rl_ft_, "", lines);

    // TYPE: two rows of plain (non-boxed) checkbox-style toggles.
    // Slowed/Ultra Slowed/Spedup are a radio group (only one can show
    // "[o]" at a time); Reverb/Remix/Other are independent.
    //
    // Built and measured as plain ASCII first, then the focus highlight
    // is spliced in by byte offset *after* pad_right/truncate_str has
    // already run -- same reasoning as the bulk-add hover fix above:
    // those two don't skip ANSI escapes when counting width, so
    // wrapping an option in "\x1b[7m" before truncating/padding the row
    // risks slicing through the reset code and leaking reverse-video
    // onto the rest of the panel. Every word here is plain ASCII, so a
    // byte offset is also a column offset -- no UTF-8 width subtleties
    // to worry about.
    auto opt_plain = [&](bool checked, const std::string& word) { return (checked ? "[o] " : " o  ") + word; };
    auto type_row = [&](std::initializer_list<std::tuple<RLField, bool, std::string>> opts) {
        std::string plain = " ";
        int focus_start = -1, focus_len = 0;
        for (const auto& [f, checked, word] : opts) {
            std::string s = opt_plain(checked, word);
            if (rl_focus_ == f) { focus_start = static_cast<int>(plain.size()); focus_len = static_cast<int>(s.size()); }
            plain += s;
            plain += " ";
        }
        std::string padded = pad_right(truncate_str(plain, box_w), box_w);
        if (focus_start >= 0 && focus_start + focus_len <= static_cast<int>(padded.size())) {
            padded = padded.substr(0, focus_start) + "\x1b[7m" + padded.substr(focus_start, focus_len) +
                     "\x1b[0m" + padded.substr(focus_start + focus_len);
        }
        return padded;
    };
    lines.push_back(wrap(label("TYPE"),
        type_row({{RLField::TypeReverb, rl_reverb_, "reverb"},
                  {RLField::TypeSlowed, rl_slowed_, "slowed"},
                  {RLField::TypeUltraSlowed, rl_ultra_slowed_, "ultra slowed"}})));
    lines.push_back(wrap(blank_label(),
        type_row({{RLField::TypeSpedup, rl_spedup_, "spedup"},
                  {RLField::TypeRemix, rl_remix_, "remix"},
                  {RLField::TypeOther, rl_other_, "other /pls specify"}})));

    // REMIX / OTHER: dynamically present -- only when their TYPE toggle
    // is on. When hidden, still reserve their 3 rows as blank (not
    // omitted) so the panel's total height never changes frame to frame
    // -- draw_floating_panel() stamps a fixed rectangle every frame with
    // no clear, so a shrinking panel would otherwise leave stale
    // characters behind at the edges it used to cover.
    if (rl_remix_) {
        text_field(RLField::RemixText, "REMIX", rl_remix_text_, "NOT SELECTED", lines);
    } else {
        for (int i = 0; i < 3; ++i) lines.push_back(wrap(blank_label(), std::string(box_w, ' ')));
    }
    if (rl_other_) {
        text_field(RLField::OtherText, "OTHER", rl_other_text_, "NOT SELECTED", lines);
    } else {
        for (int i = 0; i < 3; ++i) lines.push_back(wrap(blank_label(), std::string(box_w, ' ')));
    }

    std::string fetch_word = "enter to fetch";
    std::string hint = pad_left(fetch_word, W - 2 - 2); // 2 trailing spaces before the border, matching the reference
    lines.push_back(wrap("", pad_right(hint, W - 2)));

    lines.push_back(box_bottom(W, "[ESC] cancel", border));
    return lines;
}

// The Console and Settings overlays must always be exactly as tall as
// the Browse-mode player view -- see the comment on this declaration in
// app.h. This recomputes the same panel line counts render_frame()'s
// Browse-mode branch does, plus list_visible_rows_ (already kept
// up to date each frame -- see render_frame()) for the list/queue
// panel's share.
int App::player_view_height(int w) const {
    int h = static_cast<int>(build_metadata_panel(w).size());
    h += static_cast<int>(build_progress_panel(w).size());
    h += static_cast<int>(build_search_bar(w).size());
    h += list_visible_rows_;
    h += 2; // the list/queue box's own top+bottom border rows (build_list_panel()/
            // build_queue_panel() add them ON TOP of the content rows they're given)
    h += 1; // the row Browse draws the AcoustID prompt / loading indicator in
            // (idle Browse no longer prints anything there -- the log moved to
            // Settings -- but overlays still size to this taller frame: it is
            // the worst case, and overlay_budget() caps the result at
            // term_rows_ - 1 anyway)
    return h;
}

// ---------------------------------------------------------------------
// Frame assembly
// ---------------------------------------------------------------------

std::string App::render_frame(TerminalIO& term) {
    int term_cols = term.cols();
    // Was clamped to a minimum of 80 regardless of the real terminal
    // width -- on a narrower phone terminal (the screenshots suggest
    // something closer to 40-46 visible columns), every line rendered
    // here would already be wider than the physical screen and get
    // wrapped by the terminal itself before the next frame's cursor-home
    // redraw overwrites it. That reads exactly like "truncated mid-word"
    // or "garbled" text even though nothing in this file actually cut it
    // off -- the terminal did, one line later than expected. Lowering the
    // floor so the app actually renders to the real width instead of
    // always assuming at least 80 columns are available.
    int W = std::clamp(term_cols, 40, 200);

    // Real terminal row count -- see term_rows_'s comment in app.h for
    // the full story on why this now actually gets consulted. rows()
    // itself already falls back to a sane default (40) if the ioctl
    // fails, so no extra guarding needed here beyond a floor against
    // truly pathological values feeding into subtraction below.
    term_rows_ = std::max(term.rows(), 4);

    // Big list overlay (SHIFT+L): size it against the real terminal every
    // frame -- before anything below scrolls/paints the list -- so
    // list_nav_rows() (which every scroll-follows-the-cursor calculation
    // asks) already reflects the overlay's row count, and re-anchor the
    // window so a resize can never leave the cursor off-screen.
    int overlay_panel_w = 0;
    if (list_overlay_active()) {
        list_overlay_geometry(W, overlay_panel_w, overlay_list_rows_);
        list_overlay_fit_scroll(overlay_list_rows_);
    }

    // Browse/BulkAdd/RetryLyrics all share the same live background (the
    // latter two float a small panel on top of it -- see
    // draw_floating_panel()'s comment in app.h), so switching between
    // them never needs a full clear, only a redraw. Settings/Console/
    // Cheatsheet are still genuine full-screen takeovers, so entering or
    // leaving any of *those* still forces one, same as a real mode
    // change always has.
    auto mode_family = [](Mode m) {
        switch (m) {
            case Mode::Browse: case Mode::Search: case Mode::BulkAdd: case Mode::RetryLyrics: case Mode::ClearQueue: return 0;
            case Mode::Settings: case Mode::ColorEdit: return 1;
            case Mode::Console: return 2;
            case Mode::Cheatsheet: return 3;
            case Mode::Playlist: return 4;
            case Mode::MetaEdit: return 5;
            case Mode::History: return 6;
        }
        return 0;
    };
    bool hard_clear = (W != last_render_w_) || (mode_family(mode_) != mode_family(last_render_mode_)) || force_redraw_;
    if (last_render_w_ != -1 && W != last_render_w_) {
        // Verbose-only: raw ioctl terminal size alongside the clamped
        // app-usable width, i.e. "what the OS actually told us" versus
        // what we did with it.
        ConsoleLog::instance().log_verbose(
            "terminal resized: " + std::to_string(last_render_w_) + " -> " + std::to_string(W) +
            " cols (raw ioctl cols=" + std::to_string(term_cols) + ", rows=" + std::to_string(term_rows_) + ")");
    }
    force_redraw_ = false; // one-shot -- consumed by this frame
    last_render_w_ = W;
    last_render_mode_ = mode_;
    const char* clear_prefix = hard_clear ? "\x1b[2J\x1b[H" : "\x1b[H";

    // Every overlay below lays itself out against player_view_height() --
    // the height of the Browse view it stands in for -- but that value can
    // exceed what actually fits: list_visible_rows_ is only recomputed
    // while Browse itself is being rendered, so anything that grows the
    // metadata panel while another mode is up (playback starting, for
    // instance) leaves it stale and pvH points past the bottom of the
    // screen. clamp_output_rows() keeps term_rows_ - 1 lines, so the
    // budget asked for here is min(pvH, term_rows_ - 1): an overlay laid
    // out taller than the screen would have its own footer -- hint line
    // plus status line -- chopped off on every single frame, which is the
    // same bug that used to hide Browse's status line entirely.
    auto overlay_budget = [&](int w) {
        return std::min(player_view_height(w), std::max(1, term_rows_ - 1));
    };

    if (mode_ == Mode::Settings || mode_ == Mode::ColorEdit) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_settings_screen(frame, W, overlay_budget(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    if (mode_ == Mode::Console) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_console_screen(frame, W, overlay_budget(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    if (mode_ == Mode::Cheatsheet) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_cheatsheet_screen(frame, W);
        return clamp_output_rows(frame.str(), term_rows_);
    }

    if (mode_ == Mode::Playlist) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_playlist_screen(frame, W, overlay_budget(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    if (mode_ == Mode::MetaEdit) {
        // The five field values are resolved right before drawing them:
        // reading a tag can promote into the shared cache, and the panel
        // builder itself is const (see meta_refresh_hover_values()).
        meta_refresh_hover_values();
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_meta_screen(frame, W, overlay_budget(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    if (mode_ == Mode::History) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_history_screen(frame, W, overlay_budget(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    // --- Browse mode (and the background behind BulkAdd/RetryLyrics):
    // figure out how many list/queue rows actually fit before building
    // anything, so the panel is sized right the first time instead of
    // being built tall and then chopped.
    auto metadata_lines = build_metadata_panel(W);
    auto progress_lines = build_progress_panel(W);
    auto search_lines = build_search_bar(W);
    // The AcoustID confirmation raised by SHIFT+B lives in Browse's
    // status area too (there is no footer here), but the disclaimer alone is
    // 110 characters -- wider than many terminals -- so instead of trusting
    // one physical line to hold it (which would wrap in the terminal and
    // push the whole frame into a scroll), it is wrapped here and as many
    // rows as it actually needs are reserved for it, exactly like the single
    // loading indicator row -- and, since neither of them is there most of
    // the time, no row at all is reserved when both are absent (see below).
    std::vector<std::string> prompt_lines;
    if (meta_prompt_ != MetaPrompt::None && mode_ == Mode::Browse) {
        prompt_lines = wrap_lines(std::string(kMetaFetchDisclaimer) + "   [Y]es   [N]o   [ESC] cancel",
                                  std::max(10, W - 2), 2);
    }
    // The ONLY things still drawn below the list box are the AcoustID
    // prompt (wrapped, so as many rows as it actually needs) or -- mutually
    // exclusive with it -- the live "resolving/downloading..." indicator.
    // The log/status line that used to own this slot moved to the Settings
    // screen, so when neither is present nothing is printed down there and
    // therefore no row is reserved for it either: a row reserved only to
    // sit blank adds an extra empty line under a frame that already ends at
    // term_rows_ - 1, and it withholds a row the list/queue panes could
    // show a track in. Since status_rows always equals the number of rows
    // that will really be printed below the box (0, or the prompt's wrapped
    // line count, or 1), the frame comes out term_rows_ - 1 tall in every
    // case -- with or without a prompt/loading row, no jitter, no scroll.
    const bool show_load_line = load_in_progress_.load() && load_stage_.load() == 1;
    int status_rows = std::max<int>(static_cast<int>(prompt_lines.size()), show_load_line ? 1 : 0);
    int fixed_h = static_cast<int>(metadata_lines.size() + progress_lines.size() + search_lines.size())
                + status_rows; // rows really drawn under the list box (0 when none are)
    // Two things used to be missing from this budget, and together they made
    // the frame exactly 2 lines too tall on EVERY terminal:
    //   * build_list_panel()/build_queue_panel() draw a top and a bottom
    //     border row around the `list_h` content rows they're handed, and
    //   * there used to be a blank separator line before the status line.
    // clamp_output_rows() keeps term_rows_ - 1 lines, so those last 2 lines
    // -- blank + status/loading -- were chopped off every single frame: no
    // Browse message ever reached the screen at all ("added to queue",
    // "queued 3 tracks", the SHIFT+B AcoustID question, ...). The blank
    // line is gone (the last row sits directly under the list box now),
    // and the box's 2 border rows are subtracted from the room the list may
    // use, so the frame ends up exactly term_rows_ - 1 lines tall with the
    // prompt/loading row as its last one -- or, when there is no prompt and
    // nothing is loading, with the list box's bottom border as its last one
    // and the terminal's final row left blank. Ordinary status messages are
    // no longer printed here at all -- see where that row is built below.
    // -1 extra margin: leave the terminal's very last row untouched so a
    // trailing '\n' after the final printed line can never itself force
    // a scroll (see clamp_output_rows()'s comment for the same reasoning
    // applied as a hard backstop).
    int available_for_list = term_rows_ - fixed_h - 1 - 2;
    list_visible_rows_ = std::clamp(available_for_list, 0, kListVisibleRows);

    ensure_visible_row_meta();

    std::ostringstream frame;
    frame << clear_prefix;

    for (auto& l : metadata_lines) frame << l << "\n";
    for (auto& l : progress_lines) frame << l << "\n";
    for (auto& l : search_lines) frame << l << "\n";

    int list_h = list_visible_rows_;
    if (settings_.element_queue) {
        int list_w = W / 2;
        int queue_w = W - list_w; // exact 50/50, remainder (odd W) goes to queue
        auto list_lines = build_list_panel(list_w, list_h);
        auto queue_lines = build_queue_panel(queue_w, list_h);
        size_t rows = std::max(list_lines.size(), queue_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < list_lines.size()) ? list_lines[i] : std::string(list_w, ' ');
            std::string r = (i < queue_lines.size()) ? queue_lines[i] : std::string(queue_w, ' ');
            frame << l << r << "\n";
        }
    } else {
        for (auto& l : build_list_panel(W, list_h)) frame << l << "\n";
    }

    // No blank separator above this last row any more: with the list box's
    // border rows now counted in available_for_list, dropping that row is
    // what actually leaves room for it (see the comment there -- it used to
    // be cut off by clamp_output_rows() every frame).
    //
    // The row is NOT the log/status line any more. status_line_ (written by
    // log_event() and by every action's own message -- "added to queue",
    // "SAVED", "searching online ...") was the one line in this frame that
    // was printed raw: never width-limited, so a message longer than W
    // wrapped in the terminal and added a physical row to an already
    // full-height frame. One row too many is all it takes for the frame to
    // scroll: the terminal's own scrollbar appears, that scrollbar costs
    // the last text column, and then *every* full-width box row of the next
    // frame wraps too -- the UI visibly shifts left and keeps scrolling.
    // The log now lives on the Settings screen instead (see the status line
    // at the bottom of build_settings_screen()), where a stray wrap is
    // cosmetic and never touches this frame. So nothing is written into this
    // slot when no prompt is up and nothing is loading -- and status_rows
    // (above) reserves nothing in that case either, which is what keeps the
    // frame at exactly term_rows_ - 1 lines instead of leaving one blank
    // line short of the bottom while the panes above lose a row to it. When
    // something IS drawn here, its rows are reserved up front, so the prompt
    // and the loading indicator keep a fixed position and the layout never
    // jitters.
    if (!prompt_lines.empty()) {
        // Same yellow-on-black as the meta menu's own confirmation footer.
        // wrap_lines() already cut these to W - 2, and the " "+...+" "
        // padding adds exactly the 2 columns that takes back, so the result
        // is exactly W wide -- this line can never wrap either.
        for (const auto& l : prompt_lines) frame << "\x1b[43;30m " << l << " \x1b[0m\n";
    } else if (show_load_line) {
        // Only the online resolve/download step shows a live status —
        // local loads are probe-only now (near-instant) and deliberately
        // silent, no "loading..." flash. Short, ASCII-only and seconds-
        // limited, so it stays far inside any terminal width.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_started_at_).count();
        frame << "  resolving/downloading... (" << static_cast<int>(secs) << "s)\n";
    }

    frame << "\x1b[0J";

    // Hard safety net on top of the list_visible_rows_ sizing above: even
    // if the fixed chrome alone (metadata+progress+search bar) is taller
    // than the terminal -- a case list_visible_rows_ can't do anything
    // about, since it only controls the list panel -- this guarantees
    // the actual byte stream handed to the terminal never contains more
    // rows than the terminal has, so it structurally cannot scroll no
    // matter what future panels/config combinations produce.
    std::string out = clamp_output_rows(frame.str(), term_rows_);

    // Bulk Add / Retry Lyrics / Clear Queue: stamp their floating panel on top
    // of the still-live background just built above, rather than replacing
    // it. Uses absolute positioning (draw_floating_panel()), so it is simply
    // appended after the background's own sequential top-to-bottom writes --
    // whichever content lands on a given screen cell last in the stream
    // wins, and the panel is emitted after, so it draws over the background
    // wherever they overlap without needing a clear.
    //
    // It MUST be appended after the clamp above, not before: on a terminal
    // where the background already fills term_rows_ - 1 lines (any terminal
    // too short for the full-height list, e.g. 30 rows), clamp_output_rows()
    // cuts everything after the last kept newline -- which used to include
    // the panel, so it never appeared while the mode still swallowed keys.
    // The panel adds no '\n' (absolute moves only) and draw_floating_panel()
    // already keeps it inside the screen, so it cannot cause a scroll.
    std::ostringstream floating;
    if (list_overlay_active()) {
        // Stamped first so the small forms (Retry Lyrics, Bulk Add, Clear
        // Queue) still float on top of it when they are opened from inside
        // the overlay. Absolute moves only -- no '\n', so it cannot scroll --
        // and rows past the last usable terminal row are dropped.
        auto lines = build_list_overlay_panel(overlay_panel_w, overlay_list_rows_);
        const int start_col = 1 + std::max(0, (W - overlay_panel_w) / 2);
        const int start_row = 2; // one background row stays visible above and below
        for (size_t i = 0; i < lines.size(); ++i) {
            const int row = start_row + static_cast<int>(i);
            if (row > term_rows_ - 1) break;
            floating << "\x1b[" << row << ";" << start_col << "H" << lines[i];
        }
    }
    if (mode_ == Mode::BulkAdd) {
        draw_floating_panel(floating, build_bulk_add_panel(), kBulkAddPanelWidth, W);
    } else if (mode_ == Mode::RetryLyrics) {
        draw_floating_panel(floating, build_retry_lyrics_panel(), kRetryLyricsPanelWidth, W);
    } else if (mode_ == Mode::ClearQueue) {
        draw_floating_panel(floating, build_clear_queue_panel(), kClearQueuePanelWidth, W);
    }
    out += floating.str();
    return out;
}

// Keeps at most (term_rows - 1) lines of `frame` (the -1 leaves the
// terminal's last row untouched, so the final line's trailing '\n' can
// never itself trigger a scroll) and drops everything after that,
// escape-code prefixes and all -- this is the hard backstop described
// in term_rows_'s comment in app.h: whatever the panel-sizing logic
// above computed, the actual printed output can never exceed what the
// real terminal can show without scrolling. Content past the cutoff is
// simply not drawn this frame rather than causing any corruption.
std::string App::clamp_output_rows(const std::string& frame, int term_rows) const {
    int max_lines = std::max(1, term_rows - 1);
    int newlines_seen = 0;
    for (size_t i = 0; i < frame.size(); ++i) {
        if (frame[i] == '\n') {
            ++newlines_seen;
            if (newlines_seen >= max_lines) return frame.substr(0, i + 1);
        }
    }
    return frame; // already within budget
}

// ---------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------

int App::run() {
    ConsoleLog::instance().init(settings_.console_verbosity == 1 ? LogVerbosity::Verbose : LogVerbosity::Basic);
    // Flush what the constructor's local-library scan found before logging
    // was ready to record it -- see local_scan_diagnostics_'s declaration
    // for why this can't just be logged from inside scan() directly. Basic
    // level, not Verbose: "why does my library look wrong" is exactly the
    // kind of thing someone shouldn't need to raise console_verbosity to see.
    for (const auto& line : local_scan_diagnostics_) {
        ConsoleLog::instance().log_basic(line);
    }
    {
        // Verbose-only startup facts -- "what the OS provided" at the
        // very start of the session, before anything else has run.
#if defined(_WIN32)
        ConsoleLog::instance().log_verbose("os: Windows");
#else
        struct utsname uts{};
        if (uname(&uts) == 0) {
            ConsoleLog::instance().log_verbose(std::string("os: ") + uts.sysname + " " + uts.release + " " + uts.machine);
        }
#endif
        ConsoleLog::instance().log_verbose("home: " + std::string(std::getenv("HOME") ? std::getenv("HOME") : "(unset)"));
    }

    // Session snapshot restore -- only when the feature's on. A missing
    // or corrupt snapshot.json is treated identically to "no snapshot at
    // all" (load_snapshot() already guards that), so this always falls
    // back to the normal cold-start behavior on any failure.
    bool restored = false;
    if (settings_.autosave_enabled) {
        SnapshotData snap;
        if (load_snapshot(snap)) {
            restore_snapshot(snap);
            restored = true;
            // Consumed exactly once -- see snapshot.h's delete_snapshot()
            // comment for why this happens right after a successful
            // restore rather than only at the next autosave/exit.
            delete_snapshot();
            log_event("restored previous session");
        }
    }
    if (!restored && !local_view_.empty()) {
        selected_ = 0;
        start_local_track(local_view_[0]);
    }

    TerminalIO term;
    last_frame_time_ = std::chrono::steady_clock::now();
    last_autosave_at_ = std::chrono::steady_clock::now();
    ConsoleLog::instance().log_verbose("terminal: " + std::to_string(term.rows()) + "x" + std::to_string(term.cols()) + " (rows x cols, raw ioctl)");

    while (!quit_) {
        // Drain every key already queued before rendering, rather than
        // one per frame. A single keystroke can arrive as more than one
        // poll_key() call's worth of data -- any non-ASCII character (a
        // German umlaut, most concretely) is a multi-byte UTF-8 sequence
        // dispensed one byte per call, on both platforms (see
        // TerminalIO::poll_key() / win_poll_key()). Rendering between
        // those calls meant the frame in between showed a buffer ending
        // in a lone, incomplete lead byte -- which decodes as a
        // replacement glyph -- for one frame, before the next poll
        // completed the sequence and it snapped to the real character.
        // Draining first means the frame that actually renders always
        // has a complete, valid buffer. This never blocks waiting for
        // more input: poll_key() is non-blocking and returns 0 the
        // moment nothing already-received is left to hand back.
        // A single-line text field is being edited right now: hand Ctrl+C /
        // Ctrl+X / Ctrl+V to the app as keystrokes instead of letting the
        // console treat Ctrl+C as "kill this process" (Windows, where the
        // ENABLE_PROCESSED_INPUT bit is what does that) or swallow it via
        // ISIG (POSIX). Re-derived every frame rather than toggled on mode
        // entry/exit: mode_ can change in ways that skip a hand-off (ESC out
        // of an edit, a prompt), and leaving the flag stuck would mean
        // Ctrl+C no longer quits the app. Same key set both platforms, see
        // win_poll_key()/poll_key() for how it arrives as kKeyCtrlC/X/V.
        const bool playlist_text_field =
            mode_ == Mode::Playlist &&
            ((playlist_tab_ == 0 && playlist_edit_focus_ <= 1) ||
             (playlist_tab_ == 1 && playlist_manage_focus_ == 0));
        set_text_entry(mode_ == Mode::Search ||
                       mode_ == Mode::ColorEdit ||
                       (mode_ == Mode::MetaEdit && meta_tab_ == 0 &&
                        (meta_focus_ == 0 || meta_focus_ == 2)) ||
                       playlist_text_field);

        for (int key = term.poll_key(); key != 0; key = term.poll_key()) {
            handle_key(key);
        }

        poll_pending_search();
        poll_pending_load();
        poll_pending_waveform();
        poll_pending_bulk_add();
        poll_pending_row_meta_tags();
        poll_pending_meta_fetch(); // AcoustID results -> pending edits
        maybe_autosave();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time_).count();
        last_frame_time_ = now;
        viz_dt_ = dt;

        if (has_track_) {
            player_.poll_elapsed();
            // No auto-advance while a device handoff is still in flight:
            // finished_ belongs to the previous track until play() swaps the
            // new one in (see device_play_pending_gen_).
            if (!advancing_ && device_play_pending_gen_.load() == 0 && player_.finished()) advance_track();
        }
        // Disk only spins while something is actually playing — frozen
        // when idle or paused, per instruction.
        if (has_track_ && !player_.is_paused()) {
            angle_ = std::fmod(angle_ + kAngularVelocity *settings_.disk_rotation_speed * dt,
                               2.0 * 3.14159265358979323846);
        }
        // Listening history: accrue what was actually heard this frame.
        // Paused time never counts (that's the is_paused() check), and neither
        // does the handover gap between two tracks -- there advancing_ is set
        // or a device handoff is in flight while nothing is really sounding.
        // No-op when no play is live, so it can be called unconditionally.
        if (has_track_ && !player_.is_paused() && !advancing_ &&
            device_play_pending_gen_.load() == 0) {
            history_.add_listened(dt);
        }

        std::string frame_str = render_frame(term);
        std::cout << frame_str << std::flush;
        // 25fps (was 12.5fps) — the 700ms waveform reveal animation only
        // got ~9 frames to work with at the old 80ms cadence, which
        // showed as a handful of visible ~11% jumps rather than a smooth
        // continuous expansion. Also smooths disk rotation and the
        // visualizer's motion generally. Text-frame rendering is cheap
        // enough that doubling the rate here is not a meaningful CPU/
        // battery concern.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    // One play may still be open: quitting mid-track is not "heard to the
    // end", so it closes as a skip (history_end_current_play() reads
    // player_.finished(), which stop_device_worker() below would clear) and
    // the file gets its final write for this session.
    history_end_current_play();
    history_.save();

    // Tell the worker to stop taking new requests before touching player_
    // directly here -- if it's mid-play() this waits (briefly) on
    // player_mutex_ rather than tearing the device down out from under it.
    stop_device_worker();
    {
        std::lock_guard<std::mutex> lk(player_mutex_);
        player_.stop();
    }
    term.restore();
    save_settings(settings_);
    // Final snapshot on a clean quit -- same single-canonical-file
    // save_snapshot() the 30s autosave tick uses, just with the exact
    // position at the moment of quitting rather than up to 30s stale.
    if (settings_.autosave_enabled) {
        save_snapshot(build_snapshot());
        ConsoleLog::instance().log_basic("saved session snapshot on exit");
    }
    if (load_thread_.joinable()) load_thread_.join();
    if (search_thread_.joinable()) search_thread_.join();
    if (device_worker_thread_.joinable()) device_worker_thread_.join();
    if (bulk_add_thread_.joinable()) bulk_add_thread_.join();
    // An AcoustID batch still in flight: let it finish (it's just a
    // paced python subprocess), apply whatever it answered into the
    // session, then write the autosave backup one last time. Quitting with
    // unsaved edits is *supposed* to leave them on disk -- the session file
    // IS the backup, and only Ctrl+Shift+S or Ctrl+Shift+X ever clears it.
    if (meta_fetch_thread_.joinable()) meta_fetch_thread_.join();
    poll_pending_meta_fetch();
    meta_persist();
    std::cout << "\nbye.\n";
    return 0;
}

} // namespace muisc
