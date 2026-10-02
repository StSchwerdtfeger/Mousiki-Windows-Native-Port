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

// Ctrl+Shift+S and Ctrl+Shift+X -- the meta editor's "apply this editing
// session to the files" and "throw this editing session away" commands.
// These are modifier COMBINATIONS, so unlike a plain letter they can't be
// expressed as a hotkey string (hotkey_string_to_key() has no representation
// for them, and every letter code it could produce would be ambiguous with
// the arrow-key letters anyway), which is why they arrive as their own
// sentinel values rather than through settings_.hotkeys. On Windows they
// come from win_poll_key() reading dwControlKeyState; on POSIX from the
// xterm modifyOtherKeys/CSI-u encoding of a modified key.
// NOTE: the discard command used to be Ctrl+Shift+D, but on Windows
// Terminal that combination is the built-in "duplicate tab" shortcut, which
// intercepts the keystroke before it ever reaches this app -- pressing it
// just opened a second terminal window instead of discarding anything.
// Moved to Ctrl+Shift+X, which isn't a default Windows Terminal binding.
constexpr int kKeyCtrlShiftS = 302;
constexpr int kKeyCtrlShiftX = 303;

// Text-editing keys, the same idea again: Shift+Left/Right and Ctrl+C/X/V
// are modifier combinations, so they can't be hotkey strings and they can't
// be the bare 'A'-'D'/'c'/'x'/'v' values either (an arrow already collapsed
// to those letters, and a typed capital must still be typable into a field).
// They exist so the single-line editors (the meta editor's field editor and
// the Settings ColorEdit buffer) can move a caret, mark a selection and use
// the clipboard. They are ONLY ever meaningful inside those fields -- every
// other handler simply ignores a value it doesn't know, exactly like
// kKeyHome/kKeyDelete already do.
constexpr int kKeyShiftLeft = 304;  // Shift+Left  -- extend the selection left
constexpr int kKeyShiftRight = 305; // Shift+Right -- extend the selection right
constexpr int kKeyCtrlC = 306;      // Ctrl+C      -- copy
constexpr int kKeyCtrlX = 307;      // Ctrl+X      -- cut
constexpr int kKeyCtrlV = 308;      // Ctrl+V      -- paste
constexpr int kKeyEnd = 309;        // End (VK_END / xterm "ESC [ F")

// Alt+Left / Alt+Right -- the playlist editor's "switch tab" command.
// Plain Left/Right had to stay the caret keys in every one of that
// screen's text fields (name field, both search boxes) and its track
// list uses them for nothing at all, which meant a bare arrow could only
// ever switch tabs from the one pane that doesn't own a caret -- tabs
// were unreachable from the name field entirely. This was originally
// Ctrl+Arrow, but several terminals (Windows Terminal among them) claim
// Ctrl+Left/Right for their own word-navigation/tab shortcuts and never
// pass the keystroke through at all; Alt+Arrow isn't commonly bound that
// way, so it's used instead. Still a modifier combination no field
// claims for anything, same reasoning as kKeyCtrlShiftS/X above.
constexpr int kKeyAltLeft = 310;
constexpr int kKeyAltRight = 311;

// Ctrl+Shift+U / Ctrl+Shift+Z -- the queue's "save the queue as a playlist"
// and "undo the last clear" commands (main UI, incl. the big queue overlay).
// Modifier combinations again, so sentinels like kKeyCtrlShiftS/X above.
// (U rather than P: Windows Terminal binds Ctrl+Shift+P to its command
// palette, which swallowed the keystroke before this app ever saw it.)
constexpr int kKeyCtrlShiftU = 312;
constexpr int kKeyCtrlShiftZ = 313;

// Alt+L -- the lyrics timing overlay (main UI). Another modifier combination,
// so another sentinel (see kKeyCtrlShiftS/X above); not rebindable.
constexpr int kKeyAltL = 314;

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

// True while the key most recently returned by poll_key() was one of the
// four arrow keys. Arrows collapse to the letters 'A'-'D' (see kKeyHome's
// comment above), so a bare value of 'A' can be either an Up press or a
// real capital A -- indistinguishable by value alone. A text field that
// wants to accept those four capitals (a Windows path has to be able to
// type "C:\...") checks this to accept a typed letter while still
// ignoring arrow presses; navigation code that keys off 'A'-'D' can keep
// using the value as-is, since every return path updates this flag in
// step with what it returned.
bool last_key_was_arrow();

// True while the key most recently returned by poll_key() was an Up/Down
// arrow pressed WITH Shift held. The value itself stays a plain 'A'/'B'
// (and last_key_was_arrow() stays true), so every screen that doesn't care
// about the modifier keeps treating SHIFT+Up/Down exactly like Up/Down --
// only the list overlay (SHIFT+L) asks this, to turn them into page jumps.
// (SHIFT+Left/Right have their own sentinels, see kKeyShiftLeft/Right.)
bool last_key_was_shifted();

// ---------------------------------------------------------------------------
// Text-entry mode + clipboard
// ---------------------------------------------------------------------------
//
// While a single-line text field is being edited the console must deliver
// Ctrl+C to the app instead of raising a CTRL_C_EVENT: on Windows
// ENABLE_PROCESSED_INPUT swallows it (see win_raw_mode_enter()), on POSIX
// ISIG turns it into SIGINT, and in both cases the keystroke never reaches
// poll_key() -- so "copy" would be impossible. This flips exactly that one
// switch off (and nothing else: echo/line-buffering stay as raw mode left
// them), and the caller restores it by calling with false again. Safe to
// call every frame with the same value.
void set_text_entry(bool on);

// The system clipboard as UTF-8, and the setter for it. Used by Ctrl+C/X/V
// in the text fields, so a copied path or title can also be pasted OUTSIDE
// the app. get() returns "" when the clipboard holds no text (or reading it
// fails); control characters are stripped by the caller-side paste, not here.
// On POSIX there is no portable system clipboard, so an in-process buffer is
// used instead -- copy/paste still works within one run of the app.
std::string clipboard_get();
void clipboard_set(const std::string& utf8);

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
