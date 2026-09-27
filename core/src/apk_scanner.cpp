#include <apkcore/apk.h>

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;

namespace apkcore {
namespace {

bool isApk(const fs::path& p) {
    std::string ext = p.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext == ".apk";
}

} // namespace

std::vector<ApkInfo> scanFolder(const fs::path& folder, const ScanOptions& options, const ScanProgress& progress) {
    std::vector<fs::path> files;
    std::error_code ec;
    const auto opts = fs::directory_options::skip_permission_denied;

    auto consider = [&](const fs::directory_entry& e) {
        std::error_code ec2;
        if (e.is_regular_file(ec2) && isApk(e.path())) files.push_back(e.path());
    };

    if (options.recursive) {
        for (fs::recursive_directory_iterator it(folder, opts, ec), end; !ec && it != end; it.increment(ec))
            consider(*it);
    } else {
        for (fs::directory_iterator it(folder, opts, ec), end; !ec && it != end; it.increment(ec))
            consider(*it);
    }
    std::sort(files.begin(), files.end());

    std::vector<ApkInfo> out;
    out.reserve(files.size());
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (progress && !progress(i, files.size())) return out;
        out.push_back(parseApk(files[i]));
    }
    if (progress) progress(out.size(), files.size());
    return out;
}

} // namespace apkcore
