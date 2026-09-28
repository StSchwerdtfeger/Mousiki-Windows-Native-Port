#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "disk_art.h"
#include "fft_visualizer.h"
#include "history.h"
#include "local_source.h"
#include "lyrics_fetcher.h"
#include "meta_editor.h"
#include "metadata_probe.h"
#include "native_duration.h"
#include "online_source.h"
#include "player.h"
#include "playlist_manager.h"
#include "settings.h"
#include "snapshot.h"
#include "sphere_visualizer.h"
#include "streaming_pcm.h"
#include "terminal_ui.h"
#include "waveform.h"
#include "youtube_source.h"
#include "cache_manager.h"

namespace muisc {

enum class Mode { Browse, Search, Settings, ColorEdit, Console, Cheatsheet, BulkAdd, RetryLyrics, Playlist, MetaEdit, History, ClearQueue };
enum class ListSource { Local, Online, Playlist };

struct QueueItem {
    bool is_local;
    std::string title;
    std::string artist;
    fs::path local_path;   // valid if is_local
    std::string video_id;  // valid if !is_local
};

class App {
public:
    App();
    int run();

private:
    // --- infrastructure ---
    CacheManager cache_;
    YoutubeSource youtube_{cache_};
    OnlineSource online_;
    LocalSource local_source_;
    DiskArt disk_;
    mutable Player player_;
    fs::path lyrics_script_;
    fs::path fast_search_script_; // empty if not found -- OnlineSource falls back to yt-dlp

    // --- lists / navigation ---
    Mode mode_ = Mode::Browse;
    ListSource list_source_ = ListSource::Local;
    std::vector<LocalTrack> all_local_tracks_;
    // scan() runs from the constructor, before ConsoleLog::instance().init()
    // is called at the top of run() -- and init() wipes the in-memory log
    // buffer, so anything logged before it would just be discarded. This
    // holds scan()'s per-root diagnostics (found/missing, file counts) until
    // run() can actually flush them into the log.
    std::vector<std::string> local_scan_diagnostics_;
    std::vector<LocalTrack> local_view_;     // filtered
    std::vector<OnlineResult> online_view_;
    int selected_ = 0;
    int scroll_ = 0;
    std::string search_buffer_;
    std::string last_local_query_;
    ListSource pre_search_list_source_ = ListSource::Local;
    std::string pre_search_local_query_;
    std::string last_online_query_;
    int local_sort_mode_ = 0; // 0=folder order, 1=title A-Z, 2=artist A-Z
    static constexpr int kListVisibleRows = 8; // the *maximum*/preferred list height when there's room for it

    // --- terminal-height awareness --------------------------------------
    // The render loop used to only ever look at term.cols() (see the
    // comment on the width clamp in render_frame()) and unconditionally
    // emitted a fixed-height frame — metadata panel + progress + search
    // bar + a hardcoded kListVisibleRows-row list/queue panel + status
    // line — redrawn purely with "\x1b[H" + "\x1b[0J" and no alternate-
    // screen buffer. On a terminal shorter than that fixed height, each
    // frame overflows the viewport and scrolls; the next frame's
    // "\x1b[H" then homes to the top of the *new* scrolled-into-view
    // position rather than the top of the previous frame, so it draws a
    // fresh copy further down, which overflows again, forever — visible
    // as the panel endlessly re-duplicating itself downward.
    //
    // term_rows_ is the real, current terminal row count (from
    // TerminalIO::rows(), which does read the OS via ioctl(TIOCGWINSZ)
    // correctly -- it just wasn't being consulted anywhere). Refreshed
    // once per frame at the top of render_frame(). list_visible_rows_ is
    // how many list/queue rows *actually* fit this frame -- clamped
    // between 0 and kListVisibleRows based on how much room term_rows_
    // leaves after the fixed chrome (metadata/progress/search bar/status
    // line). Every place that used to scroll-clamp or size against the
    // kListVisibleRows constant now uses this instead, so what's
    // rendered and what the scroll math thinks is visible never
    // disagree. render_frame() also applies a hard line-count safety net
    // (see clamp_output_rows()) on top of this, so even a terminal too
    // short for the fixed chrome alone (metadata+progress+search bar)
    // still can never scroll — the two mechanisms are independent, not
    // "either/or".
    int term_rows_ = 24;
    int list_visible_rows_ = kListVisibleRows;
    std::string clamp_output_rows(const std::string& frame, int term_rows) const;

    mutable std::mutex row_meta_mutex_;
    std::unordered_map<std::string, RowMeta> row_meta_cache_;
    std::atomic<bool> row_meta_resolver_started_{false};
    void launch_row_meta_resolver();
    // Bumped by launch_row_meta_resolver()'s background thread every time it
    // resolves a new file's real tags (artist/title/album), so a search view
    // computed BEFORE that file's tags arrived can be told "something
    // changed, re-filter" instead of sitting stale until the user retypes
    // their query. See poll_pending_row_meta_tags() -- same
    // background-thread-finished-do-something-on-the-main-thread pattern as
    // poll_pending_waveform()/poll_pending_search() just below it.
    std::atomic<uint64_t> row_meta_tags_version_{0};
    uint64_t row_meta_tags_seen_ = 0; // main-thread only, no atomic needed
    void poll_pending_row_meta_tags();

    // --- queue ---
    std::vector<QueueItem> queue_;
    int queue_selected_ = 0;   // cursor/"hovering" row, only meaningful once queue_focus_ has been used
    int queue_scroll_ = 0;
    bool queue_focus_ = false; // Tab toggles which panel Up/Down navigates

    // --- playlists (local-files-only; see playlist_manager.h) -----------
    // Main UI: results of a "/p:" search (list_source_==Playlist), i.e.
    // browsing saved playlists the same way "/s:" browses online results.
    // Selecting one and hitting Enter queues every (non-missing) track it
    // contains -- see playlist_add_selected_to_queue().
    std::vector<PlaylistSummary> playlist_view_;
    std::string last_playlist_query_;
    std::vector<PlaylistSummary> filter_playlists(const std::string& query) const;
    // First configured PlaylistsPath (settings_.playlists_paths[0]) if the
    // user set one (config.txt's PlaylistsPath=), else
    // local_music_paths[0]/playlists -- this is the folder NEW playlists
    // are written to and deleted from.
    fs::path playlists_dir() const;
    // Every configured PlaylistsPath (all of them are searched when
    // listing/loading playlists); just {playlists_dir()} when none is set,
    // so every caller has at least one folder to look in.
    std::vector<fs::path> playlist_dirs() const;
    // Playlists found across playlist_dirs(), merged and de-duplicated by
    // name (the first folder containing a name wins) -- what both the
    // main "/p:" list and the playlist editor's manage tab show.
    std::vector<PlaylistSummary> playlist_summaries() const;
    // Loads a playlist by name from whichever playlist_dirs() folder has
    // it; std::nullopt if none does.
    std::optional<Playlist> load_playlist(const std::string& name) const;
    void playlist_add_selected_to_queue();

    // --- playlist editor overlay (Mode::Playlist, HKeyPlaylist) ----------
    // Two tabs: 0 = create/edit (name field + a local-library picker to
    // fuzzy-search/add from + the in-progress track list), 1 = browse
    // saved playlists (Enter loads one into tab 0 for re-editing).
    // HOME saves (deliberately not a plain letter -- "s" collided with
    // typing an "s" into the name/search fields). ESC exits; if tab 0 has
    // unsaved changes it asks first (playlist_confirm_exit_) rather than
    // silently discarding them.
    int playlist_tab_ = 0;
    int playlist_edit_focus_ = 0; // 0=name field, 1=library picker, 2=playlist-tracks list -- cycled with Tab
    std::string playlist_edit_name_;
    std::vector<PlaylistTrack> playlist_edit_tracks_;
    std::string playlist_edit_lib_query_;
    std::vector<LocalTrack> playlist_edit_lib_view_;   // filter_and_rank_local(playlist_edit_lib_query_)
    int playlist_edit_lib_selected_ = 0;
    int playlist_edit_track_selected_ = 0;
    bool playlist_edit_dirty_ = false;    // true once tab 0 has unsaved changes -- see ESC's confirm prompt below
    bool playlist_confirm_exit_ = false;  // "save before exiting?" Y/N prompt, shown in place of the hint line
    std::vector<PlaylistSummary> playlist_manage_view_; // tab 1's list
    int playlist_manage_selected_ = 0;
    // Tab 1 got a search box of its own (the same caret/selection/clipboard
    // treatment as every other text field -- see edit_text_key()). Focus
    // decides who owns the keys: in the box, typing filters and Left/Right
    // are caret keys; in the list, Up/Down/Enter/DEL work and Left/Right
    // keep switching tabs.
    std::string playlist_manage_query_; // filter behind playlist_manage_view_
    int playlist_manage_focus_ = 0;     // 0=search box, 1=list -- cycled with Tab
    bool playlist_confirm_delete_ = false; // "really delete this playlist?" Y/N prompt (tab 1, DEL key)
    std::string playlist_status_; // shown at the bottom of the overlay; cleared on (re)entry

    void playlist_refresh_lib_view();
    void playlist_refresh_manage_view();
    void playlist_open_editor();  // HKeyPlaylist entry point -- resets to a blank new playlist on tab 0
    void playlist_load_into_editor(const std::string& name);
    void playlist_add_hovering_to_edit();
    void playlist_remove_hovering_track();
    void playlist_move_hovering_track(int dir); // dir=-1 up, +1 down -- keys 4/5, mirrors queue_move_hovering
    void playlist_delete_selected(); // tab 1's DEL, after playlist_confirm_delete_ confirms
    void playlist_save_current();
    void handle_playlist_key(int key);
    void build_playlist_screen(std::ostringstream& frame, int W, int player_h) const;
    std::vector<std::string> build_playlist_library_panel(int width, int height) const;
    std::vector<std::string> build_playlist_tracks_panel(int width, int height) const;
    std::vector<std::string> build_playlist_manage_panel(int width, int height) const;

    // --- meta/tag editor overlay (Mode::MetaEdit, HKeyMetaEditor = Shift+M) ---
    // Deliberately shaped like the playlist editor above: same tab strip,
    // same boxed side-by-side panels, same hint/status footer, same Y/N
    // confirmation prompts in place of the hint line. Tab 0 = edit (a search
    // field, a library picker, and the five editable fields -- FILE plus
    // ARTIST/TITLE/ALBUM/YEAR -- for the row the picker is on); tab 1 = the
    // fetch list, filled with 'a' and run through AcoustID with Enter.
    //
    // Everything typed or fetched here goes into meta_session_ -- a pending
    // edit session that is autosaved to disk after every change (meta_editor.h)
    // but is NEVER written to the audio files on its own. Ctrl+Shift+S applies
    // it, Ctrl+Shift+X throws it away, and simply leaving (ESC or quitting)
    // keeps the autosave backup, so no amount of editing can lose work by
    // accident.
    int meta_tab_ = 0;
    int meta_focus_ = 0; // 0=search field, 1=library picker, 2=field editor -- cycled with Tab
    std::string meta_query_;
    std::vector<LocalTrack> meta_lib_view_;  // filter_and_rank_local(meta_query_)
    // 'r' in the library pane: park every file that carries a pending edit
    // at the top of the pane. Off by default, so merely opening the editor
    // never reorders the list behind the user's back -- see
    // meta_refresh_lib_view(), which does the sorting on every refresh.
    bool meta_resort_edited_ = false;
    int meta_lib_selected_ = 0;
    int meta_field_ = 0; // hovered row of the field editor (0=FILE .. 4=YEAR)
    std::vector<MetaEditEntry> meta_session_;          // the pending edits
    std::vector<std::string> meta_fetch_list_;         // paths queued for an AcoustID lookup
    int meta_fetch_selected_ = 0;
    bool meta_session_loaded_ = false; // load_session() already ran (once per run, not once per open)
    std::string meta_status_; // bottom line of the overlay; cleared on (re)entry
    // Y/N confirmation currently covering the footer (in the menu) or the
    // status line (in Browse, for Shift+B), plus the exact paths it will act
    // on once confirmed. Every key is swallowed while one is up.
    enum class MetaPrompt { None, Save, Discard, Fetch };
    MetaPrompt meta_prompt_ = MetaPrompt::None;
    std::vector<std::string> meta_prompt_paths_;
    // AcoustID batch: runs on its own thread, publishes results under
    // meta_fetch_mutex_, applied to the session on the main thread by
    // poll_pending_meta_fetch() (called from the render loop).
    fs::path meta_script_; // scripts/fetch_meta.py, next to the exe
    std::thread meta_fetch_thread_;
    std::atomic<bool> meta_fetch_running_{false};
    mutable std::mutex meta_fetch_mutex_;
    std::vector<MetaFetchResult> meta_fetch_results_;
    // Paths the current batch has already delivered successfully (filled in
    // by poll_pending_meta_fetch() on the main thread, so no lock needed).
    // On completion they leave the fetch list -- what stays queued is
    // exactly what still needs another try.
    std::vector<std::string> meta_fetch_ok_paths_;
    bool meta_fetch_done_ = false;
    MetaFetchOutcome meta_fetch_outcome_;
    std::string meta_fetch_progress_; // "3/7", shown while a batch runs
    // What the field editor panel draws for the currently hovered path.
    // Precomputed once per frame (meta_refresh_hover_values(), just before
    // rendering) because the current tag values need a mutable cache lookup,
    // while build_meta_screen() itself is const.
    std::array<std::string, kMetaFieldCount> meta_hover_values_{};
    bool meta_hover_resolved_ = false; // tags for that path are final (vs. not read yet)

    void meta_open();                  // HKeyMetaEditor entry point
    void meta_ensure_session_loaded(); // restores the autosaved session, once
    void meta_refresh_lib_view();
    void meta_toggle_resort();         // 'r', edited files to the top of the pane
    // Missing-tag filters over the library pane: 0 = off, 1 = files with no
    // metadata at all ('x'), 2 = missing title (Shift+T), 3 = missing artist
    // (Shift+A), 4 = missing year (Shift+Y). Pressing the same key again
    // clears it -- see meta_toggle_filter().
    int meta_filter_ = 0;
    void meta_toggle_filter(int filter);
    const char* meta_filter_label() const;
    void meta_persist();               // save (or delete) the autosave backup file
    const MetaEditEntry* meta_entry(const std::string& path) const;
    MetaEditEntry& meta_touch_entry(const std::string& path); // create on first edit
    void meta_set_field(const std::string& path, int field, const std::string& value);
    std::string meta_display_value(const std::string& path, int field, const MetaEditEntry* e);
    RowMeta meta_row_meta(const fs::path& path); // current tags of a file, cached + native
    std::string meta_hovering_path() const;      // path the picker/editor/fetch tab is on
    void meta_refresh_hover_values();            // fills meta_hover_values_ for this frame
    void meta_add_hovering_to_fetch();           // 'a', mirrors the queue's add key
    void meta_remove_hovering();                 // DEL/'d'
    void meta_prompt_single_fetch(const std::string& path); // Shift+B
    void meta_prompt_list_fetch();                          // Enter on tab 1
    void meta_start_fetch();
    void poll_pending_meta_fetch();
    void meta_apply_session();  // Ctrl+Shift+S, after Y
    void meta_discard_session();// Ctrl+Shift+X, after Y
    // True if a confirmation was up and the key was consumed by it (incl. the
    // swallow-everything-else case). Checked first thing in handle_key().
    bool handle_meta_prompt_key(int key);
    void handle_meta_key(int key);
    void build_meta_screen(std::ostringstream& frame, int W, int player_h) const;
    std::vector<std::string> build_meta_library_panel(int width, int height) const;
    std::vector<std::string> build_meta_fields_panel(int width, int height) const;
    std::vector<std::string> build_meta_fetch_panel(int width, int height) const;

    // --- listening history overlay (Mode::History, HKeyHistory = Shift+H) ---
    // Three tabs in the shape of the two overlays above: 1 HISTORY (the last
    // 100 plays, newest first), 2 TOP TRACKS (per title, sorted by play count
    // -- 'r' flips between most- and least-played first), 3 HABITS (session
    // and play-behaviour aggregates, each category under a header_sgr()
    // header). The data itself lives in HistoryStore history_ (appended to on
    // the main thread only, persisted to ~/.cache/mousiki/history/history.json
    // after every finished track and again at shutdown).
    int history_tab_ = 0;              // 0=History, 1=Top Tracks, 2=Habits
    int history_selected_ = 0;         // cursor within tabs 0/1
    int history_scroll_ = 0;           // manual scroll offset (tab 2's content)
    bool history_most_first_ = true;   // 'r' on the Top Tracks tab
    std::vector<HistoryTopRow> history_top_view_; // rebuilt by history_refresh_top()
    std::string history_status_;       // footer status line, set by 'r' / the queue adds
    // Top Tracks tab only: it is split into two stacked panes. 0 = the track
    // list (Up/Down move its cursor), 1 = the "ADD TOP TRACKS TO QUEUE" pane
    // below it (Up/Down pick Top 10/25/50/100, Enter queues them). TAB toggles.
    int history_pane_ = 0;
    int history_add_sel_ = 0;          // 0..3 -> kHistoryAddCounts[]
    static constexpr int kHistoryAddCounts[4] = {10, 25, 50, 100};
    HistoryStore history_;             // the store itself (also used outside this overlay)

    void history_open();               // HKeyHistory entry point
    void history_refresh_top();        // rebuilds history_top_view_ from history_
    void handle_history_key(int key);
    // Both non-const: they clamp history_selected_/history_scroll_ against the
    // content they actually ended up drawing (see the window code at the end
    // of build_history_panel()).
    void build_history_screen(std::ostringstream& frame, int W, int player_h);
    std::vector<std::string> build_history_panel(int width, int height);
    std::vector<std::string> build_history_add_panel(int width, int height) const; // Top Tracks tab, 2nd pane
    // Queues the n most-played titles (always most-played first, whatever
    // order the Top Tracks list is currently showing). Local files that no
    // longer exist are skipped and reported, like a playlist add.
    void history_add_top_to_queue(int n);
    // Play bookkeeping: called from poll_pending_load()/advance_track() when a
    // track starts or is handed over, and from the frame loop to accrue time.
    void history_end_current_play();   // closes the live record (no-op if none) + saves
    void history_begin_current_play(); // opens one for current_path_/metadata_

    // --- now playing ---
    bool has_track_ = false;
    // True from the moment advance_track() hands off to a load until
    // poll_pending_load() publishes its result (success or failure). While
    // it's set the finished track stays "current" (has_track_ remains true, so
    // the UI doesn't flash "no track loaded" between songs), and this flag is
    // what stops the main loop re-firing advance_track() every frame on the
    // still-set finished flag.
    bool advancing_ = false;
    fs::path current_path_;
    bool current_is_local_ = true;  // for snapshot identity -- current_path_ alone is ambiguous
                                     // (online tracks resolve to a cache path too)
    std::string current_video_id_;  // valid when !current_is_local_
    TrackMetadata metadata_;
    std::vector<float> waveform_envelope_;
    std::chrono::steady_clock::time_point waveform_reveal_start_;
    bool waveform_ready_ = false;
    std::shared_ptr<StreamingPcm> current_pcm_;
    size_t total_sec_ = 0;
    double angle_ = 0.0;
    std::chrono::steady_clock::time_point last_frame_time_;
    static constexpr double kAngularVelocity = (2.0 * 3.14159265358979323846 / 48.0) / 0.035;
    mutable FftVisualizer fft_;
    mutable SphereVisualizer sphere_;
    mutable std::string last_lyrics_status_;
    mutable std::chrono::steady_clock::time_point lyrics_status_shown_at_;
    mutable double viz_dt_ = 0.08;

    // --- local list marquee (hovered row's title, when too long to fit) ---
    // Tracks which row the scroll animation is currently following and
    // when it started, so moving the cursor to a different row always
    // restarts the scroll from the beginning of that row's title instead
    // of resuming wherever the previous row's animation had reached.
    mutable int marquee_row_idx_ = -1;
    mutable std::chrono::steady_clock::time_point marquee_since_;
    // Same idea, kept separate because the playlist editor's LIBRARY and
    // TRACKS panels are two independent lists with their own selection
    // cursor, visible on screen at the same time -- sharing one row/clock
    // pair between them would make switching focus between the two panels
    // restart (or skip restarting) the wrong one's scroll.
    mutable int marquee_pl_lib_row_idx_ = -1;
    mutable std::chrono::steady_clock::time_point marquee_pl_lib_since_;
    mutable int marquee_pl_track_row_idx_ = -1;
    mutable std::chrono::steady_clock::time_point marquee_pl_track_since_;

    // --- lyrics (background-fetched) ---
    mutable std::mutex lyrics_mutex_;
    mutable LyricsResult lyrics_result_;
    std::atomic<bool> lyrics_ready_{false};
    std::atomic<int> lyrics_epoch_{0};
    void launch_lyrics_fetch(std::string title, std::string artist, fs::path path, bool force_network = false);

    std::string status_line_;
    bool quit_ = false;
    bool force_redraw_ = false;
    int last_render_w_ = -1;
    Mode last_render_mode_ = Mode::Browse;

    // --- mute (volume forced to 0 without touching pause state) --------
    bool muted_ = false;
    int pre_mute_volume_ = 70;

    // --- folder filter (HKeyFilterForFolder / HKeyClearFilter) ---------
    // Parent-directory path of the currently filtered folder, or empty
    // for "no folder filter". Applied on top of whatever the search/sort
    // already produced, in refresh_local_view().
    std::string folder_filter_;

    // --- floating panels (Bulk Add, Retry Lyrics) ------------------------
    // Unlike Console/Settings/Cheatsheet (full-screen overlays that
    // replace the whole view), these render *on top of* the still-live
    // Browse view behind them: render_frame() draws the normal
    // metadata/progress/search/list/status background exactly as always,
    // then this stamps a pre-built block of lines over it at an absolute
    // screen position via "\x1b[{row};{col}H" writes -- no clear, so
    // whatever was already drawn underneath stays visible around the
    // panel's edges. `lines` must already be exactly `panel_w` display
    // columns wide (pad_right them before calling) since this does no
    // width accounting of its own, just placement.
    //
    // Horizontally centered against the background's own width (W), not
    // the raw terminal width -- if the terminal's wider than W the
    // background content itself is left-anchored, and centering against
    // the full terminal would visually detach the panel from the
    // content it's supposed to be floating over. Vertically centered
    // against term_rows_, nudged up a few rows rather than dead-center.
    void draw_floating_panel(std::ostringstream& frame, const std::vector<std::string>& lines, int panel_w, int W) const;
    static constexpr int kFloatingPanelUpShift = 3; // rows nudged above true vertical center

    // --- console / log overlay (HKeyConsole) ----------------------------
    // Backed by the global ConsoleLog (console_log.h/.cpp), which owns
    // both the on-disk $HOME/.cache/mousiki/logs/console.log and the
    // in-memory buffer this overlay renders -- see log_event() and
    // build_console_screen().
    void log_event(const std::string& msg);
    void build_console_screen(std::ostringstream& frame, int W, int target_height) const;

    // --- cheatsheet overlay (HKeyCheatsheet) ----------------------------
    // Scroll position within the key table -- the table is longer than a
    // small terminal can show at once, and (unlike Settings' Reference tab)
    // this overlay used to just silently drop whatever didn't fit. Mutable
    // because build_cheatsheet_screen() is const and clamps it against the
    // rows that actually fit as it draws.
    mutable int cheatsheet_scroll_ = 0;
    void build_cheatsheet_screen(std::ostringstream& frame, int W) const;

    // The Console and Settings overlays must always be exactly as tall as
    // the Browse-mode player view actually renders at right now -- not
    // just "whatever fits the terminal" (that's term_rows_, an upper
    // bound, not the target). Rebuilds the same panels Browse mode would
    // and sums their line counts, using the *current* list_visible_rows_
    // (itself already term_rows_-aware) for the list/queue panel's share,
    // so this always matches frame-for-frame regardless of which panels
    // are currently enabled/how tall lyrics or disk art are configured.
    int player_view_height(int w) const;

    // --- hotkey rebinding conflict check --------------------------------
    // Returns the action name already bound to key_str (excluding
    // except_action), or "" if key_str is free. Used by the Settings
    // Reference tab so rebinding a hotkey to a key another action already
    // owns is rejected instead of silently creating an overlap.
    std::string hotkey_conflict(const std::string& key_str, const std::string& except_action) const;

    // --- autosave / session snapshot ------------------------------------
    // Consumed exactly once, right after a startup restore: the very
    // first launch_device_play_async() call for the restored track uses
    // this as its start position instead of 0.0, then zeroes it out so
    // every normal track change afterwards starts at 0 like always.
    double resume_start_sec_ = 0.0;
    std::chrono::steady_clock::time_point last_autosave_at_;
    // Drives the indicator's brief "something just saved" animation --
    // ticks for kAutosavePulseSeconds after each save, then goes idle.
    std::chrono::steady_clock::time_point autosave_pulse_started_at_;
    bool autosave_pulse_active_ = false;
    static constexpr double kAutosavePulseSeconds = 1.2;
    SnapshotData build_snapshot() const;
    // Applies a loaded snapshot: queue, play_mode, mute/volume, and kicks
    // off loading the saved "now playing" track at resume_start_sec_.
    // Called once at startup, before the render loop starts.
    void restore_snapshot(const SnapshotData& snap);
    // Called every frame from run(); saves (and pulses the indicator)
    // once settings_.autosave_delay_sec has elapsed since the last save.
    void maybe_autosave();
    // Builds the little "•" (or configured glyph) indicator string for
    // the volume-bar row, honoring AutoSave/AutoSaveIndicator/
    // AutoSaveIndicatorType. Returns "" when there's no room or the
    // feature's fully off (see build_progress_panel()'s comment on why
    // that also collapses the gap rather than just hiding a char in it).
    std::string autosave_indicator_glyph() const;

    // --- bulk add (paste a YouTube playlist link while Queue is
    // focused; hovering-song add on 'a' stays the List-panel behavior) --
    // Floating panel (see draw_floating_panel()), fixed total line count
    // across both phases -- unused rows are just blank-padded rather
    // than the panel changing size -- so its on-screen footprint never
    // moves/resizes frame to frame while open. Two phases:
    //   1. Input: bulk_add_results_ready_ == false -- typing the link.
    //   2. Results: fetch succeeded -- a compact starred checklist,
    //      navigable with Up/Down, toggled per-row with Space. "a" adds
    //      every fetched track regardless of star state ("ALL"); Enter
    //      adds only the starred ones ("[SELECT]").
    std::string bulk_add_buffer_;
    bool bulk_add_results_ready_ = false;
    std::vector<bool> bulk_add_selected_;   // parallel to pending_bulk_add_.items, default all-starred
    int bulk_add_cursor_ = 0;
    int bulk_add_scroll_ = 0;
    static constexpr int kBulkAddVisibleRows = 9; // preview list height cap -- this is what keeps the panel "tiny"
    struct BulkAddResult {
        bool success = false;
        std::string error;
        std::vector<OnlineResult> items;
    };
    std::thread bulk_add_thread_;
    std::mutex bulk_add_mutex_;
    std::atomic<bool> bulk_add_in_progress_{false};
    std::atomic<bool> bulk_add_ready_{false};
    BulkAddResult pending_bulk_add_;
    void launch_bulk_add_async(const std::string& url);
    void poll_pending_bulk_add();
    void commit_bulk_add(bool all); // all=true -> every fetched track; all=false -> only starred ones
    static constexpr int kBulkAddPanelWidth = 62; // matches the reference design exactly
    std::vector<std::string> build_bulk_add_panel() const; // returns fixed-width, fixed-height lines for draw_floating_panel()

    // --- retry lyrics (HKeyRetryLyrics, 'l') ------------------------------
    // A manual override form: rather than instantly re-fetching with the
    // track's own metadata, this lets the person edit the title/artist
    // mousiki searches with and tack on edit-qualifier tags (slowed,
    // reverb, ...) -- for tracks whose auto-fetched lyrics are wrong or
    // missing because the real upload's title doesn't match what's
    // playing. Opens pre-filled from the current track's metadata_ (with
    // a best-effort "ft./feat." split out of the title into its own
    // field) rather than blank.
    enum class RLField {
        Title, Artist, Ft,
        TypeReverb, TypeSlowed, TypeUltraSlowed, TypeSpedup, TypeRemix, TypeOther,
        RemixText, OtherText,
    };
    std::string rl_title_, rl_artist_, rl_ft_;
    std::string rl_remix_text_, rl_other_text_;
    // TypeSlowed/TypeUltraSlowed/TypeSpedup are mutually exclusive (a
    // radio group -- at most one true); TypeReverb/TypeRemix/TypeOther
    // are independent toggles, any combination.
    bool rl_reverb_ = false, rl_slowed_ = false, rl_ultra_slowed_ = false;
    bool rl_spedup_ = false, rl_remix_ = false, rl_other_ = false;
    RLField rl_focus_ = RLField::Title;
    static constexpr int kRetryLyricsPanelWidth = 62; // matches the reference design exactly
    // The fields actually navigable right now, in on-screen order --
    // RemixText/OtherText only appear in this list once their checkbox
    // is on, which is what makes them "dynamically available".
    std::vector<RLField> rl_visible_fields() const;
    void rl_open_from_current_track();     // pre-fill + reset state, called when 'l' opens the panel
    void rl_submit();                       // builds the override query and launches the fetch
    bool* rl_bool_ptr(RLField f);           // nullptr for non-checkbox fields
    std::string* rl_text_ptr(RLField f);    // nullptr for checkbox fields
    std::vector<std::string> build_retry_lyrics_panel() const; // fixed-width, fixed-height lines for draw_floating_panel()

    // --- async track loading ---
    struct PendingLoad {
        bool success = false;
        std::string title, artist, location_label, error;
        fs::path path;
        std::shared_ptr<StreamingPcm> pcm;
        size_t total_sec = 0;
        TrackMetadata metadata;
        bool is_local = true;   // for snapshot/resume identity -- which of path/video_id is authoritative
        std::string video_id;   // valid if !is_local
    };
    std::thread load_thread_;
    std::mutex load_mutex_;
    std::atomic<bool> load_ready_{false};
    std::atomic<bool> load_in_progress_{false};
    std::atomic<int> load_stage_{0};
    std::chrono::steady_clock::time_point load_started_at_;

    // Every Player call (play/stop/seek/volume/...) funnels through one
    // persistent thread that lives for the whole app session, rather than a
    // fresh std::thread per track switch. On Windows this is load-bearing,
    // not just tidy: WASAPI's underlying COM objects are apartment-affine
    // to the thread that created them, and the old per-track-thread design
    // meant track 2's play() call -- which starts by tearing down track 1's
    // device -- ran on a *different* OS thread than the one that created
    // that device. That mismatch is exactly why playback worked once and
    // then silently stopped starting on every subsequent switch. Routing
    // every device operation through one fixed thread removes the mismatch
    // outright, on every platform (Linux/PulseAudio never had this
    // constraint, but there's no downside to the safer design there either).
    //
    // player_mutex_ guards every call into player_ from either this worker
    // thread or the main thread (seek/volume/pause hotkeys, the shutdown
    // path's player_.stop()) -- Player's public methods were never
    // documented as safe to call concurrently from two threads, and with a
    // long-lived worker thread now genuinely overlapping the main loop for
    // the whole session (instead of a short-lived ad-hoc thread that mostly
    // wasn't), that latent race needed closing rather than just getting
    // more likely to bite.
    std::mutex player_mutex_;
    std::thread device_worker_thread_;
    std::mutex device_request_mutex_;
    std::condition_variable device_request_cv_;
    std::atomic<int> device_gen_{0}; // incremented each launch; guards against a stale request read racing a newer post
    // Generation of the device handoff currently in flight: set the moment a
    // request is posted, cleared by the worker once player_.play() has
    // actually swapped the new track in (0 = none in flight). While this is
    // nonzero the main loop must NOT act on player_.finished(): until play()
    // runs, the PREVIOUS pcm is still the installed one, so a track that
    // already ended keeps re-latching finished_ from the audio callback every
    // few milliseconds. clear_finished() in poll_pending_load() erases it
    // once, but the old device immediately set it again -- which made a freshly
    // loaded track flip straight back to "no track loaded" (advance_track() ->
    // Stop mode -> has_track_ = false) a frame later, while its own audio
    // started a few hundred ms after that. Pressing play again "fixed" it only
    // because by then the installed pcm was the *playing* track, which can't
    // latch the flag. This holds the stale flag off for the whole handoff.
    std::atomic<int> device_play_pending_gen_{0};
    bool device_worker_stop_ = false;
    bool device_request_ready_ = false;
    struct DevicePlayRequest {
        std::shared_ptr<StreamingPcm> pcm;
        int volume = 70;
        double start_sec = 0.0;
        int generation = 0;
    };
    DevicePlayRequest device_request_;
    void device_worker_loop();       // body of device_worker_thread_, runs for the app's whole session
    void start_device_worker();      // called once, from the constructor
    void stop_device_worker();       // called once, from run()'s shutdown, before joining device_worker_thread_
    void launch_device_play_async();

    PendingLoad pending_load_;
    void launch_load_async(fs::path local_path, std::string title, std::string artist,
                            std::string location_label, bool is_local, std::string video_id);
    void poll_pending_load();
    static void write_load_timing_log(const std::string& title, bool is_local, double t_resolve,
                                       double t_probe, double t_total, const std::string& error);

    // --- deferred mini-waveform pass ---
    std::mutex waveform_mutex_;
    std::atomic<bool> waveform_pending_ready_{false};
    std::atomic<int> waveform_epoch_{0}; // incremented on each recompute; stale threads discard their result
    std::vector<float> pending_waveform_envelope_;
    void poll_pending_waveform();

    // --- async online search ---
    std::thread search_thread_;
    std::mutex search_mutex_;
    std::atomic<bool> search_ready_{false};
    std::atomic<bool> search_in_progress_{false};
    std::vector<OnlineResult> pending_search_results_;
    void launch_search_async(const std::string& query);
    void poll_pending_search();

    // --- settings panel (5 tabs: Colors, On/Off, Animation, Reference, About App) ---
    // Rendering uses absolute cursor positioning (\x1b[y;xH) rather than
    // building padded strings line by line -- each field goes exactly
    // where it's told regardless of what else is on that row, which is
    // what actually fixes the truncation-corrupts-everything fragility
    // class of bug (a mis-sized pad on one row used to bleed into
    // whatever the next escape code was).
    Settings settings_;
    static constexpr int kSettingsTabCount = 5; // Colors, On/Off, Animation, Reference, About App
    int settings_tab_ = 0;
    int settings_row_ = 0;   // resets to 0 on every tab switch
    int settings_col_ = 0;   // 0 or 1 -- only the Colors tab has 2-cell rows
    std::string color_edit_buffer_;      // live text while mode_==ColorEdit

    // --- caret / selection for the single-line text fields ----------------
    // Two BYTE offsets into whichever field is being edited right now:
    // edit_caret_ is where the next typed byte lands, edit_anchor_ is the
    // other end of the selection (equal to the caret when there is none, so
    // `caret != anchor` IS the selection test). Byte offsets, but every
    // movement is quantised to UTF-8 codepoint boundaries (see le_*() in
    // app.cpp), so a multi-byte character is never split in half.
    //
    // The pair is shared by every editor in the app, one at a time:
    // Mode::ColorEdit's buffer, the meta editor's field editor, and the
    // search/filter boxes (the main UI's "/", the meta editor's Search line,
    // the playlist editor's name/library boxes and the playlist list's
    // search). edit_owner_ tags the string the offsets were last clamped
    // against, so switching to another field -- or another row, or the 'r'
    // resort reordering the list -- retargets the caret instead of leaving
    // it pointing into a different string. edit_focus() (app.cpp) is the one
    // place that does the retarget.
    size_t edit_caret_ = 0;
    size_t edit_anchor_ = 0;
    std::string edit_owner_;
    void edit_focus(const std::string& owner, const std::string& text);
    // Returns the current value of (tab, row, col) as plain text, for
    // display and as the starting buffer when editing.
    // Returns a pointer to the color field for (row, col) on the Colors
    // tab (tab 0), or nullptr if that row/col isn't a real cell.
    std::string* color_field_ptr(int row, int col);
    std::string settings_get_value(int row, int col) const;
    // Commits color_edit_buffer_ into (settings_tab_, settings_row_,
    // settings_col_). Colors are clamped/validated as a 0-255 code;
    // everything else is stored close to verbatim.
    void settings_commit_edit();
    // Left/Right quick-cycle for rows with a fixed set of options (bools,
    // enums). No-op for rows that don't have one (colors, hotkeys) --
    // those are Enter-to-type only.
    void settings_cycle(int dir);
    std::vector<std::string> settings_options_for(int tab, int row) const;
    int settings_max_row() const; // last valid row index for the current tab
    // (main_frame_height() was removed -- see the comment where it used to
    // live in app.cpp, right before build_settings_screen(). Every overlay
    // now sizes off term_rows_ directly instead.)
    void build_settings_screen(std::ostringstream& frame, int W, int player_h) const;
    void handle_settings_key(int key);

    // --- ON/OFF tab layout model (tab 1) --------------------------------
    // The ON/OFF tab is not a flat list of toggles: underneath them it
    // also holds two editable path lists, each introduced by a section
    // header and each ending in a "+ new path" row. Headers are painted
    // but never selectable, so this struct maps the flat selectable index
    // the arrow keys walk (settings_row_) onto the display row actually
    // drawn on screen -- the same disp/scroll relationship the Reference
    // tab's ref_display_row() provides for its own headers. Both the
    // renderer and the ColorEdit cursor placement build this once per
    // frame so they can never disagree on where a row landed, and the
    // whole thing scrolls (viewport centered on settings_row_) when the
    // toggle + path rows no longer fit the terminal.
    struct OnOffRow {
        enum class Kind { Toggle, Path, AddPath, Header };
        Kind kind = Kind::Toggle;
        int sel = -1;              // selectable index, -1 for headers (unselectable)
        int path_index = -1;       // Path: index inside the owning vector
        bool playlist_path = false; // Path/AddPath: true = playlist paths, false = local music paths
        // Path: the single DOWNLOAD FOLDER field. It reads/writes
        // settings_.download_folder instead of either vector, hence the
        // path_index = -1 that never reaches them -- every consumer
        // switches on this flag first.
        bool download_folder = false;
        const char* label = "";    // Toggle: field label; Header: section title; AddPath: "+ new path"
    };
    // Every display row of the ON/OFF tab, in paint order, with `sel`
    // assigned over the selectable ones. Always at least one path row per
    // list even while the underlying vector is empty (an unset path is
    // shown as an empty field rather than as no field at all).
    std::vector<OnOffRow> build_onoff_rows() const;
    // Display row backing the given selectable row (or -1 if it has none
    // -- only possible for an out-of-range argument).
    int onoff_display_row(int selectable_row) const;
    // The row itself; its `sel` field is -1 when there is no such
    // selectable row, which is how callers detect an out-of-range index.
    OnOffRow onoff_row(int selectable_row) const;
    // True when the given selectable row edits free text -- one of the two
    // path lists -- since those need far more characters than a color/hotkey
    // field does. Drives the edit-buffer length limit.
    bool onoff_row_is_path(int selectable_row) const;

    // Title shown for `path` in the (search-)lists. Honours
    // settings_.meta_only: metadata-only mode substitutes the embedded
    // title tag for the filename stem as soon as one has been resolved
    // for that file, and falls back to the filename when there is no tag
    // (or none resolved yet), so an untagged library still renders rows.
    std::string list_row_title(const fs::path& path, const std::string& filename_title) const;
    // Playlist LIBRARY/TRACKS panels have no separate Artist/Duration
    // columns and no "Show meta data only" toggle to pick one representation
    // over the other, so unlike list_row_title() above this always shows
    // both: the filename, plus the embedded title tag appended after it
    // once/if that tag has been resolved and actually differs from the
    // filename. Untagged or not-yet-resolved files just show the filename,
    // same as before.
    std::string playlist_row_label(const fs::path& path, const std::string& filename_title) const;
    // Shared marquee-scroll math used by the LOCAL AUDIO FILES pane and the
    // playlist LIBRARY/TRACKS panels: scrolls `text` within `width` columns
    // once it no longer fits, restarting from the beginning whenever
    // `row_idx` (the row currently being drawn) differs from whatever
    // `tracked_idx` last recorded -- so switching the hovered row always
    // resumes the animation from the start rather than mid-scroll. Returns
    // `text` truncated/padded to `width` unmodified when it already fits.
    std::string marquee_or_truncate(const std::string& text, int width, int row_idx,
                                     int& tracked_idx, std::chrono::steady_clock::time_point& since) const;

    // Re-runs LocalSource::scan() over the current
    // settings_.local_music_paths and rebuilds local_view_ -- called when
    // a path is edited in the ON/OFF tab, so a path change takes effect
    // immediately instead of only on the next launch (config.txt's own
    // comment used to say "there's no live rescan"; there is now).
    void rescan_library();

    // Max row count per tab (set in build_settings_screen)


    // --- hotkey support ---
    // Resolves a key code from poll_key() to the hotkey action name.
    // Returns empty string if no match.
    std::string resolve_hotkey_action(int key) const;
    // Returns the key code that a hotkey string maps to for poll_key().
    static int hotkey_string_to_key(const std::string& s);

    // --- helpers ---
    void refresh_local_view();
    void update_live_search_preview();
    std::vector<LocalTrack> filter_and_rank_local(const std::string& query) const;
    void apply_local_sort(std::vector<LocalTrack>& tracks) const;
    std::vector<LocalTrack> filter_and_rank_local_view(const std::string& query) const; // + folder filter
    void resort_local_view_keep_selection(); // after Shift+N: re-sort, cursor stays on the same track
    static const char* sort_mode_name(int mode);
    void submit_search();
    void start_local_track(const LocalTrack& track);
    void start_online_track(const OnlineResult& result);
    void play_selected();
    void play_relative(int delta);
    void play_relative_random();
    void advance_track();
    // Where the currently-playing track sits within *this list source's*
    // current view (local_view_ or online_view_, whichever list_source_
    // is showing), by identity match (path for local, video_id for
    // online) rather than by whatever row happens to be hovered. -1 if
    // nothing's playing, or what's playing came from a different source
    // than the one currently displayed (e.g. playing local while
    // browsing online results) -- there's no meaningful "relative to
    // current" position in that case. This is what play_relative() uses
    // instead of the hover cursor, so "next" always means "next after
    // what's actually playing", not "next after wherever you happen to
    // be looking".
    int current_track_list_index() const;
    // Pops (or, in Repeat Queue mode, rotates to the back instead of
    // discarding) the next item to play from queue_, honoring the
    // current play_mode: Shuffle picks a random queue item rather than
    // strictly FIFO order, Repeat Queue keeps the queue looping
    // indefinitely instead of draining it. Shared by advance_track()
    // (auto-advance on finish) and the manual "n" key (explicit skip),
    // so both respect the queue exactly the same way. Caller must check
    // !queue_.empty() first.
    void play_next_from_queue();
    // Single letter for the mode-indicator button after the search bar:
    // L=list, R=repeat, S=shuffle, Q=repeat queue, O=stop (play-and-stop
    // -- not "S", that's shuffle's letter already).
    char play_mode_letter() const;
    void queue_add_selected();
    void queue_remove_last();
    void queue_remove_hovering();
    // Shift+X (HKeyClearQueue): asks "Want to clear queue?" (Mode::ClearQueue,
    // a floating Yes/No panel like Bulk Add / Retry Lyrics) before queue_clear()
    // actually empties queue_. The default choice is No.
    void queue_clear();
    int clear_queue_choice_ = 1;       // 0 = Yes, 1 = No
    static constexpr int kClearQueuePanelWidth = 40;
    std::vector<std::string> build_clear_queue_panel() const;
    void queue_move_hovering(int dir); // dir=-1 up, +1 down
    void clamp_queue_selected();
    void handle_key(int key);
    void ensure_visible_row_meta();
    void recompute_waveform_for_current_track();
    std::string render_frame(TerminalIO& term);

    // --- box drawing helpers (use configured border chars) ---
    std::string box_top(const std::string& label, int total_width, const std::string& border_ansi = "") const;
    std::string box_bottom(int total_width, const std::string& footer = "", const std::string& border_ansi = "") const;
    std::string box_line(const std::string& content, int total_width, const std::string& border_ansi = "") const;
    // box_top()/box_line() for a row whose TAIL is a text field with a caret
    // and a marked range: the field arrives already painted (it carries
    // reverse-video escapes), so these two pad/measure against `field_cols`
    // instead of running the rendered string through display_width(), which
    // counts escape bytes as columns. `prefix` stays plain text.
    std::string box_top_field(const std::string& prefix, const std::string& field, int field_cols,
                              int total_width, const std::string& border_ansi = "") const;
    std::string box_line_field(const std::string& prefix, const std::string& field, int field_cols,
                               int total_width, const std::string& border_ansi = "") const;

    // panel builders
    std::vector<std::string> build_metadata_panel(int width) const;
    std::vector<std::string> build_progress_panel(int width) const;
    std::vector<std::string> build_search_bar(int width) const;
    std::vector<std::string> build_list_panel(int width, int height) const;
    std::vector<std::string> build_queue_panel(int width, int height) const;
};

} // namespace muisc
