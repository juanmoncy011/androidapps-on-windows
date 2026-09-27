// aowpm - Package Manager CLI
//   aowpm [--root DIR] install <file.apk | folder> [-r] [--downgrade]
//   aowpm [--root DIR] list
//   aowpm [--root DIR] info <package>
//   aowpm [--root DIR] uninstall <package>
//   aowpm [--root DIR] verify
#include <pkgmgr/package_manager.h>

#include <ctime>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

int usage() {
    std::cerr << "usage: aowpm [--root DIR] <command>\n"
                 "  install <file.apk | folder> [-r] [--downgrade]   copy APK(s) into the managed store\n"
                 "  list                                             show installed packages\n"
                 "  info <package>                                   show one package in detail\n"
                 "  uninstall <package>                              remove a package\n"
                 "  verify                                           check files against the database\n";
    return 2;
}

std::string formatTime(std::int64_t t) {
    const std::time_t tt = std::time_t(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

int install(pkgmgr::PackageManager& pm, const fs::path& target, bool recursive, bool downgrade) {
    std::vector<fs::path> apks;
    std::error_code ec;
    if (fs::is_directory(target, ec)) {
        for (const auto& a : apkcore::scanFolder(target, apkcore::ScanOptions{recursive})) apks.push_back(a.path);
        if (apks.empty()) { std::cerr << "no .apk files in " << target.u8string() << "\n"; return 1; }
    } else {
        apks.push_back(target);
    }

    int failures = 0;
    for (const auto& apk : apks) {
        const auto r = pm.install(apk, pkgmgr::InstallOptions{downgrade});
        std::cout << (r.ok() ? "[ok]   " : "[FAIL] ") << apk.filename().u8string() << ": "
                  << (r.packageName.empty() ? "?" : r.packageName) << " - " << pkgmgr::toString(r.status);
        if (r.status == pkgmgr::InstallStatus::Updated)
            std::cout << " (" << r.previousVersionCode << " -> " << r.versionCode << ")";
        if (!r.message.empty()) std::cout << ": " << r.message;
        std::cout << "\n";
        if (!r.ok()) ++failures;
    }
    return failures ? 1 : 0;
}

int list(const pkgmgr::PackageManager& pm) {
    const auto pkgs = pm.list();
    for (const auto& p : pkgs)
        std::cout << p.packageName << "  " << p.displayName() << "  " << p.versionName << " (" << p.versionCode
                  << ")  " << (p.apkSize + 1023) / 1024 << " KB\n";
    std::cout << pkgs.size() << " package(s) installed in " << pm.root().u8string() << "\n";
    return 0;
}

int info(const pkgmgr::PackageManager& pm, const std::string& name) {
    const auto p = pm.find(name);
    if (!p) { std::cerr << name << " is not installed\n"; return 1; }
    std::cout << "Name:       " << p->displayName() << "\n"
              << "Package:    " << p->packageName << "\n"
              << "Version:    " << p->versionName << " (" << p->versionCode << ")\n"
              << "SDK:        min " << p->minSdk << ", target " << p->targetSdk << "\n"
              << "Launch:     " << (p->launchActivity.empty() ? "(none)" : p->launchActivity) << "\n"
              << "ABIs:       ";
    if (p->nativeAbis.empty()) std::cout << "none";
    for (const auto& a : p->nativeAbis) std::cout << a << ' ';
    std::cout << "\nAPK:        " << p->apkPath.u8string() << " (" << p->apkSize << " bytes)\n"
              << "SHA-256:    " << p->sha256 << "\n"
              << "Icon:       " << (p->iconPath.empty() ? "(none)" : p->iconPath.u8string()) << "\n"
              << "Source:     " << p->sourcePath.u8string() << "\n"
              << "Installed:  " << formatTime(p->installedAt) << "   Updated: " << formatTime(p->updatedAt) << "\n"
              << "Permissions (" << p->permissions.size() << "):\n";
    for (const auto& perm : p->permissions) std::cout << "  " << perm << "\n";
    return 0;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
#else
int main(int argc, char** argv) {
#endif
    std::vector<fs::path> args(argv + 1, argv + argc);
    auto arg = [&](std::size_t i) { return i < args.size() ? args[i].u8string() : std::string(); };

    fs::path root = pkgmgr::PackageManager::defaultRoot();
    if (arg(0) == "--root") {
        if (args.size() < 2) return usage();
        root = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    const std::string cmd = arg(0);
    if (cmd.empty()) return usage();

    try {
        pkgmgr::PackageManager pm(root);
        if (cmd == "install" && args.size() >= 2) {
            bool recursive = false, downgrade = false;
            for (std::size_t i = 2; i < args.size(); ++i) {
                if (arg(i) == "-r") recursive = true;
                else if (arg(i) == "--downgrade") downgrade = true;
                else return usage();
            }
            return install(pm, args[1], recursive, downgrade);
        }
        if (cmd == "list" && args.size() == 1) return list(pm);
        if (cmd == "info" && args.size() == 2) return info(pm, arg(1));
        if (cmd == "uninstall" && args.size() == 2) {
            std::string err;
            if (!pm.uninstall(arg(1), &err)) { std::cerr << "uninstall failed: " << err << "\n"; return 1; }
            std::cout << "uninstalled " << arg(1) << "\n";
            return 0;
        }
        if (cmd == "verify" && args.size() == 1) {
            const auto problems = pm.verify();
            for (const auto& p : problems) std::cout << "problem: " << p << "\n";
            if (problems.empty()) std::cout << "all packages OK\n";
            return problems.empty() ? 0 : 1;
        }
    } catch (const pkgmgr::PackageManagerError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return usage();
}
