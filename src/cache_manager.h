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

    // Deterministic, filesystem-safe path for a given song title, e.g.
    // "Never Gonna Give You Up" -> ~/.cache/muisc/never_gonna_give_you_up.opus
    fs::path path_for(const std::string& title, const std::string& ext = "opus") const;

    bool is_cached(const std::string& title, const std::string& ext = "opus") const;

private:
    fs::path cache_dir_;
    static std::string sanitize(const std::string& raw);
    // Old ASCII-only naming rule, used only to locate files cached
    // before sanitize() became UTF-8 aware.
    static std::string legacy_sanitize(const std::string& raw);
};

} // namespace muisc
