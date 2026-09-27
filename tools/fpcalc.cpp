// ---------------------------------------------------------------------------
// tools/fpcalc.cpp -- AcoustID fingerprints, built from vendored Chromaprint.
// ---------------------------------------------------------------------------
// This is a drop-in replacement for Chromaprint's own cmd/fpcalc.cpp. Mousiki's
// scripts/fetch_meta.py runs
//
//     fpcalc -json -length 120 <file>
//
// and parses `{"duration": ..., "fingerprint": ...}` from stdout, so it must
// not be able to tell the two apart:
//
//   * the fingerprint is Chromaprint's compressed, base64-encoded form
//     (chromaprint_get_fingerprint) -- exactly what the AcoustID API expects;
//   * duration is the container duration, printed with two decimals;
//   * failures are a single `ERROR: ...` line on stderr with exit status 2
//     (the fetch script reports the *last* stderr line as the failure reason).
//
// The one deliberate difference is decoding. Upstream's fpcalc *links* FFmpeg,
// which would drag an entire FFmpeg development build into third_party/. ffmpeg
// is already a hard mousiki dependency (tag writing, Opus playback, ffprobe),
// so this tool spawns it and reads raw PCM from its stdout instead. The
// conversion target is not hard-coded either: it asks Chromaprint for the
// sample rate and channel count it wants (11025 Hz / mono today) and passes
// them to ffmpeg, exactly the way upstream asks its reader for them.
//
// Supported options: -length/-t SECS, -algorithm NUM, -raw, -signed, -json,
// -text, -plain, -version, -h. Upstream's raw-input flags (-format/-rate/
// -channels) and chunked output (-chunk/-overlap/-ts) are not implemented and
// report `Unknown option`, which mousiki never asks for.
//
// Chromaprint is MIT/LGPL-2.1 -- see third_party/chromaprint/LICENSE.md.
// ---------------------------------------------------------------------------

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "chromaprint.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

enum Format { FORMAT_TEXT, FORMAT_JSON, FORMAT_PLAIN };

struct Options {
    Format format = FORMAT_TEXT;
    bool raw = false;           // comma-separated uncompressed fingerprint
    bool signed_raw = false;    // ... as signed integers (pg_acoustid compat)
    int algorithm = CHROMAPRINT_ALGORITHM_DEFAULT;
    double length = 120.0;      // -length: seconds of audio to fingerprint
    std::vector<std::string> files;
};

// What one ffmpeg run produced.
struct DecodeResult {
    bool started = false;       // ffmpeg was found and ran
    bool spawn_failed = false;  // started == false because it could not run
    std::string spawn_error;    // why, when spawn_failed
    int exit_code = -1;
    size_t frames = 0;          // frames actually fed to Chromaprint
    double duration = 0.0;      // container duration in seconds (0 = unknown)
    std::string stderr_text;    // ffmpeg's own diagnostics, for error reporting
};

const char *const kHelp =
    "Usage: fpcalc [OPTIONS] FILE [FILE...]\n"
    "\n"
    "Generate AcoustID fingerprints from audio files.\n"
    "Built from the Chromaprint sources in third_party/chromaprint/ and\n"
    "decoded with ffmpeg, so no FFmpeg development files are needed.\n"
    "\n"
    "Options:\n"
    "  -length SECS   Restrict the duration of the processed input audio (default 120)\n"
    "  -algorithm NUM Algorithm number (default 2)\n"
    "  -raw           Output fingerprints in the uncompressed format\n"
    "  -signed        Output uncompressed fingerprints as signed integers\n"
    "  -json          Print the output in JSON format\n"
    "  -text          Print the output in text format\n"
    "  -plain         Print just the fingerprint in text format\n"
    "  -version       Print version information\n"
    "\n"
    "Not supported by this build: -format/-rate/-channels (raw input),\n"
    "-chunk/-overlap/-ts (chunked output).\n";

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------

bool parse_positive_number(const std::string &s, double *out) {
    char *end = nullptr;
    const double v = strtod(s.c_str(), &end);
    if (end == s.c_str() || (end != nullptr && *end != '\0') || !(v > 0.0)) return false;
    *out = v;
    return true;
}

bool parse_algorithm(const std::string &s, int *out) {
    char *end = nullptr;
    const long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || (end != nullptr && *end != '\0') || v < 1 || v > 5) return false;
    *out = (int)v;
    return true;
}

std::string last_nonempty_line(const std::string &text) {
    std::string last;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) last = line;
        start = end + 1;
    }
    return last;
}

// ffmpeg prints the container duration as `Duration: HH:MM:SS.cc, start: ...`
// in its header block (which is why the tool logs at `info` level). Upstream
// reads the same value from AVFormatContext, truncated to milliseconds.
double parse_duration(const std::string &stderr_text) {
    const std::string key = "Duration:";
    size_t p = stderr_text.find(key);
    if (p == std::string::npos) return 0.0;
    p += key.size();

    int h = 0, m = 0, s = 0;
    if (sscanf(stderr_text.c_str() + p, "%d:%d:%d", &h, &m, &s) != 3) return 0.0; // "N/A"
    if (h < 0 || m < 0 || s < 0) return 0.0;

    double frac = 0.0;
    const size_t dot = stderr_text.find('.', p);
    if (dot != std::string::npos && dot < p + 12) { // still inside HH:MM:SS.xx
        std::string digits;
        for (size_t d = dot + 1; d < stderr_text.size() && isdigit((unsigned char)stderr_text[d]); ++d)
            digits += stderr_text[d];
        if (digits.size() == 1) frac = atoi(digits.c_str()) / 100.0;      // centiseconds
        else if (digits.size() == 2) frac = atoi(digits.c_str()) / 100.0;
        else if (digits.size() >= 3) frac = atoi(digits.substr(0, 3).c_str()) / 1000.0;
    }
    const double v = h * 3600.0 + m * 60.0 + s + frac;
    return v > 0.0 ? v : 0.0;
}

std::vector<std::string> ffmpeg_argv(const std::string &file, int rate, int channels, double length) {
    char num[64];
    std::vector<std::string> a;
    a.push_back("ffmpeg");
    a.push_back("-nostdin");               // never read our stdin
    a.push_back("-hide_banner");           // keep error output to the real error
    a.push_back("-v"); a.push_back("info"); // info level so `Duration:` is printed
    a.push_back("-i"); a.push_back(file);
    a.push_back("-vn");                    // audio only (raw PCM out has no video)
    a.push_back("-f"); a.push_back("s16le");   // headerless interleaved PCM
    a.push_back("-acodec"); a.push_back("pcm_s16le");
    snprintf(num, sizeof num, "%d", rate);     a.push_back("-ar"); a.push_back(num);
    snprintf(num, sizeof num, "%d", channels); a.push_back("-ac"); a.push_back(num);
    snprintf(num, sizeof num, "%g", length);   a.push_back("-t"); a.push_back(num);
    a.push_back("-");                      // ... to stdout, which we capture
    return a;
}

std::string read_whole_file(const char *path) {
    std::string out;
    FILE *f = fopen(path, "rb");
    if (!f) return out;
    char buf[4096];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string &s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string wide_to_utf8(const wchar_t *s, int len) {
    if (s == nullptr || (len == 0)) return std::string();
    if (len < 0) len = (int)wcslen(s);
    if (len <= 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string o((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, &o[0], n, nullptr, nullptr);
    return o;
}

// Quote one argv element the way CommandLineToArgvW/MSVC CRT parse it:
// backslashes are doubled before a quote, the quote itself is backslash-escaped.
std::wstring quote_windows_arg(const std::wstring &a) {
    if (a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring out;
    out += L'"';
    size_t i = 0;
    while (i < a.size()) {
        size_t backslashes = 0;
        while (i < a.size() && a[i] == L'\\') { ++backslashes; ++i; }
        if (i == a.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            ++i;
        } else {
            out.append(backslashes, L'\\');
            out += a[i++];
        }
    }
    out += L'"';
    return out;
}

// argv as UTF-8, taken from the raw UTF-16 command line. main()'s char** is
// converted through the ANSI code page on Windows and would mangle non-ASCII
// paths, which a music library will hit immediately.
std::vector<std::string> command_line_args() {
    std::vector<std::string> args;
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv == nullptr) return args;
    for (int i = 1; i < wargc; ++i) args.push_back(wide_to_utf8(wargv[i], -1));
    LocalFree(wargv);
    return args;
}

#endif // _WIN32

// ---------------------------------------------------------------------------
// decoding
// ---------------------------------------------------------------------------

// Feeds interleaved s16le to Chromaprint, at most `max_frames` frames of it.
// Extra bytes are drained but dropped -- upstream does the same with its
// `stream_limit`, so the fingerprint of a >120 s file only ever sees the first
// 120 s no matter how ffmpeg frames its output.
struct Feeder {
    ChromaprintContext *ctx;
    size_t frame_bytes;
    size_t max_frames;
    size_t frames = 0;
    std::string pending; // bytes that do not yet make a whole frame

    void consume(const char *data, size_t size) {
        pending.append(data, size);
        const size_t usable = (pending.size() / frame_bytes) * frame_bytes;
        if (usable == 0) return;
        if (frames < max_frames) {
            const size_t room = (max_frames - frames) * frame_bytes;
            const size_t use = usable < room ? usable : room;
            // chromaprint_feed() takes a count of int16 values, i.e. frames * channels.
            chromaprint_feed(ctx, reinterpret_cast<const int16_t *>(pending.data()),
                             (int)(use / sizeof(int16_t)));
            frames += use / frame_bytes;
        }
        pending.erase(0, usable);
    }
};

DecodeResult decode_and_feed(ChromaprintContext *ctx, const std::string &file,
                             int rate, int channels, size_t max_frames, double length) {
    DecodeResult r;
    const std::vector<std::string> argv = ffmpeg_argv(file, rate, channels, length);
    Feeder feeder{ctx, sizeof(int16_t) * (size_t)channels, max_frames};

#ifdef _WIN32
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += L' ';
        cmd += quote_windows_arg(utf8_to_wide(argv[i]));
    }

    // ffmpeg's stderr goes to a temp file rather than a second pipe: we read it
    // only after the process exits, which is exactly where a pipe would fill up
    // and deadlock both sides.
    wchar_t tmp_dir[MAX_PATH];
    wchar_t tmp_file[MAX_PATH];
    tmp_dir[0] = L'\0';
    tmp_file[0] = L'\0';
    std::string err_path;
    HANDLE err_handle = INVALID_HANDLE_VALUE;
    if (GetTempPathW(MAX_PATH, tmp_dir) != 0 && GetTempFileNameW(tmp_dir, L"fpc", 0, tmp_file) != 0) {
        SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        err_handle = CreateFileW(tmp_file, GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (err_handle != INVALID_HANDLE_VALUE) err_path = wide_to_utf8(tmp_file, -1);
    }

    HANDLE read_end = INVALID_HANDLE_VALUE;
    HANDLE write_end = INVALID_HANDLE_VALUE;
    SECURITY_ATTRIBUTES pipe_sa = { sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&read_end, &write_end, &pipe_sa, 0)) {
        r.spawn_failed = true;
        r.spawn_error = "could not create a pipe for ffmpeg";
        if (err_handle != INVALID_HANDLE_VALUE) CloseHandle(err_handle);
        if (!err_path.empty()) DeleteFileW(tmp_file);
        return r;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0); // only the child writes

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_end;
    si.hStdError = (err_handle != INVALID_HANDLE_VALUE) ? err_handle : GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);

    const BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (err_handle != INVALID_HANDLE_VALUE) CloseHandle(err_handle); // child keeps its copy
    if (!ok) {
        const DWORD e = GetLastError();
        CloseHandle(read_end);
        if (!err_path.empty()) DeleteFileW(tmp_file);
        r.spawn_failed = true;
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
            r.spawn_error = "ffmpeg not found - install FFmpeg (e.g. `winget install Gyan.FFmpeg`)";
        else {
            char msg[80];
            snprintf(msg, sizeof msg, "could not start ffmpeg (Windows error %lu)", (unsigned long)e);
            r.spawn_error = msg;
        }
        return r;
    }
    CloseHandle(pi.hThread);

    char buf[16384];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(read_end, buf, sizeof buf, &got, nullptr) || got == 0) break; // EOF
        feeder.consume(buf, (size_t)got);
    }
    CloseHandle(read_end);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    r.started = true;
    r.exit_code = (int)code;
    r.frames = feeder.frames;
    if (!err_path.empty()) {
        r.stderr_text = read_whole_file(err_path.c_str());
        DeleteFileW(tmp_file);
    }
    r.duration = parse_duration(r.stderr_text);
    return r;

#else  // POSIX
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        r.spawn_failed = true;
        r.spawn_error = "could not create a pipe for ffmpeg";
        return r;
    }

    char err_template[] = "/tmp/mousiki-fpcalc-XXXXXX";
    const int err_fd = mkstemp(err_template);

    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (err_fd >= 0) { close(err_fd); unlink(err_template); }
        r.spawn_failed = true;
        r.spawn_error = "could not start ffmpeg";
        return r;
    }
    if (pid == 0) {
        // Child: wire ffmpeg's stdout to the pipe, stderr to the temp file,
        // then replace ourselves. No C++ objects are created past this point.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        if (err_fd >= 0) { dup2(err_fd, STDERR_FILENO); }
        if (err_fd > STDERR_FILENO) close(err_fd);
        std::vector<char *> cargv;
        cargv.reserve(argv.size() + 1);
        for (size_t i = 0; i < argv.size(); ++i) cargv.push_back(const_cast<char *>(argv[i].c_str()));
        cargv.push_back(nullptr);
        execvp("ffmpeg", cargv.data());
        _exit(127); // shell convention: command not found
    }

    close(pipefd[1]);
    if (err_fd >= 0) close(err_fd);

    char buf[16384];
    ssize_t got = 0;
    while ((got = read(pipefd[0], buf, sizeof buf)) > 0) feeder.consume(buf, (size_t)got);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

    r.started = true;
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    r.frames = feeder.frames;
    if (err_fd >= 0) {
        r.stderr_text = read_whole_file(err_template);
        unlink(err_template);
    }
    r.duration = parse_duration(r.stderr_text);
    return r;
#endif
}

// The single `ERROR: ...` line mousiki will surface as the failure reason.
std::string decode_error_message(const DecodeResult &d, bool *echo_stderr) {
    *echo_stderr = false;
    if (d.spawn_failed) return d.spawn_error;
    if (!d.started && d.exit_code == 127)
        return "ffmpeg not found - install FFmpeg (e.g. `winget install Gyan.FFmpeg`)";

    const std::string &e = d.stderr_text;
    if (e.find("No such file or directory") != std::string::npos) {
        *echo_stderr = true;
        return "Could not open the input file (No such file or directory)";
    }
    if (e.find("matches no streams") != std::string::npos ||
        e.find("does not contain any stream") != std::string::npos) {
        *echo_stderr = true;
        return "Could not find any audio stream in the file";
    }
    // Anything else: ffmpeg's own last line is the most precise description
    // there is ("...: Invalid data found when processing input").
    const std::string last = last_nonempty_line(e);
    if (!last.empty()) return last;
    return "Could not decode the audio source";
}

// ---------------------------------------------------------------------------
// output (kept byte-compatible with upstream's PrintResult)
// ---------------------------------------------------------------------------

void print_result(ChromaprintContext *ctx, double duration, bool first, const Options &opt) {
    int size = 0;
    if (!chromaprint_get_raw_fingerprint_size(ctx, &size)) {
        fprintf(stderr, "ERROR: Could not get the fingerprinting size\n");
        exit(2);
    }
    if (size <= 0) {
        // Upstream exits on the first empty result and quietly skips later ones.
        if (first) {
            fprintf(stderr, "ERROR: Empty fingerprint\n");
            exit(2);
        }
        return;
    }

    std::string raw;
    const char *fp = nullptr;
    std::string owned;
    if (opt.raw) {
        uint32_t *data = nullptr;
        int count = 0;
        if (!chromaprint_get_raw_fingerprint(ctx, &data, &count)) {
            fprintf(stderr, "ERROR: Could not get the fingerprinting\n");
            exit(2);
        }
        for (int i = 0; i < count; ++i) {
            if (i > 0) raw += ',';
            char num[32];
            if (opt.signed_raw) snprintf(num, sizeof num, "%d", (int32_t)data[i]);
            else snprintf(num, sizeof num, "%u", data[i]);
            raw += num;
        }
        chromaprint_dealloc(data);
        fp = raw.c_str();
    } else {
        char *encoded = nullptr;
        if (!chromaprint_get_fingerprint(ctx, &encoded)) {
            fprintf(stderr, "ERROR: Could not get the fingerprinting\n");
            exit(2);
        }
        owned = encoded ? encoded : "";
        chromaprint_dealloc(encoded);
        fp = owned.c_str();
    }

    switch (opt.format) {
        case FORMAT_TEXT:
            if (!first) printf("\n");
            printf("DURATION=%d\nFINGERPRINT=%s\n", (int)duration, fp);
            break;
        case FORMAT_JSON:
            if (opt.raw) printf("{\"duration\": %.2f, \"fingerprint\": [%s]}\n", duration, fp);
            else printf("{\"duration\": %.2f, \"fingerprint\": \"%s\"}\n", duration, fp);
            break;
        case FORMAT_PLAIN:
            printf("%s\n", fp);
            break;
    }
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// argument parsing / main
// ---------------------------------------------------------------------------

bool parse_options(const std::vector<std::string> &args, Options *opt, bool *want_help,
                   bool *want_version) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "-json") {
            opt->format = FORMAT_JSON;
        } else if (a == "-text") {
            opt->format = FORMAT_TEXT;
        } else if (a == "-plain") {
            opt->format = FORMAT_PLAIN;
        } else if (a == "-raw") {
            opt->raw = true;
        } else if (a == "-signed") {
            opt->signed_raw = true;
        } else if (a == "-length" || a == "-t") {
            if (i + 1 >= args.size() || !parse_positive_number(args[i + 1], &opt->length)) {
                fprintf(stderr, "ERROR: The argument for %s must be a positive number\n", a.c_str());
                return false;
            }
            ++i;
        } else if (a == "-algorithm") {
            if (i + 1 >= args.size() || !parse_algorithm(args[i + 1], &opt->algorithm)) {
                fprintf(stderr, "ERROR: The argument for %s must be 1 - 5\n", a.c_str());
                return false;
            }
            ++i;
        } else if (a == "-version") {
            *want_version = true;
        } else if (a == "-h" || a == "-help" || a == "--help") {
            *want_help = true;
        } else if (!a.empty() && a[0] == '-') {
            fprintf(stderr, "ERROR: Unknown option %s\n", a.c_str());
            return false;
        } else {
            opt->files.push_back(a);
        }
    }
    return true;
}

int run(const std::vector<std::string> &args) {
    Options opt;
    bool want_help = false;
    bool want_version = false;
    if (!parse_options(args, &opt, &want_help, &want_version)) return 2;

    if (want_version) {
        printf("fpcalc version %s (Chromaprint built from third_party/chromaprint, decoded by ffmpeg)\n",
               chromaprint_get_version());
        return 0;
    }
    if (want_help) {
        fputs(kHelp, stdout);
        return 0;
    }
    if (opt.files.empty()) {
        fprintf(stderr, "ERROR: No input files\n");
        return 2;
    }

    bool first = true;
    for (size_t i = 0; i < opt.files.size(); ++i) {
        const std::string &file = opt.files[i];

        ChromaprintContext *ctx = chromaprint_new(opt.algorithm);
        if (ctx == nullptr) {
            fprintf(stderr, "ERROR: Could not initialize the fingerprinting process\n");
            return 2;
        }
        // Chromaprint states the format it wants; we hand the same values to
        // ffmpeg, exactly as upstream hands them to its reader.
        const int rate = chromaprint_get_sample_rate(ctx);
        const int channels = chromaprint_get_num_channels(ctx);
        if (rate <= 0 || channels <= 0) {
            fprintf(stderr, "ERROR: Invalid sample rate\n");
            chromaprint_free(ctx);
            return 2;
        }
        if (!chromaprint_start(ctx, rate, channels)) {
            fprintf(stderr, "ERROR: Could not initialize the fingerprinting process\n");
            chromaprint_free(ctx);
            return 2;
        }

        const size_t max_frames = (size_t)(opt.length * (double)rate);
        const DecodeResult dec = decode_and_feed(ctx, file, rate, channels, max_frames, opt.length);

        if (!dec.started || dec.frames == 0) {
            bool echo = false;
            const std::string message = decode_error_message(dec, &echo);
            if (echo) fputs(dec.stderr_text.c_str(), stderr);
            fprintf(stderr, "ERROR: %s\n", message.c_str());
            fflush(stderr);
            chromaprint_free(ctx);
            return 2;
        }

        if (!chromaprint_finish(ctx)) {
            fprintf(stderr, "ERROR: Could not finish the fingerprinting process\n");
            chromaprint_free(ctx);
            return 2;
        }

        print_result(ctx, dec.duration, first, opt);
        first = false;
        chromaprint_free(ctx);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    return run(command_line_args());
#else
    return run(std::vector<std::string>(argv + 1, argv + argc));
#endif
}
