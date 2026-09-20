#pragma once
#include <string>
#include <vector>

namespace muisc {

// Raw, non-canonical, no-echo terminal mode + non-blocking key reads.
// Panel/box drawing lives in app.cpp; this is just the terminal plumbing.
class TerminalIO {
public:
    TerminalIO();
    ~TerminalIO();

    void restore();

    // Non-blocking single "logical" key read. Arrow keys (3-byte escape
    // sequences) collapse to 'A'/'B'/'C'/'D' (up/down/right/left). A lone
    // Escape key returns 27. Backspace returns 127. Returns 0 if nothing
    // is waiting.
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

} // namespace muisc
