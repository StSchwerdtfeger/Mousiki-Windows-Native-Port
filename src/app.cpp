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
#include <random>
#include <sstream>
#include <thread>
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

} // namespace

App::App() {
    settings_ = load_settings();
    set_emoji_replacement(settings_.replace_emoji);
    player_.set_stereo(settings_.stereo);
    player_.set_normalization(settings_.normalize,
                              static_cast<float>(settings_.normalize_target_lufs),
                              static_cast<float>(settings_.normalize_max_boost_db));
    lyrics_script_ = find_lyrics_script();
    fast_search_script_ = find_fast_search_script();

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

const char* App::sort_mode_name(int mode) {
    switch (mode) {
        case 1: return "title A-Z";
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
        std::stable_sort(tracks.begin(), tracks.end(), [](const LocalTrack& a, const LocalTrack& b) {
            return lower(a.title) < lower(b.title);
        });
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

void App::refresh_local_view() {
    local_view_ = filter_and_rank_local(last_local_query_);
    // Folder filter (HKeyFilterForFolder) stacks on top of the search/sort
    // result rather than replacing it, so filtering-by-folder while a
    // search is active narrows to just that folder's matches.
    if (!folder_filter_.empty()) {
        std::vector<LocalTrack> filtered;
        filtered.reserve(local_view_.size());
        for (auto& t : local_view_) {
            if (path_utf8(t.path.parent_path()) == folder_filter_) filtered.push_back(t);
        }
        local_view_ = std::move(filtered);
    }
    selected_ = 0;
    scroll_ = 0;
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
        local_view_ = filter_and_rank_local(pre_search_local_query_);
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

    list_source_ = ListSource::Local;
    local_view_ = filter_and_rank_local(buf);
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
    } else {
        last_local_query_ = buf;
        list_source_ = ListSource::Local;
        refresh_local_view();
    }
}

// Local, case-insensitive substring match on playlist name -- cheap
// enough (just a directory scan) to re-run on every keystroke, same as
// the "p:" live preview above does.
std::vector<PlaylistSummary> App::filter_playlists(const std::string& query) const {
    auto all = PlaylistManager::list(playlists_dir());
    if (query.empty()) return all;
    std::vector<PlaylistSummary> out;
    out.reserve(all.size());
    for (auto& p : all) if (contains_ci(p.name, query)) out.push_back(p);
    return out;
}

// settings_.playlists_path if the user set one (config.txt's
// PlaylistsPath=), otherwise settings_.local_music_paths[0]/playlists --
// same "first configured local path" fallback HKeyDownloadStream uses
// (~/Music if none configured at all, which by the time this runs may
// itself have become the cache folder -- see load_library()'s
// cache-dir injection). Computed fresh every call, not cached, so it
// always reflects whatever the user currently has set in Settings.
fs::path App::playlists_dir() const {
    if (!settings_.playlists_path.empty()) return path_from_utf8(settings_.playlists_path);
    std::string base;
    if (!settings_.local_music_paths.empty()) {
        base = settings_.local_music_paths[0];
    } else {
        const char* home = std::getenv("HOME");
        base = home ? (std::string(home) + "/Music") : "./Music";
    }
    return path_from_utf8(base) / "playlists";
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
        if (was_advancing) has_track_ = false;
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
    player_.clear_finished(); // see clear_finished()'s comment — closes the race that caused the double-skip bug
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
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size() : online_view_.size();
    if (list_len == 0 || selected_ < 0 || selected_ >= static_cast<int>(list_len)) return;
    if (list_source_ == ListSource::Local) start_local_track(local_view_[selected_]);
    else start_online_track(online_view_[selected_]);
}

int App::current_track_list_index() const {
    if (!has_track_) return -1;
    if (list_source_ == ListSource::Playlist) return -1; // no "now playing" identity in a list of playlist names
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
    if (list_source_ == ListSource::Playlist) return;
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
    if (selected_ >= scroll_ + list_visible_rows_) scroll_ = selected_ - list_visible_rows_ + 1;
    if (selected_ < scroll_) scroll_ = selected_;
    play_selected();
}

void App::play_relative_random() {
    if (list_source_ == ListSource::Playlist) return;
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
    if (selected_ >= scroll_ + list_visible_rows_) scroll_ = selected_ - list_visible_rows_ + 1;
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
        player_.clear_finished();
        return;
    }
    if (settings_.play_mode == 3 /*stop*/) {
        has_track_ = false; // the only mode where "no track loaded" is the right message
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
    auto pl = PlaylistManager::load(playlists_dir(), summary.name);
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
// The Reference tab lists every rebindable hotkey (kRefRows), grouped into
// categories via the optional `header` field -- set only on a category's
// first row, and rendered as a section title above it -- followed by a
// read-only "HARDCODED / NOT REBINDABLE" section (kRefHardcoded) for key
// commands that are NOT wired through settings_.hotkeys at all (fixed
// literal key codes checked directly in the various handle_*_key()
// functions), and finally the read-only font-mapping table loaded from
// config.txt. All three sections scroll together as one list; see
// ref_display_row() below for how a selectable row (settings_row_) maps
// to the row it's actually drawn on, once the section headers/dividers
// are accounted for.
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
    {nullptr, "HKeyRetryLyrics", "Retry Lyrics"},
    // --- Search ---
    {"SEARCH", "HKeySearch", "Search Local"},
    {nullptr, "HKeySearchOnline", "Search Online"},
    {nullptr, "HKeySearchPlaylist", "Search Playlists"},
    // --- Queue ---
    {"QUEUE", "HKeyAddHoveringSongToQueue", "Add To Queue"},
    {nullptr, "HKeyRemoveHoveringSongFromQueue", "Remove From Queue"},
    {nullptr, "HKeyQueueMoveUp", "Queue Move Up"},
    {nullptr, "HKeyQueueMoveDown", "Queue Move Down"},
    // --- Playlists ---
    {"PLAYLISTS", "HKeyPlaylist", "Open Playlist Editor"}, // opens the playlist create/manage screen
    // --- Downloads ---
    {"DOWNLOADS", "HKeyDownloadStream", "Download Stream"},
    // --- System ---
    {"SYSTEM", "HKeySetting", "Open Settings"},
    {nullptr, "HKeyConsole", "Console / Logs"},
    {nullptr, "HKeyCheatsheet", "Cheatsheet"},
    {nullptr, "HKeyQuit", "Quit Application"},
};
static constexpr int kRefRowCount = sizeof(kRefRows) / sizeof(kRefRows[0]);

// Key commands that are NOT in settings_.hotkeys -- fixed literal key
// codes checked directly in handle_*_key(), so rebinding a similarly-
// named action above (if any) does NOT affect these. Read-only in the UI;
// listed here purely for reference. See e.g. handle_settings_key() (Enter/
// Esc/S), the Playlist-editor track-list handler (4/5, D/DEL/Backspace),
// and the Bulk-Add results handler (A, Space) for where each is checked.
struct RefHardcodedRow { const char* keys; const char* label; };
static const RefHardcodedRow kRefHardcoded[] = {
    {"ESC", "Close Setting"},
    {"S", "Save and Quit Settings"},
    {"ENTER", "Confirm / Select"},
    {"ARROW KEYS", "Navigate"},
    {"Y / N", "Confirm Or Cancel"},
    {"4 / 5", "Move Track Up/Down"},
    {"D / DEL / BACKSPACE", "Removal commands"},
};
static constexpr int kRefHardcodedCount = sizeof(kRefHardcoded) / sizeof(kRefHardcoded[0]);

// Maps a selectable row index -- 0..kRefRowCount-1 for hotkeys,
// kRefRowCount..+kRefHardcodedCount-1 for the hardcoded rows, then the
// font-map letters -- to the row it's actually drawn on, once the section
// header/divider lines inserted along the way (one above each hotkey
// category, one above the hardcoded section, one above the font map) are
// accounted for. Used by both the render block and the ColorEdit cursor
// placement below, so the two always agree on where a given row lands.
static int ref_display_row(int selectable_row) {
    int headers = 0;
    for (int i = 0; i <= selectable_row; ++i) {
        if (i < kRefRowCount) { if (kRefRows[i].header) headers += 2; }
        else if (i == kRefRowCount || i == kRefRowCount + kRefHardcodedCount) headers += 2;
    }
    return selectable_row + headers;
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
        default: return nullptr;
    }
}

int App::settings_max_row() const {
    // Matches get_max_row(): SCHEMA.size() - 1 for each tab, extended for
    // the Reference tab's appended font-map rows and the About tab's
    // scrollable text (both computed dynamically, not hardcoded, so they
    // track the actual font_map/about_app_lines content).
    switch (settings_tab_) {
        case 0: return 13; // COLOR_SCHEMA: 14 rows
        case 1: return 8;  // ONOFF_SCHEMA: 9 rows
        case 2: return 7;  // ANIM_SCHEMA: 8 rows
        case 3: {
            int letters = 0;
            for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) ++letters;
            return kRefRowCount + kRefHardcodedCount + letters - 1; // rebindable hotkeys + hardcoded rows + N font-map rows
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
    if (settings_tab_ == 3 && row >= 0 && row < kRefRowCount) {
        auto it = settings_.hotkeys.find(kRefRows[row].action);
        return it != settings_.hotkeys.end() ? it->second : "";
    }
    return "";
}

// Matches g_options: the fixed value lists that Left/Right cycles
// through. Empty return = not cyclable (Colors and Reference rows,
// exactly like the reference's g_options map has no entries for those).
std::vector<std::string> App::settings_options_for(int tab, int row) const {
    if (tab == 1) return {"true", "false"};
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
    } else if (settings_tab_ == 3 && settings_row_ >= 0 && settings_row_ < kRefRowCount) {
        settings_.hotkeys[kRefRows[settings_row_].action] = buf;
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
}

void App::handle_settings_key(int key) {
    if (mode_ == Mode::ColorEdit) {
        if (key == 27) { mode_ = Mode::Settings; return; } // cancel, discard buffer
        if (key == '\r' || key == '\n') {
            std::string key_name = (settings_tab_ == 3 && settings_row_ >= 0 && settings_row_ < kRefRowCount)
                                  ? kRefRows[settings_row_].action : "";
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
                    force_redraw_ = true;
                    return; // stay in ColorEdit: redraw and repeat
                }
            }
            settings_commit_edit();
            status_line_ = key_name.empty() ? "UPDATED" : ("UPDATED " + key_name);
            mode_ = Mode::Settings;
            return;
        }
        if (key == 127 || key == 8) { pop_utf8_char(color_edit_buffer_); return; }
        // Same bug as Mode::Search below: arrow keys collapse to 'A'-'D',
        // which sit inside 32-126 and would otherwise get typed as literal
        // letters. No navigable list here to repurpose them for, so they're
        // just excluded -- also avoids letting a hotkey rebind on this same
        // buffer accidentally capture an arrow-key code, which would create
        // exactly this collision for whatever action got bound to it.
        if ((key == 'A' || key == 'B' || key == 'C' || key == 'D')) return;
        if (is_text_key(key) && color_edit_buffer_.size() < 18) color_edit_buffer_ += static_cast<char>(key);
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
        if (settings_tab_ == 3 && settings_row_ >= kRefRowCount) return; // hardcoded/font-map rows are read-only display
        color_edit_buffer_ = settings_get_value(settings_row_, settings_col_);
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
    launch_load_async(track.path, track.title, track.folder_artist == "-" ? "" : track.folder_artist,
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

    if (mode_ == Mode::Cheatsheet) {
        if (key == 27 || key == '?') mode_ = Mode::Browse;
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
        if (key == 127 || key == 8) {
            pop_utf8_char(search_buffer_);
            update_live_search_preview();
            return;
        }
        // BUGFIX (pre-existing, not Windows-specific): arrow keys collapse
        // to the same 'A'-'D' codes terminal_ui.h documents for Up/Down/
        // Right/Left, which sit inside the printable ASCII range the catch
        // below appends to the query -- so every arrow press was getting
        // typed into the search box as a literal letter instead of doing
        // anything. update_live_search_preview() already keeps local_view_
        // fully populated and live as you type, using the same selected_/
        // scroll_ state Browse mode does, so Up/Down here just navigates
        // that same list; Right/Left mirror Browse mode's seek-by-5-seconds
        // so you can still adjust playback without leaving the search box.
        if (key == 'A') { // up
            if (selected_ > 0) --selected_;
            if (selected_ < scroll_) scroll_ = selected_;
            return;
        }
        if (key == 'B') { // down
            // While actively typing "p:...", the live preview below is
            // already showing playlist_view_ (see update_live_search_preview())
            // -- navigate that instead of local_view_ in that case, same
            // as Enter/submit_search() would commit to.
            size_t list_len = (list_source_ == ListSource::Playlist) ? playlist_view_.size() : local_view_.size();
            if (list_len > 0 && selected_ < static_cast<int>(list_len) - 1) ++selected_;
            if (selected_ >= scroll_ + list_visible_rows_) scroll_ = selected_ - list_visible_rows_ + 1;
            return;
        }
        if (key == 'C') { // right = seek forward
            if (has_track_) player_.seek_relative(5.0);
            return;
        }
        if (key == 'D') { // left = seek back
            if (has_track_) player_.seek_relative(-5.0);
            return;
        }
        if (is_text_key(key)) {
            search_buffer_ += static_cast<char>(key);
            update_live_search_preview();
            return;
        }
        return;
    }

    // Mode::Browse
    size_t list_len = (list_source_ == ListSource::Local) ? local_view_.size()
                     : (list_source_ == ListSource::Online) ? online_view_.size()
                     : playlist_view_.size();

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
    } else if (action == "HKeySwitchBetweenCards") { // Tab: toggle Up/Down + reorder focus between the list and the queue
        queue_focus_ = !queue_focus_;
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
            if (selected_ >= scroll_ + list_visible_rows_) scroll_ = selected_ - list_visible_rows_ + 1;
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
    } else if (action == "HKeyDownloadStream") { // save cached stream to local music path
        if (has_track_) {
            if (path_utf8(current_path_).find(".cache") != std::string::npos || metadata_.location == "youtube") {
                std::string dest_dir;
                if (!settings_.local_music_paths.empty()) {
                    dest_dir = settings_.local_music_paths[0];
                } else {
                    const char* home = std::getenv("HOME");
                    dest_dir = home ? std::string(home) + "/Music" : "./Music";
                }
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
        log_event(std::string("sort: ") + sort_mode_name(local_sort_mode_));
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
        // to be rebound (see kRefHardcoded in the Reference tab).
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
// call from the render thread. For an MP3 with a plain, well-formed ID3v2
// tag (the common case) this is a complete answer on its own. For anything
// this can't handle (a non-MP3 format's tags, or an MP3 tag laid out in a
// way probe_id3v2_native() declines to guess at), it comes back with
// whatever duration native parsing found, tags_resolved left false --
// callers fall back to ffprobe for tags in that case.
//
// This is what turns "resolving an 800-track library's metadata" from
// hundreds of ffprobe subprocess spawns (the actual bottleneck -- each one
// costs tens to hundreds of milliseconds, worse under antivirus real-time
// scanning on Windows) into a few pread() syscalls per file for the
// overwhelming majority of a typical MP3 library.
RowMeta try_native_row_meta(const fs::path& path) {
    RowMeta rm;
    uint32_t dur = probe_duration_native(path);
    if (dur > 0) rm.duration_sec = static_cast<double>(dur);

    std::string ext = lower(path_utf8(path.extension()));
    if (ext == ".mp3") {
        NativeId3Tags tags = probe_id3v2_native(path);
        if (tags.resolved) {
            rm.title = tags.title;
            rm.artist = tags.artist;
            rm.album = tags.album;
            rm.tags_resolved = true; // fully resolved without ffprobe
        }
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
    for (int i = scroll_; i < std::min<int>(local_view_.size(), scroll_ + list_visible_rows_); ++i) {
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
    if (row_meta_resolver_started_.exchange(true)) return; // only ever runs once per app session
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

    local_view_ = filter_and_rank_local(q);

    if (had_selection) {
        selected_ = 0;
        for (size_t i = 0; i < local_view_.size(); ++i) {
            if (local_view_[i].path == prev_selected_path) { selected_ = static_cast<int>(i); break; }
        }
    }
    if (local_view_.empty()) selected_ = 0;
    else if (selected_ >= static_cast<int>(local_view_.size())) selected_ = static_cast<int>(local_view_.size()) - 1;
    if (scroll_ > selected_) scroll_ = selected_;
    if (selected_ >= scroll_ + list_visible_rows_) scroll_ = selected_ - list_visible_rows_ + 1;
}

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
        int meta_row_cursor_ = 1; // row 0 stays reserved for the "no track loaded" message
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
        meta_rows[0] = "no track loaded - press / to search, Enter to play";
    }
    for (int i = 0; i < static_cast<int>(meta_rows.size()); ++i) {
        if (meta_rows[i].empty()) {
            meta_rows[i] = std::string(meta_w, ' ');
        } else if (!has_track_ && i == 0) {
            meta_rows[i] = pad_right(meta_rows[i], meta_w);
        }
        // all other rows (kv data, visualizer) are already perfectly padded
        // by their respective builders, and padding them again would miscount 
        // their ANSI color escapes as visible columns, truncating them.
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

    if (!lyrics_avail) {
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
    out.push_back(bar + " " + row3_content + " " + bar + vol_tail);
    out.push_back(bottom_line);
    return out;
}
std::vector<std::string> App::build_search_bar(int total_width) const {
    std::string label = (list_source_ == ListSource::Online) ? "SEARCH ONLINE"
                       : (list_source_ == ListSource::Playlist) ? "SEARCH PLAYLISTS"
                       : "SEARCH LOCAL";

    std::string content;
    if (mode_ == Mode::Search) {
        content = "/" + search_buffer_ + "\u2588"; // block cursor
    } else if (list_source_ == ListSource::Online) {
        content = "/s:" + last_online_query_;
    } else if (list_source_ == ListSource::Playlist) {
        content = "/p:" + last_playlist_query_;
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
    out.push_back(box_line(content, search_w, border_ansi) + border_ansi + settings_.box_vertical
                  + " " + mode_letter + " " + settings_.box_vertical + "\x1b[0m");
    out.push_back(box_bottom(search_w, "", border_ansi_bottom) + border_ansi_bottom + "╰───╯\x1b[0m");
    return out;
}

std::vector<std::string> App::build_list_panel(int total_width, int height) const {
    bool online = (list_source_ == ListSource::Online);
    bool playlists_mode = (list_source_ == ListSource::Playlist);
    std::string label = online ? "ONLINE RESULTS"
                       : playlists_mode ? "SAVED PLAYLISTS (Enter: queue all)"
                       : "LOCAL AUDIO FILES (sort: " + std::string(sort_mode_name(local_sort_mode_)) + ")";
    size_t total = online ? online_view_.size() : playlists_mode ? playlist_view_.size() : local_view_.size();
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);

    // Whenever the hovered row changes, restart the marquee clock -- this
    // runs unconditionally (not just when the new row's title overflows)
    // so that hovering away and back to a long title always begins its
    // scroll from the start again, rather than resuming mid-scroll from
    // whatever an earlier visit had reached. Only meaningful for the
    // local list's title column (see below), so only tracked there.
    if (list_source_ == ListSource::Local && selected_ != marquee_row_idx_) {
        marquee_row_idx_ = selected_;
        marquee_since_ = std::chrono::steady_clock::now();
    }

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
                        if (!it->second.artist.empty()) artist = it->second.artist;
                    }
                }
                std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
                std::string t_title = apply_font_map(t.title, settings_.font_map);
                std::string t_artist = apply_font_map(artist, settings_.font_map);
                std::string t_dur = apply_font_map(fmt_mmss(dur), settings_.font_map);

                // Only the hovered row animates, and only when its title
                // is actually too long to fit -- every other row still
                // gets the same static truncate_str() as before, so
                // nothing about the rest of the list changes.
                std::string title_shown;
                if (idx == selected_ && display_width(t_title) > title_w) {
                    const double hold_secs = 1.2;    // pause on the title's start before scrolling
                    const double cols_per_sec = 4.0; // scroll speed
                    const std::string gap = "    ";  // seam between one loop and the next
                    std::string loop_text = t_title + gap;
                    int period = display_width(loop_text);
                    double elapsed = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - marquee_since_).count();
                    int start_col = 0;
                    if (elapsed > hold_secs && period > 0) {
                        double scrolled = (elapsed - hold_secs) * cols_per_sec;
                        start_col = static_cast<int>(scrolled) % period;
                    }
                    // Three repeats guarantee a full-width window is always
                    // available no matter where start_col lands in the cycle.
                    std::string doubled = loop_text + loop_text + loop_text;
                    title_shown = pad_right(utf8_skip_take(doubled, start_col, title_w), title_w);
                } else {
                    title_shown = truncate_str(t_title, title_w);
                }
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
    playlist_manage_view_ = PlaylistManager::list(playlists_dir());
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
    playlist_refresh_lib_view();
    playlist_refresh_manage_view();
}

void App::playlist_load_into_editor(const std::string& name) {
    auto pl = PlaylistManager::load(playlists_dir(), name);
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
    if (key == 'C' || key == 'D') { // left/right arrow -- the only 2 top-level tabs, so either just toggles
        playlist_tab_ = (playlist_tab_ + 1) % 2;
        if (playlist_tab_ == 1) playlist_refresh_manage_view();
        return;
    }

    if (playlist_tab_ == 1) {
        // --- Tab 1: browse/manage saved playlists ---
        int total = static_cast<int>(playlist_manage_view_.size());
        if (key == 'A') { // up
            if (playlist_manage_selected_ > 0) --playlist_manage_selected_;
            return;
        }
        if (key == 'B') { // down
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
        if (key == 127 || key == 8) { pop_utf8_char(playlist_edit_name_); playlist_edit_dirty_ = true; return; }
        if (key == '\r' || key == '\n') { playlist_edit_focus_ = 1; return; } // confirm name, jump to picking tracks
        // Up/Down arrows collapse to 'A'/'B', which sit inside the
        // printable-ASCII range is_text_key() below accepts -- without
        // this they'd get typed as literal "A"/"B" characters (the same
        // reason Mode::Search and Mode::BulkAdd filter them out too).
        if (key == 'A' || key == 'B') return;
        if (is_text_key(key) && playlist_edit_name_.size() < 80) {
            playlist_edit_name_ += static_cast<char>(key);
            playlist_edit_dirty_ = true;
        }
        return;
    }

    if (playlist_edit_focus_ == 1) { // library picker -- typing filters live, same as the main search box
        int total = static_cast<int>(playlist_edit_lib_view_.size());
        if (key == 'A') {
            if (playlist_edit_lib_selected_ > 0) --playlist_edit_lib_selected_;
            return;
        }
        if (key == 'B') {
            if (total > 0 && playlist_edit_lib_selected_ < total - 1) ++playlist_edit_lib_selected_;
            return;
        }
        if (key == '\r' || key == '\n') { playlist_add_hovering_to_edit(); return; }
        if (key == 127 || key == 8) {
            pop_utf8_char(playlist_edit_lib_query_);
            playlist_refresh_lib_view();
            return;
        }
        if (is_text_key(key)) {
            playlist_edit_lib_query_ += static_cast<char>(key);
            playlist_refresh_lib_view();
            return;
        }
        return;
    }

    // playlist_edit_focus_ == 2: the in-progress playlist's track list
    {
        int total = static_cast<int>(playlist_edit_tracks_.size());
        if (key == 'A') {
            if (playlist_edit_track_selected_ > 0) --playlist_edit_track_selected_;
            return;
        }
        if (key == 'B') {
            if (total > 0 && playlist_edit_track_selected_ < total - 1) ++playlist_edit_track_selected_;
            return;
        }
        // DEL, Backspace, or 'd' (lowercase only -- uppercase 'D' is the
        // globally-collapsed Left-arrow code, already intercepted above
        // for tab switching, so it never reaches here).
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

std::vector<std::string> App::build_playlist_library_panel(int total_width, int height) const {
    int inner = total_width - 4;
    std::string border_ansi = ansi_for(settings_.border_color, false);
    std::string border_ansi_bottom = ansi_for(settings_.border_color_bottom, false);
    std::string bar = border_ansi + settings_.box_vertical + "\x1b[0m";
    std::vector<std::string> out;

    std::string label = "LIBRARY  /" + playlist_edit_lib_query_ + (playlist_edit_focus_ == 1 ? "\u2588" : "");
    out.push_back(box_top(label, total_width, border_ansi));

    int total = static_cast<int>(playlist_edit_lib_view_.size());
    int scroll = std::clamp(playlist_edit_lib_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& t = playlist_edit_lib_view_[idx];
            int title_w = std::max(5, inner - idx_w - 2);
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_title = apply_font_map(t.title, settings_.font_map);
            // Every column padded to its own fixed width *before*
            // concatenating (rather than truncating the assembled whole
            // afterward) -- matches build_list_panel()'s row construction.
            // A one-off outer truncate/pad on the joined string is more
            // exposed to a single title's display_width() landing a
            // column short (an under-measured character widens the
            // padding that follows it), which visibly shifts every
            // border to its right; padding each piece independently
            // can't drift the same way.
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_title, title_w), title_w);
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

    int scroll = std::clamp(playlist_edit_track_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
        int idx = scroll + row;
        std::string content;
        if (idx < total) {
            const auto& t = playlist_edit_tracks_[idx];
            int title_w = std::max(5, inner - idx_w - 2);
            std::string shown = t.missing ? (t.title + " [missing]") : t.title;
            std::string t_idx = apply_font_map(std::to_string(idx + 1), settings_.font_map);
            std::string t_title = apply_font_map(shown, settings_.font_map);
            content = pad_right(t_idx, idx_w) + settings_.list_separator + " "
                    + pad_right(truncate_str(t_title, title_w), title_w);
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

    int total = static_cast<int>(playlist_manage_view_.size());
    if (total == 0) {
        int mid = height / 2;
        for (int row = 0; row < height; ++row) {
            std::string content;
            if (row == mid) {
                std::string text = apply_font_map("NO SAVED PLAYLISTS YET", settings_.font_map);
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

    int scroll = std::clamp(playlist_manage_selected_ - height / 2, 0, std::max(0, total - height));
    const int idx_w = 3;
    for (int row = 0; row < height; ++row) {
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
    int remaining = total - (scroll + height);
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
        std::string cursor = (playlist_edit_focus_ == 0) ? "\u2588" : "";
        std::string name_display = playlist_edit_name_.empty() ? "(untitled)" : playlist_edit_name_;
        std::string dirty_mark = playlist_edit_dirty_ ? " *" : "";
        frame << box_line("Name: " + name_display + cursor + dirty_mark, W, border) << "\n";
        fixed_rows += 1;
    }
    frame << box_bottom(W, "", border_bottom) << "\n";

    // Deliberately NOT sized to fill whatever room player_view_height()
    // happens to have (that made the list feel oddly tall/short
    // depending on terminal size) -- clamped to a fixed, comfortable
    // range instead so up to 22 tracks are visible regardless of
    // terminal height (fewer on a short terminal, down to the 8-row
    // floor).
    int panel_h = std::clamp(target_height - fixed_rows - 2, 8, 22); // -2: hint line + status line below
    if (playlist_tab_ == 0) {
        int left_w = W / 2;
        int right_w = W - left_w;
        auto left_lines = build_playlist_library_panel(left_w, panel_h);
        auto right_lines = build_playlist_tracks_panel(right_w, panel_h);
        size_t rows = std::max(left_lines.size(), right_lines.size());
        for (size_t i = 0; i < rows; ++i) {
            std::string l = (i < left_lines.size()) ? left_lines[i] : std::string(left_w, ' ');
            std::string r = (i < right_lines.size()) ? right_lines[i] : std::string(right_w, ' ');
            frame << l << r << "\n";
        }
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
        std::string hint = "[\u2190\u2192] Switch Tab | [TAB] Focus | [\u2191\u2193] Navi. | [ENTER] Add/Load | "
                            "[DEL] Remove  | [4/5] Move \u2191\u2193 | [HOME] Save | [ESC] Exit";
        frame << "\x1b[90m" << hint << "\x1b[0m\n";
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
        static const char* grp[14]  = {"BORDER_COLOR", "DISK", "METADATA", "VIZ", "PROGRESS_BAR",
                                        "LIST", "", "", "QUEUE", "", "", "LYRICS", "", ""};
        static const char* l1n[14]  = {"TOP", "TOP", "KEY", "LEFT", "PLAYED",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "PLAYING   FG", "CURSOR    FG",
                                        "INACTIVE  FG", "ACTIVE L  FG", "ACTIVE W  FG"};
        static const char* l2n[14]  = {"BOTTOM", "BOTTOM", "VAL", "RIGHT", "PENDING",
                                        "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG", "BG"};
        for (int i = 0; i < 14; ++i) {
            if (i == 5) { pos(y, 1, B(y) + "\u251c" + repeat("\u2500", W - 2) + "\u2524" + R); y++; }
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);

            pos(y, 3, pad(grp[i], 15)); pos(y, 18, ":");

            pos(y, 20, pad(l1n[i], 14, false)); pos(y, 35, ":");
            {
                bool sel = (i == settings_row_ && settings_col_ == 0 && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && settings_col_ == 0 && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 7) : pad(settings_get_value(i, 0), 7);
                pos(y, 38, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
            }

            pos(y, 48, pad(l2n[i], 7, false)); pos(y, 56, ":");
            {
                bool sel = (i == settings_row_ && settings_col_ == 1 && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && settings_col_ == 1 && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 7) : pad(settings_get_value(i, 1), 7);
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
            }
            if (!rt.empty()) pos(y, 72, rt);
            y++;
        }
    } else if (settings_tab_ == 1 || settings_tab_ == 2) {
        static const char* onoff_l[9] = {"Eliment Disk", "Dummy Buttons", "Queue Display", "WaveForm",
                                          "Lyrics Engine", "Lyric Ball", "Visualizer", "Stereo Sound",
                                          "Normalize Volume"};
        static const char* anim_l[8] = {"Vis. Fluidity", "Waveform Style", "Disk Speed", "Playback Mode",
                                         "Vis. Degradation", "Vis. Viscosity", "Lyrics Alignment", "Lyrics Animation"};
        int count = (settings_tab_ == 1) ? 9 : 8; // ON/OFF now has 9 rows, ANIMATION still 8
        const char* const* labels = (settings_tab_ == 1) ? onoff_l : anim_l;
        for (int i = 0; i < count; ++i) {
            pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
            pos(y, 6, pad(labels[i], 25)); pos(y, 32, ":");

            bool sel = (i == settings_row_ && mode_ != Mode::ColorEdit);
            bool ed = (i == settings_row_ && mode_ == Mode::ColorEdit);
            std::string v = ed ? pad(color_edit_buffer_, 20) : pad(settings_get_value(i, 0), 20);
            pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);

            if (sel && !settings_options_for(settings_tab_, i).empty()) pos(y, 57, "\x1b[90m< \u2194 >\x1b[0m");
            y++;
        }
    } else if (settings_tab_ == 3) {
        // Reference tab: the rebindable hotkeys grouped under category
        // headers (kRefRows), then a read-only "HARDCODED / NOT
        // REBINDABLE" section (kRefHardcoded), then a read-only display
        // of the font-mapping table (section 4 of the config, "A={A,a}"
        // style) loaded from config.txt -- as "A = A, a" rows. Combined
        // they're usually taller than the player view, so this scrolls as
        // one list (viewport follows settings_row_, centered) rather than
        // ever growing the panel past player_h. See ref_display_row() for
        // how a selectable row maps to the row it's drawn on.
        std::vector<char> letters;
        for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
        int total_selectable = kRefRowCount + kRefHardcodedCount + static_cast<int>(letters.size());
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

            // the header itself, in color 10 of the 256-color palette + bold
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, "\x1b[1;38;5;10m" + std::string(text) + R);
                y++;
            }
            disp++;
        };
        auto draw_hotkey_row = [&](int i) {
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, pad(kRefRows[i].label, 25)); pos(y, 32, ":");
                bool sel = (i == settings_row_ && mode_ != Mode::ColorEdit);
                bool ed = (i == settings_row_ && mode_ == Mode::ColorEdit);
                std::string v = ed ? pad(color_edit_buffer_, 20) : pad(settings_get_value(i, 0), 20);
                pos(y, 35, (sel ? HI : "") + (ed ? "\x1b[41;37m" : "") + v + R);
                y++;
            }
            disp++;
        };
        auto draw_hardcoded_row = [&](int i, int selectable_row) {
            if (in_view()) {
                pos(y, 1, B(y) + "\u2502" + R); pos(y, W, B(y) + "\u2502" + R);
                pos(y, 6, pad(kRefHardcoded[i].label, 25)); pos(y, 32, ":");
                bool sel = (selectable_row == settings_row_);
                pos(y, 35, (sel ? HI : "") + pad(kRefHardcoded[i].keys, 20) + R);
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

        for (int i = 0; i < kRefRowCount && y < MAX_Y; ++i) {
            if (kRefRows[i].header) draw_header(kRefRows[i].header);
            if (y < MAX_Y) draw_hotkey_row(i);
        }
        if (y < MAX_Y) draw_header("HARDCODED / NOT REBINDABLE");
        for (int i = 0; i < kRefHardcodedCount && y < MAX_Y; ++i) draw_hardcoded_row(i, kRefRowCount + i);
        if (y < MAX_Y) draw_header("FONT / CHARACTER MAP");
        for (size_t li = 0; li < letters.size() && y < MAX_Y; ++li)
            draw_font_row(letters[li], kRefRowCount + kRefHardcodedCount + static_cast<int>(li));
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
    if (!status_line_.empty()) pos(y, 1, "\x1b[32m" + status_line_ + "\x1b[0m");

    // 4. In-place text editing cursor placement.
    if (mode_ == Mode::ColorEdit) {
        if (settings_tab_ == 0) {
            int cy = 3 + settings_row_ + (settings_row_ >= 5 ? 1 : 0);
            int cx = (settings_col_ == 0) ? 38 : 59;
            frame << "\x1b[" << cy << ";" << (cx + static_cast<int>(color_edit_buffer_.size())) << "H\x1b[?25h";
        } else {
            int cy = 3 + settings_row_;
            if (settings_tab_ == 3) {
                // Reference tab scrolls once its row list exceeds the
                // visible window -- the common case, since it holds every
                // rebindable hotkey plus the hardcoded-keys section and
                // any font-map rows. Recompute the same scroll offset
                // used when rendering (see the settings_tab_==3 branch
                // above, and ref_display_row()) so the text cursor lands
                // on the row actually drawn there instead of one that's
                // already scrolled off-screen. Only ever reached with
                // settings_row_ < kRefRowCount: the Enter handler blocks
                // entering ColorEdit for the read-only hardcoded/font-map
                // rows below that.
                int visible = std::max(1, MAX_Y - 3);
                int cur_display = ref_display_row(settings_row_);
                std::vector<char> letters;
                for (char c = 'A'; c <= 'Z'; ++c) if (settings_.font_map.count(c)) letters.push_back(c);
                int total_selectable = kRefRowCount + kRefHardcodedCount + static_cast<int>(letters.size());
                int display_count = ref_display_row(total_selectable - 1) + 1;
                int scroll = std::clamp(cur_display - visible / 2, 0, std::max(0, display_count - visible));
                cy = 3 + (cur_display - scroll);
            }
            frame << "\x1b[" << cy << ";" << (35 + static_cast<int>(color_edit_buffer_.size())) << "H\x1b[?25h";
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

    // action, human-readable description -- key shown is whatever the
    // user actually has bound (config.txt / rebound in Settings), not a
    // hardcoded assumption, so this stays accurate after remapping.
    static const std::pair<const char*, const char*> rows[] = {
        {"HKeySearch",                      "Search local folder"},
        {"HKeySearchOnline",                "Search online (YouTube)"},
        {"HKeyDownloadStream",              "Download stream to 1st local path"},
        {"HKeyTogglePlayPause",             "Play / pause"},
        {"HKeyPlayNextSong",                "Play next in list/queue"},
        {"HKeyPlayPreviousSong",            "Play previous in list"},
        {"HKeySeekForward",                 "Seek forward 5s"},
        {"HKeySeekBackward",                "Seek backward 5s"},
        {"HKeyIncreaseVolume",              "Volume up"},
        {"HKeyDecreaseVolume",              "Volume down"},
        {"HKeyCyclePlayMode",               "Cycle play mode (list/repeat/shuffle/repeat queue/stop)"},
        {"HKeyRefreshUi",                   "Refresh UI (redraw)"},
        {"HKeyConsole",                     "Console / logs"},
        {"HKeySwitchBetweenCards",          "Switch between panels"},
        {"HKeyAddHoveringSongToQueue",      "Add hovering track to queue"},
        {"HKeyRemoveHoveringSongFromQueue", "Remove hovering track from queue"},
        {"HKeyFilterForFolder",             "Filter by folder"},
        {"HKeyClearFilter",                 "Clear filter"},
        {"HKeyQuit",                        "Quit"},
        {"HKeySetting",                     "Settings panel"},
        {"HKeyNavigateUp",                  "Explore list (up)"},
        {"HKeyNavigateDown",                "Explore list (down)"},
        {"HKeyToggleMute",                  "Mute (without pausing)"},
        {"HKeyCheatsheet",                  "This cheatsheet"},
        {"HKeyRetryLyrics",                 "Retry lyrics"},
        {"HKeyShuffleNext",                 "Shuffle to a random next track"},
        {"HKeyToggleLyrics",                "Toggle lyrics on/off"},
        {"HKeyQueueMoveUp",                 "Move hovering queue item up"},
        {"HKeyQueueMoveDown",               "Move hovering queue item down"},
        {"HKeyToggleWaveform",              "Toggle waveform style (raw/smooth)"},
        {"HKeyCycleSortMode",               "Cycle local list sort mode"},
        {"HKeyPlaylist",                    "Create/manage playlists"},
        {"HKeySearchPlaylist",              "Search saved playlists (type /p:query)"},
        {"HKeyToggleNormalize",             "Toggle loudness normalization"},
    };

    int height = std::max(term_rows_ - 4, 8); // real terminal height, minus this overlay's own top/bottom border rows
    int visible = std::max(1, height - 2);
    int total = static_cast<int>(std::size(rows));
    for (int r = 0; r < visible; ++r) {
        if (r >= total) { frame << box_line("", W, border) << "\n"; continue; }
        auto it = settings_.hotkeys.find(rows[r].first);
        std::string key = (it != settings_.hotkeys.end() && !it->second.empty()) ? it->second : "-";
        std::string line = pad_right(key, 14) + rows[r].second;
        frame << box_line(line, W, border) << "\n";
    }
    frame << box_bottom(W, "[? / ESC] close", border) << "\n";
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
    h += 1; // blank separator line
    h += 1; // status/loading line -- reserved even when currently empty, so this doesn't jitter frame to frame
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

    // Browse/BulkAdd/RetryLyrics all share the same live background (the
    // latter two float a small panel on top of it -- see
    // draw_floating_panel()'s comment in app.h), so switching between
    // them never needs a full clear, only a redraw. Settings/Console/
    // Cheatsheet are still genuine full-screen takeovers, so entering or
    // leaving any of *those* still forces one, same as a real mode
    // change always has.
    auto mode_family = [](Mode m) {
        switch (m) {
            case Mode::Browse: case Mode::Search: case Mode::BulkAdd: case Mode::RetryLyrics: return 0;
            case Mode::Settings: case Mode::ColorEdit: return 1;
            case Mode::Console: return 2;
            case Mode::Cheatsheet: return 3;
            case Mode::Playlist: return 4;
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

    if (mode_ == Mode::Settings || mode_ == Mode::ColorEdit) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_settings_screen(frame, W, player_view_height(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    if (mode_ == Mode::Console) {
        std::ostringstream frame;
        frame << "\x1b[2J\x1b[H\x1b[?25l";
        build_console_screen(frame, W, player_view_height(W));
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
        build_playlist_screen(frame, W, player_view_height(W));
        return clamp_output_rows(frame.str(), term_rows_);
    }

    // --- Browse mode (and the background behind BulkAdd/RetryLyrics):
    // figure out how many list/queue rows actually fit before building
    // anything, so the panel is sized right the first time instead of
    // being built tall and then chopped.
    auto metadata_lines = build_metadata_panel(W);
    auto progress_lines = build_progress_panel(W);
    auto search_lines = build_search_bar(W);
    int fixed_h = static_cast<int>(metadata_lines.size() + progress_lines.size() + search_lines.size())
                + 1  // blank separator line
                + 1; // status/loading line (reserved even when empty, so it doesn't jitter frame to frame)
    // -1 extra margin: leave the terminal's very last row untouched so a
    // trailing '\n' after the final printed line can never itself force
    // a scroll (see clamp_output_rows()'s comment for the same reasoning
    // applied as a hard backstop).
    int available_for_list = term_rows_ - fixed_h - 1;
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

    frame << "\n";
    if (load_in_progress_.load() && load_stage_.load() == 1) {
        // Only the online resolve/download step shows a live status —
        // local loads are probe-only now (near-instant) and deliberately
        // silent, no "loading..." flash.
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - load_started_at_).count();
        frame << "  resolving/downloading... (" << static_cast<int>(secs) << "s)\n";
    } else if (!status_line_.empty()) {
        frame << "  " << status_line_ << "\n";
    }

    frame << "\x1b[0J";

    // Bulk Add / Retry Lyrics: stamp their floating panel on top of the
    // still-live background just built above, rather than replacing it.
    // Uses absolute positioning (draw_floating_panel()), so it's simply
    // appended after the background's own sequential top-to-bottom
    // writes -- whichever content lands on a given screen cell last in
    // the stream wins, and the panel is emitted after, so it draws over
    // the background wherever they overlap without needing a clear.
    if (mode_ == Mode::BulkAdd) {
        draw_floating_panel(frame, build_bulk_add_panel(), kBulkAddPanelWidth, W);
    } else if (mode_ == Mode::RetryLyrics) {
        draw_floating_panel(frame, build_retry_lyrics_panel(), kRetryLyricsPanelWidth, W);
    }

    // Hard safety net on top of the list_visible_rows_ sizing above: even
    // if the fixed chrome alone (metadata+progress+search bar) is taller
    // than the terminal -- a case list_visible_rows_ can't do anything
    // about, since it only controls the list panel -- this guarantees
    // the actual byte stream handed to the terminal never contains more
    // rows than the terminal has, so it structurally cannot scroll no
    // matter what future panels/config combinations produce. Only
    // applied to the background portion's line count implicitly (the
    // floating panel's absolute-positioned writes come after and are
    // already bounds-checked by draw_floating_panel() itself, so
    // clamping here by counting trailing '\n's is still correct -- the
    // panel's writes don't add any that would trip this).
    return clamp_output_rows(frame.str(), term_rows_);
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
        for (int key = term.poll_key(); key != 0; key = term.poll_key()) {
            handle_key(key);
        }

        poll_pending_search();
        poll_pending_load();
        poll_pending_waveform();
        poll_pending_bulk_add();
        poll_pending_row_meta_tags();
        maybe_autosave();

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time_).count();
        last_frame_time_ = now;
        viz_dt_ = dt;

        if (has_track_) {
            player_.poll_elapsed();
            if (!advancing_ && player_.finished()) advance_track();
        }
        // Disk only spins while something is actually playing — frozen
        // when idle or paused, per instruction.
        if (has_track_ && !player_.is_paused()) {
            angle_ = std::fmod(angle_ + kAngularVelocity * settings_.disk_rotation_speed * dt,
                               2.0 * 3.14159265358979323846);
        }

        std::cout << render_frame(term) << std::flush;
        // 25fps (was 12.5fps) — the 700ms waveform reveal animation only
        // got ~9 frames to work with at the old 80ms cadence, which
        // showed as a handful of visible ~11% jumps rather than a smooth
        // continuous expansion. Also smooths disk rotation and the
        // visualizer's motion generally. Text-frame rendering is cheap
        // enough that doubling the rate here is not a meaningful CPU/
        // battery concern.
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

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
    std::cout << "\nbye.\n";
    return 0;
}

} // namespace muisc
