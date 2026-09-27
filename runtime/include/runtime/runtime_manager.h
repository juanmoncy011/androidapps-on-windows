#pragma once
// runtime - Runtime Manager (M3): launches installed Android apps in a runtime
// backend and tracks their processes (running / exited / crashed / stopped).
//
// Backends:
//   adb  - a real Android runtime reached through adb: an emulator, a device,
//          Windows Subsystem for Android, BlueStacks, ... The APK is deployed
//          from the Package Manager store on first launch or after an update.
//   host - a simulated runtime for demos and tests without Android: each app runs
//          as a stand-in native process (aowhost) that can be tracked the same way.
//
// Sessions are recorded in runtime.db next to packages.db, so any process (the
// Launcher, the aowrt CLI) sees the same set of running apps.
#include <pkgmgr/package_manager.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace runtime {

enum class AppState {
    Running,
    Exited,   // ended on its own (exit code 0 or unknown)
    Crashed,  // ended on its own with a non-zero exit code
    Stopped,  // ended by RuntimeManager::stop()
    Failed,   // could not be launched
};

const char* toString(AppState s);

struct AppSession {
    std::int64_t id = 0;
    std::string packageName;
    std::string label;
    std::int64_t versionCode = 0;
    std::string backend;       // "adb" / "host"
    std::string device;        // adb serial, or "local"
    std::int64_t pid = 0;      // process id inside the runtime
    std::string processToken;  // guards against pid reuse
    AppState state = AppState::Running;
    std::int64_t startedAt = 0;  // unix seconds
    std::int64_t endedAt = 0;    // 0 while running
    std::optional<int> exitCode;
    std::string detail;          // error or extra information

    std::string displayName() const { return label.empty() ? packageName : label; }
};

// ---- backends ----------------------------------------------------------------

struct Liveness {
    bool alive = false;
    std::optional<int> exitCode;  // when known
    std::string detail;
};

class RuntimeBackend {
public:
    struct Process {
        std::int64_t pid = 0;
        std::string token;
    };

    virtual ~RuntimeBackend() = default;
    virtual std::string name() const = 0;
    virtual std::string device() const = 0;
    virtual std::string description() const = 0;  // for status bars / CLI output

    virtual bool launch(const pkgmgr::InstalledPackage& pkg, Process& out, std::string& error) = 0;
    virtual Liveness check(const AppSession& session) = 0;
    virtual bool stop(const AppSession& session, std::string& error) = 0;
};

struct BackendConfig {
    std::string kind = "auto";               // auto | adb | host  (env AOW_RUNTIME overrides "auto")
    std::filesystem::path adb;               // adb executable; found automatically if empty
    std::string serial;                      // adb device; first connected device if empty
    std::filesystem::path hostExe;           // aowhost; next to the running program if empty
    std::vector<std::string> hostExtraArgs;  // passed to aowhost (tests use --exit-after-ms)
    std::chrono::milliseconds stopGrace{3000};
};

// "auto" picks adb when adb is installed and a device is connected, else host.
// Returns nullptr and sets *error if the requested backend is unavailable.
std::unique_ptr<RuntimeBackend> createBackend(const BackendConfig& config, std::string* error = nullptr);

// Locates adb: AOW_ADB, ANDROID_HOME / ANDROID_SDK_ROOT, the default SDK folder, then PATH.
std::optional<std::filesystem::path> findAdb();
// Serials of devices in the "device" state.
std::vector<std::string> adbDevices(const std::filesystem::path& adb);

// ---- manager -----------------------------------------------------------------

struct LaunchResult {
    bool ok = false;
    bool alreadyRunning = false;  // the app was running; nothing new was started
    AppSession session;
    std::string error;
};

enum class EventKind { Started, Exited, Crashed, Stopped };
struct RuntimeEvent {
    EventKind kind;
    AppSession session;
};

struct RuntimeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class RuntimeManager {
public:
    using Listener = std::function<void(const RuntimeEvent&)>;

    // Opens <pm.root()>/runtime.db. Throws RuntimeError.
    RuntimeManager(pkgmgr::PackageManager& pm, std::unique_ptr<RuntimeBackend> backend);
    ~RuntimeManager();  // stops the monitor; running apps keep running
    RuntimeManager(const RuntimeManager&) = delete;
    RuntimeManager& operator=(const RuntimeManager&) = delete;

    const RuntimeBackend& backend() const { return *backend_; }

    // Starts an installed app. One instance per app: if it is already running,
    // returns that session with alreadyRunning = true.
    LaunchResult launch(const std::string& packageName);
    bool stop(const std::string& packageName, std::string* error = nullptr);

    // Re-checks every running session of this backend/device and records exits.
    // Also reports sessions started or ended by other processes sharing runtime.db.
    void refresh();

    std::optional<AppSession> running(const std::string& packageName) const;
    std::vector<AppSession> running() const;  // as of the last refresh
    std::vector<AppSession> history(std::size_t limit = 50) const;  // newest first

    // Events fire on the thread that detected them (the monitor thread or the caller).
    void setListener(Listener listener);
    void startMonitor(std::chrono::milliseconds interval = std::chrono::milliseconds(1000));
    void stopMonitor();

private:
    struct Db;
    std::vector<RuntimeEvent> refreshLocked(const std::string& onlyPackage);
    void reconcileLocked(std::vector<RuntimeEvent>& events);
    std::vector<AppSession> querySessions(const std::string& where, const std::string& arg, std::size_t limit) const;
    void emit(const std::vector<RuntimeEvent>& events);

    pkgmgr::PackageManager& pm_;
    std::unique_ptr<RuntimeBackend> backend_;
    std::unique_ptr<Db> db_;
    mutable std::mutex mutex_;
    std::set<std::int64_t> known_;  // running session ids as of the last reconcile
    bool knownInit_ = false;

    std::mutex listenerMutex_;
    Listener listener_;

    std::thread monitor_;
    std::mutex monitorMutex_;
    std::condition_variable monitorWake_;
    bool monitorStop_ = false;
};

} // namespace runtime
