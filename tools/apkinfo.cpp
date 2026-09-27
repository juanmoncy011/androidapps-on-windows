// apkinfo <file.apk | folder> [-r]
#include <apkcore/apk.h>

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static void print(const apkcore::ApkInfo& a) {
    std::cout << "== " << a.path.filename().u8string() << "\n";
    if (!a.ok()) { std::cout << "   ERROR: " << a.error << "\n\n"; return; }
    std::cout << "   Name:      " << a.displayName() << "\n"
              << "   Package:   " << a.packageName << "\n"
              << "   Version:   " << a.versionName << " (" << a.versionCode << ")\n"
              << "   SDK:       min " << a.minSdk << ", target " << a.targetSdk << "\n"
              << "   Launch:    " << (a.launchActivity.empty() ? "(none)" : a.launchActivity) << "\n"
              << "   ABIs:      ";
    if (a.nativeAbis.empty()) std::cout << "none";
    for (const auto& abi : a.nativeAbis) std::cout << abi << ' ';
    std::cout << "\n   Icon:      " << (a.iconPath.empty() ? "(none)" : a.iconPath)
              << " [" << a.iconData.size() << " bytes]\n"
              << "   Perms:     " << a.permissions.size() << "\n\n";
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
#else
int main(int argc, char** argv) {
#endif
    if (argc < 2) {
        std::cerr << "usage: apkinfo <file.apk | folder> [-r]\n";
        return 2;
    }
    const fs::path target(argv[1]);
    const bool recursive = argc > 2 && fs::path(argv[2]).u8string() == "-r";

    std::error_code ec;
    if (fs::is_directory(target, ec)) {
        const auto apps = apkcore::scanFolder(target, apkcore::ScanOptions{recursive});
        for (const auto& a : apps) print(a);
        std::cout << apps.size() << " APK(s) scanned.\n";
    } else {
        const auto a = apkcore::parseApk(target);
        print(a);
        return a.ok() ? 0 : 1;
    }
    return 0;
}
