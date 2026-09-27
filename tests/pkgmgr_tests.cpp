// Package Manager tests. Each test gets a fresh root under the temp directory.
#include <pkgmgr/package_manager.h>

#include "sha256.h"
#include "test_apk.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;
using pkgmgr::InstallStatus;
using pkgmgr::PackageManager;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::cerr << "  FAILED: " #cond "  (" << __FILE__ << ":" << __LINE__ << ")\n"; \
            ++g_failures;                                                                  \
        }                                                                                  \
    } while (0)

struct Sandbox {
    fs::path dir;
    Sandbox() {
        static int counter = 0;
        dir = fs::temp_directory_path() /
              ("aow_pkgmgr_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
               "_" + std::to_string(counter++));
        fs::create_directories(dir / "src");
    }
    ~Sandbox() { std::error_code ec; fs::remove_all(dir, ec); }
    fs::path root() const { return dir / "store"; }
    fs::path apk(const std::string& name, const TestApk& a) const {
        const fs::path p = dir / "src" / name;
        if (!writeTestApk(p, a)) { std::cerr << "  cannot write test APK\n"; ++g_failures; }
        return p;
    }
};

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::size_t countFiles(const fs::path& dir) {
    std::size_t n = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir)) n += e.is_regular_file() ? 1 : 0;
    return n;
}

void testSha256() {
    using pkgmgr::detail::Sha256;
    auto hex = [](const std::string& s) { Sha256 h; h.update(s.data(), s.size()); return h.finishHex(); };
    CHECK(hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 chunked;  // same input split across block boundaries
    const std::string million(1000000, 'a');
    for (std::size_t i = 0; i < million.size(); i += 777)
        chunked.update(million.data() + i, std::min<std::size_t>(777, million.size() - i));
    CHECK(chunked.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

void testPackageNames() {
    CHECK(pkgmgr::isValidPackageName("com.example.app"));
    CHECK(pkgmgr::isValidPackageName("a"));
    CHECK(pkgmgr::isValidPackageName("org.app_2.x9"));
    CHECK(!pkgmgr::isValidPackageName(""));
    CHECK(!pkgmgr::isValidPackageName("../evil"));
    CHECK(!pkgmgr::isValidPackageName("com..app"));
    CHECK(!pkgmgr::isValidPackageName("com.app."));
    CHECK(!pkgmgr::isValidPackageName("com.1app"));
    CHECK(!pkgmgr::isValidPackageName("com/app"));
    CHECK(!pkgmgr::isValidPackageName("com\\app"));
}

void testFreshInstall() {
    Sandbox sb;
    PackageManager pm(sb.root());
    TestApk a;
    a.permissions = {"android.permission.INTERNET", "android.permission.CAMERA"};
    a.abis = {"arm64-v8a", "x86_64"};
    const fs::path src = sb.apk("app.apk", a);

    const auto r = pm.install(src);
    CHECK(r.ok());
    CHECK(r.status == InstallStatus::Installed);
    CHECK(r.packageName == "com.example.app");
    CHECK(r.previousVersionCode == 0);

    const auto p = pm.find("com.example.app");
    CHECK(p.has_value());
    if (!p) return;
    CHECK(p->label == "Example");
    CHECK(p->versionName == "1.0");
    CHECK(p->versionCode == 1);
    CHECK(p->minSdk == 21 && p->targetSdk == 34);
    CHECK(p->launchActivity == "com.example.app.MainActivity");
    CHECK(p->permissions == (std::vector<std::string>{"android.permission.CAMERA", "android.permission.INTERNET"}));
    CHECK(p->nativeAbis == (std::vector<std::string>{"arm64-v8a", "x86_64"}));
    CHECK(p->apkPath == sb.root() / "apps" / "com.example.app" / "base.apk");
    CHECK(fs::is_regular_file(p->apkPath));
    CHECK(readFile(p->apkPath) == readFile(src));
    CHECK(p->apkSize == fs::file_size(src));
    CHECK(p->sha256.size() == 64);
    CHECK(p->installedAt > 0 && p->installedAt == p->updatedAt);
    CHECK(countFiles(p->apkPath.parent_path()) == 1);  // no staging leftovers
    CHECK(pm.list().size() == 1);
    CHECK(pm.verify().empty());
}

void testReinstallAndUpdate() {
    Sandbox sb;
    PackageManager pm(sb.root());
    TestApk v1;
    v1.permissions = {"android.permission.INTERNET"};
    const fs::path src1 = sb.apk("v1.apk", v1);
    CHECK(pm.install(src1).status == InstallStatus::Installed);
    const auto first = pm.find(v1.package);

    // Identical file -> nothing to do.
    CHECK(pm.install(src1).status == InstallStatus::AlreadyInstalled);

    // Same version, different bytes -> reinstall.
    TestApk v1b = v1;
    v1b.payload = "rebuilt";
    CHECK(pm.install(sb.apk("v1b.apk", v1b)).status == InstallStatus::Reinstalled);

    // Newer version -> update; permissions replaced; first-install time kept.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    TestApk v2 = v1;
    v2.versionCode = 2;
    v2.versionName = "2.0";
    v2.permissions = {"android.permission.CAMERA"};
    const auto r = pm.install(sb.apk("v2.apk", v2));
    CHECK(r.status == InstallStatus::Updated);
    CHECK(r.previousVersionCode == 1 && r.versionCode == 2);

    const auto p = pm.find(v1.package);
    CHECK(p && first);
    if (!p || !first) return;
    CHECK(p->versionCode == 2 && p->versionName == "2.0");
    CHECK(p->permissions == std::vector<std::string>{"android.permission.CAMERA"});
    CHECK(p->installedAt == first->installedAt);
    CHECK(p->updatedAt > first->updatedAt);
    CHECK(p->sha256 != first->sha256);
    CHECK(pm.list().size() == 1);
    CHECK(pm.verify().empty());
}

void testDowngrade() {
    Sandbox sb;
    PackageManager pm(sb.root());
    TestApk v5;
    v5.versionCode = 5;
    TestApk v3 = v5;
    v3.versionCode = 3;
    CHECK(pm.install(sb.apk("v5.apk", v5)).ok());

    const auto refused = pm.install(sb.apk("v3.apk", v3));
    CHECK(refused.status == InstallStatus::DowngradeRefused);
    CHECK(!refused.ok());
    CHECK(pm.find(v5.package)->versionCode == 5);

    const auto forced = pm.install(sb.dir / "src" / "v3.apk", pkgmgr::InstallOptions{true});
    CHECK(forced.status == InstallStatus::Updated);
    CHECK(pm.find(v5.package)->versionCode == 3);
}

void testRejectsBadInput() {
    Sandbox sb;
    PackageManager pm(sb.root());

    const fs::path junk = sb.dir / "src" / "junk.apk";
    std::ofstream(junk) << "not a zip";
    CHECK(pm.install(junk).status == InstallStatus::InvalidApk);
    CHECK(pm.install(sb.dir / "src" / "missing.apk").status == InstallStatus::InvalidApk);

    TestApk split;
    split.split = "config.arm64_v8a";
    CHECK(pm.install(sb.apk("split.apk", split)).status == InstallStatus::InvalidApk);

    TestApk evil;
    evil.package = "../../evil";
    CHECK(pm.install(sb.apk("evil.apk", evil)).status == InstallStatus::InvalidApk);
    CHECK(!fs::exists(sb.dir / "evil"));

    CHECK(pm.list().empty());
    CHECK(fs::is_empty(sb.root() / "apps"));
}

void testPersistenceAndUninstall() {
    Sandbox sb;
    TestApk a, b;
    b.package = "org.other.tool";
    b.label = "Another";
    {
        PackageManager pm(sb.root());
        CHECK(pm.install(sb.apk("a.apk", a)).ok());
        CHECK(pm.install(sb.apk("b.apk", b)).ok());
    }
    PackageManager pm(sb.root());  // reopen: data must come back from SQLite
    const auto all = pm.list();
    CHECK(all.size() == 2);
    if (all.size() == 2) CHECK(all[0].label == "Another" && all[1].label == "Example");  // sorted by name

    std::string err;
    CHECK(pm.uninstall(a.package, &err));
    CHECK(!pm.find(a.package));
    CHECK(!fs::exists(sb.root() / "apps" / a.package));
    CHECK(pm.list().size() == 1);

    CHECK(!pm.uninstall(a.package, &err));
    CHECK(err.find("not installed") != std::string::npos);
    CHECK(!pm.uninstall("../x", &err));
}

void testVerifyDetectsProblems() {
    Sandbox sb;
    PackageManager pm(sb.root());
    TestApk a;
    CHECK(pm.install(sb.apk("a.apk", a)).ok());
    const auto p = pm.find(a.package);
    if (!p) { CHECK(p.has_value()); return; }

    std::ofstream(p->apkPath, std::ios::app) << "tampered";
    auto problems = pm.verify();
    CHECK(problems.size() == 1 && problems[0].find("checksum") != std::string::npos);

    fs::remove(p->apkPath);
    fs::create_directories(sb.root() / "apps" / "com.stray.folder");
    problems = pm.verify();
    CHECK(problems.size() == 2);

    // Reinstalling repairs the missing file.
    CHECK(pm.install(sb.dir / "src" / "a.apk").status == InstallStatus::Reinstalled);
    CHECK(fs::is_regular_file(p->apkPath));
}

void testConcurrentInstalls() {
    Sandbox sb;
    PackageManager pm(sb.root());
    std::vector<fs::path> apks;
    for (int i = 0; i < 8; ++i) {
        TestApk t;
        t.package = "com.example.app" + std::to_string(i);
        apks.push_back(sb.apk("c" + std::to_string(i) + ".apk", t));
    }
    std::vector<std::thread> threads;
    std::atomic<int> ok{0};
    for (const auto& p : apks) threads.emplace_back([&, p] { if (pm.install(p).ok()) ++ok; });
    for (auto& t : threads) t.join();
    CHECK(ok == 8);
    CHECK(pm.list().size() == 8);

    // Racing installs of different versions of one package: exactly one wins per
    // step, the database stays consistent and no staging files are left behind.
    std::vector<fs::path> versions;
    for (int v = 1; v <= 6; ++v) {
        TestApk t;
        t.package = "com.example.race";
        t.versionCode = v;
        versions.push_back(sb.apk("race" + std::to_string(v) + ".apk", t));
    }
    threads.clear();
    for (const auto& p : versions) threads.emplace_back([&, p] { pm.install(p); });
    for (auto& t : threads) t.join();
    const auto race = pm.find("com.example.race");
    CHECK(race.has_value());
    CHECK(pm.verify().empty());
    CHECK(countFiles(sb.root() / "apps" / "com.example.race") == 1);
}

} // namespace

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"sha256", testSha256},
        {"package names", testPackageNames},
        {"fresh install", testFreshInstall},
        {"reinstall and update", testReinstallAndUpdate},
        {"downgrade", testDowngrade},
        {"rejects bad input", testRejectsBadInput},
        {"persistence and uninstall", testPersistenceAndUninstall},
        {"verify", testVerifyDetectsProblems},
        {"concurrent installs", testConcurrentInstalls},
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
