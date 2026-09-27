#pragma once
// Cross-platform process helpers (Win32 / POSIX) used by the runtime backends.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace runtime::detail {

struct ExecResult {
    int exitCode = -1;
    std::string output;  // stdout and stderr combined
    bool timedOut = false;
    std::string error;   // non-empty if the program could not be started

    bool ok() const { return error.empty() && !timedOut && exitCode == 0; }
};

// Runs argv[0] (a path, not searched in PATH) and captures its output.
ExecResult execCapture(const std::vector<std::string>& argv,
                       std::chrono::milliseconds timeout = std::chrono::seconds(30));

// A process we started and own: we can collect its exit code.
class ChildProcess {
public:
    // Starts a detached process (no console, own process group). nullptr + *error on failure.
    static std::unique_ptr<ChildProcess> spawn(const std::vector<std::string>& argv, std::string* error);
    ~ChildProcess();  // does not kill the process
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    std::int64_t pid() const { return pid_; }
    // Exit code once the process has ended, nullopt while it is running.
    std::optional<int> poll();
    std::optional<int> wait(std::chrono::milliseconds timeout);

private:
    ChildProcess() = default;
    std::int64_t pid_ = 0;
    std::optional<int> exitCode_;
#ifdef _WIN32
    void* handle_ = nullptr;
#endif
};

// Identifies one incarnation of a pid (its start time), so a reused pid is not
// mistaken for our app. Empty if no such process is running.
std::string processToken(std::int64_t pid);
bool processAlive(std::int64_t pid, const std::string& token);

// Asks the process to close (WM_CLOSE / SIGTERM), then kills it after `grace`.
bool terminateProcess(std::int64_t pid, std::chrono::milliseconds grace);

std::filesystem::path currentExecutableDir();
std::optional<std::filesystem::path> findInPath(const std::string& program);

} // namespace runtime::detail
