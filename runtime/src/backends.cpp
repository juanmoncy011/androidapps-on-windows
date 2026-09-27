#include <runtime/runtime_manager.h>

#include "subprocess.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono;
using namespace runtime::detail;

namespace runtime {
namespace {

#ifdef _WIN32
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif

std::string env(const char* name) {
#ifdef _WIN32
    const std::wstring wname = fs::u8path(name).wstring();
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    v.resize(GetEnvironmentVariableW(wname.c_str(), v.data(), n));
    return fs::path(v).u8string();
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

std::string trim(std::string s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// ---- host: simulated runtime ---------------------------------------------------

class HostBackend final : public RuntimeBackend {
public:
    HostBackend(fs::path hostExe, std::vector<std::string> extraArgs, milliseconds grace)
        : hostExe_(std::move(hostExe)), extraArgs_(std::move(extraArgs)), grace_(grace) {}

    std::string name() const override { return "host"; }
    std::string device() const override { return "local"; }
    std::string description() const override { return "simulated runtime (aowhost)"; }

    bool launch(const pkgmgr::InstalledPackage& pkg, Process& out, std::string& error) override {
        std::vector<std::string> argv = {hostExe_.u8string(), "--package", pkg.packageName,
                                         "--label", pkg.displayName(), "--version", pkg.versionName,
                                         "--apk", pkg.apkPath.u8string()};
        if (!pkg.launchActivity.empty()) { argv.push_back("--activity"); argv.push_back(pkg.launchActivity); }
        if (!pkg.iconPath.empty()) { argv.push_back("--icon"); argv.push_back(pkg.iconPath.u8string()); }
        argv.insert(argv.end(), extraArgs_.begin(), extraArgs_.end());

        auto child = ChildProcess::spawn(argv, &error);
        if (!child) return false;
        out.pid = child->pid();
        out.token = processToken(out.pid);
        if (out.token.empty()) {  // exited immediately (bad arguments, missing DLL, ...)
            const auto code = child->wait(seconds(1));
            error = "app process exited immediately" + (code ? " with code " + std::to_string(*code) : std::string());
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        children_[out.pid] = std::move(child);
        return true;
    }

    Liveness check(const AppSession& s) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = children_.find(s.pid); it != children_.end()) {
            if (auto code = it->second->poll()) {  // our child: exact exit code
                children_.erase(it);
                return {false, code, {}};
            }
            return {true, std::nullopt, {}};
        }
        // Started by another process (e.g. the aowrt CLI): only liveness is observable.
        return {processAlive(s.pid, s.processToken), std::nullopt, {}};
    }

    bool stop(const AppSession& s, std::string& error) override {
        const bool ok = !processAlive(s.pid, s.processToken) || terminateProcess(s.pid, grace_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = children_.find(s.pid); it != children_.end()) {
            it->second->wait(seconds(2));  // reap it
            children_.erase(it);
        }
        if (!ok) error = "could not terminate process " + std::to_string(s.pid);
        return ok;
    }

private:
    fs::path hostExe_;
    std::vector<std::string> extraArgs_;
    milliseconds grace_;
    std::mutex mutex_;
    std::map<std::int64_t, std::unique_ptr<ChildProcess>> children_;
};

// ---- adb: real Android runtime -----------------------------------------------

class AdbBackend final : public RuntimeBackend {
public:
    AdbBackend(fs::path adb, std::string serial) : adb_(std::move(adb)), serial_(std::move(serial)) {}

    std::string name() const override { return "adb"; }
    std::string device() const override { return serial_; }
    std::string description() const override { return "Android device " + serial_ + " (adb)"; }

    bool launch(const pkgmgr::InstalledPackage& pkg, Process& out, std::string& error) override {
        if (!deploy(pkg, error)) return false;

        ExecResult r;
        if (!pkg.launchActivity.empty()) {
            r = shell({"am", "start", "-W", "-n", pkg.packageName + "/" + pkg.launchActivity});
            if (r.ok() && r.output.find("Error") != std::string::npos) r.exitCode = 1;
        } else {  // no declared launcher activity: let the system pick one
            r = shell({"monkey", "-p", pkg.packageName, "-c", "android.intent.category.LAUNCHER", "1"});
        }
        if (!r.ok()) { error = "launch failed: " + describe(r); return false; }

        // The process may take a moment to show up.
        for (int i = 0; i < 25; ++i) {
            if (const auto pid = pidOf(pkg.packageName)) {
                out.pid = *pid;
                out.token = serial_ + ":" + std::to_string(*pid);
                return true;
            }
            std::this_thread::sleep_for(milliseconds(200));
        }
        error = "the app started but its process disappeared (crashed on launch?)";
        return false;
    }

    Liveness check(const AppSession& s) override {
        const auto r = shell({"pidof", s.packageName});
        if (!r.error.empty() || r.timedOut) return {true, std::nullopt, "adb unavailable"};  // unknown: assume alive
        if (r.exitCode != 0 && r.output.find("device") != std::string::npos)  // "device not found/offline"
            return {false, std::nullopt, "device disconnected"};
        std::istringstream in(r.output);
        for (std::int64_t pid; in >> pid;)
            if (pid == s.pid) return {true, std::nullopt, {}};
        return {false, std::nullopt, {}};
    }

    bool stop(const AppSession& s, std::string& error) override {
        const auto r = shell({"am", "force-stop", s.packageName});
        if (!r.ok()) error = "force-stop failed: " + describe(r);
        return r.ok();
    }

private:
    ExecResult adb(std::vector<std::string> args, milliseconds timeout = seconds(30)) {
        args.insert(args.begin(), {adb_.u8string(), "-s", serial_});
        return execCapture(args, timeout);
    }
    ExecResult shell(std::vector<std::string> args) {
        args.insert(args.begin(), "shell");
        return adb(std::move(args));
    }
    static std::string describe(const ExecResult& r) {
        if (!r.error.empty()) return r.error;
        if (r.timedOut) return "adb timed out";
        return trim(r.output).empty() ? "exit code " + std::to_string(r.exitCode) : trim(r.output);
    }

    std::optional<std::int64_t> pidOf(const std::string& pkg) {
        const auto r = shell({"pidof", pkg});
        std::istringstream in(r.output);
        std::int64_t pid = 0;
        if (r.ok() && in >> pid) return pid;
        return std::nullopt;
    }

    // Installs the store's copy on the device when it is missing or a different version.
    bool deploy(const pkgmgr::InstalledPackage& pkg, std::string& error) {
        const auto r = shell({"dumpsys", "package", pkg.packageName});
        std::int64_t deviceVersion = -1;
        if (const auto at = r.output.find("versionCode="); at != std::string::npos)
            deviceVersion = std::strtoll(r.output.c_str() + at + 12, nullptr, 10);
        if (deviceVersion == pkg.versionCode) return true;

        const auto inst = adb({"install", "-r", "-d", pkg.apkPath.u8string()}, minutes(5));
        if (!inst.ok() || inst.output.find("Success") == std::string::npos) {
            error = "adb install failed: " + describe(inst);
            return false;
        }
        return true;
    }

    fs::path adb_;
    std::string serial_;
};

} // namespace

std::optional<fs::path> findAdb() {
    const std::string name = std::string("adb") + kExe;
    auto exists = [](const fs::path& p) { std::error_code ec; return fs::is_regular_file(p, ec); };
    if (const std::string p = env("AOW_ADB"); !p.empty() && exists(fs::u8path(p))) return fs::u8path(p);
    for (const char* var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
        if (const std::string sdk = env(var); !sdk.empty())
            if (const auto p = fs::u8path(sdk) / "platform-tools" / name; exists(p)) return p;
#ifdef _WIN32
    if (const std::string local = env("LOCALAPPDATA"); !local.empty())
        if (const auto p = fs::u8path(local) / "Android" / "Sdk" / "platform-tools" / name; exists(p)) return p;
#else
    if (const std::string home = env("HOME"); !home.empty())
        if (const auto p = fs::path(home) / "Android" / "Sdk" / "platform-tools" / name; exists(p)) return p;
#endif
    return findInPath(name);
}

std::vector<std::string> adbDevices(const fs::path& adb) {
    std::vector<std::string> out;
    const auto r = execCapture({adb.u8string(), "devices"}, seconds(15));
    if (!r.ok()) return out;
    std::istringstream in(r.output);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string serial, state;
        if (fields >> serial >> state && state == "device") out.push_back(serial);
    }
    return out;
}

std::unique_ptr<RuntimeBackend> createBackend(const BackendConfig& config, std::string* error) {
    auto fail = [&](std::string msg) -> std::unique_ptr<RuntimeBackend> {
        if (error) *error = std::move(msg);
        return nullptr;
    };
    std::string kind = config.kind;
    if (kind == "auto" || kind.empty())
        if (const std::string e = env("AOW_RUNTIME"); !e.empty()) kind = e;

    if (kind == "adb" || kind == "auto") {
        std::optional<fs::path> adb = config.adb.empty() ? findAdb() : std::optional<fs::path>(config.adb);
        std::string why;
        if (!adb) {
            why = "adb not found (install Android SDK platform-tools or set AOW_ADB)";
        } else {
            const auto devices = adbDevices(*adb);
            std::string serial = config.serial;
            if (serial.empty() && !devices.empty()) serial = devices.front();
            if (!serial.empty() && std::find(devices.begin(), devices.end(), serial) != devices.end())
                return std::make_unique<AdbBackend>(*adb, serial);
            why = serial.empty() ? "no Android device/emulator connected to adb" : "device " + serial + " not connected";
        }
        if (kind == "adb") return fail(why);
    }
    if (kind == "host" || kind == "auto") {
        fs::path host = config.hostExe;
        if (host.empty()) host = currentExecutableDir() / (std::string("aowhost") + kExe);
        std::error_code ec;
        if (!fs::is_regular_file(host, ec)) return fail("simulated runtime not found: " + host.u8string());
        return std::make_unique<HostBackend>(host, config.hostExtraArgs, config.stopGrace);
    }
    return fail("unknown runtime backend '" + kind + "' (use auto, adb or host)");
}

} // namespace runtime
