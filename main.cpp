#include <clocale>
#include <iostream>
#include <string>
#include "app.h"

#if defined(_WIN32)
#include "win_compat.h"
#endif

int main() {
#if defined(_WIN32)
    // Order matters. Both of these have to happen before App's constructor
    // runs: it loads settings, which resolves $HOME, and it may log, which
    // writes UTF-8 through the console.
    //
    //  - win_bootstrap_env() points HOME at %USERPROFILE%, so the seven
    //    getenv("HOME") call sites across settings/cache/snapshot/log all
    //    land in C:\Users\you\ instead of silently falling back to "."
    //    and littering the launch directory with dot-folders.
    //  - win_console_init() switches the console to UTF-8 and turns on VT
    //    escape processing, without which the entire UI renders as literal
    //    escape-sequence text.
    muisc::win_bootstrap_env();
    if (!muisc::win_console_init()) {
        std::cerr << "mousiki: this console does not support ANSI escape sequences.\n"
                     "Run it in Windows Terminal (wt.exe) rather than the legacy\n"
                     "conhost window, or enable \"Use the new console host\" in\n"
                     "the properties of the window you are using.\n";
        return 1;
    }
#endif

    if (!std::setlocale(LC_ALL, "")) {
        std::setlocale(LC_ALL, "C.UTF-8");
    } else {
        const char* cur = std::setlocale(LC_CTYPE, nullptr);
        if (cur && std::string(cur) == "C") {
            std::setlocale(LC_ALL, "C.UTF-8");
        }
    }
    // Every std::stod() call in this codebase -- settings parsing
    // (app.cpp's settings-panel input, settings.cpp's config.txt loader),
    // ffprobe/JSON duration parsing (metadata_probe.cpp, online_source.cpp),
    // and LRC lyric timestamps (lyrics_fetcher.cpp) -- assumes a period
    // decimal separator, because that's what writes it: save_settings()
    // uses a plain ostream `<<`, which is locale-INDEPENDENT and always
    // writes a period, and ffprobe/most CLI tools/APIs emit numbers in a
    // fixed, non-localized format regardless of OS region. std::stod()
    // itself, though, goes through the C library's LC_NUMERIC category --
    // which the setlocale(LC_ALL, "") call above just set to the OS's
    // regional format. On a machine set to German (or French, or any other
    // comma-decimal locale), "0.5" typed into a settings field, or
    // "215.34" read back from ffprobe, gets silently misparsed as "0" /
    // "215" at the first '.', with no exception thrown -- a partial parse
    // still "succeeds" as far as std::stod is concerned. Pinning
    // LC_NUMERIC back to "C" here keeps LC_CTYPE (and whatever else the
    // block above cares about) on the OS locale while making every number
    // this app touches parse the same way on every machine.
    std::setlocale(LC_NUMERIC, "C");

    int rc;
    try {
        muisc::App app;
        rc = app.run();
    } catch (const std::exception& e) {
        // Belt-and-braces on top of settings.cpp's own try/catch around
        // config.txt parsing: this catches anything else startup could
        // throw (a bad path, a filesystem error, whatever else), so a
        // hand-edited config -- or any other environment-specific issue --
        // prints a reason and exits cleanly instead of disappearing with
        // no output, which is what a plain uncaught exception does on
        // Windows (it fails fast with no console message at all).
        std::cerr << "mousiki: " << e.what() << "\n";
        rc = 1;
    } catch (...) {
        std::cerr << "mousiki: startup failed for an unrecognized reason.\n";
        rc = 1;
    }

#if defined(_WIN32)
    // App's TerminalIO destructor already left the alternate screen; this
    // puts the code pages and console modes back the way we found them.
    muisc::win_console_restore();
#endif
    return rc;
}
