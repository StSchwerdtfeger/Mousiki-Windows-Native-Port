#pragma once
#include <string>
#include <vector>

namespace muisc {

// Non-ASCII-range sentinel for the Home key (VT/xterm "\x1b[H", or
// VK_HOME on Windows) -- returned by poll_key() same as any other key.
// Deliberately outside 0-255 so it can never collide with a raw
// (possibly UTF-8 continuation) byte value coming through the same
// channel, unlike 'A'/'B'/'C'/'D' (the arrow keys), which *do* sit
// inside the printable-ASCII range and so still have to be explicitly
// filtered out of any text-entry field that doesn't want them typed
// literally (see e.g. Mode::Search's and Mode::BulkAdd's key handling).
constexpr int kKeyHome = 300;

// Same idea, for the forward-Delete key (VK_DELETE on Windows, xterm's
// "\x1b[3~" elsewhere) -- distinct from Backspace (127), which is a
// different physical key that some overlays additionally accept as a
// delete/remove action.
constexpr int kKeyDelete = 301;

// Raw, non-canonical, no-echo terminal mode + non-blocking key reads.
// Panel/box drawing lives in app.cpp; this is just the terminal plumbing.
class TerminalIO {
public:
    TerminalIO();
    ~TerminalIO();

    void restore();

    // Non-blocking single "logical" key read. Arrow keys (3-byte escape
    // sequences) collapse to 'A'/'B'/'C'/'D' (up/down/right/left); Home
    // (2- or 3-byte, terminal-dependent) collapses to kKeyHome. A lone
    // Escape key returns 27. Backspace returns 127. Returns 0 if nothing
    // is waiting. A non-ASCII keystroke (an umlaut, any other accented or
    // non-Latin character) arrives as its UTF-8 encoding, one byte per
    // call -- the same shape a plain read() off a UTF-8 terminal already
    // produces, so text-entry fields that accept it need no special
    // casing per platform.
    int poll_key();

    int rows() const;
    int cols() const;

private:
    bool raw_mode_active_ = false;
    void reassert_raw_mode(); // see poll_key()'s definition for why this exists
};

// Truncates/right-pads (by byte length — good enough for the mostly-ASCII
// UI text here; multi-byte titles may render slightly short) to exactly
// `width` visible columns.
std::string pad_right(const std::string& s, int width);
std::string pad_left(const std::string& s, int width);
std::string truncate_str(const std::string& s, int width);
std::string utf8_take(const std::string& s, int width);

// Word-wraps `s` to `width` display columns, across at most `max_lines`
// lines. A word wider than `width` on its own (a long filename, or any
// title with no spaces at all -- CJK, Thai, etc.) is chunked across
// successive lines rather than left to overflow or being flattened to one
// line. If the text still doesn't fit in `max_lines`, the last line ends
// in "..." exactly like truncate_str(). Returns fewer than `max_lines`
// entries when the text is shorter; never more.
std::vector<std::string> wrap_lines(const std::string& s, int width, int max_lines);

// Skips `skip_cols` display columns from the start of `s`, then returns up
// to the next `take_cols` display columns. A window into the middle of a
// string, rather than a truncation from its front -- what the local list's
// marquee scroll needs to show a moving slice of an over-wide title.
std::string utf8_skip_take(const std::string& s, int skip_cols, int take_cols);

// Computes terminal display width of a UTF-8 string based on wcwidth.
int display_width(const std::string& s);

// Emoji handling (see the long comment above replace_emoji() in
// terminal_ui.cpp). On (the default): every emoji cluster is measured and
// drawn as a single "?" by display_width()/pad_right()/truncate_str()/
// wrap_lines()/utf8_take()/utf8_skip_take(), so a title containing an emoji
// can never push the box borders out of line, whatever the terminal thinks the
// emoji's width is. Off: emoji pass through and are measured by the width
// table. Driven by config.txt's ReplaceEmoji.
void set_emoji_replacement(bool on);

} // namespace muisc
