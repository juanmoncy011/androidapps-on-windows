#include <pkgmgr/package_manager.h>

#include "sha256.h"
#include "sqlite_db.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace pkgmgr::detail;

namespace pkgmgr {
namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS packages (
    package_name    TEXT PRIMARY KEY,
    label           TEXT NOT NULL,
    version_name    TEXT NOT NULL,
    version_code    INTEGER NOT NULL,
    min_sdk         INTEGER NOT NULL,
    target_sdk      INTEGER NOT NULL,
    launch_activity TEXT NOT NULL,
    apk_path        TEXT NOT NULL,   -- relative to the package manager root
    icon_path       TEXT NOT NULL,   -- relative; '' if none
    apk_size        INTEGER NOT NULL,
    sha256          TEXT NOT NULL,
    source_path     TEXT NOT NULL,
    installed_at    INTEGER NOT NULL,
    updated_at      INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS permissions (
    package_name TEXT NOT NULL REFERENCES packages(package_name) ON DELETE CASCADE,
    name         TEXT NOT NULL,
    PRIMARY KEY (package_name, name)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS native_abis (
    package_name TEXT NOT NULL REFERENCES packages(package_name) ON DELETE CASCADE,
    abi          TEXT NOT NULL,
    PRIMARY KEY (package_name, abi)
) WITHOUT ROWID;
)sql";

constexpr const char* kSelectPackage =
    "SELECT package_name, label, version_name, version_code, min_sdk, target_sdk, launch_activity,"
    " apk_path, icon_path, apk_size, sha256, source_path, installed_at, updated_at FROM packages";

std::string toUtf8(const fs::path& p) { return p.generic_u8string(); }
fs::path fromUtf8(const std::string& s) { return fs::u8path(s); }

fs::path packageDir(const std::string& pkg) { return fs::path("apps") / fs::u8path(pkg); }

InstallResult fail(InstallStatus status, const std::string& pkg, std::string message) {
    InstallResult r;
    r.status = status;
    r.packageName = pkg;
    r.message = std::move(message);
    return r;
}

// Streams `from` to `to` while hashing it. Returns an error message, or empty on success.
std::string copyAndHash(const fs::path& from, const fs::path& to, std::string& sha, std::uint64_t& size) {
    std::ifstream in(from, std::ios::binary);
    if (!in) return "cannot read " + toUtf8(from);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return "cannot write " + toUtf8(to);

    Sha256 hash;
    std::vector<char> buf(1u << 20);
    size = 0;
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        const auto n = in.gcount();
        if (n <= 0) break;
        hash.update(buf.data(), std::size_t(n));
        out.write(buf.data(), n);
        size += std::uint64_t(n);
    }
    if (in.bad()) return "read error on " + toUtf8(from);
    out.close();
    if (!out) return "write error on " + toUtf8(to) + " (disk full?)";
    sha = hash.finishHex();
    return {};
}

std::string hashFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    Sha256 hash;
    std::vector<char> buf(1u << 20);
    while (in) {
        in.read(buf.data(), std::streamsize(buf.size()));
        if (in.gcount() > 0) hash.update(buf.data(), std::size_t(in.gcount()));
    }
    return in.bad() ? std::string() : hash.finishHex();
}

std::string iconExtension(const std::string& iconPathInApk) {
    std::string ext = fs::u8path(iconPathInApk).extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".webp" || ext == ".jpg" || ext == ".jpeg" ? ext : ".png";
}

} // namespace

// ---------------------------------------------------------------------------

struct PackageManager::Db {
    explicit Db(const fs::path& file) : db(file) {}
    Database db;
};

const char* toString(InstallStatus s) {
    switch (s) {
    case InstallStatus::Installed: return "installed";
    case InstallStatus::Updated: return "updated";
    case InstallStatus::Reinstalled: return "reinstalled";
    case InstallStatus::AlreadyInstalled: return "already installed";
    case InstallStatus::InvalidApk: return "invalid APK";
    case InstallStatus::DowngradeRefused: return "downgrade refused";
    case InstallStatus::IoError: return "I/O error";
    case InstallStatus::DatabaseError: return "database error";
    }
    return "unknown";
}

bool isValidPackageName(const std::string& name) {
    if (name.empty() || name.size() > 255) return false;
    bool segmentStart = true;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '.') {
            if (segmentStart) return false;  // empty segment
            segmentStart = true;
        } else if (std::isalpha(u) || (!segmentStart && (std::isdigit(u) || c == '_'))) {
            segmentStart = false;
        } else {
            return false;
        }
    }
    return !segmentStart;
}

fs::path PackageManager::defaultRoot() {
#ifdef _WIN32
    auto env = [](const wchar_t* name) -> fs::path {
        const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
        if (n == 0) return {};
        std::wstring v(n, L'\0');
        v.resize(GetEnvironmentVariableW(name, v.data(), n));
        return v;
    };
    if (fs::path p = env(L"AOW_HOME"); !p.empty()) return p;
    if (fs::path p = env(L"LOCALAPPDATA"); !p.empty()) return p / "AndroidAppsOnWindows";
    return fs::temp_directory_path() / "AndroidAppsOnWindows";
#else
    if (const char* p = std::getenv("AOW_HOME"); p && *p) return p;
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p) return fs::path(p) / "aow";
    if (const char* p = std::getenv("HOME"); p && *p) return fs::path(p) / ".local" / "share" / "aow";
    return fs::temp_directory_path() / "aow";
#endif
}

PackageManager::PackageManager(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    fs::create_directories(root_ / "apps", ec);
    if (ec) throw PackageManagerError("cannot create " + toUtf8(root_ / "apps") + ": " + ec.message());
    try {
        db_ = std::make_unique<Db>(root_ / "packages.db");
        db_->db.exec("PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;");
        auto version = db_->db.prepare("PRAGMA user_version");
        version.step();
        const auto current = version.int64(0);
        if (current > kSchemaVersion)
            throw PackageManagerError("packages.db was created by a newer version (schema " +
                                      std::to_string(current) + ")");
        db_->db.exec(kSchema);
        db_->db.exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
    } catch (const DbError& e) {
        throw PackageManagerError(std::string("database: ") + e.what());
    }
}

PackageManager::~PackageManager() = default;

InstallResult PackageManager::install(const fs::path& apk, const InstallOptions& options) {
    return install(apkcore::parseApk(apk), options);
}

InstallResult PackageManager::install(const apkcore::ApkInfo& apk, const InstallOptions& options) {
    const std::string& pkg = apk.packageName;
    if (!apk.ok()) return fail(InstallStatus::InvalidApk, pkg, apk.error);
    if (!apk.splitName.empty())
        return fail(InstallStatus::InvalidApk, pkg, "split APKs (" + apk.splitName + ") are not supported");
    if (!isValidPackageName(pkg)) return fail(InstallStatus::InvalidApk, pkg, "invalid package name '" + pkg + "'");

    auto downgradeRefused = [&](std::int64_t installed) {
        auto r = fail(InstallStatus::DowngradeRefused, pkg,
                      "installed version " + std::to_string(installed) + " is newer than " +
                          std::to_string(apk.versionCode));
        r.previousVersionCode = installed;
        r.versionCode = apk.versionCode;
        return r;
    };
    // Cheap early check so a refused downgrade does not copy the whole APK first.
    if (const auto current = find(pkg); current && apk.versionCode < current->versionCode && !options.allowDowngrade)
        return downgradeRefused(current->versionCode);

    // 1. Stage the copy next to its final location (same volume, so the final rename
    //    is atomic). Done without the lock: copying a large APK must not block readers.
    const fs::path relDir = packageDir(pkg);
    const fs::path dir = root_ / relDir;
    const fs::path relApk = relDir / "base.apk";
    const fs::path finalApk = root_ / relApk;
    static std::atomic<unsigned> stageCounter{0};
    const fs::path staged = dir / ("base.apk." + std::to_string(std::time(nullptr)) + "_" +
                                   std::to_string(stageCounter++) + ".tmp");
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return fail(InstallStatus::IoError, pkg, "cannot create " + toUtf8(dir) + ": " + ec.message());

    std::string sha;
    std::uint64_t size = 0;
    if (std::string err = copyAndHash(apk.path, staged, sha, size); !err.empty()) {
        fs::remove(staged, ec);
        fs::remove(dir, ec);  // only succeeds if the folder is empty (fresh install)
        return fail(InstallStatus::IoError, pkg, err);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto discardStaged = [&] {
        fs::remove(staged, ec);
        fs::remove(dir, ec);
    };

    std::optional<InstalledPackage> existing;
    try {
        existing = findLocked(pkg);  // re-read: another install may have finished meanwhile
    } catch (const DbError& e) {
        discardStaged();
        return fail(InstallStatus::DatabaseError, pkg, e.what());
    }
    if (existing && apk.versionCode < existing->versionCode && !options.allowDowngrade) {
        discardStaged();
        return downgradeRefused(existing->versionCode);
    }

    InstallResult result;
    result.packageName = pkg;
    result.versionCode = apk.versionCode;
    result.previousVersionCode = existing ? existing->versionCode : 0;

    if (existing && existing->sha256 == sha && fs::exists(finalApk, ec)) {
        discardStaged();
        result.status = InstallStatus::AlreadyInstalled;
        return result;
    }
    result.status = !existing                                  ? InstallStatus::Installed
                    : existing->versionCode == apk.versionCode ? InstallStatus::Reinstalled
                                                               : InstallStatus::Updated;

    // 2. Icon (best effort - an app without one is still installable).
    fs::path relIcon;
    if (!apk.iconData.empty()) {
        relIcon = relDir / ("icon" + iconExtension(apk.iconPath));
        std::ofstream out(root_ / relIcon, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(apk.iconData.data()), std::streamsize(apk.iconData.size()));
        out.close();
        if (!out) { fs::remove(root_ / relIcon, ec); relIcon.clear(); }
    }
    if (existing && !existing->iconPath.empty() && existing->iconPath != root_ / relIcon)
        fs::remove(existing->iconPath, ec);

    // 3. Record metadata, then move the APK into place; the transaction only
    //    commits once the file is there, so the database never points at a missing APK.
    try {
        Transaction tx(db_->db);
        const std::int64_t now = std::int64_t(std::time(nullptr));
        db_->db.prepare(R"sql(
            INSERT INTO packages (package_name, label, version_name, version_code, min_sdk, target_sdk,
                                  launch_activity, apk_path, icon_path, apk_size, sha256, source_path,
                                  installed_at, updated_at)
            VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?13)
            ON CONFLICT(package_name) DO UPDATE SET
                label = excluded.label, version_name = excluded.version_name,
                version_code = excluded.version_code, min_sdk = excluded.min_sdk,
                target_sdk = excluded.target_sdk, launch_activity = excluded.launch_activity,
                apk_path = excluded.apk_path, icon_path = excluded.icon_path, apk_size = excluded.apk_size,
                sha256 = excluded.sha256, source_path = excluded.source_path, updated_at = excluded.updated_at
        )sql")
            .bind(1, pkg).bind(2, apk.label).bind(3, apk.versionName).bind(4, std::int64_t(apk.versionCode))
            .bind(5, std::int64_t(apk.minSdk)).bind(6, std::int64_t(apk.targetSdk)).bind(7, apk.launchActivity)
            .bind(8, toUtf8(relApk)).bind(9, relIcon.empty() ? std::string() : toUtf8(relIcon))
            .bind(10, std::int64_t(size)).bind(11, sha).bind(12, toUtf8(fs::absolute(apk.path, ec)))
            .bind(13, now)
            .run();

        db_->db.prepare("DELETE FROM permissions WHERE package_name = ?1").bind(1, pkg).run();
        db_->db.prepare("DELETE FROM native_abis WHERE package_name = ?1").bind(1, pkg).run();
        auto perm = db_->db.prepare("INSERT OR IGNORE INTO permissions (package_name, name) VALUES (?1, ?2)");
        for (const auto& p : apk.permissions) perm.bind(1, pkg).bind(2, p).run();
        auto abi = db_->db.prepare("INSERT OR IGNORE INTO native_abis (package_name, abi) VALUES (?1, ?2)");
        for (const auto& a : apk.nativeAbis) abi.bind(1, pkg).bind(2, a).run();

        fs::rename(staged, finalApk, ec);
        if (ec) {
            const std::string why = ec.message();
            if (!relIcon.empty() && !existing) fs::remove(root_ / relIcon, ec);
            discardStaged();
            return fail(InstallStatus::IoError, pkg,
                        "cannot replace " + toUtf8(finalApk) + " (is the app running?): " + why);
        }
        tx.commit();
    } catch (const DbError& e) {
        if (!relIcon.empty() && !existing) fs::remove(root_ / relIcon, ec);
        discardStaged();
        return fail(InstallStatus::DatabaseError, pkg, e.what());
    }
    return result;
}

bool PackageManager::uninstall(const std::string& packageName, std::string* error) {
    auto setError = [&](std::string msg) { if (error) *error = std::move(msg); return false; };
    if (!isValidPackageName(packageName)) return setError("invalid package name '" + packageName + "'");

    std::lock_guard<std::mutex> lock(mutex_);
    try {
        Transaction tx(db_->db);
        auto del = db_->db.prepare("DELETE FROM packages WHERE package_name = ?1");
        del.bind(1, packageName).step();
        auto changes = db_->db.prepare("SELECT changes()");
        changes.step();
        if (changes.int64(0) == 0) return setError(packageName + " is not installed");

        std::error_code ec;
        fs::remove_all(root_ / packageDir(packageName), ec);
        if (ec) return setError("cannot delete files (is the app running?): " + ec.message());
        tx.commit();
    } catch (const DbError& e) {
        return setError(e.what());
    }
    return true;
}

std::optional<InstalledPackage> PackageManager::find(const std::string& packageName) const {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        return findLocked(packageName);
    } catch (const DbError&) {
        return std::nullopt;
    }
}

std::optional<InstalledPackage> PackageManager::findLocked(const std::string& packageName) const {
    auto rows = query(packageName);
    if (rows.empty()) return std::nullopt;
    return std::move(rows.front());
}

std::vector<InstalledPackage> PackageManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        auto out = query({});
        std::sort(out.begin(), out.end(), [](const InstalledPackage& a, const InstalledPackage& b) {
            return a.displayName() < b.displayName();
        });
        return out;
    } catch (const DbError&) {
        return {};
    }
}

std::vector<InstalledPackage> PackageManager::query(const std::string& onlyPackage) const {
    std::vector<InstalledPackage> out;
    Database& db = db_->db;
    auto q = db.prepare(onlyPackage.empty() ? kSelectPackage
                                            : (std::string(kSelectPackage) + " WHERE package_name = ?1").c_str());
    if (!onlyPackage.empty()) q.bind(1, onlyPackage);
    while (q.step()) {
        InstalledPackage p;
        p.packageName = q.text(0);
        p.label = q.text(1);
        p.versionName = q.text(2);
        p.versionCode = q.int64(3);
        p.minSdk = int(q.int64(4));
        p.targetSdk = int(q.int64(5));
        p.launchActivity = q.text(6);
        p.apkPath = root_ / fromUtf8(q.text(7));
        if (const std::string icon = q.text(8); !icon.empty()) p.iconPath = root_ / fromUtf8(icon);
        p.apkSize = std::uint64_t(q.int64(9));
        p.sha256 = q.text(10);
        p.sourcePath = fromUtf8(q.text(11));
        p.installedAt = q.int64(12);
        p.updatedAt = q.int64(13);
        out.push_back(std::move(p));
    }

    auto perms = db.prepare("SELECT name FROM permissions WHERE package_name = ?1 ORDER BY name");
    auto abis = db.prepare("SELECT abi FROM native_abis WHERE package_name = ?1 ORDER BY abi");
    for (auto& p : out) {
        perms.bind(1, p.packageName);
        while (perms.step()) p.permissions.push_back(perms.text(0));
        perms.reset();
        abis.bind(1, p.packageName);
        while (abis.step()) p.nativeAbis.push_back(abis.text(0));
        abis.reset();
    }
    return out;
}

std::vector<std::string> PackageManager::verify() const {
    std::vector<std::string> problems;
    for (const auto& p : list()) {
        std::error_code ec;
        if (!fs::is_regular_file(p.apkPath, ec)) {
            problems.push_back(p.packageName + ": APK missing (" + toUtf8(p.apkPath) + ")");
            continue;
        }
        if (hashFile(p.apkPath) != p.sha256)
            problems.push_back(p.packageName + ": APK checksum mismatch (file modified or corrupt)");
        if (!p.iconPath.empty() && !fs::is_regular_file(p.iconPath, ec))
            problems.push_back(p.packageName + ": icon missing (" + toUtf8(p.iconPath) + ")");
    }
    // Orphaned folders (e.g. left behind by a crash) that the database does not know about.
    std::error_code ec;
    for (fs::directory_iterator it(root_ / "apps", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().u8string();
        if (it->is_directory(ec) && !find(name))
            problems.push_back(name + ": folder is not registered in packages.db");
    }
    return problems;
}

} // namespace pkgmgr
