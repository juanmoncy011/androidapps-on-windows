#pragma once
// resources.arsc reader - resolves @string and @mipmap/@drawable references.
#include "binary.h"
#include "string_pool.h"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace apkcore::detail {

class ResourceTable {
public:
    static ResourceTable parse(ByteView data);

    // Resolve to a string value, preferring the default (non-localized) config.
    std::optional<std::string> resolveString(std::uint32_t resId) const;
    // Resolve to a file path inside the APK, preferring raster images at the highest density.
    std::optional<std::string> resolveFile(std::uint32_t resId) const;

private:
    struct Candidate {
        ResValue value;
        std::uint16_t density = 0;
        bool defaultLocale = true;
    };

    void parsePackage(const ByteView& c, const ChunkHeader& h);
    void parseType(const ByteView& c, const ChunkHeader& h, std::uint32_t pkgId, std::uint32_t typeIdOffset);
    std::optional<std::string> resolve(std::uint32_t resId, bool wantFile) const;
    int score(const Candidate& c, bool wantFile) const;

    StringPool globalStrings_;
    std::unordered_map<std::uint32_t, std::vector<Candidate>> entries_;
};

} // namespace apkcore::detail
