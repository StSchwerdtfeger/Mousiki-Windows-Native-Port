#pragma once
#include <filesystem>
#include <string>

namespace muisc {

namespace fs = std::filesystem;

class CacheManager {
public:
    CacheManager();

    // $HOME/.cache/muisc  (created if missing)
    const fs::path& cache_dir() const { return cache_dir_; }

    // Where yt-dlp writes: the configured download folder, or -- when none
    // is set -- the cache folder itself. Kept separate from cache_dir_ so
    // anything else living under ~/.cache/mousiki stays where it always was,
    // and so a folder set later still finds the downloads already in the
    // cache (path_for() falls back to it).
    const fs::path& download_dir() const { return download_dir_; }
    // Empty (or exactly the cache folder) restores the default; anything
    // else replaces it, creating the folder if needed.
    void set_download_dir(const fs::path& dir);

    // Deterministic, filesystem-safe path for a given song title, e.g.
    // "Never Gonna Give You Up" -> ~/.cache/muisc/never_gonna_give_you_up.opus
    fs::path path_for(const std::string& title, const std::string& ext = "opus") const;

    bool is_cached(const std::string& title, const std::string& ext = "opus") const;

private:
    fs::path cache_dir_;
    fs::path download_dir_; // == cache_dir_ unless Settings sets a folder
    static std::string sanitize(const std::string& raw);
    // Old ASCII-only naming rule, used only to locate files cached
    // before sanitize() became UTF-8 aware.
    static std::string legacy_sanitize(const std::string& raw);
};

} // namespace muisc
