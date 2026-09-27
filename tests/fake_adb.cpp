// fake_adb - stands in for adb in tests. Emulates the handful of commands the
// adb runtime backend uses against one device ("emulator-5554"), keeping state
// in the file named by FAKE_ADB_STATE. Extra test hooks: echo, sleep, kill.
#include <apkcore/apk.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

const std::string kDevice = "emulator-5554";

struct State {
    std::map<std::string, std::string> installed;  // package -> versionCode
    std::map<std::string, std::string> running;    // package -> pid
    long long nextPid = 4000;
    long long installs = 0;
};

fs::path statePath() {
    const char* p = std::getenv("FAKE_ADB_STATE");
    return p ? fs::u8path(p) : fs::temp_directory_path() / "fake_adb_state.txt";
}

State load() {
    State s;
    std::ifstream in(statePath());
    std::string kind, a, b;
    while (in >> kind) {
        if (kind == "installed" && in >> a >> b) s.installed[a] = b;
        else if (kind == "running" && in >> a >> b) s.running[a] = b;
        else if (kind == "nextpid") in >> s.nextPid;
        else if (kind == "installs") in >> s.installs;
    }
    return s;
}

void save(const State& s) {
    const fs::path tmp = statePath().u8string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "nextpid " << s.nextPid << "\ninstalls " << s.installs << "\n";
        for (const auto& [k, v] : s.installed) out << "installed " << k << " " << v << "\n";
        for (const auto& [k, v] : s.running) out << "running " << k << " " << v << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, statePath(), ec);
}

int start(State& s, const std::string& pkg, const std::string& component) {
    if (!s.installed.count(pkg)) {
        std::cout << "Error: Activity class {" << component << "} does not exist.\n";
        return 0;  // like the real `am start`, failure is only reported in the output
    }
    if (!s.running.count(pkg)) s.running[pkg] = std::to_string(s.nextPid++);
    save(s);
    std::cout << "Starting: Intent { cmp=" << component << " }\nStatus: ok\n";
    return 0;
}

int run(std::vector<std::string> a) {
    if (a.size() >= 2 && a[0] == "-s") {
        if (a[1] != kDevice) {
            std::cout << "error: device '" << a[1] << "' not found\n";
            return 1;
        }
        a.erase(a.begin(), a.begin() + 2);
    }
    if (a.empty()) return 1;

    if (a[0] == "echo") {
        for (std::size_t i = 1; i < a.size(); ++i) std::cout << (i > 1 ? "|" : "") << a[i];
        std::cout << "\n";
        return 0;
    }
    if (a[0] == "sleep" && a.size() == 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoll(a[1].c_str())));
        return 0;
    }
    if (a[0] == "devices") {
        std::cout << "List of devices attached\n" << kDevice << "\tdevice\n\n";
        return 0;
    }

    State s = load();
    if (a[0] == "install" && a.size() >= 2) {
        const auto info = apkcore::parseApk(fs::u8path(a.back()));
        if (!info.ok()) { std::cout << "adb: failed to install: " << info.error << "\n"; return 1; }
        s.installed[info.packageName] = std::to_string(info.versionCode);
        s.running.erase(info.packageName);  // installing kills the old process
        ++s.installs;
        save(s);
        std::cout << "Performing Streamed Install\nSuccess\n";
        return 0;
    }
    if (a[0] == "kill" && a.size() == 2) {  // test hook: the app dies on the device
        s.running.erase(a[1]);
        save(s);
        return 0;
    }
    if (a[0] != "shell" || a.size() < 2) return 1;

    const std::vector<std::string> sh(a.begin() + 1, a.end());
    if (sh[0] == "dumpsys" && sh.size() == 3 && sh[1] == "package") {
        if (auto it = s.installed.find(sh[2]); it != s.installed.end())
            std::cout << "Packages:\n  Package [" << sh[2] << "]\n    versionCode=" << it->second
                      << " minSdk=21 targetSdk=34\n";
        return 0;
    }
    if (sh[0] == "am" && sh.size() >= 3 && sh[1] == "start") {
        const std::string component = sh.back();
        return start(s, component.substr(0, component.find('/')), component);
    }
    if (sh[0] == "monkey" && sh.size() >= 3 && sh[1] == "-p") return start(s, sh[2], sh[2]);
    if (sh[0] == "pidof" && sh.size() == 2) {
        if (auto it = s.running.find(sh[1]); it != s.running.end()) {
            std::cout << it->second << "\n";
            return 0;
        }
        return 1;
    }
    if (sh[0] == "am" && sh.size() == 3 && sh[1] == "force-stop") {
        s.running.erase(sh[2]);
        save(s);
        return 0;
    }
    std::cout << "fake_adb: unsupported command\n";
    return 1;
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
    return run(args);
}
