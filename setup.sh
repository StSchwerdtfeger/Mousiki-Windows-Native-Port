#!/usr/bin/env bash
#
# setup.sh -- Linux / macOS equivalent of setup.ps1: installs mousiki's build
# and runtime dependencies, then configures and builds it.
#
# mousiki itself is a C++17 binary, but it shells out to three external tools
# at runtime:
#
#   ffmpeg  - decodes Opus (miniaudio's built-in decoders don't cover it, and
#             Opus is exactly what the yt-dlp cache stores), supplies ffprobe
#             track metadata, and decodes the audio that fpcalc fingerprints
#             for the AcoustID fetch.
#   yt-dlp  - online search, playlist listing and streaming.
#   python3 - runs scripts/fetch_lyrics.py (synced lyrics, needs the `requests`
#             package) and scripts/fetch_meta.py (AcoustID metadata fetch,
#             which drives fpcalc -- built by CMake from third_party/chromaprint/).
#
# All three are independent of each other: mousiki plays local files fine with
# none of them installed.
#
# Usage:
#   ./setup.sh [options]
#
# Options:
#   --skip-deps         Configure and build only; don't touch the package manager / pip.
#   --debug             Debug build (default: Release).
#   --build-type TYPE   Release or Debug.
#   --install           Also create a launcher "mousiki" in ~/.local/bin.
#   -y, --yes           Don't ask before installing packages (uses sudo if needed).
#   -h, --help          Show this help.

set -u
set -o pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build"

SKIP_DEPS=0
BUILD_TYPE="Release"
DO_INSTALL=0
ASSUME_YES=0

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------
if [ -t 1 ]; then
    C_CYAN=$'\033[36m'; C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_OFF=$'\033[0m'
else
    C_CYAN=""; C_GREEN=""; C_YELLOW=""; C_RED=""; C_OFF=""
fi

step() { printf '%s==> %s%s\n' "$C_CYAN" "$*" "$C_OFF"; }
warn() { printf '%sWarning: %s%s\n' "$C_YELLOW" "$*" "$C_OFF" >&2; }
die()  { printf '%sError: %s%s\n' "$C_RED" "$*" "$C_OFF" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

usage() { sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' | sed '$d'; }

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-deps)  SKIP_DEPS=1 ;;
        --debug)      BUILD_TYPE="Debug" ;;
        --build-type)
            shift
            [ $# -gt 0 ] || die "--build-type needs a value (Release or Debug)"
            case "$1" in
                Release|Debug) BUILD_TYPE="$1" ;;
                *) die "--build-type must be Release or Debug, got '$1'" ;;
            esac
            ;;
        --install)    DO_INSTALL=1 ;;
        -y|--yes)     ASSUME_YES=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Platform detection
# ---------------------------------------------------------------------------
OS="$(uname -s)"
case "$OS" in
    Linux)  PLATFORM="linux" ;;
    Darwin) PLATFORM="macos" ;;
    *)      die "unsupported OS '$OS' -- on Windows use setup.ps1" ;;
esac

PM=""
if [ "$PLATFORM" = "macos" ]; then
    PM="brew"
elif have apt-get; then PM="apt"
elif have dnf;     then PM="dnf"
elif have pacman;  then PM="pacman"
elif have zypper;  then PM="zypper"
elif have apk;     then PM="apk"
fi

# ---------------------------------------------------------------------------
# Package manager plumbing
# ---------------------------------------------------------------------------
SUDO=""
if [ "$PLATFORM" = "linux" ] && [ "$(id -u)" -ne 0 ]; then
    if have sudo; then SUDO="sudo"; fi
fi

APT_UPDATED=0

# pkg_install <package>...   -- returns non-zero if the install failed
pkg_install() {
    case "$PM" in
        apt)
            if [ "$APT_UPDATED" -eq 0 ]; then
                $SUDO apt-get update || return 1
                APT_UPDATED=1
            fi
            $SUDO apt-get install -y "$@" ;;
        dnf)    $SUDO dnf install -y "$@" ;;
        pacman) $SUDO pacman -S --needed --noconfirm "$@" ;;
        zypper) $SUDO zypper --non-interactive install "$@" ;;
        apk)    $SUDO apk add "$@" ;;
        brew)   brew install "$@" ;;
        *)      return 1 ;;
    esac
}

# Like pkg_install, but a failure only produces a warning (optional packages).
pkg_install_optional() {
    local what="$1"; shift
    pkg_install "$@" || warn "could not install $what ($*) -- continuing without it"
}

# ---------------------------------------------------------------------------
# Python helpers
# ---------------------------------------------------------------------------
# mousiki calls plain "python3" on Linux/macOS, so that is what has to work.
python_has_requests() { have python3 && python3 -c 'import requests' >/dev/null 2>&1; }

pip_install_requests() {
    have python3 || return 1
    python3 -m pip --version >/dev/null 2>&1 || return 1
    # Modern distros mark the system Python as "externally managed" (PEP 668)
    # and refuse a plain pip install; --user is tried first, then the override.
    python3 -m pip install --quiet --user --upgrade requests 2>/dev/null && return 0
    python3 -m pip install --quiet --user --break-system-packages --upgrade requests
}

pip_install_ytdlp() {
    have python3 || return 1
    python3 -m pip --version >/dev/null 2>&1 || return 1
    python3 -m pip install --quiet --user --upgrade yt-dlp 2>/dev/null && return 0
    python3 -m pip install --quiet --user --break-system-packages --upgrade yt-dlp
}

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
confirm_install() {
    [ "$ASSUME_YES" -eq 1 ] && return 0
    [ -t 0 ] || return 0   # non-interactive (CI, pipe): don't hang on a prompt
    local reply
    printf 'Install missing packages with %s%s? [Y/n] ' "${SUDO:+$SUDO }" "$PM"
    read -r reply
    case "$reply" in [nN]*) return 1 ;; *) return 0 ;; esac
}

install_deps() {
    if [ -z "$PM" ]; then
        warn "no supported package manager found (apt, dnf, pacman, zypper, apk, brew)."
        warn "Install a C++17 compiler, CMake >= 3.16, ffmpeg, yt-dlp, python3 and ALSA/PulseAudio"
        warn "yourself, then re-run with --skip-deps."
        return 0
    fi

    if [ "$PLATFORM" = "macos" ]; then
        if ! have brew; then
            warn "Homebrew not found. Install it from https://brew.sh, then re-run this script"
            warn "(or install cmake, ffmpeg, yt-dlp and python3 yourself and use --skip-deps)."
            return 0
        fi
        if ! xcode-select -p >/dev/null 2>&1; then
            warn "Xcode Command Line Tools are missing (they provide the C++ compiler)."
            warn "Run:  xcode-select --install   -- then re-run this script when it has finished."
            exit 1
        fi
    elif [ -n "$SUDO" ] || [ "$(id -u)" -eq 0 ]; then
        :
    else
        warn "not root and 'sudo' not found -- package installation will probably fail."
    fi

    # Work out what is missing, so that nothing is touched if everything is there.
    local need_compiler=0 need_cmake=0 need_ffmpeg=0 need_ytdlp=0 need_python=0 need_audio=0
    if ! have c++ && ! have g++ && ! have clang++; then need_compiler=1; fi
    have cmake   || need_cmake=1
    have ffmpeg  || need_ffmpeg=1
    have ffprobe || need_ffmpeg=1
    have yt-dlp  || need_ytdlp=1
    have python3 || need_python=1
    # ALSA/PulseAudio: miniaudio dlopen()s them at runtime, so there is no
    # reliable "is it installed" test that doesn't depend on the distro.
    # Installing the (tiny) dev packages is idempotent and guarantees both.
    [ "$PLATFORM" = "linux" ] && need_audio=1

    if [ $((need_compiler + need_cmake + need_ffmpeg + need_ytdlp + need_python + need_audio)) -gt 0 ]; then
        confirm_install || { warn "skipping package installation at your request."; return 0; }
    fi

    # --- compiler + cmake ---------------------------------------------------
    if [ "$need_compiler" -eq 1 ]; then
        step "Installing a C++ compiler"
        case "$PM" in
            apt)    pkg_install build-essential ;;
            dnf)    pkg_install gcc-c++ make ;;
            pacman) pkg_install base-devel ;;
            zypper) pkg_install gcc-c++ make ;;
            apk)    pkg_install build-base ;;
            brew)   : ;;  # handled by the Xcode CLT check above
        esac || warn "compiler installation failed"
    fi
    if [ "$need_cmake" -eq 1 ]; then
        step "Installing CMake"
        pkg_install cmake || warn "CMake installation failed"
    fi

    # --- audio libraries (Linux) ---------------------------------------------
    if [ "$need_audio" -eq 1 ]; then
        step "Installing ALSA / PulseAudio libraries"
        case "$PM" in
            apt)    pkg_install_optional "audio libraries" libasound2-dev libpulse-dev ;;
            dnf)    pkg_install_optional "audio libraries" alsa-lib-devel pulseaudio-libs-devel ;;
            pacman) pkg_install_optional "audio libraries" alsa-lib libpulse ;;
            zypper) pkg_install_optional "audio libraries" alsa-devel libpulse-devel ;;
            apk)    pkg_install_optional "audio libraries" alsa-lib-dev pulseaudio-dev ;;
        esac
    fi

    # --- ffmpeg --------------------------------------------------------------
    if [ "$need_ffmpeg" -eq 1 ]; then
        step "Installing FFmpeg"
        if [ "$PM" = "dnf" ]; then
            # Fedora's stock repos only carry ffmpeg-free (reduced codec set);
            # the full ffmpeg needs RPM Fusion. Try both, explain if neither works.
            pkg_install ffmpeg || pkg_install ffmpeg-free ||
                warn "FFmpeg not available from your repositories -- enable RPM Fusion (https://rpmfusion.org) and install 'ffmpeg'."
        else
            pkg_install ffmpeg || warn "FFmpeg installation failed -- Opus playback, metadata and AcoustID will not work"
        fi
    fi

    # --- python3 -------------------------------------------------------------
    if [ "$need_python" -eq 1 ]; then
        step "Installing Python 3"
        case "$PM" in
            apt)    pkg_install python3 python3-pip ;;
            dnf)    pkg_install python3 python3-pip ;;
            pacman) pkg_install python python-pip ;;
            zypper) pkg_install python3 python3-pip ;;
            apk)    pkg_install python3 py3-pip ;;
            brew)   pkg_install python ;;
        esac || warn "Python installation failed -- lyrics, fast online search and AcoustID will be unavailable"
    fi

    # --- python `requests` (lyrics only) ---------------------------------------
    if have python3 && ! python_has_requests; then
        step "Installing the Python 'requests' package (used by scripts/lrc.py)"
        # Prefer the distro package: it plays nicely with PEP 668.
        case "$PM" in
            apt|dnf|zypper) pkg_install python3-requests >/dev/null 2>&1 ;;
            pacman)         pkg_install python-requests  >/dev/null 2>&1 ;;
            apk)            pkg_install py3-requests     >/dev/null 2>&1 ;;
            *)              false ;;
        esac
        python_has_requests || pip_install_requests ||
            warn "could not install 'requests'. Lyrics will be unavailable; everything else still works."
    elif ! have python3; then
        warn "no Python 3 found. Lyrics, fast online search and AcoustID will be unavailable; everything else still works."
    fi

    # --- yt-dlp --------------------------------------------------------------
    if [ "$need_ytdlp" -eq 1 ]; then
        step "Installing yt-dlp"
        if ! pkg_install yt-dlp 2>/dev/null; then
            warn "yt-dlp is not in your package repositories -- trying pip instead"
            pip_install_ytdlp ||
                warn "could not install yt-dlp. Online search/streaming will be unavailable (see https://github.com/yt-dlp/yt-dlp#installation)."
        fi
    fi

    # --- clipboard helper (Linux, optional) -----------------------------------
    # mousiki pastes via pbpaste (macOS, built in), wl-paste (Wayland) or xclip (X11).
    if [ "$PLATFORM" = "linux" ] && ! have xclip && ! have wl-paste; then
        step "Installing a clipboard helper (optional, for pasting into search)"
        if [ -n "${WAYLAND_DISPLAY:-}" ]; then
            pkg_install_optional "clipboard helper" wl-clipboard
        else
            pkg_install_optional "clipboard helper" xclip
        fi
    fi

    # ~/.local/bin is where pip --user puts yt-dlp; warn if it isn't on PATH.
    if have yt-dlp; then :; elif [ -x "$HOME/.local/bin/yt-dlp" ]; then
        warn "yt-dlp was installed to ~/.local/bin, which is not on your PATH."
        warn "Add this to your shell profile:  export PATH=\"\$HOME/.local/bin:\$PATH\""
    fi
}

if [ "$SKIP_DEPS" -eq 0 ]; then
    install_deps
fi

# ---------------------------------------------------------------------------
# Toolchain check
# ---------------------------------------------------------------------------
have cmake || die "cmake is not on PATH. Install CMake >= 3.16 (or drop --skip-deps)."

if ! have c++ && ! have g++ && ! have clang++; then
    if [ "$PLATFORM" = "macos" ]; then
        die "no C++ compiler found. Run 'xcode-select --install' and re-run this script."
    else
        die "no C++17 compiler found. Install g++ or clang++ (e.g. build-essential) and re-run this script."
    fi
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if have nproc; then
    JOBS="$(nproc)"
elif [ "$PLATFORM" = "macos" ]; then
    JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
else
    JOBS=2
fi

step "Configuring ($BUILD_TYPE)"
# miniaudio, kissfft and chromaprint are vendored in third_party/, so
# configuring needs no network access.
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" || die "cmake configure failed"

step "Building"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$JOBS" || die "cmake build failed"

EXE=""
for cand in "$BUILD_DIR/mousiki" "$BUILD_DIR/$BUILD_TYPE/mousiki"; do
    if [ -x "$cand" ]; then EXE="$cand"; break; fi
done
[ -n "$EXE" ] || die "build reported success but the mousiki executable was not found under $BUILD_DIR"

EXE_DIR="$(dirname "$EXE")"
if [ ! -d "$EXE_DIR/scripts" ]; then
    warn "scripts/ was not copied next to the executable -- lyrics, fast online search and AcoustID will not work."
elif [ ! -x "$EXE_DIR/scripts/fpcalc" ]; then
    warn "fpcalc was not built -- the AcoustID metadata fetch will not work."
fi

# ---------------------------------------------------------------------------
# Optional launcher
# ---------------------------------------------------------------------------
# A wrapper script rather than a symlink: macOS' executable-path lookup does not
# resolve symlinks, so a symlinked binary would not find its scripts/ folder.
if [ "$DO_INSTALL" -eq 1 ]; then
    BIN_DIR="$HOME/.local/bin"
    mkdir -p "$BIN_DIR"
    LAUNCHER="$BIN_DIR/mousiki"
    printf '#!/bin/sh\nexec "%s" "$@"\n' "$EXE" > "$LAUNCHER"
    chmod +x "$LAUNCHER"
    step "Launcher created: $LAUNCHER"
    case ":$PATH:" in
        *":$BIN_DIR:"*) ;;
        *) warn "$BIN_DIR is not on your PATH. Add this to your shell profile:  export PATH=\"\$HOME/.local/bin:\$PATH\"" ;;
    esac
fi

echo
printf '%sBuilt: %s%s\n' "$C_GREEN" "$EXE" "$C_OFF"
echo "Run it with:  '$EXE'"
[ "$DO_INSTALL" -eq 1 ] && echo "or simply:    mousiki"
echo
echo "Config will be generated at: \$HOME/.config/mousiki/config.txt"
echo "Keep the scripts/ folder next to the executable if you move it."
if [ "$PLATFORM" = "linux" ]; then
    echo "Audio goes through PulseAudio/PipeWire-pulse or ALSA; use a UTF-8 terminal."
fi
