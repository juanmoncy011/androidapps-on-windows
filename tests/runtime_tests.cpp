// Runtime Manager tests: the host backend with real aowhost processes, and the
// adb backend against fake_adb. Paths to both come from CMake.
#include <runtime/runtime_manager.h>

#include "subprocess.h"
#include "test_apk.h"

#include <algorithm>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using runtime::AppState;
using runtime::EventKind;
using runtime::RuntimeManager;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "  FAILED: " #cond "  (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

struct Sandbox {
    fs::path dir;
    std::unique_ptr<pkgmgr::PackageManager> pm;
    Sandbox() {
        static int counter = 0;
        dir = fs::temp_directory_path() /
              ("aow_runtime_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               "_" + std::to_string(counter++));
        fs::create_directories(dir / "src");
        pm = std::make_unique<pkgmgr::PackageManager>(dir / "store");
    }
    ~Sandbox() {
        pm.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    void install(const TestApk& a) {
        const fs::path p = dir / "src" / (a.package + "_" + std::to_string(a.versionCode) + ".apk");
        CHECK(writeTestApk(p, a));
        CHECK(pm->install(p).ok());
    }
};

std::unique_ptr<runtime::RuntimeBackend> hostBackend(std::vector<std::string> extra = {}) {
    runtime::BackendConfig cfg;
    cfg.kind = "host";
    cfg.hostExe = fs::u8path(AOWHOST_EXE);
    cfg.hostExtraArgs = std::move(extra);
    cfg.stopGrace = 2s;
    std::string err;
    auto b = runtime::createBackend(cfg, &err);
    if (!b) std::cerr << "  host backend: " << err << "\n";
    return b;
}

// Collects events and lets a test wait for a particular one.
struct EventLog {
    std::mutex m;
    std::condition_variable cv;
    std::vector<runtime::RuntimeEvent> events;

    RuntimeManager::Listener listener() {
        return [this](const runtime::RuntimeEvent& e) {
            std::lock_guard<std::mutex> lock(m);
            events.push_back(e);
            cv.notify_all();
        };
    }
    bool waitFor(EventKind kind, std::chrono::milliseconds timeout = 10s) {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, timeout, [&] {
            for (const auto& e : events)
                if (e.kind == kind) return true;
            return false;
        });
    }
    std::vector<EventKind> kinds() {
        std::lock_guard<std::mutex> lock(m);
        std::vector<EventKind> k;
        for (const auto& e : events) k.push_back(e.kind);
        return k;
    }
};

void testProcessHelpers() {
    using namespace runtime::detail;
    auto r = execCapture({FAKE_ADB_EXE, "echo", "a b", "say \"hi\"", "back\\slash\\", "", "\xC3\xBC"});
    CHECK(r.ok());
    r.output.erase(std::remove(r.output.begin(), r.output.end(), '\r'), r.output.end());  // Windows text mode
    CHECK(r.output == "a b|say \"hi\"|back\\slash\\||\xC3\xBC\n");  // arguments survive quoting intact

    const auto missing = execCapture({"/definitely/not/here/adb"});
    CHECK(!missing.error.empty());

    const auto slow = execCapture({FAKE_ADB_EXE, "sleep", "5000"}, 300ms);
    CHECK(slow.timedOut);

    std::string err;
    auto child = ChildProcess::spawn({AOWHOST_EXE, "--package", "com.x", "--exit-after-ms", "100", "--exit-code", "7"}, &err);
    CHECK(child != nullptr);
    if (!child) return;
    const std::string token = processToken(child->pid());
    CHECK(!token.empty());
    CHECK(processAlive(child->pid(), token));
    CHECK(!processAlive(child->pid(), "not-the-same-process"));
    CHECK(child->wait(5s) == 7);
    CHECK(!processAlive(child->pid(), token));
}

void testHostLaunchAndStop() {
    Sandbox sb;
    TestApk app;
    sb.install(app);
    RuntimeManager rt(*sb.pm, hostBackend());
    EventLog log;
    rt.setListener(log.listener());

    const auto r = rt.launch(app.package);
    CHECK(r.ok);
    CHECK(!r.alreadyRunning);
    CHECK(r.session.state == AppState::Running);
    CHECK(r.session.pid > 0);
    CHECK(r.session.label == "Example");
    CHECK(runtime::detail::processAlive(r.session.pid, r.session.processToken));

    const auto again = rt.launch(app.package);  // single instance
    CHECK(again.ok && again.alreadyRunning);
    CHECK(again.session.id == r.session.id);

    rt.refresh();
    CHECK(rt.running().size() == 1);
    CHECK(rt.running(app.package).has_value());

    std::string err;
    CHECK(rt.stop(app.package, &err));
    CHECK(!runtime::detail::processAlive(r.session.pid, r.session.processToken));
    CHECK(rt.running().empty());
    CHECK(!rt.stop(app.package, &err));
    CHECK(err.find("not running") != std::string::npos);

    const auto hist = rt.history();
    CHECK(hist.size() == 1);
    if (!hist.empty()) {
        CHECK(hist[0].state == AppState::Stopped);
        CHECK(hist[0].endedAt >= hist[0].startedAt);
    }
    CHECK((log.kinds() == std::vector<EventKind>{EventKind::Started, EventKind::Stopped}));

    const auto missing = rt.launch("com.not.installed");
    CHECK(!missing.ok);
    CHECK(missing.error.find("not installed") != std::string::npos);
}

void testCrashAndExitDetection() {
    Sandbox sb;
    TestApk app;
    sb.install(app);
    {
        RuntimeManager rt(*sb.pm, hostBackend({"--exit-after-ms", "300", "--exit-code", "3"}));
        EventLog log;
        rt.setListener(log.listener());
        rt.startMonitor(50ms);
        CHECK(rt.launch(app.package).ok);
        CHECK(log.waitFor(EventKind::Crashed));
        rt.stopMonitor();
        const auto hist = rt.history(1);
        CHECK(hist.size() == 1 && hist[0].state == AppState::Crashed && hist[0].exitCode == 3);
        CHECK(rt.running().empty());
    }
    {
        RuntimeManager rt(*sb.pm, hostBackend({"--exit-after-ms", "200", "--exit-code", "0"}));
        EventLog log;
        rt.setListener(log.listener());
        rt.startMonitor(50ms);
        CHECK(rt.launch(app.package).ok);  // a new session after the crash
        CHECK(log.waitFor(EventKind::Exited));
        rt.stopMonitor();
        const auto hist = rt.history();
        CHECK(hist.size() == 2 && hist[0].state == AppState::Exited && hist[0].exitCode == 0);
    }
}

void testSharedAcrossManagers() {
    Sandbox sb;
    TestApk app;
    sb.install(app);
    RuntimeManager a(*sb.pm, hostBackend());
    RuntimeManager b(*sb.pm, hostBackend());  // e.g. the aowrt CLI next to the Launcher

    const auto r = a.launch(app.package);
    CHECK(r.ok);
    b.refresh();
    CHECK(b.running().size() == 1);  // b adopts a's session: same database, liveness by pid
    const auto dup = b.launch(app.package);
    CHECK(dup.ok && dup.alreadyRunning && dup.session.id == r.session.id);

    EventLog log;  // a hears about b's actions on its next refresh
    a.setListener(log.listener());
    a.refresh();
    std::string err;
    CHECK(b.stop(app.package, &err));
    a.refresh();
    CHECK(a.running().empty());
    CHECK((log.kinds() == std::vector<EventKind>{EventKind::Stopped}));
    CHECK(b.launch(app.package).ok);
    a.refresh();
    CHECK((log.kinds() == std::vector<EventKind>{EventKind::Stopped, EventKind::Started}));
    CHECK(b.stop(app.package, &err));
    CHECK(a.history(1).size() == 1 && a.history(1)[0].state == AppState::Stopped);
}

long long fakeAdbInstalls(const fs::path& state) {
    std::ifstream in(state);
    std::string k;
    long long v = 0;
    while (in >> k)
        if (k == "installs") { in >> v; return v; }
        else in.ignore(1 << 20, '\n');
    return 0;
}

void testAdbBackend() {
    Sandbox sb;
    const fs::path state = sb.dir / "fake_adb_state.txt";
    setEnv("FAKE_ADB_STATE", state.u8string());

    CHECK(runtime::adbDevices(FAKE_ADB_EXE) == std::vector<std::string>{"emulator-5554"});
    runtime::BackendConfig cfg;
    cfg.kind = "adb";
    cfg.adb = FAKE_ADB_EXE;
    std::string err;
    cfg.serial = "wrong-serial";
    CHECK(runtime::createBackend(cfg, &err) == nullptr);
    CHECK(err.find("not connected") != std::string::npos);
    cfg.serial.clear();
    auto backend = runtime::createBackend(cfg, &err);
    CHECK(backend != nullptr);
    if (!backend) return;
    CHECK(backend->name() == "adb" && backend->device() == "emulator-5554");

    TestApk app;
    sb.install(app);
    RuntimeManager rt(*sb.pm, std::move(backend));
    EventLog log;
    rt.setListener(log.listener());

    auto r = rt.launch(app.package);  // first launch deploys the APK to the device
    CHECK(r.ok);
    CHECK(r.session.pid >= 4000);
    CHECK(fakeAdbInstalls(state) == 1);
    CHECK(rt.launch(app.package).alreadyRunning);

    CHECK(rt.stop(app.package, &err));
    CHECK(rt.launch(app.package).ok);
    CHECK(fakeAdbInstalls(state) == 1);  // same version already on the device

    TestApk v2 = app;
    v2.versionCode = 2;
    sb.install(v2);
    CHECK(rt.stop(app.package, &err));
    r = rt.launch(app.package);
    CHECK(r.ok);
    CHECK(fakeAdbInstalls(state) == 2);  // updated in the store -> redeployed
    CHECK(r.session.versionCode == 2);

    // The app dies on the device -> the monitor notices.
    runtime::detail::execCapture({FAKE_ADB_EXE, "kill", app.package});
    rt.startMonitor(50ms);
    CHECK(log.waitFor(EventKind::Exited));
    rt.stopMonitor();
    CHECK(rt.running().empty());

    // "auto" picks adb when a device is connected.
    setEnv("AOW_ADB", FAKE_ADB_EXE);
    runtime::BackendConfig autoCfg;
    auto picked = runtime::createBackend(autoCfg, &err);
    CHECK(picked && picked->name() == "adb");
    setEnv("AOW_ADB", "");
}

} // namespace

int main() {
    setEnv("AOW_RUNTIME", "");
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"process helpers", testProcessHelpers},
        {"host launch and stop", testHostLaunchAndStop},
        {"crash and exit detection", testCrashAndExitDetection},
        {"shared across managers", testSharedAcrossManagers},
        {"adb backend", testAdbBackend},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        try {
            fn();
        } catch (const std::exception& e) {
            std::cerr << "  EXCEPTION: " << e.what() << "\n";
            ++g_failures;
        }
        std::cout << (g_failures == before ? "[pass] " : "[FAIL] ") << name << "\n";
    }
    std::cout << (g_failures ? "FAILED" : "all tests passed") << "\n";
    return g_failures ? 1 : 0;
}
