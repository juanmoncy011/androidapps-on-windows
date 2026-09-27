#pragma once
// apkcore - APK inspection shared by the Launcher (M1) and Package Manager (M2).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace apkcore {

struct ApkInfo {
    std::filesystem::path path;
    std::uint64_t fileSize = 0;

    std::string packageName;
    std::string label;            // resolved app name (see displayName())
    std::string versionName;
    std::int64_t versionCode = 0;
    int minSdk = 0;               // 0 = not declared
    int targetSdk = 0;
    std::string splitName;        // non-empty for split APKs
    std::string launchActivity;   // fully-qualified MAIN/LAUNCHER activity (used by Runtime Manager later)
    std::vector<std::string> permissions;
    std::vector<std::string> nativeAbis;  // from lib/<abi>/ ; empty = no native code

    std::string iconPath;                 // path inside APK, e.g. res/mipmap-xxhdpi/ic_launcher.png
    std::vector<std::uint8_t> iconData;   // raw PNG/WebP bytes (empty if none or vector-only icon)

    std::string error;            // non-empty if the APK could not be parsed

    bool ok() const { return error.empty(); }
    std::string displayName() const;
};

// Parse one APK. Never throws; failures are reported in ApkInfo::error.
ApkInfo parseApk(const std::filesystem::path& apkPath);

struct ScanOptions {
    bool recursive = false;
};

// Called before each APK is parsed and once at the end. Return false to cancel.
using ScanProgress = std::function<bool(std::size_t done, std::size_t total)>;

// Find every *.apk in `folder` and parse it. Results are sorted by path.
std::vector<ApkInfo> scanFolder(const std::filesystem::path& folder,
                                const ScanOptions& options = {},
                                const ScanProgress& progress = {});

} // namespace apkcore
