// aowrt - Runtime Manager CLI
//   aowrt [--root DIR] [--backend auto|adb|host] [--adb PATH] [--serial S] <command>
//     backends                show which runtimes are available
//     launch <package> [--wait]
//     stop <package>
//     ps                      running apps
//     history [N]             recent sessions
//     watch                   print start/exit/crash events until Ctrl+C
#include <runtime/runtime_manager.h>

#include <atomic>
#include <csignal>
#include <ctime>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_interrupted{false};
void onSignal(int) { g_interrupted = true; }

int usage() {
    std::cerr << "usage: aowrt [--root DIR] [--backend auto|adb|host] [--adb PATH] [--serial S] <command>\n"
                 "  backends                  show which runtimes are available\n"
                 "  launch <package> [--wait] start an installed app (--wait: until it exits)\n"
                 "  stop <package>            stop a running app\n"
                 "  ps                        list running apps\n"
                 "  history [N]               show the last N sessions (default 20)\n"
                 "  watch                     print start/exit/crash events until Ctrl+C\n";
    return 2;
}

std::string formatTime(std::int64_t t) {
    if (t == 0) return "-";
    const std::time_t tt = std::time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string describe(const runtime::AppSession& s) {
    std::string out = s.packageName + " [" + runtime::toString(s.state) + "] pid " + std::to_string(s.pid) + " on " +
                      s.backend + ":" + s.device;
    if (s.exitCode) out += ", exit code " + std::to_string(*s.exitCode);
    if (!s.detail.empty()) out += " (" + s.detail + ")";
    return out;
}

const char* eventName(runtime::EventKind k) {
    switch (k) {
    case runtime::EventKind::Started: return "started";
    case runtime::EventKind::Exited: return "exited";
    case runtime::EventKind::Crashed: return "CRASHED";
    case runtime::EventKind::Stopped: return "stopped";
    }
    return "?";
}

int backends(const runtime::BackendConfig& cfg) {
    const auto adb = cfg.adb.empty() ? runtime::findAdb() : std::optional<fs::path>(cfg.adb);
    std::cout << "adb:  ";
    if (!adb) {
        std::cout << "not found (install Android SDK platform-tools or set AOW_ADB)\n";
    } else {
        const auto devices = runtime::adbDevices(*adb);
        std::cout << adb->u8string() << " - " << devices.size() << " device(s)";
        for (const auto& d : devices) std::cout << " " << d;
        std::cout << "\n";
    }
    runtime::BackendConfig host = cfg;
    host.kind = "host";
    std::string err;
    std::cout << "host: " << (runtime::createBackend(host, &err) ? "available (simulated runtime)" : err) << "\n";
    runtime::BackendConfig chosen = cfg;
    if (auto b = runtime::createBackend(chosen, &err)) std::cout << "selected: " << b->description() << "\n";
    else std::cout << "selected: none - " << err << "\n";
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
#else
int main(int argc, char** argv) {
#endif
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(fs::path(argv[i]).u8string());

    fs::path root = pkgmgr::PackageManager::defaultRoot();
    runtime::BackendConfig cfg;
    while (!args.empty() && args[0].rfind("--", 0) == 0) {
        if (args.size() < 2) return usage();
        const std::string opt = args[0], val = args[1];
        if (opt == "--root") root = fs::u8path(val);
        else if (opt == "--backend") cfg.kind = val;
        else if (opt == "--adb") cfg.adb = fs::u8path(val);
        else if (opt == "--serial") cfg.serial = val;
        else return usage();
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.empty()) return usage();
    const std::string cmd = args[0];
    if (cmd == "backends") return backends(cfg);

    try {
        pkgmgr::PackageManager pm(root);
        std::string err;
        auto backend = runtime::createBackend(cfg, &err);
        if (!backend) { std::cerr << "error: " << err << "\n"; return 1; }
        runtime::RuntimeManager rt(pm, std::move(backend));
        rt.refresh();

        if (cmd == "launch" && (args.size() == 2 || (args.size() == 3 && args[2] == "--wait"))) {
            const auto r = rt.launch(args[1]);
            if (!r.ok) { std::cerr << "launch failed: " << r.error << "\n"; return 1; }
            std::cout << (r.alreadyRunning ? "already running: " : "launched: ") << describe(r.session) << "\n";
            if (args.size() == 3) {
                std::signal(SIGINT, onSignal);
                while (!g_interrupted) {
                    std::this_thread::sleep_for(500ms);
                    rt.refresh();
                    const auto now = rt.running(args[1]);
                    if (!now || now->id != r.session.id) break;
                }
                for (const auto& s : rt.history(50))
                    if (s.id == r.session.id) {
                        std::cout << "ended: " << describe(s) << "\n";
                        return s.state == runtime::AppState::Crashed ? 3 : 0;
                    }
            }
            return 0;
        }
        if (cmd == "stop" && args.size() == 2) {
            if (!rt.stop(args[1], &err)) { std::cerr << "stop failed: " << err << "\n"; return 1; }
            std::cout << "stopped " << args[1] << "\n";
            return 0;
        }
        if (cmd == "ps" && args.size() == 1) {
            const auto running = rt.running();
            for (const auto& s : running)
                std::cout << s.packageName << "  " << s.displayName() << "  pid " << s.pid << "  since "
                          << formatTime(s.startedAt) << "\n";
            std::cout << running.size() << " app(s) running on " << rt.backend().description() << "\n";
            return 0;
        }
        if (cmd == "history" && args.size() <= 2) {
            const std::size_t n = args.size() == 2 ? std::size_t(std::stoul(args[1])) : 20;
            for (const auto& s : rt.history(n))
                std::cout << formatTime(s.startedAt) << "  " << formatTime(s.endedAt) << "  " << describe(s) << "\n";
            return 0;
        }
        if (cmd == "watch" && args.size() == 1) {
            std::signal(SIGINT, onSignal);
            rt.setListener([](const runtime::RuntimeEvent& e) {
                std::cout << formatTime(std::int64_t(std::time(nullptr))) << "  " << eventName(e.kind) << "  "
                          << describe(e.session) << std::endl;
            });
            std::cout << "watching " << rt.backend().description() << " - Ctrl+C to quit\n";
            for (const auto& s : rt.running()) std::cout << "running  " << describe(s) << "\n";
            rt.startMonitor(1s);
            while (!g_interrupted) std::this_thread::sleep_for(200ms);
            rt.stopMonitor();
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return usage();
}
