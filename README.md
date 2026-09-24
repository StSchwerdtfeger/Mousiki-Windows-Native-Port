<div align="center">
    
# Mousiki Windows Native Port 🎵 

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
[![Language](https://img.shields.io/badge/Language-C++17-orange.svg)](https://github.com/itzender5820/mousiki)
[![Platform](https://img.shields.io/badge/Platform-Linux_%7C_Android_%7C_MacOS-brightgreen.svg)](https://github.com/itzender5820/mousiki)

</div>

A native Windows port of the amazing [itzender5820/mousiki](https://github.com/itzender5820/mousiki) — a terminal music player built for people who prefer control, simplicity, and a keyboard. All credit for the design, feature set, and the vast majority of the code goes to the original author. 

Some minor and major additions where made, e.g. a key to shuffle to a next title (before only next title in the list was possible) a **menu to create playlists from local (or downloaded) tracks**, toggle the lyrics on/off (also via a key command).... See section [beyond the port](#Beyond-the-port) further below for details.  

This fork exists because the original targets POSIX (Linux/macOS/Termux) and has no Windows build path at all — no WSL, no MSYS runtime, no POSIX emulation layer, just a plain `mousiki.exe` built against the Win32 API and WASAPI. Porting it surfaced a long list of platform differences beyond the obvious ones (see [What had to change](#what-had-to-change), below), plus a small number of pre-existing bugs in the original codebase that had nothing to do with Windows and got fixed along the way.

- **Original:** [github.com/itzender5820/mousiki](https://github.com/itzender5820/mousiki) — ender ([itzender5820](https://github.com/itzender5820))
- **Windows port:** Steffen Schwerdtfeger ([StSchwerdtfeger](https://github.com/StSchwerdtfeger)), ported and adjusted with the help of AI tools (only free versions, mostly Sonett 5 set on medium). Therefore take the below with a grain of salt, since I am not a developer for applications like this. However, I liked this music player way too much to not want to use it on my Windows setup, so I went this path and vibe coded a port for Windows. *Huge shout out for the great work by itzender5820 for this beautiful music player.* <3 
- **License:** Apache 2.0 — see [LICENSE](LICENSE)

![preview](preview.gif)

My current setup looks like the below. The config.txt and everything that comes along with (FastFetch and Oh-My-Posh configs) can be found in my cyber-cat themed [MeowerShell repository](https://github.com/StSchwerdtfeger/Meower-Shell):

<img width="779" height="392" alt="grafik" src="https://github.com/user-attachments/assets/a16c6728-37e1-4124-86a4-591677656f00" />

## Current Status of the Port and Modification

For now Mousiki includes everything I at least wanted so there might be no further major releases that add new features. I will adjust the code though to be more polished and might release an "installer version" without dependencies for those that don't want to install all the requirements, such as Visual Studio 2022 Build Tools (never done something like that so it might be good exercise)... 

## Quick start (for PowerShell 7.6.6, not tested with other shells or older versions of PWSH)

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
Personally, I recommend writing a function in your Powershell profile.ps1 in order to be able to run the app via a command (in my case I set the command to be "lala"):
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

`setup.ps1` installs these via winget in PowerShell 7, except the compiler:

| Tool | Why | Install |
|---|---|---|
| Visual Studio 2022 Build Tools, "Desktop development with C++" | compiles the app | must be selected interactively — winget's silent mode won't pick the C++ workload |
| CMake ≥ 3.16 | build system | `winget install Kitware.CMake` |
| FFmpeg | decodes Opus (miniaudio can't), `ffprobe` supplies metadata | `winget install Gyan.FFmpeg` |
| yt-dlp | online search fallback, playlists, streaming | `winget install yt-dlp.yt-dlp` |
| Python 3 + `requests` | lyrics and fast online search (`scripts/`) | `winget install Python.Python.3.12` then `py -3 -m pip install requests` |

All of these are independent of each other and of the core player. With none of them installed, local playback still works. 

MinGW-w64 (MSYS2 UCRT64) also builds this — configure with `-G "MinGW Makefiles"`. The code guards on `_WIN32`, not on `_MSC_VER`, except where MSVC genuinely differs (noted inline where it matters).

`third_party/` (miniaudio v0.11.25, kissfft) is vendored in this repo, so configuring and building needs no internet connection — `CMakeLists.txt` no longer downloads anything, it just stops with a clear error if either is missing. miniaudio is public domain / MIT-0, kissfft is BSD-3-Clause (see the headers in `third_party/`). To update either, replace the files in `third_party/` with a newer upstream copy.

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

`LocalMusicPath=` and `Playlitspath=` entries accept Windows paths, both slash directions should work (`std::filesystem` normalizes them):

```
LocalMusicPath=C:\Users\you\Music
LocalMusicPath=~/Music
```
Playlists folder (optional -- overrides the `LocalMusicPath[0]/playlists` default). Default is located in `C:\Users\YOUR NAME\.cache\mousiki\playlists` 

```
PlaylistsPath=C:\Users\YOUR NAME !!!!!!!\Music\playlists
```


There's no in-app UI for adding a folder — that isn't a Windows-port limitation, the original never had one either; `config.txt` is the only way, on every platform. The library is scanned once at startup, so add or edit `LocalMusicPath=` lines while mousiki is closed; there's no live rescan.

## Default Keybindings

Configurable in `C:\Users\USER\.config\mousiki\config.txt`.

What's new: Shuffle next key and lyrics on/off key (when off, sphere visualisation is shown). 
References with in settings is now categorized. Some keys are not rebindable!

### Search & Playback
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Local Search** | `/` | Filter and search local library 
| **Online Stream Search** | `/s: <query>` | Search and stream music online |
| **Search Playlists** | `/p: <query>` | Search and stream local playlists |
| **Open Playlist** | `SHIFT` / `p` | Open Playlist Creator/Editor |
| **Download Stream** | `y` | Download currently streaming track |
| **Play / Pause** | `p` (or `ENTER`) | Toggle playback |
| **Next / Previous Track** | `n` / `b` | Skip between songs |
| **Seek** | `ARROW_LEFT` / `ARROW_RIGHT` | Seek backward / forward |
| **Volume** | `1` / `2` | Decrease / Increase volume |
| **Stereo** | `v` | Toggle Stereo/Mono mode |
| **Shuffle / Repeat** | `m` / `r` | Toggle shuffle or repeat mode |
| **Shuffle Next** | `#` | Shuffle to next song |
| **Toggle Lyrics** | `+` | Turn Lyrics on/off |

### Navigation & Queue
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Setting** |`s` |Enter Settings menu|
| **Exit Setting** |`ESC` |Exit Settings menu|
| **Exit + Save Setting** |`s` |Exit + Save again via `s`|    
| **Playlists** | `SHIFT+P` |Enter Playlist menu |
| **Navigate** | `ARROW_UP` / `ARROW_DOWN` | Move selection |
| **Switch Tabs/Cards** | `TAB` | Cycle between UI panels |
| **Add to Queue** | `a` | Enqueue selected track |
| **Remove from Queue** | `d` | Dequeue selected track |
| **Move Track Up** | ``4` | Move up in Queue/Playlist |
| **Move Track down** | `5` | Move down in Queue/Playlist |
| **Filter by Folder** | `f` | Apply folder filter |
| **Clear Filter** | `c` | Reset active search/filters |
| **Quit** | `q` | Exit application |

</div>

Every hotkey in config.txt — and everything editable under Settings → Reference in the app itself — is genuinely rebindable, including the five new ones this fork adds (see below). This is worth calling out specifically because it wasn't actually true in the original codebase either (**According to my AI helper, since I can't test it myself, but it didn't work with my Windows port at first somehow!!!!!**): the lookup function that turns a configured key string into an action existed there, fully implemented, but nothing ever called it, so editing a binding only ever changed what was displayed, never what the key actually did. This port wires that lookup into the real input dispatch, so config.txt and the in-app editor now mean what they say — on top of everything already provided upstream, not as a Windows-specific feature.

In the config.txt you will also see liens like A = A, a. This is used so you can re-font the UI without changing the font of the terminal, e.g. via A={𝓐,𝓪}.... 

## What had to change

Thirteen files needed direct `#ifdef _WIN32` branches; a similar number needed changes that apply on every platform but were only ever exposed by something Windows does differently (mostly the UTF-8 path handling below). Everything else compiled and ran unmodified.

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

## Beyond the port

A few things added on top of the original design rather than required to run it at all:

- **Playlist manager** - Via `SHIFT + p` or `P` respectively a playlist menu can be entered and playlists from local files can be created; search in main UI via `/p:`, hit `Enter` and its titles are added to the current queue.

 <img width="2295" height="864" alt="grafik" src="https://github.com/user-attachments/assets/bc0c27fe-59c6-4af9-838a-a3d3bd2ffab6" />

<img width="2302" height="1064" alt="grafik" src="https://github.com/user-attachments/assets/633b4686-1c00-452b-9d39-b3002854f660" />


- **Special letters** (see above) — Fixed displaying and typing special letters like Umlaute (ä, ö ü) or accents á, à etc.
- **Loudness Normalization** - Parameters can be set in the config.txt and toggled on and off via `v`.
- **Stereo Playback** - Can be toggled in the settings menu. Visualizations rely on a the usual duplicate mono channel.
- **Hotkey remapping actually works (see above)** — arguably a bug fix rather than a feature, but it's new behavior either way.
- **Fast online search.** `scripts/fast_yt_search.py` hits YouTube's internal search endpoint directly instead of shelling out to `yt-dlp` for every keystroke-triggered search — `yt-dlp` is a general-purpose extractor for hundreds of sites and pays for that generality in startup time. `yt-dlp`'s own search is the fallback whenever the fast path comes back empty for any reason (script missing, network hiccup, or a genuine zero-result query), so nothing regresses if the fast path is ever unavailable. It approximates `yt-dlp`'s old `duration >= 90s` result filter (dropping shorts and live streams) but can't replicate the `categories *= 'Music'` half without a second request per result, which would defeat the point.
- **Long-title handling.** Track titles that overflow their column now word-wrap (up to 3 lines) in the metadata panel, aligned under the value rather than repeating the label, and marquee-scroll horizontally in the local list when a track is hovered — both width-aware for wide (CJK) characters, not just byte-counted.
- **A Lyrics Engine toggle that actually gates fetching**, not just the panel's visibility (`+` to toggle, or Settings → On/Off) — previously the fetch ran and hit the network every single track regardless of whether the panel was shown. Toggling it off now shows the sphere visualization in that space instead of leaving it blank.
- **Shuffle-to-next** (`#`) — a manual one-off jump to a random track, independent of the persistent Shuffle play mode, and independent of the queue (which stays FIFO on purpose).


