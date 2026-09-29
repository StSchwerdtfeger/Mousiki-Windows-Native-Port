<div align="center">
    
# Mousiki Windows Native Port v2.1.0 🎵 

</p>

<p align="center">
  <a href="https://opensource.org/" target="_blank">
    <img src="https://i0.wp.com/opensource.org/wp-content/uploads/2023/03/cropped-OSI-horizontal-large.png?fit=640%2C229&quality=80&ssl=1" alt="OSI" height="52" /></a>
&nbsp;
  <a href="https://www.apache.org/" target="_blank">
    <img src="https://www.apache.org/images/oakleaf.svg" alt="Apache" height="52" /></a>
</p>


> [!NOTE]
> **Developer note:** Mousiki is released under the Apache License 2.0.
> You are free to use, modify, fork, re-distribute, and sell the software,
> subject to the terms of the license.

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://github.com/itzender5820/mousiki/blob/main/LICENSE)
[![Language](https://img.shields.io/badge/Language-C++17-orange.svg)]([https://github.com/itzender5820/mousiki](https://github.com/StSchwerdtfeger/Mousiki-Windows-Native-Port/tree/main))
[![Windows](https://img.shields.io/badge/Windows-Supported-0078D4.svg?logo=windows&logoColor=white)](https://github.com/StSchwerdtfeger/Mousiki-Windows-Native-Port/tree/main)

</div>

A native Windows port (including a bunch of modifications and additions; design maintained) of the amazing [itzender5820/mousiki](https://github.com/itzender5820/mousiki) — a terminal music player for macOS/Linux built for people who prefer control, simplicity, and a keyboard. Note about the name: it is the greek word for music and is pronounced mousi-**key**! ;) All credits for the design, main feature set, and the vast majority of the code goes to the original author. Feel free to give feedback in the discussions and report issues you might experience using this modified port.

Build yourself (see [prerequesites](+prerequesite) below) **or use installer(x64)** that is entailed in the latest release (since v2.1.0). 

This fork exists because the original targets POSIX (Linux/macOS/Termux) and has no Windows build path at all. This native port uses no WSL, no MSYS runtime, no POSIX emulation layer, just a plain `mousiki.exe` built against the Win32 API and WASAPI. Porting it surfaced a long list of platform differences beyond the obvious ones (see [What had to change](#what-had-to-change), below), plus a small number of pre-existing bugs in the original codebase that had nothing to do with Windows and got fixed along the way.

Along the Win32 port, **some minor and major additions where made too**. Major changes/additions are a most of all a **menu to create playlists from local (or downloaded) tracks**, a **meta data editor menu, including fetching meta data from AcoustID**, a **listening histoy** incl. the ability to add top tracks to the playback queue and a **user's manual**, added a **user manual** (.md and .pdf version)... Minor changes/additions are e.g. a general key to shuffle to a next title (before only next title in the list was possible), stereo audio and loudness normalization, add path via settings menu, toggle the lyrics on/off (also via a key command), optimized search engine for windows (searching metadata was very slow, only available after 2-3 min. after starting app), added fuzzy search (e.g. "X-Files" didn't show up when searching "X Files" without dash) a cheat sheet... The desing remained the same for obvious reasons; only thing added is a Braille-ASCII of a music cassette shown when no track is loaded... See section [added features beyond the port](#added-features-beyond-the-port) for a full detailed list.  

- **Original:** [github.com/itzender5820/mousiki](https://github.com/itzender5820/mousiki) — ender ([itzender5820](https://github.com/itzender5820))
- **License:** Apache 2.0 — see [LICENSE](LICENSE)
- **Windows port:** Steffen Schwerdtfeger ([StSchwerdtfeger](https://github.com/StSchwerdtfeger)), ported and adjusted with the help of AI tools (only free versions, mostly MiMo V2.6 and Sonett 5 set on medium). Therefore take some of the below with a grain of salt (explicitly the documentation of the Win native port and some assumed bugs of the original, which I can't confirm myself), since I am not a developer for applications like this and I do not fully understand how the porting was actually done. Still took me around 50h in the last weeks to perform porting to Win, modify (feature design) and debug this version... The repo code could also be optimized in that respect, but apparently is supposed to be done quite well (from feedback I got so far and evaluated myself, as far as I am capable to do so). Even though I am not that big fan of using AI for scientific applications (which I usually do), e.g. in the context of data science - since someone has to understand how sh** works and understanding is beautiful and mind blowing - I still liked this music player way too much the first time I saw it on social media to not want to use it on my Windows setup... Sooooo, I went this path and vibe coded a lot to create a port for Windows and still learned a lot as well (especially on UI and feature design). *In general, a huge shout out for the great work by itzender5820 for this beautiful music player.* <3 It's the best and most fun music player I ever found. Makes me want to listen to music all the time :D 

![preview](preview.gif)

My current setup looks like the below. The config.txt and everything that comes along with it (FastFetch and Oh-My-Posh configs) can be found in my cyber-cat themed [MeowerShell repository](https://github.com/StSchwerdtfeger/Meower-Shell):

<img width="779" height="392" alt="grafik" src="https://github.com/user-attachments/assets/a16c6728-37e1-4124-86a4-591677656f00" />

## Current Status of the Port and Modification (v2.1.0, now with standalone setup.exe, see latest release)

For now the Mousiki port works well and also includes everything I at least wanted and made sense to me for a music player, so there might be no further major releases that add new features, except of bug-fixes that might appear to me or others in the future (feel free to start discussions or report issues!!). I will adjust the code though to be more polished and might optimize the installer release (currently ~250MB size, installer itself ~80MB) ... I might even "re-port" my version of Mousiki back to macOS/Linux to make it integratable into the main branch (which currently seems way to hard after dozens of comments in the last two weeks, at least from my perspective). However, this wont happen until I am certain there is nothing that I want or should change in this version. 
Concerning new feature, further below you'll find a list of [current Ideas on features and modifications](#Current-Ideas-on-Features-and-Modifications). An online-radio function as well as a mixtape creater would be cool, but I'll see. Again, feel free to give feedback in the discussions and report issues you might experience using this modified port.

## Quick start (port was tested with PowerShell 7.6.6 and 5.1)

Installer (x64) is entailed in the latest release (since v2.1.0) or build yourself via:

```powershell
# from the repo root
.\setup.ps1
```
This temporarily disables script blocking and warning prompts for the currently active PowerShell session only,
in case the above does not compute in your PowerShell.

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\setup.ps1
```

You can build the .exe via the following, however, the setup.ps1 already performs building the file
(so the below is just in case something didn't work computing setup.ps1 and you want to build after debugging):

```powershell
.\build\Release\mousiki.exe
```

Run app e.g. via the commad below (adjust username in the path before executing!!).
Note that you have to add your local files path via the config.txt file in "C:\Users\YOURNAME\.config" (more details further below).
See original repo by itzender5820 for an introduction on how to use Mousiki.
```powershell
& 'C:\Users\YOURNAME\mousiki\build\Release\mousiki.exe'
```
Personally, I recommend writing a function in your PowerShell profile.ps1 in order to be able to run the app via a command (in my case I set the command to be "lala"):
To do so, open your profile file via:

```powershell
notepad $PROFILE
```

Then add the following (as said, I called the function lala but you can choose whatever you want; there is certainly a bunch of already existing commands, such as e.g. python, but you should be safe for most of the cases). 
Again, add your Username in the path!

```powershell
function lala {
    & 'C:\Users\YOURNAME\mousiki\build\Release\mousiki.exe'
}
```
Save your profile.ps1 via Ctrl + S and open a new terminal in order to be able to test your new function.
Voilá, you can now open Mousiki from any folder you're at using the command "lala", or whatever you set as command respectively...

## Prerequisites

Note, I had a bunch of the below already installed, so I am not sure how smooth setup.ps1 runs installing the below for the first time using setup.ps1 (such as installing Visual Studio 2022 Build Tools...).

`setup.ps1` installs these via winget in PowerShell 7, except the compiler "VS 2022 Build tools":

| Tool | Why | Install |
|---|---|---|
| Visual Studio 2022 Build Tools, "Desktop development with C++" | compiles the app | must be selected interactively — winget's silent mode won't pick the C++ workload |
| CMake ≥ 3.16 | build system | `winget install Kitware.CMake` |
| FFmpeg | decodes Opus (miniaudio can't), `ffprobe` supplies metadata | `winget install Gyan.FFmpeg` |
| yt-dlp | online search fallback, playlists, streaming | `winget install yt-dlp.yt-dlp` |
| Python 3 + `requests` | lyrics and fast online search (`scripts/`) | `winget install Python.Python.3.12` then `py -3 -m pip install requests` |

All of these are independent of each other and of the core player. With none of them installed, local playback still works. 

MinGW-w64 (MSYS2 UCRT64) also builds this — configure with `-G "MinGW Makefiles"`. The code guards on `_WIN32`, not on `_MSC_VER`, except where MSVC genuinely differs (noted inline where it matters).

`third_party/` (miniaudio v0.11.25, kissfft, chromaprint for AcoustID meta data fetch) is vendored in this repo, so configuring and building needs no internet connection — `CMakeLists.txt` no longer downloads anything, it just stops with a clear error if either is missing. miniaudio is public domain / MIT-0, kissfft is BSD-3-Clause (see the headers in `third_party/`). To update either, replace the files in `third_party/` with a newer upstream copy (not tested if it is that ease now that chromaprint is also included).

## Use Windows Terminal

The entire UI is ANSI escape sequences. `mousiki.exe` enables `ENABLE_VIRTUAL_TERMINAL_PROCESSING` at startup and exits with a clear message if that fails, rather than rendering garbage. Windows Terminal (`wt.exe`) works; the legacy conhost window on pre-1511 Windows builds does not.

## Where your files go

`$HOME` doesn't exist on Windows, and the original codebase looks it up in seven different places to find its directories. Rather than rewrite all seven call sites to be platform-aware, this port points `HOME` at `%USERPROFILE%` for its own process at startup, so every one of those paths resolves exactly the way it does on Linux/macOS:

| | Path |
|---|---|
| Config | `%USERPROFILE%\.config\mousiki\config.txt` |
| Cache (downloaded/streamed tracks) | `%USERPROFILE%\.cache\mousiki\` |
| Log | `%USERPROFILE%\.cache\mousiki\logs\console.log` |
| Session snapshot | `%USERPROFILE%\.cache\mousiki\snapshot\snapshot.json` |

`LocalMusicPath=`,  `Playlistpath=` and `DownloadFolder`= entries accept Windows paths, both slash directions should work (`std::filesystem` normalizes them):

```
LocalMusicPath=C:\Users\you\Music
LocalMusicPath=~/Music
```
Playlists folder (optional -- overrides the `LocalMusicPath[0]/playlists` default). Default is located in `C:\Users\YOUR NAME\.cache\mousiki\playlists` 
You can now add folders in the settings (`s`). Same goes for the Download folder...

```
PlaylistsPath=C:\Users\YOUR NAME !!!!!!!\Music\playlists
```

## Default Keybindings

Configurable in `C:\Users\USER\.config\mousiki\config.txt` or in the settings. Some commands are hard coded.
See the new **user manual** in the repo above or use `?` for the cheat sheet with all commands inside the app.

### Key Commands Overview
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Cheat Sheet** | `?` | List of key commands (on German keyboards its `SHIFT + ß` == `?` |

### Search & Playback
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Local Search** | `/` | Filter and search local library 
| **Online Stream Search** | `/s: <query>` | Search and stream music online |
| **Search Playlists** | `/p: <query>` | Search and stream local playlists |
| **Open Playlist** | `SHIFT + p` | Open Playlist Creator/Editor |
| **Open Meta Data** | `SHIFT + m` | Open Meta Data Editor |
| **Download Stream** | `y` | Download currently streaming track to default or set path |
| **Play / Pause** | `p` | Toggle playback |
| **Play / reload selected track** | `Enter` | Play or reload selected track |
| **Next / Previous Track** | `n` / `b` | Skip between songs |
| **Shuffle Next** | `#` | Shuffle to a next song |
| **Filename / Meta Data** | `SHIFT + n` | Toggle to show meta data only in lists |
| **Seek** | `ARROW_LEFT` / `ARROW_RIGHT` | Seek backward / forward |
| **Volume** | `1` / `2` |  Increase / Decrease in-app volume |
| **Normalize** | `v` | Toggle loudness normalization, edit in settings' reference tab |
| **Change Play-/Cyclemode** | `m` | Toggle Playmode (shuffle, stop, repeat queue, loop, list) |
| **Toggle Lyrics** | `+` | Turn Lyrics on/off |

### Navigation & Queue
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Setting** |`s` |Enter Settings menu|
| **Exit Setting** |`ESC` |Exit Settings menu|
| **Quit** | `q` | Quit (when in main UI page) |
| **Exit + Save Setting** |`s` |Exit + Save again via `s`|   
| **Console / Logs** | `t` | Show console logs for debugging |
| **Playlists** | `SHIFT+P` |Enter Playlist menu |
| **Navigate** | `ARROW_UP` / `ARROW_DOWN` | Move selection |
| **Cycle Sort Mode** | `SHIFT + t` | title/artist a-z, folder order |
| **Switch Tabs/Cards** | `TAB` | Cycle between UI panels |
| **Add to Queue** | `a` | Enqueue selected track |
| **Remove from Queue** | `d` | Dequeue selected track |
| **Move Track Up** | `4` | Move up in Queue/Playlist |
| **Move Track down** | `5` | Move down in Queue/Playlist |
| **Clear Queue** | `SHIFT+x` | Clear Queue | 
| **Filter by Folder** | `f` | Apply folder filter |
| **Clear Filter** | `c` | Reset active search/filters |
| **Quit** | `q` | Exit application |

</div>

Every editable/rebindable hotkey can also be found in the config.txt and — for an overview — in the in-app cheat sheet overview (`?`). Not every key was set to be rebindable. Rebinding can also be done in the config.txt file and  under Settings → Reference in the app itself.

Changing Fonts: In the config.txt you will also see liens like A = A, a. This is used so you can re-font the UI without changing the font of the terminal, e.g. via A={𝓐,𝓪}.... 

## Playlist and Meta Data UI Behavior

The playlist UI starts in the Name field, where a name for the playlist can be chosen. The pane focus can be changed via TAB and the tabs of the playlist menu can be changed via ALT+LEFT/RIGHT. This was a design compromise that I did, since it starts in the name field and SHIFT+LEFT/RIGHT is reserved for marking text input... 
In the meta data menu, the tabs can be changed via `left / right`. I guess the handling of the meta data menu needs some practice, since it is rather complex task to perform, however after fitting in it works well. To sort the list (e.g. all edited titles on top, or show only titles with no meta data, or not title or no artist) you have to set the focus on the library pane and then use `x/T/A/Y` for all missing, title missing, artist missing, year missing. Using metatogger can be a bit faster, however it is not the Mousi-**key** way of doing things! Gotta love the terminal. 

## What had to change

Around thirteen files needed direct `#ifdef _WIN32` branches; a similar number needed changes that apply on every platform but were only ever exposed by something Windows does differently (mostly the UTF-8 path handling below). Everything else compiled and ran unmodified. Due to the fast modifications, it became to hard to track what has changed compared the original version. In the future I might create a "re-port" back to macOS/Linux including the various additions I made, such that they eventually can be added to the original branch of this fork... This will only be done when I stopped adjusting and gave a while in order to get feedback from other users, did enough testing myself etc.

### Console & terminal I/O

POSIX raw mode (`termios`), key polling (`read()` off `STDIN_FILENO`), window size (`ioctl(TIOCGWINSZ)`), and even `wcwidth()` for East-Asian character width have no Windows equivalent. All of it is replaced by Win32 Console API calls behind a small platform shim (`win_compat.h`/`.cpp`, new in this fork) — `SetConsoleMode`, `_kbhit`/`_getch` (with its own extended-key scan codes translated to match the POSIX escape-sequence path, so the rest of the app never has to know which platform it's on), `GetConsoleScreenBufferInfo`, and a hardcoded East-Asian-width table for `wcwidth`, since MSVC's CRT doesn't ship one at all.

Rendering initially used `SetConsoleOutputCP(CP_UTF8)`, which turned out to be unreliable across console hosts — box-drawing and other multi-byte glyphs could still come out as mojibake depending on the terminal. `win_compat.cpp` now converts to UTF-16 and writes through `WriteConsoleW` directly, which removes the ambiguity.

### Filenames, paths, and non-ASCII text

This was the deepest rabbit hole, and the one most likely to still bite on an untested edge case. MSVC's `std::filesystem::path::string()` converts through the process's **ANSI code page** — and a character with no mapping in that code page (which, for the vast majority of Windows installs, includes almost anything outside Western European Latin script) doesn't get substituted, it throws `std::system_error`. A library with any Japanese, Korean, Cyrillic, or similar filenames would crash outright the moment such a file scrolled into view, with no way to work around it from inside the app.

The fix is a small header, `path_utf8.h` (new), providing `path_utf8()` / `path_from_utf8()` as the only sanctioned way to convert between `fs::path` and the UTF-8 `std::string`s the rest of the codebase already speaks — UTF-16⇄UTF-8 conversion on Windows, a plain passthrough everywhere else. Every `.string()` call and every `fs::path(some_std_string)` construction across the tree (about twenty call sites, in `app.cpp`, `local_source.cpp`, `metadata_probe.cpp`, `settings.cpp`, `cache_manager.cpp`, `youtube_source.cpp`, `lyrics_fetcher.cpp`, `native_duration.cpp`, `console_log.cpp`) was audited and routed through it. A handful of related fixes came out of the same pass:

- `waveform.cpp`'s use of `miniaudio`'s narrow file-open API silently failed (and fell through to a much slower `ffmpeg` decode) for any non-ASCII path — switched to `ma_decoder_init_file_w` on Windows.
- Case-insensitive matching (search, extension checks) used to fold text byte-by-byte with `std::tolower`, which corrupts multi-byte UTF-8 sequences under a single-byte locale. Replaced with ASCII-only folding that leaves every non-ASCII byte untouched.
- The cache filename sanitizer kept only `[A-Za-z0-9]`, so any track with no Latin characters in its title collapsed to a generic `untitled` filename, and every such track collided on the same name. Non-ASCII codepoints are now kept verbatim (they're legal in NTFS filenames; the characters Windows actually forbids are all ASCII, and were already excluded).

### Subprocess execution

The original shells out to `ffprobe`, `ffmpeg`, `yt-dlp`, and Python for a handful of tasks. POSIX quoting/spawning and Windows' `CreateProcess`/command-line quoting rules are entirely different beasts, so `process_util.cpp` gained a full Windows implementation: an sh-compatible tokenizer that parses the POSIX-style quoted command string the rest of the app already builds, then re-serializes each argument using the (unintuitive, backslash-doubling) rules `CreateProcessW` actually expects. `CreateProcess`'s handle inheritance also turned out to be racy across concurrent spawns in a way POSIX's `posix_spawn` isn't — mousiki spawns several subprocesses from different threads at once by design (a background metadata sweep, a track's own `ffprobe`/`ffmpeg`, yt-dlp resolution, the lyrics helper), so every spawn now gets an explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` instead of inheriting every handle open in the process.

### Native audio-file parsing

`native_duration.cpp` reads MP4/OGG/MP3 headers directly (no subprocess) for fast duration lookups. It leaned on `pread()`, `off_t`, and `open()` — none of which exist as such on Windows. Replaced with `_wopen()` (taking the native wide path, with `_O_BINARY` so the CRT doesn't mangle binary audio data by translating CRLF sequences inside it), `_fstat64`, and a `pread` emulation built on `OVERLAPPED` I/O. `off_t` is 32-bit under MSVC, so every offset in this file is a plain `long long` now instead.

### The playback thread

Track switches used to spin up a fresh OS thread per track to call into WASAPI. WASAPI's COM objects are apartment-affine to whichever thread created them, which a fresh thread per track violates outright. Playback now runs through one persistent device-worker thread for the whole session, which made `Player` genuinely concurrent with the main thread for the first time — it gained an internal mutex (`player.h`/`.cpp`) to guard against the two actually racing.

### Locale

`main()` calls `setlocale(LC_ALL, "")` to get correct character handling for the active locale — inherited from the original, not Windows-specific. What's Windows-specific is the consequence: that call also sets `LC_NUMERIC`, and on a comma-decimal Windows locale (German, French, ...), every `std::stod()` call in the app — settings parsing, `ffprobe`/JSON durations, lyric timestamps — silently truncated at the first `.` with no exception thrown. `LC_NUMERIC` is now pinned back to `"C"` immediately after, independent of whatever the rest of the locale is doing.

### Thread-safety net

Every background `std::thread` (metadata sweep, decode, lyrics fetch, waveform pass, search, playlist add, the device worker) is now wrapped so an exception escaping it becomes a log line instead of an immediate, silent `std::terminate()` — the default behavior for an uncaught exception in a detached thread, and on Windows that means the whole process vanishes with no message at all. Several of the bugs above were originally diagnosed by their symptom being exactly this: total, silent process death with nothing to go on.

### Python helper scripts

`scripts/fetch_lyrics.py` and `scripts/lrc.py` are unchanged in what they fetch, but gained a UTF-8 stdout/stderr reconfiguration at startup. Python picks a text encoding for a redirected pipe from the OS locale; Linux desktop sessions inherit a UTF-8 `LANG` down to every child process automatically, Windows has no equivalent, so Python fell back to the ANSI code page — meaning a lookup for a Japanese, Cyrillic, or otherwise non-Latin track title could crash the script outright the instant it tried to print that title back (even just to report "no lyrics found"), which looked from the app's side identical to the script not existing at all. `scripts/fast_yt_search.py` — present upstream but never actually called from the C++ side — is now wired in as the default online search path (see below), with the same UTF-8 safeguard applied on principle.

## Added Features Beyond the Port

A few things added on top of the original design rather than required to run it at all, some of them minor, some of them major changes/additions.

### Major Additions / Modifications

- **Installer (x64)** entailed in the latest release (since v2.1.0) as alternative to building the app oneself. Installer size is currently ~80MB and results in a ~250MB build (might optimize in the future).
- **Playlist manager** - Via `SHIFT + p` or `P` respectively a playlist menu can be entered and playlists from local files can be created; search in main UI via `/p:`, hit `Enter` and its titles are added to the current queue.

<img width="2287" height="1064" alt="grafik" src="https://github.com/user-attachments/assets/daa15642-6d8d-4f78-8208-4db488e9d61a" />

<img width="2295" height="1056" alt="grafik" src="https://github.com/user-attachments/assets/7ce6f723-ac7d-4a31-900b-efbeb24a3a10" />

- **Meta/tag editor incl. fetch via AcoustID** (`SHIFT+M`, rebindable as `HKeyMetaEditor`) — a second full-screen overlay shaped exactly like the playlist menu (tab strip, boxed panels, search field, hint/status footer) for changing a file's **name**, **artist**, **title**, **album** and **year**:
  - `TAB` cycles search field → library list → the five field rows, where typing edits the hovered field directly; `SHIFT+←/→` switches between the **EDIT** tab and the **FETCH LIST** tab.
  - Every field that was touched — typed *or* filled in by a lookup — stays drawn in the **header colour**, and the matching **file rows in the LIBRARY panel are drawn in that same header colour** (with an `[ N edited ]` count in the panel's footer), so "which files does this session touch, and what exactly will be written?" is answerable at a glance.
  - sorting of the library pane for titles without any meta data (`x`) with missing title (`SHIFT+t`), missing artist (`SHIFT+a`) and missing year (`SHIFT+y`) is possible for better overview. 
  - **AcoustID fetch (audio fingerprint)**: `SHIFT+B` looks up the hovered title (`Browse` list or inside the menu), `a` queues a title for the batch like the queue's add key, `ENTER` on the FETCH LIST tab runs the whole batch. Both ask the exact disclaimer *"Fetching meta data via AcoustID; not always accurate and previous meta data will be overwritten. Continue?"* first (Yes/No/ESC) and results only ever land **in the edit session**, never directly in the files. The lookup identifies the *audio*, not the text in the file name: `scripts/fetch_meta.py` runs `fpcalc` — a drop-in rebuild of Chromaprint's own tool, compiled by CMake from the vendored [Chromaprint](https://github.com/acoustid/chromaprint) 1.6.1 sources in `third_party/chromaprint/` (`tools/fpcalc.cpp` drives the library, ffmpeg does the decoding, so no prebuilt binary ships) — to fingerprint the first ~120 s of the file, sends fingerprint + duration to the AcoustID web service (paced to its 3 requests/second limit) and takes **artist + title** from the best-matching recording — confidence below 0.5 is reported as "no match" rather than written as a wrong tag, and among equally confident recordings the one whose duration is closest to the file wins. Progress shows as `3/7 …` while it runs.
  - Note that "year" and "album" is not fetched since it becomes rather complicated for a lot of albums, considering re-releases such as remasterd album versions. MusicBrainz (which could be added) would be necessary for that and an option to choose between different possible years and album names that were fetched. Since this makes the process of meta data editing rather complex, I discarded the idea to include it. It also might not work well with titles fetched from you tube via yt-dlp. 
  - **API key**: every AcoustID request is signed with an application key that is hard-coded as `API_KEY` at the top of `scripts/fetch_meta.py` — deliberately *not* a setting, because an AcoustID key belongs to one registered application rather than to a user, and the account-key/application-key mix-up is exactly what the service answers with *"invalid API key"*. A highly modified or rebranded build should maybe register its own application (its free!) at <https://acoustid.org/new-application> and change that one line; an invalid key fails with a status line that says so (`NO_KEY`) instead of guessing from the file name.
  - **Always-autosaved session**: the pending edits are written to `~/.cache/mousiki/meta_session/session.json` after every keystroke, so ESC, quitting or crashing keeps them as a backup — the audio files themselves are *never* touched by merely editing.
  - `CTRL+SHIFT+S` applies the session (asks *"Want to save?"*) — tags go through an `ffmpeg -c copy` remux into a temp file that is renamed over the original (audio stays bit-for-bit identical), a name edit becomes a plain rename; `CTRL+SHIFT+X` throws the pending edits away (asks *"Want to discard changes?"*). Failed entries stay in the session so they can be retried. These two are deliberately **not** rebindable: the input layer reports the arrow keys as the letters A/B/C/D, so a rebindable `"B"`/`"S"` would race the arrows — the same reason `SHIFT+B` is matched directly too.

<img width="1794" height="873" alt="grafik" src="https://github.com/user-attachments/assets/01642720-6e02-4fb2-a7be-a030a16ca1f6" />

<img width="1798" height="863" alt="grafik" src="https://github.com/user-attachments/assets/60822ff7-32f0-4e9a-ba09-bcd58f0eb2fe" />

- **New Screen when no title loaded in Playmode "stop" mode** Added an Braille-Ascii music cassette and centered the statement that no track is currently loaded. Not thaaat of major change, but since it adds a design feature, which I didn't do before, I listeded here. 

<img width="2294" height="1077" alt="grafik" src="https://github.com/user-attachments/assets/f04a4225-f4d1-48c4-a8c5-8ff9adf80a0a" />

- **Editable path lists in the settings panel** — Settings → ON/OFF now has a **LOCAL PATH**, **DOWNLOAD FOLDER** section and a **PLAYLIST PATH** section, each one row per configured path and each ending in a `(+ new path)` row that appends a new empty line to type into (Enter on it opens the field immediately). Emptying a line removes that path. `LocalMusicPath=`/`PlaylistsPath=` in config.txt still work identically. Paths in those lists:
  - are *live*: committing a local path rescans the library on the spot instead of waiting for the next launch;
  - take effect for playlists too — playlist folders are now searched across **all** configured `PlaylistsPath=` lines (listed/loaded from every one of them, saved/deleted in the first), instead of only a single one.
  - yt-dlp download folder can now be set in the Setting; only one folder is possible and the folder will automatically be added to local paths, so no extra path adding necessary
    
<img width="2301" height="1046" alt="grafik" src="https://github.com/user-attachments/assets/43241d83-9cdb-41f4-ad37-8793d2fae580" />

- **Listening history** via `SHIFT+h` including the last 100 tracks that had been played, the duration of titles where resorting can be done via the `r` key (default sort is "most played tracks on top" second sort is "least played title on top"), and tracking listening habits containing average session length, time music has been played per day, tracks per session, number of skips, replays and completion rates (how many times did a song finish).
  - top tracks can be added to queue in the second menu tab. In the main UI `SHIFT+x` can now be used to clear the queue incl. a warning message that pops up. 

<img width="2295" height="1009" alt="grafik" src="https://github.com/user-attachments/assets/9e1b1971-5223-4f0d-b3d6-02e9da4874ba" />

<img width="2280" height="1016" alt="grafik" src="https://github.com/user-attachments/assets/7b69dd69-81e2-4a15-a6ae-16f6d08d0f02" />

<img width="2284" height="1019" alt="grafik" src="https://github.com/user-attachments/assets/c47d2304-3d89-473d-aa12-5322619410df" />


### Minor Additions / Modifications

- **Icon for .exe** is now included.
- **Folder Order and Sorting** `f` shows only the titles in a folder of the hovering track in the list. It now shows which folder. `SHIFT+n` was added in the past to toggle between showing the file name and the meta data track name in the local audio files list. I adjusted the sorting algorithm now sorts what is shown in the respective column, adapting to the set `SHIFT+n` mode. 
- **Copy/Paste/Cut in Search and Path Fields** All search fields now allow copy/paste/cut and the necessary marking. Same for fields to add local path.
- **Clear QUEUE** In the main UI `SHIFT+x` can now be used to clear the queue incl. a warning message that pops up. 
- **Fetched Lyrics Folder** is now placed in an extra folder such that lyrics of song_a in folder_a are located in folder_a/lyrics/ and are not placed directly next to track files. Note that changing the name of the file may trigger a new lyrics fetch process (did not find a reasonable solution for that since filenames can eventually also be changed via the Windows explorer or other programs such as Midnight commander or Yazi... Hard to keep track of...
- **Stereo Playback** - Can be toggled in the settings menu. Visualizations rely on a the usual duplicate mono channel.
- **Loudness Normalization** - Parameters can be set in the config.txt and toggled on and off via `v` and in the "ON / OFF" settings menu tab.
- **Search Engine Optimization** - Added fuzzy search ("X-Files" didn't show up when searched "X Files", i.e. without dash) and optimized speed for also searching through meta data (not only file titles), which in Windows took >2-3 min. after the app was started to be available (cache cap was also an issue and a bunch of subprocess handling via ffprobe, which remains as a fallback method in case the newly included ID3v2.3/2.4 frame-walker that reads TIT2/TPE1/TALB directly out can't handle the tag layout of a file for some reason...). The speedup currently covers MP3 handling FLAC (Vorbis comments), OGG/Opus, and M4A/AAC. 
- **Hotkey remapping actually works in app (see above)** — as much as I understood a bug fix rather than a feature, but it's new behavior either way.
- **Shuffle-to-next** (`#`) — a manual one-off jump to a random track, independent of the persistent Shuffle play mode, and independent of the queue (which stays FIFO on purpose).
- **Categorized** Key commands in Reference tab within the Settings. The header color is no longer hardcoded: the COLORS tab has an **`HEADER`** field (config.txt: `ColorHeader=`, default `10` = palette index 10 of 256) which drives both those category titles and the path-list titles described below.
 
  <img width="2294" height="1080" alt="grafik" src="https://github.com/user-attachments/assets/e060895e-e6fa-4132-a9c9-67e2d0ea2167" />

- **Categorized cheat sheet** available via `?`.

  <img width="2286" height="964" alt="grafik" src="https://github.com/user-attachments/assets/f7b2b425-1156-4017-b354-bae54a7a3fb9" />
  
- **Special letters** (see above) — Fixed displaying and typing special letters like Umlaute (ä, ö ü) or accents á, à, Japanese letters etc. Emojis also work, but some 3-byte Emojis may mess up the UI when shown (can be turned off in settings)... 
- **Metadata-only / filename list rows** — `SHIFT+N` (or Settings → ON/OFF → **"Show meta data only"**) swaps every (search-)list row between the long-standing *filename + metadata* presentation and *metadata only*, i.e. the embedded title tag instead of the filename stem. Untagged files (and rows whose tags haven't been probed yet) keep their filename, so an untagged library never turns into a blank list. Applies to the main list, its search results, and both lists in the playlist editor. Also applies to the field between the disk animation and lyrics/sphere. Rebindable like every other hotkey (`HKeyToggleMetaOnly`; use `g` instead if you'd rather not rely on Shift).

- **Scrollable settings panel** — ON/OFF (with its path sections) and REFERENCE now scroll with the cursor instead of growing past the panel, so the tab stays usable on a short terminal (32 rows and below).
- **Fast online search.** `scripts/fast_yt_search.py` hits YouTube's internal search endpoint directly instead of shelling out to `yt-dlp` for every keystroke-triggered search — `yt-dlp` is a general-purpose extractor for hundreds of sites and pays for that generality in startup time. `yt-dlp`'s own search is the fallback whenever the fast path comes back empty for any reason (script missing, network hiccup, or a genuine zero-result query), so nothing regresses if the fast path is ever unavailable. It approximates `yt-dlp`'s old `duration >= 90s` result filter (dropping shorts and live streams) but can't replicate the `categories *= 'Music'` half without a second request per result, which would defeat the point.
- **Long-title handling.** Track titles that overflow their column now word-wrap (up to 3 lines) in the metadata panel, aligned under the value rather than repeating the label, and marquee-scroll horizontally in the local list when a track is hovered — both width-aware for wide (CJK) characters, not just byte-counted.
- **A Lyrics Engine toggle that actually gates fetching**, not just the panel's visibility (`+` to toggle, or Settings → On/Off) — previously the fetch ran and hit the network every single track regardless of whether the panel was shown. Toggling it off now shows the sphere visualization in that space instead of leaving it blank.

## Current Ideas on Features and Modifications 

**Basic Features (that will definitely be implemented soon):**
- no current to-dos... Feel free to start discussion or report issue! 

**Major New Features:**

- (NOT SURE ABOUT THIS, but idea sounds nice) Apart from regular playlists a modified playlist feature could be added: pixel art cassette tapes with limited number of tracks, A/B side, which can be shared. The cassettes could consist of a number of basic components like cassette style, label style, a decent number color sets (incl. a randomizer for composing the cassette style that optionally keeps track of what had been used already in the list of "cassette mixtapes"(i.e. with or without possible color redundancies or so))), may incl. a yt-dlp feature, where you can share a "cassette files / mixtapes (.mix files)" with others which include a list of commands (youtube urls) that can be shared and uploaded to your Mousiki player (commands that initialize starting fetching songs from you tube or elsewhere(local search included)); possible royalty free art that could be adjusted for that purpose (https://pixabay.com/illustrations/search/cassette%20tape/)
- Search online radio channels incl. a key toggle to switch to radio mode (`SHIFT+r`), slight main UI changes where the progress bar could become a radio frequency bar including truning buttons, where different chosen online channels could be assigned to certain frequencies, including a fade effect with an overlay of a selection of noisy sounds when changing the channel (would limit the number of possible channels)... Key toggles for next song (`n` and shuffle next `#` could be re-used as commands to change channels; probably makes more sense than fiddling around with arrow key (I grew up with classic radios and it was fun but also daunting)); 


