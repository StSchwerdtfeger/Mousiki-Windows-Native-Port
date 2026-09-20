#pragma once
#include <cstddef>
#include <memory>
#include <string>

namespace muisc {

struct ProcResult {
    std::string out;   // combined stdout (stderr redirected away unless merge_stderr=true)
    int exit_code = -1;
    bool ok() const { return exit_code == 0; }
};

// Runs `cmd` and captures stdout. Caller is responsible for quoting any
// interpolated arguments (see shell_quote()).
ProcResult run_capture(const std::string& cmd, bool merge_stderr = false);

// Wraps a string in single quotes, safely escaping any embedded quotes, so it
// can be dropped into a command line.
//
// On Windows there is no shell involved (see process_util.cpp), but the same
// POSIX single-quote convention is kept: the Windows implementation parses the
// command string with sh-compatible word-splitting rules and then re-quotes
// each resulting argument the way CommandLineToArgvW expects. Keeping one
// convention means every call site in the codebase stays platform-agnostic.
std::string shell_quote(const std::string& s);

// ---------------------------------------------------------------------------
// Streaming variant
//
// run_capture() reads the child to completion before returning, which is fine
// for ffprobe and yt-dlp but wrong for the ffmpeg decode fallback, where the
// whole point is to consume PCM as it is produced. This exposes the same spawn
// semantics with an incremental read instead.
//
// The child is always started with stdin detached from the terminal. That is
// not cosmetic: ffmpeg reads keystrokes for interactive control whenever its
// stdin is a real console, and manipulates the terminal to do so, which used
// to leave the parent's input back in line-buffered mode and stall the entire
// render loop.
// ---------------------------------------------------------------------------

class ChildProcess {
public:
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Reads up to `n` bytes of the child's stdout. Returns the byte count,
    // 0 at EOF, or -1 on error. Blocks until at least one byte is available.
    std::ptrdiff_t read(char* buf, std::size_t n);

    // Waits for exit and returns the exit code, or -1 if it could not be
    // determined. Idempotent.
    int wait();

private:
    ChildProcess();
    friend std::unique_ptr<ChildProcess> spawn_capture(const std::string&, bool);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns nullptr if the process could not be started at all.
std::unique_ptr<ChildProcess> spawn_capture(const std::string& cmd, bool merge_stderr);

} // namespace muisc
