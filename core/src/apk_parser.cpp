#include <apkcore/apk.h>
#include <apkcore/zip_reader.h>

#include "arsc.h"
#include "axml.h"

#include <algorithm>
#include <optional>

namespace fs = std::filesystem;
using namespace apkcore::detail;

namespace apkcore {
namespace {

std::string qualifyClass(const std::string& pkg, const std::string& name) {
    if (name.empty()) return name;
    if (name[0] == '.') return pkg + name;
    if (name.find('.') == std::string::npos) return pkg + "." + name;
    return name;
}

// First <activity>/<activity-alias> whose intent-filter has MAIN + LAUNCHER.
std::string findLauncherActivity(const XmlDocument& doc, const std::string& pkg) {
    const auto& els = doc.elements;
    for (std::size_t f = 0; f < els.size(); ++f) {
        if (els[f].name != "intent-filter") continue;
        bool isMain = false, isLauncher = false;
        for (std::size_t c = f + 1; c < els.size(); ++c) {
            if (els[c].parent != int(f)) continue;
            const XmlAttribute* n = els[c].attr("name");
            if (!n) continue;
            const std::string v = n->text();
            if (els[c].name == "action" && v == "android.intent.action.MAIN") isMain = true;
            if (els[c].name == "category" && v == "android.intent.category.LAUNCHER") isLauncher = true;
        }
        const int p = els[f].parent;
        if (isMain && isLauncher && p >= 0 &&
            (els[p].name == "activity" || els[p].name == "activity-alias")) {
            if (const XmlAttribute* n = els[p].attr("name")) return qualifyClass(pkg, n->text());
        }
    }
    return {};
}

} // namespace

std::string ApkInfo::displayName() const {
    if (!label.empty()) return label;
    if (!packageName.empty()) return packageName;
    return path.filename().u8string();
}

ApkInfo parseApk(const fs::path& apkPath) {
    ApkInfo info;
    info.path = apkPath;
    std::error_code ec;
    info.fileSize = fs::file_size(apkPath, ec);

    try {
        ZipReader zip(apkPath);
        if (!zip.isOpen()) { info.error = "Not a valid ZIP/APK archive"; return info; }

        const auto manifestBytes = zip.read("AndroidManifest.xml");
        if (!manifestBytes) { info.error = "AndroidManifest.xml not found"; return info; }
        const XmlDocument doc = XmlDocument::parse(ByteView(*manifestBytes));

        std::optional<ResourceTable> res;
        if (const auto arsc = zip.read("resources.arsc", 256u << 20)) {
            try { res = ResourceTable::parse(ByteView(*arsc)); } catch (const ParseError&) {}
        }

        // Attribute -> text, resolving @string/... references through resources.arsc.
        auto text = [&](const XmlAttribute* a) -> std::string {
            if (!a) return {};
            if (a->isReference()) {
                if (res)
                    if (auto s = res->resolveString(a->value.data)) return *s;
                return {};
            }
            return a->text();
        };

        for (const XmlElement& el : doc.elements) {
            if (el.name == "manifest") {
                info.packageName = text(el.attr("package"));
                info.versionName = text(el.attr("versionName"));
                info.splitName = text(el.attr("split"));
                if (const auto* a = el.attr("versionCode")) info.versionCode = a->asInt();
            } else if (el.name == "uses-sdk") {
                if (const auto* a = el.attr("minSdkVersion")) info.minSdk = a->asInt();
                if (const auto* a = el.attr("targetSdkVersion")) info.targetSdk = a->asInt();
            } else if (el.name == "application") {
                info.label = text(el.attr("label"));
                const auto* icon = el.attr("icon");
                if (icon && icon->isReference() && res)
                    if (auto p = res->resolveFile(icon->value.data)) info.iconPath = *p;
            } else if (el.name == "uses-permission" || el.name == "uses-permission-sdk-23") {
                const std::string perm = text(el.attr("name"));
                if (!perm.empty()) info.permissions.push_back(perm);
            }
        }
        if (info.packageName.empty()) { info.error = "Manifest has no package name"; return info; }

        info.launchActivity = findLauncherActivity(doc, info.packageName);

        for (const std::string& e : zip.entries()) {
            if (e.rfind("lib/", 0) != 0) continue;
            const auto slash = e.find('/', 4);
            if (slash == std::string::npos || slash == 4) continue;
            std::string abi = e.substr(4, slash - 4);
            if (std::find(info.nativeAbis.begin(), info.nativeAbis.end(), abi) == info.nativeAbis.end())
                info.nativeAbis.push_back(std::move(abi));
        }

        if (!info.iconPath.empty())
            if (auto data = zip.read(info.iconPath, 4u << 20)) info.iconData = std::move(*data);
    } catch (const std::exception& e) {
        info.error = std::string("Parse error: ") + e.what();
    }
    return info;
}

} // namespace apkcore
