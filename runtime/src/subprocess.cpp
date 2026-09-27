#include "subprocess.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;
using namespace std::chrono;

namespace runtime::detail {

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string lastError() {
    const DWORD code = GetLastError();
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string msg = buf ? fs::path(buf).u8string() : "error " + std::to_string(code);
    LocalFree(buf);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' ')) msg.pop_back();
    return msg;
}

// Quote one argument so CommandLineToArgvW / the MSVC CRT parse it back unchanged.
void appendQuoted(std::wstring& cmd, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') { ++it; ++backslashes; }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') cmd.append(backslashes * 2 + 1, L'\\');
        else cmd.append(backslashes, L'\\');
        cmd += *it;
    }
    cmd += L'"';
}

std::wstring commandLine(const std::vector<std::string>& argv) {
    std::wstring cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += L' ';
        appendQuoted(cmd, widen(argv[i]));
    }
    return cmd;
}

std::string creationTime(HANDLE h) {
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(h, &created, &exited, &kernel, &user)) return {};
    return std::to_string((std::uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime);
}

} // namespace

ExecResult execCapture(const std::vector<std::string>& argv, milliseconds timeout) {
    ExecResult r;
    if (argv.empty()) { r.error = "empty command"; return r; }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) { r.error = lastError(); return r; }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine(argv);
    const std::wstring exe = widen(argv[0]);
    const BOOL started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);
    if (!started) r.error = "cannot run " + argv[0] + ": " + lastError();
    CloseHandle(writePipe);  // only the child holds the write end now
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started) { CloseHandle(readPipe); return r; }

    std::thread reader([&] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(readPipe, buf, sizeof buf, &n, nullptr) && n > 0) r.output.append(buf, n);
    });
    if (WaitForSingleObject(pi.hProcess, DWORD(timeout.count())) == WAIT_TIMEOUT) {
        r.timedOut = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    reader.join();  // EOF once the child (and anything it spawned with our pipe) has exited
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exitCode = int(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    return r;
}

std::unique_ptr<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& argv, std::string* error) {
    if (argv.empty()) { if (error) *error = "empty command"; return nullptr; }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = commandLine(argv);
    const std::wstring exe = widen(argv[0]);
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (error) *error = "cannot start " + argv[0] + ": " + lastError();
        return nullptr;
    }
    CloseHandle(pi.hThread);
    std::unique_ptr<ChildProcess> c(new ChildProcess);
    c->pid_ = std::int64_t(pi.dwProcessId);
    c->handle_ = pi.hProcess;
    return c;
}

ChildProcess::~ChildProcess() {
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
}

std::optional<int> ChildProcess::poll() { return wait(milliseconds(0)); }

std::optional<int> ChildProcess::wait(milliseconds timeout) {
    if (exitCode_) return exitCode_;
    if (WaitForSingleObject(static_cast<HANDLE>(handle_), DWORD(timeout.count())) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    exitCode_ = int(code);
    return exitCode_;
}

std::string processToken(std::int64_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return {};
    DWORD code = 0;
    std::string token;
    if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE) token = creationTime(h);
    CloseHandle(h);
    return token;
}

bool terminateProcess(std::int64_t pid, milliseconds grace) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return false;
    // Polite first: close the process's top-level windows.
    struct Ctx { DWORD pid; } ctx{DWORD(pid)};
    EnumWindows([](HWND w, LPARAM lp) -> BOOL {
        DWORD owner = 0;
        GetWindowThreadProcessId(w, &owner);
        if (owner == reinterpret_cast<Ctx*>(lp)->pid) PostMessageW(w, WM_CLOSE, 0, 0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    bool ok = WaitForSingleObject(h, DWORD(grace.count())) == WAIT_OBJECT_0;
    if (!ok) ok = TerminateProcess(h, 1) && WaitForSingleObject(h, 5000) == WAIT_OBJECT_0;
    CloseHandle(h);
    return ok;
}

fs::path currentExecutableDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n < buf.size()) { buf.resize(n); break; }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

std::optional<fs::path> findInPath(const std::string& program) {
    wchar_t buf[MAX_PATH];
    const std::wstring name = widen(program);
    if (SearchPathW(nullptr, name.c_str(), L".exe", MAX_PATH, buf, nullptr)) return fs::path(buf);
    return std::nullopt;
}

#else  // POSIX -----------------------------------------------------------------

namespace {

std::vector<char*> cArgs(const std::vector<std::string>& argv) {
    std::vector<char*> out;
    for (const auto& a : argv) out.push_back(const_cast<char*>(a.c_str()));
    out.push_back(nullptr);
    return out;
}

// Parses /proc/<pid>/stat: returns {state, starttime} or empty state if missing.
std::pair<char, std::string> procStat(std::int64_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(in, line)) return {'\0', {}};
    const auto close = line.rfind(')');  // comm may contain spaces and ')'
    if (close == std::string::npos) return {'\0', {}};
    std::vector<std::string> fields;
    std::size_t pos = close + 2;
    while (pos < line.size()) {
        const auto sp = line.find(' ', pos);
        fields.push_back(line.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos));
        if (sp == std::string::npos) break;
        pos = sp + 1;
    }
    // fields[0] is field 3 (state); starttime is field 22 -> fields[19]
    if (fields.size() < 20 || fields[0].empty()) return {'\0', {}};
    return {fields[0][0], fields[19]};
}

} // namespace

ExecResult execCapture(const std::vector<std::string>& argv, milliseconds timeout) {
    ExecResult r;
    if (argv.empty()) { r.error = "empty command"; return r; }
    int fds[2];
    if (pipe(fds) != 0) { r.error = "pipe failed"; return r; }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    pid_t pid = 0;
    auto args = cArgs(argv);
    const int rc = posix_spawn(&pid, argv[0].c_str(), &fa, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        r.error = "cannot run " + argv[0] + ": " + std::strerror(rc);
        return r;
    }

    const auto deadline = steady_clock::now() + timeout;
    char buf[4096];
    for (;;) {
        const auto left = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
        if (left <= 0) { r.timedOut = true; kill(pid, SIGKILL); break; }
        pollfd p{fds[0], POLLIN, 0};
        if (::poll(&p, 1, int(std::min<long long>(left, INT_MAX))) <= 0) continue;
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        r.output.append(buf, std::size_t(n));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return r;
}

std::unique_ptr<ChildProcess> ChildProcess::spawn(const std::vector<std::string>& argv, std::string* error) {
    if (argv.empty()) { if (error) *error = "empty command"; return nullptr; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);  // Ctrl+C in a terminal must not kill the app
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = 0;
    auto args = cArgs(argv);
    const int rc = posix_spawn(&pid, argv[0].c_str(), &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        if (error) *error = "cannot start " + argv[0] + ": " + std::strerror(rc);
        return nullptr;
    }
    std::unique_ptr<ChildProcess> c(new ChildProcess);
    c->pid_ = pid;
    return c;
}

ChildProcess::~ChildProcess() = default;

std::optional<int> ChildProcess::poll() { return wait(milliseconds(0)); }

std::optional<int> ChildProcess::wait(milliseconds timeout) {
    const auto deadline = steady_clock::now() + timeout;
    while (!exitCode_) {
        int status = 0;
        const pid_t r = waitpid(pid_t(pid_), &status, WNOHANG);
        if (r == pid_t(pid_)) {
            exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        } else if (r < 0) {
            exitCode_ = -1;  // not our child any more (already reaped)
        } else if (steady_clock::now() >= deadline) {
            break;
        } else {
            std::this_thread::sleep_for(milliseconds(10));
        }
    }
    return exitCode_;
}

std::string processToken(std::int64_t pid) {
    const auto [state, start] = procStat(pid);
    return state == '\0' || state == 'Z' || state == 'X' ? std::string() : start;
}

bool terminateProcess(std::int64_t pid, milliseconds grace) {
    const std::string token = processToken(pid);
    if (token.empty()) return false;
    kill(pid_t(pid), SIGTERM);
    const auto deadline = steady_clock::now() + grace;
    while (steady_clock::now() < deadline) {
        if (!processAlive(pid, token)) return true;
        std::this_thread::sleep_for(milliseconds(20));
    }
    kill(pid_t(pid), SIGKILL);
    for (int i = 0; i < 250 && processAlive(pid, token); ++i) std::this_thread::sleep_for(milliseconds(20));
    return !processAlive(pid, token);
}

fs::path currentExecutableDir() {
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path();
}

std::optional<fs::path> findInPath(const std::string& program) {
    const char* path = std::getenv("PATH");
    if (!path) return std::nullopt;
    std::string p(path);
    std::size_t start = 0;
    while (start <= p.size()) {
        const auto end = p.find(':', start);
        const fs::path dir = p.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const fs::path candidate = dir / program;
        if (!dir.empty() && access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

#endif

bool processAlive(std::int64_t pid, const std::string& token) {
    if (pid <= 0) return false;
    const std::string now = processToken(pid);
    return !now.empty() && (token.empty() || now == token);
}

} // namespace runtime::detail
