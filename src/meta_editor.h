#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// ------------------------------------------------------------------------
// Meta/tag editor support: the pending-edit session (autosaved to disk, NOT
// applied to the audio files), applying that session to the files, and the
// AcoustID lookup that fills it in. Kept out of app.cpp because all three
// are plain file/subprocess work with no UI involvement -- app.cpp only owns
// the Mode::MetaEdit screen and the keys around these functions.
// ------------------------------------------------------------------------

namespace muisc {

namespace fs = std::filesystem;

// The five editable fields, in the order the editor panel draws them.
// FileName is a file *name* edit (a rename), the other four are tags.
enum class MetaField : int { FileName = 0, Artist = 1, Title = 2, Album = 3, Year = 4 };
constexpr int kMetaFieldCount = 5;

// Session-file key for field i ("filename", "artist", ...).
const char* meta_field_name(int i);
// Panel label for field i ("FILE", "ARTIST", ...), padded by the caller.
const char* meta_field_label(int i);

// One file's pending edits. `path` is the stable identity (absolute UTF-8);
// nothing is written to the audio file until the session is applied.
struct MetaEditEntry {
    std::string path;
    // Per-field "this field was touched" flag -- set by typing a value OR by
    // an AcoustID answer, and exactly what the header-colour highlight is
    // driven by, so a fetched value is as visibly "changed by me" as a typed
    // one.
    std::array<bool, kMetaFieldCount> edited{};
    // The pending value for each field. Only meaningful where edited[i] is
    // true; untouched fields fall back to whatever the file already has.
    std::array<std::string, kMetaFieldCount> value{};

    bool any_edited() const;
};

// --- autosaved session (a backup of the edits; the files stay untouched) ---
fs::path meta_session_path();
bool load_meta_session(std::vector<MetaEditEntry>& entries, std::vector<std::string>& fetch_list);
void save_meta_session(const std::vector<MetaEditEntry>& entries,
                       const std::vector<std::string>& fetch_list);
// Removes the session file. Used when the last pending edit is applied or the
// session is explicitly discarded -- never as a side effect of just exiting.
void delete_meta_session();

// Applies ONE entry to disk: edited tags go through a `ffmpeg -c copy` remux
// (audio stream copied bit-for-bit, only container tags rewritten; written to
// a sibling temp file and renamed over the original so a failure never leaves
// a half-written file behind), then an edited filename becomes a plain
// rename. Returns false and fills *err with a human-readable reason.
// If new_path is non-null it receives the file's path AFTER a filename edit
// (empty when the file was not renamed), so callers holding references to the
// file -- the queue, the now-playing path -- can follow it to its new name.
bool apply_meta_entry(const MetaEditEntry& e, std::string* err, std::string* new_path = nullptr);

// --- AcoustID (scripts/fetch_meta.py) --------------------------------
struct MetaFetchRequest {
    std::string path;   // the file to fingerprint; echoed back in the result
    std::string title;  // only used for a "no match for: ..." message
    std::string artist; // may be empty; likewise message-only
};

struct MetaFetchResult {
    std::string path;
    bool ok = false;
    // Which of the four tag fields this answer actually provides (Year
    // included); FileName is never set -- AcoustID does not name files.
    std::array<bool, kMetaFieldCount> set{};
    std::array<std::string, kMetaFieldCount> value{};
    std::string error;
};

struct MetaFetchOutcome {
    int ok_count = 0;
    int fail_count = 0;
    bool python_missing = false;  // no Python interpreter -> cannot run the helper
    bool script_missing = false;  // scripts/fetch_meta.py not found next to the exe
    std::string error;            // first fatal/uncategorised failure, for the status line
};

// Runs scripts/fetch_meta.py once for the WHOLE batch: one Python process
// fingerprints each file (fpcalc/Chromaprint) and paces itself to AcoustID's
// 3-requests-per-second limit, printing one JSON object per line as each
// lookup lands, so on_result fires incrementally (that is what keeps progress
// visible during a long fetch). The application key every request is signed
// with is hard-coded in the script itself (API_KEY): an AcoustID key belongs
// to a registered application, not to a user, so there is nothing to
// configure here. Blocking -- callers run this on their own worker thread.
MetaFetchOutcome run_acoustid_fetch(const std::vector<MetaFetchRequest>& reqs,
                                    const fs::path& script,
                                    const std::function<void(const MetaFetchResult&)>& on_result);

} // namespace muisc
