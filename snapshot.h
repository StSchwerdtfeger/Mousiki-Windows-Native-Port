#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// One entry -- either the currently-playing track or a queued one.
// Mirrors just enough of QueueItem/LocalTrack/OnlineResult to be able to
// re-resolve the exact same track next launch (local path, or online
// video id) without depending on app.h from this header.
struct SnapshotTrack {
    bool is_local = true;
    std::string path;      // absolute path, valid if is_local
    std::string video_id;  // valid if !is_local
    std::string title;
    std::string artist;
};

// Deliberately does NOT include anything about appearance/colors --
// only the "basic" things: what's playing, exactly where, the queue,
// and the playback-mode toggles (repeat/shuffle/volume/mute). Matches
// the requirement that color/theme settings are never snapshotted.
struct SnapshotData {
    bool has_now_playing = false;
    SnapshotTrack now_playing;
    double position_sec = 0.0;

    int play_mode = 0;      // 0=list 1=loop 2=shuffle 3=stop
    bool muted = false;
    int volume = 70;         // the pre-mute/real volume, not the forced-0 muted value

    std::vector<SnapshotTrack> queue;
};

fs::path snapshot_path();

// Reads snapshot.json if present and parses cleanly; returns false (and
// leaves `out` untouched) if the file doesn't exist or is corrupt --
// callers should just skip restoring in that case, never crash/throw.
bool load_snapshot(SnapshotData& out);

// Overwrites snapshot.json with `data` (single canonical file --
// autosave and the on-exit save both just call this, replacing whatever
// was there before rather than accumulating history).
void save_snapshot(const SnapshotData& data);

// Removes snapshot.json. Called right after a successful restore at
// startup, so a snapshot is "consumed" exactly once -- if the app
// crashes immediately after resuming without ever autosaving again,
// the next launch starts fresh instead of silently reusing a stale
// snapshot indefinitely.
void delete_snapshot();

} // namespace muisc
