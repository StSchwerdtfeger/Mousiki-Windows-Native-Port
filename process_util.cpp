#include "process_util.h"
#include "console_log.h"

#include <array>
#include <cstring>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include "win_compat.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace muisc {

std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

// =====================================================================
// POSIX implementation
// =====================================================================
#if !defined(_WIN32)

struct ChildProcess::Impl {
    int   out_fd = -1;
    pid_t pid    = -1;
    int   exit_code = -1;
    bool  reaped = false;
};

ChildProcess::ChildProcess() : impl_(new Impl) {}

ChildProcess::~ChildProcess() {
    if (impl_->out_fd >= 0) ::close(impl_->out_fd);
    wait();
}

std::ptrdiff_t ChildProcess::read(char* buf, std::size_t n) {
    if (impl_->out_fd < 0) return -1;
    return static_cast<std::ptrdiff_t>(::read(impl_->out_fd, buf, n));
}

int ChildProcess::wait() {
    if (impl_->reaped) return impl_->exit_code;
    impl_->reaped = true;
    int status = 0;
    if (impl_->pid > 0 && waitpid(impl_->pid, &status, 0) == impl_->pid && WIFEXITED(status)) {
        impl_->exit_code = WEXITSTATUS(status);
    }
    return impl_->exit_code;
}

// Deliberately NOT popen()/fork()+exec() -- popen() forks, and fork()ing a
// multithreaded process is unsafe: if another thread holds a libc lock
// (malloc's arena lock, etc.) at the instant of fork(), the child inherits
// that lock permanently held with no thread left alive to release it, and can
// hang forever the next time it needs that lock -- intermittently, depending
// on timing. posix_spawn() is specified to be safe to call from a
// multithreaded process, which is why this uses it instead.
std::unique_ptr<ChildProcess> spawn_capture(const std::string& cmd, bool merge_stderr) {
    int out_pipe[2];
    if (pipe(out_pipe) != 0) return nullptr;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    // Redirecting every child's stdin to /dev/null means no subprocess we
    // spawn can ever see or touch our terminal, regardless of what that
    // program's stdin behavior is.
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    if (merge_stderr) {
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

    // posix_spawnp (not posix_spawn) + a bare "sh" resolves through PATH
    // instead of assuming /bin/sh exists -- Termux's filesystem lives under
    // its own prefix, not the standard FHS layout.
    const char* argv[] = {"sh", "-c", cmd.c_str(), nullptr};
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, "sh", &actions, nullptr, const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out_pipe[1]);

    if (rc != 0) {
        close(out_pipe[0]);
        ConsoleLog::instance().log_command(cmd, std::string("spawn failed: ") + std::strerror(rc), -1);
        return nullptr;
    }

    std::unique_ptr<ChildProcess> child(new ChildProcess());
    child->impl_->out_fd = out_pipe[0];
    child->impl_->pid    = pid;
    return child;
}

// =====================================================================
// Windows implementation
// =====================================================================
#else

namespace {

// sh-compatible word splitting. The command strings this codebase builds use
// exactly three quoting constructs -- bare words, '...' from shell_quote(),
// and hand-written "..." around things like yt-dlp's --match-filters
// expression -- so that is all this needs to understand. There is no
// globbing, no redirection and no pipeline anywhere in the call sites, which
// is why no shell is needed on this platform at all.
//
// Going through cmd.exe /c instead would be actively worse: yt-dlp's
// --match-filters value contains '&', which cmd would try to parse as a
// command separator, and cmd's own quoting rules do not compose with the
// POSIX ones the rest of the codebase already speaks.
std::vector<std::string> tokenize(const std::string& cmd) {
    std::vector<std::string> args;
    std::string cur;
    bool have_cur = false;
    enum class Q { None, Single, Double } q = Q::None;

    for (size_t i = 0; i < cmd.size(); ++i) {
        char c = cmd[i];
        if (q == Q::None) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (have_cur) { args.push_back(cur); cur.clear(); have_cur = false; }
                continue;
            }
            have_cur = true;
            if (c == '\'')      { q = Q::Single; continue; }
            if (c == '"')       { q = Q::Double; continue; }
            // Backslash escapes the next character. In practice this only
            // fires for the '\'' sequence shell_quote() emits for an embedded
            // apostrophe -- Windows path separators always arrive inside
            // single quotes, where a backslash is literal.
            if (c == '\\' && i + 1 < cmd.size()) { cur += cmd[++i]; continue; }
            cur += c;
        } else if (q == Q::Single) {
            if (c == '\'') { q = Q::None; continue; }
            cur += c;
        } else {
            if (c == '"') { q = Q::None; continue; }
            if (c == '\\' && i + 1 < cmd.size() && (cmd[i + 1] == '"' || cmd[i + 1] == '\\')) {
                cur += cmd[++i];
                continue;
            }
            cur += c;
        }
    }
    if (have_cur) args.push_back(cur);
    return args;
}

// The inverse: CommandLineToArgvW's quoting rules, which are what
// CreateProcessW hands the child. Backslashes are only special immediately
// before a quote, where they must be doubled.
std::string quote_arg(const std::string& arg, bool force) {
    bool needs = force || arg.empty() ||
                 arg.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needs) return arg;

    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');   // trailing run precedes the closing quote
    out += '"';
    return out;
}

std::wstring widen_utf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

bool ends_with_ci(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    return _stricmp(s.c_str() + (s.size() - n), suffix) == 0;
}

HANDLE open_nul(DWORD access) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa, OPEN_EXISTING, 0, nullptr);
}

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST \
    ProcThreadAttributeValue(9, FALSE, TRUE, FALSE)
#endif

// RAII wrapper around a PROC_THREAD_ATTRIBUTE_LIST restricted to an explicit
// set of handles.
//
// Plain `bInheritHandles=TRUE` (what the first version of this file used)
// inherits *every* inheritable handle open anywhere in the process at the
// instant CreateProcess snapshots the handle table -- not just the ones this
// call put in STARTUPINFO. mousiki spawns subprocesses from several threads
// concurrently by design (the background per-track metadata sweep in
// launch_row_meta_resolver(), a track's own ffprobe/ffmpeg calls, yt-dlp
// resolution, the lyrics helper), so two CreateProcess calls landing close
// together is routine, not an edge case. When that happens, a handle one
// thread just created (or is about to close) for its own spawn can get
// duplicated into a completely different, concurrently-starting child --
// there's no synchronization protecting the process-wide handle table
// between "open the pipe" and "call CreateProcess". Whichever spawn ends up
// with an extra, unexpected write handle to its stdout pipe never sees EOF
// on that pipe, because something it doesn't know about is still holding it
// open; ReadFile() in ChildProcess::read() then blocks forever. If that
// happens to the metadata probe inside App::launch_load_async() -- which
// playback genuinely waits on before calling Player::play() -- the result
// looks exactly like "the track just never starts", intermittently, with no
// error anywhere.
//
// An explicit handle list closes this off completely: CreateProcess is told
// precisely which handles this one call may inherit, so what any other
// thread is doing to the process-wide handle table at the same moment
// becomes irrelevant.
class HandleInheritList {
public:
    explicit HandleInheritList(std::vector<HANDLE> handles) : handles_(std::move(handles)) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        buf_.resize(size);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buf_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) {
            list_ = nullptr;
            return;
        }
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       handles_.data(), handles_.size() * sizeof(HANDLE),
                                       nullptr, nullptr)) {
            DeleteProcThreadAttributeList(list_);
            list_ = nullptr;
        }
    }
    ~HandleInheritList() { if (list_) DeleteProcThreadAttributeList(list_); }
    HandleInheritList(const HandleInheritList&) = delete;
    HandleInheritList& operator=(const HandleInheritList&) = delete;

    bool valid() const { return list_ != nullptr; }
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }

private:
    std::vector<HANDLE> handles_;   // must outlive list_ -- UpdateProcThreadAttribute stores the pointer, not a copy
    std::vector<char> buf_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

} // namespace

struct ChildProcess::Impl {
    HANDLE out_read = INVALID_HANDLE_VALUE;
    HANDLE process  = INVALID_HANDLE_VALUE;
    int    exit_code = -1;
    bool   reaped = false;
};

ChildProcess::ChildProcess() : impl_(new Impl) {}

ChildProcess::~ChildProcess() {
    if (impl_->out_read != INVALID_HANDLE_VALUE) CloseHandle(impl_->out_read);
    impl_->out_read = INVALID_HANDLE_VALUE;
    wait();
    if (impl_->process != INVALID_HANDLE_VALUE) CloseHandle(impl_->process);
}

std::ptrdiff_t ChildProcess::read(char* buf, std::size_t n) {
    if (impl_->out_read == INVALID_HANDLE_VALUE) return -1;
    DWORD got = 0;
    if (!ReadFile(impl_->out_read, buf, static_cast<DWORD>(n), &got, nullptr)) {
        // The child closing its end of the pipe surfaces as ERROR_BROKEN_PIPE
        // rather than a zero-byte read; that is a clean EOF, not a failure.
        return (GetLastError() == ERROR_BROKEN_PIPE) ? 0 : -1;
    }
    return static_cast<std::ptrdiff_t>(got);
}

int ChildProcess::wait() {
    if (impl_->reaped) return impl_->exit_code;
    impl_->reaped = true;
    if (impl_->process == INVALID_HANDLE_VALUE) return impl_->exit_code;
    WaitForSingleObject(impl_->process, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(impl_->process, &code)) impl_->exit_code = static_cast<int>(code);
    return impl_->exit_code;
}

std::unique_ptr<ChildProcess> spawn_capture(const std::string& cmd, bool merge_stderr) {
    std::vector<std::string> args = tokenize(cmd);
    if (args.empty()) return nullptr;

    // CreateProcess only ever appends ".exe" when resolving a bare name, so a
    // yt-dlp installed by pipx or scoop as a .cmd shim would otherwise look
    // like "yt-dlp is not installed".
    std::string exe = win_find_executable(args[0]);
    if (exe.empty()) {
        ConsoleLog::instance().log_command(cmd, "spawn failed: '" + args[0] + "' not found on PATH", -1);
        return nullptr;
    }

    // A .cmd/.bat shim is a script, not an image -- it has to be handed to
    // the command interpreter. Everything gets force-quoted in that case so
    // cmd.exe cannot reinterpret '&' in a playlist URL as a separator.
    bool via_cmd = ends_with_ci(exe, ".cmd") || ends_with_ci(exe, ".bat");

    std::string cmdline;
    if (via_cmd) {
        cmdline = "cmd.exe /c " + quote_arg(exe, true);
    } else {
        cmdline = quote_arg(exe, false);
    }
    for (size_t i = 1; i < args.size(); ++i) {
        cmdline += ' ';
        cmdline += quote_arg(args[i], via_cmd);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE out_read = INVALID_HANDLE_VALUE, out_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) return nullptr;
    // Our end of the pipe must not leak into the child, or the read loop
    // below never sees EOF: the child would still be holding a write handle
    // open and ReadFile would block forever after the real writer exits.
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul_in  = open_nul(GENERIC_READ);
    HANDLE nul_err = merge_stderr ? INVALID_HANDLE_VALUE : open_nul(GENERIC_WRITE);

    STARTUPINFOEXW six{};
    six.StartupInfo.cb = sizeof(six);
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdInput  = nul_in;
    six.StartupInfo.hStdOutput = out_write;
    six.StartupInfo.hStdError  = merge_stderr ? out_write : nul_err;

    // The list passed here must be exactly the set of handles this process
    // needs -- CreateProcess rejects the call if a STARTUPINFO std handle is
    // set but missing from the list. de-duplicated because merge_stderr
    // reuses out_write for both stdout and stderr, and the same HANDLE value
    // must not appear twice.
    std::vector<HANDLE> inherit = {nul_in, out_write};
    if (!merge_stderr) inherit.push_back(nul_err);
    HandleInheritList handle_list(inherit);

    PROCESS_INFORMATION pi{};
    std::wstring wcmdline = widen_utf8(cmdline);
    wcmdline.push_back(L'\0');   // CreateProcessW may modify the buffer in place

    // CREATE_NO_WINDOW is the Windows counterpart of the /dev/null stdin
    // redirect: the child gets no console of its own and no access to ours,
    // so ffmpeg cannot read our keystrokes or leave our console mode altered,
    // and yt-dlp does not flash a black window on every download.
    //
    // EXTENDED_STARTUPINFO_PRESENT + the attribute list is what actually
    // enforces the explicit handle set below -- without this flag Windows
    // ignores lpAttributeList entirely and falls back to inheriting
    // everything, silently reintroducing the race this exists to close.
    // If the attribute list itself failed to build (old SDK / OS too old --
    // shouldn't happen on any supported Windows release, but this is not a
    // path worth hard-failing on), fall back to the old whole-process
    // inheritance rather than not spawning at all.
    DWORD flags = CREATE_NO_WINDOW;
    LPSTARTUPINFOW si_ptr = &six.StartupInfo;
    if (handle_list.valid()) {
        flags |= EXTENDED_STARTUPINFO_PRESENT;
        six.lpAttributeList = handle_list.get();
    } else {
        // Not using the extended form after all -- cb must describe the
        // plain STARTUPINFOW Windows will actually interpret it as.
        six.StartupInfo.cb = sizeof(STARTUPINFOW);
    }

    BOOL ok = CreateProcessW(nullptr, &wcmdline[0], nullptr, nullptr,
                             /*bInheritHandles=*/TRUE, flags,
                             nullptr, nullptr, si_ptr, &pi);

    CloseHandle(out_write);
    if (nul_in  != INVALID_HANDLE_VALUE) CloseHandle(nul_in);
    if (nul_err != INVALID_HANDLE_VALUE) CloseHandle(nul_err);

    if (!ok) {
        CloseHandle(out_read);
        ConsoleLog::instance().log_command(
            cmd, "spawn failed: CreateProcess error " + std::to_string(GetLastError()), -1);
        return nullptr;
    }
    CloseHandle(pi.hThread);

    std::unique_ptr<ChildProcess> child(new ChildProcess());
    child->impl_->out_read = out_read;
    child->impl_->process  = pi.hProcess;
    return child;
}

#endif // _WIN32

// =====================================================================
// Shared
// =====================================================================

ProcResult run_capture(const std::string& cmd, bool merge_stderr) {
    ProcResult result;

    std::unique_ptr<ChildProcess> child = spawn_capture(cmd, merge_stderr);
    if (!child) {
        result.exit_code = -1;
        return result;   // spawn_capture already logged the reason
    }

    std::array<char, 4096> buf{};
    std::ostringstream oss;
    std::ptrdiff_t n;
    while ((n = child->read(buf.data(), buf.size())) > 0) {
        oss.write(buf.data(), static_cast<std::streamsize>(n));
    }
    result.out = oss.str();
    result.exit_code = child->wait();

    ConsoleLog::instance().log_command(cmd, result.out, result.exit_code);
    return result;
}

} // namespace muisc
