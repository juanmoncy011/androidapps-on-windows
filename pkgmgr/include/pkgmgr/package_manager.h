#pragma once
// pkgmgr - Package Manager (M2): installs APKs into a managed directory and
// records their metadata in SQLite. Used by the Launcher now, Runtime Manager next.
//
// On-disk layout under root():
//   packages.db                 SQLite catalogue (packages, permissions, native_abis)
//   apps/<package>/base.apk     managed copy of the APK
//   apps/<package>/icon.<ext>   launcher icon extracted at install time (optional)
#include <apkcore/apk.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace pkgmgr {

struct InstalledPackage {
    std::string packageName;
    std::string label;
    std::string versionName;
    std::int64_t versionCode = 0;
    int minSdk = 0;
    int targetSdk = 0;
    std::string launchActivity;
    std::vector<std::string> permissions;
    std::vector<std::string> nativeAbis;

    std::filesystem::path apkPath;     // absolute path of the managed copy
    std::filesystem::path iconPath;    // absolute; empty if the APK has no raster icon
    std::uint64_t apkSize = 0;
    std::string sha256;                // hex digest of base.apk
    std::filesystem::path sourcePath;  // where it was installed from
    std::int64_t installedAt = 0;      // unix seconds (first install)
    std::int64_t updatedAt = 0;        // unix seconds (last install/update)

    std::string displayName() const { return label.empty() ? packageName : label; }
};

enum class InstallStatus {
    Installed,         // new package
    Updated,           // replaced an older (or, with allowDowngrade, newer) version
    Reinstalled,       // same versionCode, different file contents
    AlreadyInstalled,  // identical file already installed - nothing changed
    InvalidApk,        // unparsable APK, split APK or bad package name
    DowngradeRefused,  // lower versionCode than installed and allowDowngrade == false
    IoError,
    DatabaseError,
};

const char* toString(InstallStatus s);

struct InstallOptions {
    bool allowDowngrade = false;
};

struct InstallResult {
    InstallStatus status = InstallStatus::IoError;
    std::string packageName;
    std::int64_t versionCode = 0;
    std::int64_t previousVersionCode = 0;  // 0 if it was not installed before
    std::string message;                   // human-readable detail on failure

    bool ok() const {
        return status == InstallStatus::Installed || status == InstallStatus::Updated ||
               status == InstallStatus::Reinstalled || status == InstallStatus::AlreadyInstalled;
    }
};

struct PackageManagerError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// True for Android-style package names (a.b.c; letters, digits, '_'), which
// also makes them safe to use as directory names.
bool isValidPackageName(const std::string& name);

// Thread-safe; several processes may share one root (SQLite handles locking).
class PackageManager {
public:
    // %LOCALAPPDATA%\AndroidAppsOnWindows on Windows, $XDG_DATA_HOME/aow or
    // ~/.local/share/aow elsewhere. The AOW_HOME environment variable overrides it.
    static std::filesystem::path defaultRoot();

    // Creates root and the database if needed. Throws PackageManagerError.
    explicit PackageManager(std::filesystem::path root = defaultRoot());
    ~PackageManager();
    PackageManager(const PackageManager&) = delete;
    PackageManager& operator=(const PackageManager&) = delete;

    const std::filesystem::path& root() const { return root_; }

    InstallResult install(const std::filesystem::path& apk, const InstallOptions& options = {});
    // Same, reusing an ApkInfo the caller already parsed (e.g. from a Launcher scan).
    InstallResult install(const apkcore::ApkInfo& apk, const InstallOptions& options = {});

    // Removes the package's files and records. Returns false (with *error set) if
    // it is not installed or its files could not be deleted.
    bool uninstall(const std::string& packageName, std::string* error = nullptr);

    std::optional<InstalledPackage> find(const std::string& packageName) const;
    std::vector<InstalledPackage> list() const;  // sorted by display name

    // Checks every package's files against the database; returns one line per problem.
    std::vector<std::string> verify() const;

private:
    struct Db;
    std::optional<InstalledPackage> findLocked(const std::string& packageName) const;
    std::vector<InstalledPackage> query(const std::string& onlyPackage) const;  // caller holds mutex_

    std::filesystem::path root_;
    std::unique_ptr<Db> db_;
    mutable std::mutex mutex_;
};

} // namespace pkgmgr
