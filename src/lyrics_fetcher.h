#pragma once
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace muisc {

namespace fs = std::filesystem;

// One LRC line, optionally with per-word timestamps (enhanced/A2 LRC),
// which is what makes word-level highlighting possible during playback.
struct LyricLine {
    double start_time = 0.0;
    std::string full_text;
    std::vector<std::pair<double, std::string>> words; // empty if not word-synced
};

enum class LyricsStatus {
    Ok,
    ModuleMissing,   // Python 'requests' package not installed -> show pip-install hint
    PythonMissing,   // python3 not found on PATH
    NotFound,        // ran fine, no lyrics available for this track
    Error,
};

struct LyricsResult {
    LyricsStatus status = LyricsStatus::Error;
    std::vector<LyricLine> lines;
    std::string message;   // human-readable status/error, shown in the lyrics panel
    std::string source;    // "local" | "better-lyrics" | "lrclib" | ""
    std::string raw_lrc;   // the raw LRC text, kept so it can be cached to a sidecar file
    // Timing correction in seconds, as the lyrics editor (ALT+L) shows it:
    // POSITIVE = the lyrics appear LATER than their timestamps say, negative =
    // earlier. Read from the sidecar's "[offset:...]" tag (which uses the LRC
    // convention, in milliseconds, positive = earlier -- the sign is flipped on
    // the way in and out), 0 when there is none.
    double delay = 0.0;
};

// Priority chain: a local sidecar .lrc file in a "lyrics" subfolder of the
// track's own folder (checked first, no subprocess spawned at all; a sidecar
// left next to the track by an older build is still read as a fallback) -> the Python helper script
// (scripts/fetch_lyrics.py), which tries Better Lyrics first (word-level
// TTML, converted to enhanced LRC) and falls back to LRCLIB (line-synced
// only) if Better Lyrics has nothing -- see scripts/lrc.py. Whatever
// comes back from a network fetch is written back to the sidecar file,
// so the next time this track plays (even offline) it's a local-file
// hit.
LyricsResult fetch_synced_lyrics(const std::string& title, const std::string& artist,
                                  const std::string& helper_script_path,
                                  const fs::path& track_path = fs::path(),
                                  bool force_network = false);

// Writes `delay_sec` (same sign as LyricsResult::delay) into the track's sidecar
// .lrc as an "[offset:...]" tag, replacing an existing one (0 removes the tag).
// The timestamps themselves are not touched. False when there is no sidecar to
// write into (nothing fetched/cached for this track) or the write failed.
bool save_lyrics_delay(const fs::path& track_path, double delay_sec);

} // namespace muisc
