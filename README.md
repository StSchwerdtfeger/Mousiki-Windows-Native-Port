<div align="center">
    
# Mousiki v2.5.0 (Windows · Linux · macOS) 🎵 

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

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Language](https://img.shields.io/badge/Language-C++17-orange.svg)](https://github.com/StSchwerdtfeger/Mousiki-Windows-Native-Port/tree/main)
[![Windows](https://img.shields.io/badge/Windows-x64-0078D4.svg?logo=windows&logoColor=white)](https://github.com/StSchwerdtfeger/Mousiki-Windows-Native-Port/tree/main) [![Linux](https://img.shields.io/badge/Linux-x64-FCC624.svg?logo=linux&logoColor=black)](https://github.com/StSchwerdtfeger/Mousiki-Windows-Native-Port/tree/main) [![macOS](https://img.shields.io/badge/macOS-x64-000000.svg?logo=apple&logoColor=white)](https://github.com/StSchwerdtfeger/Mousiki-Windows-Native-Port/tree/main)
</div>

Mousiki port that also runs natively on Windows (including a bunch of modifications and additions; design maintained; Linux/macOS compatibility re-added). Mousiki is a terminal music player that was originally build for macOS/Linux built by the amazing [itzender5820/mousiki, v1.0](https://github.com/itzender5820/mousiki) for people who prefer control, simplicity, and a keyboard (mousi-**key**). All credits for the design, main feature set, and the vast majority of the code goes to the original author. Since v2.5.0 the same code base builds natively on **Linux and macOS** again, too (see [Quick start (Linux / macOS)](#quick-start-linux--macos)); Windows remains the primary and most tested platform. Feel free to give feedback in the discussions and report issues you might experience using this modified port.

Build yourself (see [prerequisites](#prerequisites) below) **or use the installer/portable (x64) version** that is included in the latest release (since v2.1.0). A full **[user manual](Mousiki_User_Manual_v2.x.x/Mousiki_User_Guide.md)** (also as [PDF](Mousiki_User_Manual_v2.x.x/Mousiki_User_Guide.pdf)) walks through every entry of the in-app cheat sheet, every settings tab and every overlay.

This fork exists because the original targets POSIX (Linux/macOS/Termux) and had no Windows build path at all. The Windows build uses no WSL, no MSYS runtime, no POSIX emulation layer, just a plain `mousiki.exe` built against the Win32 API and WASAPI. All Windows-specific code is guarded by `_WIN32`, so the very same sources also build on Linux (PulseAudio/PipeWire/ALSA) and macOS (CoreAudio) with the included `setup.sh`. Porting it surfaced a long list of platform differences beyond the obvious ones (see [What had to change](#what-had-to-change), below), plus a small number of pre-existing bugs in the original codebase that had nothing to do with Windows and got fixed along the way.

Along the Win32 port, **some minor and major additions were made too**. The major ones are a **playlist menu** to create playlists from local (or downloaded) tracks, a **meta data editor** including **fetching artist/title via AcoustID** (audio fingerprinting), a **listening history** (incl. the ability to add top tracks to the playback queue), an **XY oscilloscope** as an alternative to the lyrics ball, a **10 band EQ with 12 presets**, enlarged **list/queue overlays**, and a **user manual** (.md and .pdf). Minor changes/additions are e.g. a general key to shuffle to a next title (before only the next title in the list was possible), stereo audio and loudness normalization, adding paths via the settings menu, toggling the lyrics on/off (also via a key command), an optimized search engine for Windows (searching metadata was very slow and only available 2-3 min. after starting the app), fuzzy search (e.g. "X-Files" didn't show up when searching "X Files" without the dash), a categorized cheat sheet... The design remained the same for obvious reasons; the only thing added is a Braille-ASCII music cassette shown when no track is loaded... See section [added features beyond the port](#added-features-beyond-the-port) for a full detailed list.  

- **Original:** [github.com/itzender5820/mousiki](https://github.com/itzender5820/mousiki) — ender ([itzender5820](https://github.com/itzender5820))
- **License:** Apache 2.0 — see [LICENSE](LICENSE)
- **Windows port:** Steffen Schwerdtfeger ([StSchwerdtfeger](https://github.com/StSchwerdtfeger)), ported and adjusted with the help of AI tools (only free versions, mostly MiMo V2.6 and Sonnet 5 set on medium). Therefore take some of the below with a grain of salt (explicitly the documentation of the Win native port and some assumed bugs of the original, which I can't confirm myself), since I am not a developer for applications like this and I do not fully understand how the porting was actually done. Still took me around 50h in the last weeks to perform porting to Win, modify (feature design) and debug this version... The repo code could also be optimized in that respect, but apparently is supposed to be done quite well (from feedback I got so far and evaluated myself, as far as I am capable to do so). Even though I am not that big fan of using AI for scientific applications (which I usually do), e.g. in the context of data science - since someone has to understand how sh** works and understanding is beautiful and mind blowing - I still liked this music player way too much the first time I saw it on social media to not want to use it on my Windows setup... Sooooo, I went this path and vibe coded a lot to create a port for Windows and still learned a lot as well (especially on UI and feature design). *In general, a huge shout out for the great work by itzender5820 for this beautiful music player.* <3 It's the best and most fun music player I ever found. Makes me want to listen to music all the time :D 

<p align="center"><img width="800" alt="preview" src="preview.gif" /></p>

My current setup looks like the below. The config.txt and everything that comes along with it (FastFetch and Oh-My-Posh configs) can be found in my cyber-cat themed [MeowerShell repository](https://github.com/StSchwerdtfeger/Meower-Shell):

<p align="center"><img width="779" height="392" alt="grafik" src="https://github.com/user-attachments/assets/a16c6728-37e1-4124-86a4-591677656f00" /></p>

## Current Status of the Port and Modification (v2.5.0, now with standalone setup.exe / portable, see latest release)

For now the Mousiki port works well and also includes everything I at least wanted and made sense to me for a music player, so there might be no further major releases that add new features, except of bug-fixes that might appear to me or others in the future (feel free to start discussions or report issues!!). I will adjust the code though to be more polished and might optimize the installer release (currently ~250MB size, installer itself ~80MB, portable .zip ~100MB) ... Since v2.5.0 my version of Mousiki also builds on macOS/Linux again (`setup.sh`), which is a first step towards making it potentially integratable into the main branch of the original project (which still seems way too hard after dozens of comments in the last two weeks, at least from my perspective). The Linux/macOS builds are less tested than the Windows one, so reports from users on those platforms are especially welcome. 
Concerning new feature, further below you'll find a list of [current Ideas on features and modifications](#Current-Ideas-on-Features-and-Modifications). An online-radio function as well as a mixtape creator would be cool, but I'll see. Again, feel free to give feedback in the discussions and report issues you might experience using this modified port.

## Quick start (Windows)

Installer/portable (x64) is included in the latest release (since v2.1.0) or build yourself via the commands below. **On Linux or macOS?** Jump to [Quick start (Linux / macOS)](#quick-start-linux--macos).

```powershell
# from the repo root
.\setup.ps1
```
> [!IMPORTANT]
> `setup.ps1` itself **requires PowerShell 7 or newer** (`#Requires -Version 7.0`); it will refuse to run in Windows PowerShell 5.1. The finished `mousiki.exe` was tested in PowerShell 7.6.6 *and* 5.1. If you only have 5.1, build manually (see below).

Optional parameters: `-SkipDeps` (configure and build only, don't touch winget/pip) and `-BuildType Debug` (default is `Release`).

If PowerShell blocks the script, the following temporarily disables script blocking and warning prompts for the current PowerShell session only:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
.\setup.ps1
```

The setup.ps1 already performs the build. If something didn't work and you want to build manually after debugging (this also works in PowerShell 5.1), use CMake directly:

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The build copies `scripts\` (including the freshly built `fpcalc.exe`) next to `mousiki.exe`. Keep that `scripts\` folder next to the exe if you move it, otherwise lyrics, fast online search and the AcoustID fetch stop working.

Run app e.g. via the command below (adjust username in the path before executing!!).
Note that you have to add your local files path via the config file `C:\Users\YOURNAME\.config\mousiki\config.txt` or via Settings (`s`) inside the app (more details further below). The `config.txt` in the repo root is a documented template of that file.
See the [user manual](Mousiki_User_Manual_v2.x.x/Mousiki_User_Guide.md) or the original repo by itzender5820 for an introduction on how to use Mousiki.
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
Voilà, you can now open Mousiki from any folder you're at using the command "lala", or whatever you set as command respectively...

## Prerequisites (Windows)

*(This section describes the Windows setup; for Linux/macOS see [Quick start (Linux / macOS)](#quick-start-linux--macos) above.)*

Note, I had a bunch of the below already installed, so I am not sure how smooth setup.ps1 runs installing the below for the first time using setup.ps1 (such as installing Visual Studio 2022 Build Tools...).

`setup.ps1` (PowerShell 7+) installs these via winget (and pip for `requests`), except the compiler "VS 2022 Build tools":

| Tool | Why | Install |
|---|---|---|
| Visual Studio 2022 Build Tools, "Desktop development with C++" | compiles the app (and the `fpcalc` helper) | `winget install --id Microsoft.VisualStudio.2022.BuildTools`, then tick the C++ workload interactively — winget's silent mode won't pick it. `setup.ps1` only warns if no compiler is found. |
| CMake ≥ 3.16 | build system | `winget install Kitware.CMake` |
| FFmpeg | decodes Opus (miniaudio can't), `ffprobe` supplies metadata, decodes audio for the AcoustID fingerprint and writes tags in the meta editor (`ffmpeg -c copy`) | `winget install Gyan.FFmpeg` |
| yt-dlp | online search fallback, YouTube playlists (bulk add), streaming, downloads | `winget install yt-dlp.yt-dlp` |
| Python 3 | runs the helper scripts in `scripts/`: fast online search (`fast_yt_search.py`), AcoustID fetch (`fetch_meta.py`) and lyrics (`fetch_lyrics.py`) — the first two use the standard library only | `winget install Python.Python.3.12` |
| Python package `requests` | lyrics only (`fetch_lyrics.py` / `lrc.py`) | `py -3 -m pip install requests` (done by `setup.ps1`) |
| `fpcalc` (Chromaprint) | audio fingerprint for the AcoustID fetch | **nothing to install** — built by CMake from `third_party/chromaprint/` and copied to `scripts\` |

All of the runtime tools (FFmpeg, yt-dlp, Python) are independent of each other and of the core player. With none of them installed, local playback of MP3/FLAC/WAV etc. still works (Opus/some other formats need FFmpeg). 

MinGW-w64 (MSYS2 UCRT64) also builds this — configure with `-G "MinGW Makefiles"`. The code guards on `_WIN32`, not on `_MSC_VER`, except where MSVC genuinely differs (noted inline where it matters).

`third_party/` (miniaudio v0.11.25, kissfft, chromaprint 1.6.1 for the AcoustID meta data fetch) is vendored in this repo, so configuring and building needs no internet connection — `CMakeLists.txt` no longer downloads anything, it just stops with a clear error if one of them is missing. miniaudio is public domain / MIT-0, kissfft is BSD-3-Clause, Chromaprint is MIT but — because it bundles some FFmpeg code — is to be treated as LGPL-2.1 as a whole (see `third_party/chromaprint/LICENSE.md` and the headers in `third_party/`). To update either, replace the files in `third_party/` with a newer upstream copy (not tested if it is that ease now that chromaprint is also included).

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
| Listening history | `%USERPROFILE%\.cache\mousiki\history\history.json` |
| Meta editor session (pending edits) | `%USERPROFILE%\.cache\mousiki\meta_session\session.json` |
| Playlists (default) | `<first LocalMusicPath>\playlists\`; falls back to `%USERPROFILE%\.cache\mousiki\playlists\` if no `LocalMusicPath` is set |
| Fetched lyrics | `<folder of the track>\lyrics\` |

`LocalMusicPath=`, `PlaylistsPath=` and `DownloadFolder=` entries accept Windows paths, both slash directions should work (`std::filesystem` normalizes them). A leading `~` is expanded to your user profile; **environment variables such as `%USERPROFILE%` are *not* expanded**, so use `~` or a full path:

```
LocalMusicPath=C:\Users\you\Music
LocalMusicPath=~/Music
```
Playlists folder (optional, one `PlaylistsPath=` line per folder). Without it, playlists live in `<first LocalMusicPath>\playlists`. Playlists are listed/loaded from all configured folders, saving/deleting always uses the first one.
You can also add folders in the settings (`s`). Same goes for the (single) download folder (`DownloadFolder=`, default is `%USERPROFILE%\.cache\mousiki`), which is automatically added to the local music paths.

```
PlaylistsPath=C:\Users\YOUR NAME !!!!!!!\Music\playlists
DownloadFolder=D:\Downloads\mousiki
```

## Quick start (Linux / macOS)

Since v2.5.0 the code builds again on Linux and macOS as well (same sources, platform selected at compile time by `CMakeLists.txt`; audio via PulseAudio/PipeWire-pulse or ALSA on Linux and CoreAudio on macOS). Windows remains the primary, most tested platform. `setup.sh` is the counterpart of `setup.ps1`:

```bash
# from the repo root
chmod +x setup.sh   # only needed once, if the executable bit got lost (e.g. after unzipping)
./setup.sh
```

It detects your package manager (apt, dnf, pacman, zypper, apk, or Homebrew on macOS), installs only what is missing (see the table below), then configures and builds with CMake. The binary ends up in `build/mousiki`, with `scripts/` (including the freshly built `fpcalc`) copied next to it.

Options:

| Option | Effect |
|---|---|
| `--skip-deps` | configure and build only, don't touch the package manager / pip |
| `--debug`, `--build-type Release\|Debug` | build type (default `Release`) |
| `--install` | additionally creates a launcher `~/.local/bin/mousiki`, so you can start the app from anywhere by typing `mousiki` |
| `-y`, `--yes` | don't ask before installing packages |
| `-h`, `--help` | show the options |

> [!NOTE]
> Package installation uses `sudo` on Linux when you are not root. On macOS the script needs [Homebrew](https://brew.sh) and the Xcode Command Line Tools (`xcode-select --install`, they provide the C++ compiler); if one is missing, the script tells you and stops. The launcher from `--install` is a small wrapper script on purpose and not a symlink: macOS does not resolve symlinks when the binary looks up its `scripts/` folder.

Run it:

```bash
./build/mousiki
```

The config file is created at `~/.config/mousiki/config.txt` (the `config.txt` in the repo root is a documented template; Linux/macOS paths such as `LocalMusicPath=~/Music` work, the Windows examples in it do not apply). Any UTF-8 terminal works.

| Tool | Why | Installed by `setup.sh` as |
|---|---|---|
| C++17 compiler, CMake ≥ 3.16 | builds the app and the `fpcalc` helper | `build-essential` / `gcc-c++` / `base-devel` / `build-base` + `cmake`; Xcode CLT on macOS |
| ALSA + PulseAudio libraries (Linux) | audio output (miniaudio loads them at runtime; PipeWire works through `pipewire-pulse` / `pipewire-alsa`) | `libasound2-dev libpulse-dev` (apt), `alsa-lib-devel pulseaudio-libs-devel` (dnf), … |
| FFmpeg (incl. `ffprobe`) | Opus decoding, metadata, AcoustID decoding, tag writing | `ffmpeg` (on Fedora from RPM Fusion; `setup.sh` falls back to `ffmpeg-free` and prints a hint) |
| yt-dlp | online search fallback, playlists, streaming, downloads | `yt-dlp` from the package manager, otherwise `pip install --user yt-dlp` (then `~/.local/bin` has to be on your `PATH`) |
| Python 3 + `requests` | lyrics, fast online search, AcoustID fetch (`requests` is only needed for lyrics) | `python3`, `python3-requests` (or `pip install --user requests`) |
| `xclip` / `wl-clipboard` (Linux, optional) | pasting into the search field; macOS uses the built-in `pbpaste` | `xclip` on X11, `wl-clipboard` on Wayland |

Just like on Windows, FFmpeg, yt-dlp and Python are independent of each other and of the core player: without them, local playback of MP3/FLAC/WAV etc. still works. If you prefer to install everything yourself, build manually:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Keep the `scripts/` folder next to the binary if you move it, otherwise lyrics, fast online search and the AcoustID fetch stop working.

## Default Keybindings

Rebindable in `C:\Users\USER\.config\mousiki\config.txt` or in Settings → Reference (`s`). Keys written as `SHIFT+x` are the **uppercase letter** (e.g. `HKeyCycleSortMode="T"`), because the plain lowercase letter already does something else. Some commands are fixed (not rebindable): `ESC`, `Y`/`N` in prompts, `SHIFT+B`, `SHIFT+↑/↓` in the overlays, and the keys inside the playlist and meta editors including `CTRL+SHIFT+S/X`.
See the **[user manual](Mousiki_User_Manual_v2.x.x/Mousiki_User_Guide.md)** for every command in detail or use `?` for the cheat sheet inside the app (it always shows the keys you actually have bound).

Note that macOS uses the Option key as substitute for ALT! When installing on macOS all key command legends and the cheat sheet should be adjusted accordingly. 

### System
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Cheat Sheet** | `?` | List of all key commands (on German keyboards it is `SHIFT + ß`) |
| **Settings** | `s` | Open Settings; inside Settings `s` again saves to `config.txt` and returns |
| **Exit Settings** | `ESC` / `q` | Return without writing `config.txt` right now (config is also written on a normal quit) |
| **Console / Logs** | `t` | Show console logs for debugging |
| **Quit** | `q` | Quit (from the main UI) |
| **Home view** | `ESC` | On the main screen: back to the full local library, no search/folder filter, scrolled to top (sort mode stays) |
| **Confirm prompt** | `Y` / `N` | Answer Yes/No prompts (clear queue, AcoustID disclaimer, ...) |

### Playback
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Play / reload selected track** | `Enter` | Play or reload selected track (on a playlist row: queue all of its tracks) |
| **Play / Pause** | `p` | Toggle playback |
| **Next / Previous Track** | `n` / `b` | Skip between songs (`n` takes the queue first) |
| **Shuffle Next** | `#` | Jump to a random track of the current list (ignores the queue) |
| **Change Play-/Cycle mode** | `m` | list → repeat → shuffle → stop → repeat queue (shown as `L` `R` `S` `O` `Q` next to the search bar) |
| **Seek** | `ARROW_LEFT` / `ARROW_RIGHT` | Seek backward / forward (5 s) |
| **Volume** | `1` / `2` | Increase / Decrease in-app volume (steps of 5) |
| **Mute** | `x` | Set volume to 0 without pausing; press again to restore |
| **Normalize** | `v` | Toggle loudness normalization, edit in settings' reference tab, use `SHIFT + v` for overlay menu |

### Navigation & View
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Navigate** | `ARROW_UP` / `ARROW_DOWN` | Move selection |
| **Switch Tabs/Cards** | `TAB` | Switch focus between list and queue |
| **Filter by Folder** | `f` | Show only the folder of the hovering track |
| **Clear Filter** | `c` | Reset the folder filter |
| **Cycle Sort Mode** | `SHIFT + t` | folder order → title A-Z → artist A-Z |
| **Filename / Meta Data** | `SHIFT + n` | Toggle between filename + meta data and meta data only in lists |
| **Cycle Lyrics View** | `+` | Cycle the lyrics area: lyrics → sphere → oscilloscope |
| **Retry Lyrics** | `l` | Form to fetch lyrics again with a manual title/artist (only while the lyrics engine is on) |
| **Waveform style** | `w` | Toggle waveform raw / smooth |
| **Refresh UI** | `k` | Force a full redraw (e.g. after a terminal resize) |
| **Track list overlay** | `SHIFT + l` | Enlarged overlay of the track list pane; `SHIFT + ↑/↓` pages, `ESC` closes |
| **Queue list overlay** | `SHIFT + k` | Enlarged overlay of the queue pane (same paging, `ESC` closes) |
| **Oscilloscope tuning** | `SHIFT + o` | Overlay to tune the oscilloscope's afterglow, dot threshold and tail live (`R` resets, saved on close) |
| **Normalization tuning** | `SHIFT + v` | Overlay to adjust the loudness normalization parameters |
| **Lyrics timing** | `ALT + l` | Overlay to shift the lyrics of the playing track earlier / later (`←`/`→` ±0.1 s, `↑`/`↓` ±0.5 s, `R` reset, `ENTER` saves the offset into the track's `.lrc`, `ESC` cancels). |
| **Equalizer** | `SHIFT + e` | Open Equalizer overlay menu, includes a 10 band EQ with 13 presets. | 
| **Sleep timer** | `SHIFT + z` | Small overlay: pause playback after 15 / 30 / 60 / 90 / 120 minutes, or stop after the current song (or switch it off). Independent of the Stop play mode |

### Search
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Local Search** | `/` | Filter and search local library (fuzzy, live as you type) |
| **Online Stream Search** | `/s: <query>` | Search and stream music online (runs on `Enter`) |
| **Search Playlists** | `/p: <query>` | Search saved playlists; `Enter` on a playlist queues all of its tracks |
| **Search Folders by Name** | `/f: <query>` | Search folder; hit `Enter` to open content in local audio pane (similar using `f`) |

### Queue
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Add to Queue (next)** | `a` | Enqueue selected track as the *next* one (several `a` presses keep their order); with the queue focused: bulk-add panel for a pasted YouTube playlist link |
| **Add to End of Queue** | `e` | Enqueue selected track at the *end* of the queue |
| **Lock Queue** | `!` | Toggle "locked" (default: on): a played track moves to the end of the queue so it loops; unlocked, a played track leaves the queue (shown as `locked` in the queue title) |
| **Remove from Queue** | `d` | Dequeue selected track |
| **Move Track Up** | `4` | Move up in Queue/Playlist |
| **Move Track down** | `5` | Move down in Queue/Playlist |
| **Move to Top / Bottom** | `SHIFT + 4` / `SHIFT + 5` | Move the hovering queue item to the very top / bottom |
| **Clear Queue** | `SHIFT + x` | Clear Queue (asks first) |
| **Undo Clear Queue** | `CTRL + SHIFT + z` | Bring back the queue that was cleared last (in front of anything queued since) |
| **Queue to Playlist** | `CTRL + SHIFT + u` | Open the playlist editor with the queue's local tracks, name field focused (type a name, `HOME` saves and stays in the menu) |

### Playlists, Meta Data, History, Downloads
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Open Playlist Editor** | `SHIFT + p` | Open Playlist Creator/Editor (see the editor keys below) |
| **Open Meta Data Editor** | `SHIFT + m` | Open Meta Data Editor (see the editor keys below) |
| **Fetch meta data (AcoustID)** | `SHIFT + b` | Fingerprint lookup of the hovered local title (fixed key, also works from the main list) |
| **Listening History** | `SHIFT + h` | Open the history overlay; `1` / `2` / `3` switch tabs, `r` re-sorts, `TAB`/`Enter` on Top Tracks add the top 10-25-50-100 to the queue |
| **Download Stream** | `y` | Download currently streaming track to the download folder (Settings → Download Folder, default `.cache\mousiki`) |

Keys inside the **playlist editor**: `ALT+←/→` switch tab, `TAB` cycle focus, `Enter` add track / load playlist, `4`/`5` move track, `D`/`DEL`/`BACKSPACE` remove track (delete playlist on the Saved tab), `HOME` save playlist, `SHIFT+←/→` mark text, `CTRL+C/X/V` copy/cut/paste.

Keys inside the **meta data editor**: `←/→` switch EDIT / FETCH LIST tab, `TAB` cycle focus, `Enter` edit field (on FETCH LIST: run the whole batch), `a` add file to the fetch list, `r` edited files on top, `x` / `SHIFT+T` / `SHIFT+A` / `SHIFT+Y` filter for missing any / title / artist / year, `DEL`/`d` remove from the fetch list, `CTRL+SHIFT+S` apply edits to the files, `CTRL+SHIFT+X` discard edits.

Every editable/rebindable hotkey can also be found in the config.txt and — for an overview — in the in-app cheat sheet overview (`?`). Not every key was set to be rebindable. Rebinding can also be done in the config.txt file and under Settings → Reference in the app itself (a key already used by another action is refused).

Changing Fonts: In the config.txt you will also see lines like `A={A,a}`. This is used so you can re-font the UI without changing the font of the terminal, e.g. via `A={𝓐,𝓪}` (edit while the app is closed). 

## Playlist and Meta Data UI Behavior

The playlist UI starts in the Name field, where a name for the playlist can be chosen. The pane focus can be changed via TAB, the tabs of the playlist menu can be changed via ALT+LEFT/RIGHT and the playlist is saved via HOME. This was a design compromise that I did, since it starts in the name field and SHIFT+LEFT/RIGHT is reserved for marking text input... 
In the meta data menu, the tabs can be changed via `left / right`. I guess the handling of the meta data menu needs some practice, since it is rather complex task to perform, however after fitting in it works well. To sort the list (e.g. all edited titles on top, or show only titles with no meta data, or not title or no artist) you have to set the focus on the library pane and then use `x` / `SHIFT+T` / `SHIFT+A` / `SHIFT+Y` for all missing, title missing, artist missing, year missing (press again to clear). Using metatogger can be a bit faster, however it is not the Mousi-**key** way of doing things! Gotta love the terminal. 

## What had to change

Around thirteen files needed direct `#ifdef _WIN32` branches; a similar number needed changes that apply on every platform but were only ever exposed by something Windows does differently (mostly the UTF-8 path handling below). Everything else compiled and ran unmodified. Due to the fast modifications, it became to hard to track what has changed compared the original version. Since v2.5.0 this adjusted code also builds on macOS/Linux again, additions included (`setup.sh`), so that they can eventually be added to the original branch of this fork. The Linux/macOS side has seen less testing than Windows so far; feedback from other users is welcome.

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

`main()` calls `setlocale(LC_ALL, "")` to get correct character handling for the active locale — inherited from the original, not Windows-specific. What's Windows-specific is the consequence: that call also sets `LC_NUMERIC`, and on a comma-decimal Windows locale (German, French, ...), every `std::stod()` call in the app — settings parsing, `ffprobe`/JSON duration, lyric timestamps — silently truncated at the first `.` with no exception thrown. `LC_NUMERIC` is now pinned back to `"C"` immediately after, independent of whatever the rest of the locale is doing.

### Thread-safety net

Every background `std::thread` (metadata sweep, decode, lyrics fetch, waveform pass, search, playlist add, the device worker) is now wrapped so an exception escaping it becomes a log line instead of an immediate, silent `std::terminate()` — the default behavior for an uncaught exception in a detached thread, and on Windows that means the whole process vanishes with no message at all. Several of the bugs above were originally diagnosed by their symptom being exactly this: total, silent process death with nothing to go on.

### Python helper scripts

`scripts/fetch_lyrics.py` and `scripts/lrc.py` are unchanged in what they fetch, but gained a UTF-8 stdout/stderr reconfiguration at startup. Python picks a text encoding for a redirected pipe from the OS locale; Linux desktop sessions inherit a UTF-8 `LANG` down to every child process automatically, Windows has no equivalent, so Python fell back to the ANSI code page — meaning a lookup for a Japanese, Cyrillic, or otherwise non-Latin track title could crash the script outright the instant it tried to print that title back (even just to report "no lyrics found"), which looked from the app's side identical to the script not existing at all. `scripts/fast_yt_search.py` — present upstream but never actually called from the C++ side — is now wired in as the default online search path (see below), with the same UTF-8 safeguard applied on principle.

## Added Features Beyond the Port

Below is a list of major and minor addition on top of the original v1.0. The design remained untouched.

### Major Additions / Modifications

- **Port on linux/macOS** I adjusted the code so the current v2.5.0 also runs on the initial platforms again. I haven't tested this yet and there might be adjustments in the future. Note that macOS has no ALT key. When installing on macOS all cheat sheet and command legends will be adjusted accordingly.
- **Installer/portable (x64)** included in the latest release (since v2.1.0) as an alternative to building the app yourself. Installer size is currently ~80MB and results in a ~250MB build (might optimize in the future), the portable .zip has ~100MB.
- **YX mode oscilloscope** as alternative to the lyrics ball. Parameters such as decay can be changed in an overlay menu via `SHIFT+o`.

<p align="center"><img width="850" height="399" alt="grafik" src="https://github.com/user-attachments/assets/05bb7c75-0e44-4769-b348-13c7eea584e7" /></p>

- **Sleep timer** with several options to choose from (open via `SHIFT+z`. Can be turned off gain. Timer resets after restart of the app and "off" is set as default.
 
 <p align="center"><img width="848" height="395" alt="grafik" src="https://github.com/user-attachments/assets/39607465-4293-47b3-a380-dd49fe936a6d" /></p>

- **Adjust lyrics timing** menu where an offset of max. +/-120s can be added tot he lyrics. This is specially helpful when songs where downloaded from youtube, where a video version includes scenes before the actual song starts etc.

<p align="center"><img width="848" height="397" alt="grafik" src="https://github.com/user-attachments/assets/05c23222-efb6-44be-b74b-3e47803df2d1" /></p>

- **10 band EQ** overlay with 13 presets which can be opend in the main playback UI via `SHIFT+e`.

<p align="center"><img width="815" height="380" alt="grafik" src="https://github.com/user-attachments/assets/861491c8-68d5-46f1-bf9f-c38e413f1f12" /></p>

- **Track and queue list overlay** for the playback UI: `SHIFT+L` opens an enlarged list pane, `SHIFT+K` an enlarged queue pane (kept off `q` on purpose, so you cannot quit by accident), `SHIFT+↑/↓` scroll page-wise, `ESC` or the same key closes. Playback keys keep working inside the overlays. Queues are locked by default (a played track moves to the end instead of disappearing; unlock with `!` and it leaves the queue) and the queue can be saved as playlist (moving to the playlist menu). Several other commands such as add to end and move to top/bottom are also added
 
<p align="center"><img width="850" height="402" alt="grafik" src="https://github.com/user-attachments/assets/52a502e6-2ccc-4dd9-94e2-84663221bda6" /></p>

- **Playlist manager** - Via `SHIFT + p` or `P` respectively a playlist menu can be entered and playlists from local files can be created; search in main UI via `/p:`, hit `Enter` and its titles are added to the current queue.

<p align="center"><img width="850" height="395" alt="grafik" src="https://github.com/user-attachments/assets/daa15642-6d8d-4f78-8208-4db488e9d61a" /></p>

<p align="center"><img width="850" height="391" alt="grafik" src="https://github.com/user-attachments/assets/7ce6f723-ac7d-4a31-900b-efbeb24a3a10" /></p>

- **Meta/tag editor incl. fetch via AcoustID** (`SHIFT+M`, rebindable as `HKeyMetaEditor`) — a second full-screen overlay shaped similar to the playlist menu (tab strip, boxed panels, search field, hint/status footer) for changing a file's **name**, **artist**, **title**, **album** and **year**:
  - `TAB` cycles search field → library list → the five field rows, where typing edits the hovered field directly; `←/→` switches between the **EDIT** tab and the **FETCH LIST** tab (while a field row is being edited, `SHIFT+←/→` marks text instead).
  - Every field that was touched — typed *or* filled in by a lookup — stays drawn in the **header colour**, and the matching **file rows in the LIBRARY panel are drawn in that same header colour** (with an `[ N edited ]` count in the panel's footer), so "which files does this session touch, and what exactly will be written?" is answerable at a glance.
  - sorting of the library pane for titles without any meta data (`x`) with missing title (`SHIFT+t`), missing artist (`SHIFT+a`) and missing year (`SHIFT+y`) is possible for better overview; `r` toggles the edited files to the top. `DEL`/`d` removes a title from the fetch list.
  - **AcoustID fetch (audio fingerprint)**: `SHIFT+B` looks up the hovered title (`Browse` list or inside the menu), `a` queues a title for the batch like the queue's add key, `ENTER` on the FETCH LIST tab runs the whole batch. Both ask the exact disclaimer *"Fetching meta data via AcoustID; not always accurate and previous meta data will be overwritten. Continue?"* first (Yes/No/ESC) and results only ever land **in the edit session**, never directly in the files. The lookup identifies the *audio*, not the text in the file name: `scripts/fetch_meta.py` runs `fpcalc` — a drop-in rebuild of Chromaprint's own tool, compiled by CMake from the vendored [Chromaprint](https://github.com/acoustid/chromaprint) 1.6.1 sources in `third_party/chromaprint/` (`tools/fpcalc.cpp` drives the library, ffmpeg does the decoding, so no prebuilt binary ships) — to fingerprint the first ~120 s of the file, sends fingerprint + duration to the AcoustID web service (paced to its 3 requests/second limit) and takes **artist + title** from the best-matching recording — confidence below 0.5 is reported as "no match" rather than written as a wrong tag, and among equally confident recordings the one whose duration is closest to the file wins. Progress shows as `3/7 …` while it runs.
  - Note that "year" and "album" is not fetched since it becomes rather complicated for a lot of albums, considering re-releases such as remasterd album versions. MusicBrainz (which could be added) would be necessary for that and an option to choose between different possible years and album names that were fetched. Since this makes the process of meta data editing rather complex, I discarded the idea to include it. It also might not work well with titles fetched from you tube via yt-dlp. 
  - **API key**: every AcoustID request is signed with an application key that is hard-coded as `API_KEY` at the top of `scripts/fetch_meta.py` — deliberately *not* a setting, because an AcoustID key belongs to one registered application rather than to a user, and the account-key/application-key mix-up is exactly what the service answers with *"invalid API key"*. A highly modified or rebranded build should maybe register its own application (its free!) at <https://acoustid.org/new-application> and change that one line; an invalid key fails with a status line that says so (`NO_KEY`) instead of guessing from the file name.
  - **Always-autosaved session**: the pending edits are written to `~/.cache/mousiki/meta_session/session.json` after every keystroke, so ESC, quitting or crashing keeps them as a backup — the audio files themselves are *never* touched by merely editing.
  - `CTRL+SHIFT+S` applies the session (asks *"Want to save?"*) — tags go through an `ffmpeg -c copy` remux into a temp file that is renamed over the original (audio stays bit-for-bit identical), a name edit becomes a plain rename; `CTRL+SHIFT+X` throws the pending edits away (asks *"Want to discard changes?"*). Failed entries stay in the session so they can be retried. These two are deliberately **not** rebindable: the input layer reports the arrow keys as the letters A/B/C/D, so a rebindable `"B"`/`"S"` would race the arrows — the same reason `SHIFT+B` is matched directly too.

<p align="center"><img width="850" height="414" alt="grafik" src="https://github.com/user-attachments/assets/01642720-6e02-4fb2-a7be-a030a16ca1f6" /></p>

<p align="center"><img width="850" height="408" alt="grafik" src="https://github.com/user-attachments/assets/60822ff7-32f0-4e9a-ba09-bcd58f0eb2fe" /></p>

- **New Screen when no title loaded in Playmode "stop" mode** Added an Braille-Ascii music cassette and centered the statement that no track is currently loaded. Not thaaat of major change, but since it adds a design feature, which I didn't do before, I listeded here. 

<p align="center"><img width="850" height="399" alt="grafik" src="https://github.com/user-attachments/assets/f04a4225-f4d1-48c4-a8c5-8ff9adf80a0a" /></p>

- **Editable path lists in the settings panel** — Settings → ON/OFF now has a **LOCAL PATH**, **DOWNLOAD FOLDER** section and a **PLAYLIST PATH** section, each one row per configured path and each ending in a `(+ new path)` row that appends a new empty line to type into (Enter on it opens the field immediately). Emptying a line removes that path. `LocalMusicPath=`/`PlaylistsPath=` in config.txt still work identically. Paths in those lists:
  - are *live*: committing a local path rescans the library on the spot instead of waiting for the next launch;
  - take effect for playlists too — playlist folders are now searched across **all** configured `PlaylistsPath=` lines (listed/loaded from every one of them, saved/deleted in the first), instead of only a single one.
  - yt-dlp download folder can now be set in the Setting; only one folder is possible and the folder will automatically be added to local paths, so no extra path adding necessary
    
<p align="center"><img width="850" height="386" alt="grafik" src="https://github.com/user-attachments/assets/43241d83-9cdb-41f4-ad37-8793d2fae580" /></p>

- **Listening history** via `SHIFT+h` including the last 100 tracks that had been played, the duration of titles where resorting can be done via the `r` key (default sort is "most played tracks on top" second sort is "least played title on top"), and tracking listening habits containing average session length, time music has been played per day, tracks per session, number of skips, replays and completion rates (how many times did a song finish).
  - top tracks (top 10 / 25 / 50 / 100) can be added to the queue in the second menu tab (`TAB` switches pane, `Enter` adds). Tabs are switched with `1` / `2` / `3`.

<p align="center"><img width="850" height="374" alt="grafik" src="https://github.com/user-attachments/assets/9e1b1971-5223-4f0d-b3d6-02e9da4874ba" /></p>

<p align="center"><img width="850" height="379" alt="grafik" src="https://github.com/user-attachments/assets/7b69dd69-81e2-4a15-a6ae-16f6d08d0f02" /></p>

<p align="center"><img width="850" height="379" alt="grafik" src="https://github.com/user-attachments/assets/c47d2304-3d89-473d-aa12-5322619410df" /></p>

- **Stereo Playback** - Can be toggled in the settings menu. Visualizations rely on a the usual duplicate mono channel.
- **Loudness Normalization** - Parameters can be set in the config.txt and toggled on and off via `v` and in the "ON / OFF" settings menu tab. Adjust parameters in the reference tab.

<p align="center"><img width="874" height="397" alt="grafik" src="https://github.com/user-attachments/assets/c4a217b5-8a81-4707-ad13-dc47b79df95a" /></p>

### Minor Additions / Modifications

- **Categorized** Key commands in Reference tab within the Settings. The header color is no longer hardcoded: the COLORS tab has an **`HEADER`** field (config.txt: `ColorHeader=`, default `10` = palette index 10 of 256) which drives both those category titles and the path-list titles described below.
 
  <p align="center"><img width="850" height="400" alt="grafik" src="https://github.com/user-attachments/assets/e060895e-e6fa-4132-a9c9-67e2d0ea2167" /></p>

- **Categorized cheat sheet** available via `?`.

  <p align="center"><img width="850" height="358" alt="grafik" src="https://github.com/user-attachments/assets/f7b2b425-1156-4017-b354-bae54a7a3fb9" /></p>
  
- **Icon for .exe** is now included.
- **Full resizing of main playback UI** and menus when size of terminal is changed, e.g. fullscreen
- **Added color for legends** except those that are right on the border which remain using the border_color. 
- **Search Folder by Name via `/f:`** hit `Enter` and the content is shown in the local audio pane, similar using `f`; use `c` or `ESC` to clear. 
- **Folder Order and Sorting** `f` shows only the titles in a folder of the hovering track in the list. It now shows which folder. `SHIFT+n` was added in the past to toggle between showing the file name and the meta data track name in the local audio files list. I adjusted the sorting algorithm now sorts what is shown in the respective column, adapting to the set `SHIFT+n` mode. 
- **Copy/Paste/Cut in Search and Path Fields** All search fields now allow copy/paste/cut and the necessary marking. Same for fields to add local path.
- **Clear QUEUE** In the main UI `SHIFT+x` can now be used to clear the queue incl. a warning message that pops up. 
- **Fetched Lyrics Folder** is now placed in an extra folder such that lyrics of song_a in folder_a are located in folder_a/lyrics/ and are not placed directly next to track files. Note that changing the name of the file may trigger a new lyrics fetch process (did not find a reasonable solution for that since filenames can eventually also be changed via the Windows explorer or other programs such as Midnight commander or Yazi... Hard to keep track of...
- **Search Engine Optimization** - Added fuzzy search ("X-Files" didn't show up when searched "X Files", i.e. without dash) and optimized speed for also searching through meta data (not only file titles), which in Windows took >2-3 min. after the app was started to be available (cache cap was also an issue and a bunch of subprocess handling via ffprobe, which remains as a fallback method in case the newly included ID3v2.3/2.4 frame-walker that reads TIT2/TPE1/TALB directly out can't handle the tag layout of a file for some reason...). The speedup currently covers MP3 handling FLAC (Vorbis comments), OGG/Opus, and M4A/AAC. 
- **Hotkey remapping actually works in app (see above)** — as much as I understood a bug fix rather than a feature, but it's new behavior either way.
- **Shuffle-to-next** (`#`) — a manual one-off jump to a random track, independent of the persistent Shuffle play mode, and independent of the queue (which stays FIFO on purpose).
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
- Search online radio channels incl. a key toggle to switch to radio mode (`SHIFT+r`), slight main UI changes where the progress bar could become a radio frequency bar including turning buttons, where different chosen online channels could be assigned to certain frequencies, including a fade effect with an overlay of a selection of noisy sounds when changing the channel (would limit the number of possible channels)... Key toggles for next song (`n` and shuffle next `#` could be re-used as commands to change channels; probably makes more sense than fiddling around with arrow key (I grew up with classic radios and it was fun but also daunting)); 


