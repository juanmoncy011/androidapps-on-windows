#pragma once
// Builds minimal but real APKs (ZIP + binary AndroidManifest.xml) for tests.
#include <filesystem>
#include <string>
#include <vector>

struct TestApk {
    std::string package = "com.example.app";
    std::string label = "Example";
    std::string versionName = "1.0";
    int versionCode = 1;
    int minSdk = 21;
    int targetSdk = 34;
    std::string split;                     // non-empty -> split APK
    std::vector<std::string> permissions;
    std::vector<std::string> abis;         // adds lib/<abi>/libtest.so
    std::string payload = "classes";       // classes.dex contents; vary to change the file hash
};

// Writes the APK; returns false on failure.
bool writeTestApk(const std::filesystem::path& file, const TestApk& apk);
