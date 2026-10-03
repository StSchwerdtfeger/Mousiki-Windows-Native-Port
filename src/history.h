#pragma once
#include <map>
#include <string>
#include <vector>

namespace muisc {

// ---------------------------------------------------------------------------
// Listening history (Mode::History, HKeyHistory = Shift+H)
//
// One record per start of a track. The store keeps them newest-first and
// persists them to ~/.cache/mousiki/history/history.json so the numbers are
// cumulative across runs -- the History tab only ever shows the newest 100,
// but "played today", "total listened" and the play counts behind Top Tracks
// / Habits are meant to mean something a month from now.
//
// Everything about a play is decided at the moment it ENDS (finish_live()),
// because that is the first point where any of it is knowable: how much was
// actually heard, and whether it was heard to the end or skipped over.
// ---------------------------------------------------------------------------

struct HistoryPlay {
    std::string id;             // aggregation key: local path, or "yt:<video id>" for streams
    std::string title;
    std::string artist;
    double len_sec = 0.0;       // length of the track itself
    double listened_sec = 0.0;  // seconds of audio actually heard (pause time never counts)
    long long started_at = 0;   // unix seconds, local clock, when this play began
    bool finished = false;      // heard to the (near) end -- see finish_live()
};

// One aggregated title on the "Top Tracks" tab.
struct HistoryTopRow {
    std::string id;
    std::string title;
    double len_sec = 0.0;
    int plays = 0;
    double listened_sec = 0.0;
    std::string artist;         // newest known artist of this title (for "add top tracks to queue")
};

// Everything the "Habits" tab shows, derived from the stored plays.
struct HistoryStats {
    int plays = 0;
    int finished = 0;
    int skipped = 0;            // plays that did NOT reach the end (incl. the app closing mid-track)
    int replays = 0;            // every play of a title beyond its first one
    double completion = 0.0;    // finished / plays, 0..1
    int sessions = 0;           // runs of plays split by a >30 min silence
    double avg_session_sec = 0.0;    // wall-clock span of one sitting
    double tracks_per_session = 0.0;
    double today_sec = 0.0;     // listened this calendar day
    double avg_day_sec = 0.0;   // listened per day that had any music at all
    double total_sec = 0.0;
    int days = 0;               // distinct calendar days with music
};

// ---------------------------------------------------------------------------
// Lifetime totals. The store only keeps the newest kMaxPlays individual
// records (that is what keeps history.json small -- it is rewritten after every
// track), but nothing counted may ever get lost with them: a record that falls
// out of the window is FOLDED into this archive first. The archive grows with
// the number of distinct titles and distinct days -- not with the number of
// plays -- so a few hundred KB cover years of listening.
// ---------------------------------------------------------------------------
struct HistoryArchiveTitle {
    std::string title, artist;
    double len_sec = 0.0;
    int plays = 0;
    double listened_sec = 0.0;
};

struct HistoryArchive {
    long long plays = 0;        // every folded play ...
    long long finished = 0;     // ... and how many of them were heard to the end
    double listened_sec = 0.0;
    std::map<std::string, HistoryArchiveTitle> titles;  // per title: play count + time (Top Tracks, Replays)
    std::map<std::string, double> per_day;              // "YYYY-MM-DD" -> seconds (Days with music, averages)
    // Sessions, kept exactly: closed ones as count + summed length, plus the
    // one still "open" at the oldest end of the live window, which the window's
    // oldest plays may still continue (see history_stats()).
    int sessions_closed = 0;
    double session_sum_sec = 0.0;
    bool session_open = false;
    long long sess_start = 0, sess_end = 0;

    // Adds one record. Must be called oldest -> newest.
    void fold(const HistoryPlay& p);
    bool empty() const { return plays == 0; }
};

class HistoryStore {
public:
    // Reads history.json if it exists. A missing or corrupt file is not an
    // error: history is a convenience, never something that should stop the
    // app from starting.
    void load();
    // Written after every finished play and again at shutdown.
    void save() const;

    // Starts a new record at the front of the list (newest-first) and marks
    // it as the live one; the previously live record, if any, is left as it
    // is -- callers end it first.
    void begin_play(const HistoryPlay& p);
    // Adds elapsed seconds to the live record. A no-op when nothing is live,
    // so the main loop can call it unconditionally.
    void add_listened(double sec);
    // Closes the live record: `completed` is the caller's judgement of "heard
    // to the end" (the player's finished flag, or >=90% of the track).
    void finish_live(bool completed);

    bool live() const { return live_index_ >= 0; }
    double live_listened() const;
    double live_len() const;

    // Newest first. Includes the still-playing record, so the History tab
    // shows the current track too.
    const std::vector<HistoryPlay>& plays() const { return plays_; }

    // Scratch accessors for the current play's identity (used to build the
    // "completed" verdict when a track is handed over to its successor).
    const HistoryPlay* live_play() const;

    // Totals of everything that has already left the window of individual records.
    const HistoryArchive& archive() const { return archive_; }

private:
    void trim_overflow();             // folds the oldest records into archive_ while over the cap
    HistoryArchive archive_;
    std::vector<HistoryPlay> plays_;  // newest first, capped
    int live_index_ = -1;             // index of the in-progress record, -1 when none
};

// Aggregation. `most_first` is the Top Tracks default (play count descending);
// false is the `r` key's "least played on top" alternative.
// Both take the store's archive so the numbers are lifetime totals, not just
// "what is still in the window of individual records".
std::vector<HistoryTopRow> history_top(const std::vector<HistoryPlay>& plays, bool most_first,
                                       const HistoryArchive* archive = nullptr);
HistoryStats history_stats(const std::vector<HistoryPlay>& plays, const HistoryArchive* archive = nullptr);

// The key a play is recorded and aggregated under: a local file's path, or
// "yt:<video id>" for a stream (so the same track reached two ways counts
// once). Empty means "nothing to aggregate by" and such plays are skipped by
// history_top().
std::string history_track_id(bool is_local, const std::string& path, const std::string& video_id);

// Formatting shared by the UI: "3:42" / "1:02:03", "42m" / "1h 12m", "09-27 14:03".
std::string format_mmss(double sec);
std::string format_len(double sec);
std::string format_when(long long unix_sec);

} // namespace muisc
