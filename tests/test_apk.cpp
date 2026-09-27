#include "test_apk.h"

#include <miniz.h>

#include <cstdint>
#include <map>

namespace {

constexpr std::uint32_t kNone = 0xFFFFFFFFu;
constexpr std::uint8_t kTypeString = 0x03, kTypeIntDec = 0x10;

class AxmlWriter {
public:
    struct Attr { std::string name; bool isString; std::string str; int value; };

    void start(const std::string& name, const std::vector<Attr>& attrs) {
        std::vector<std::uint8_t> c;
        put16(c, 0x0102); put16(c, 16); put32(c, 0);          // header, size patched below
        put32(c, 1); put32(c, kNone);                          // line, comment
        put32(c, kNone); put32(c, str(name));                  // ns, name
        put16(c, 20); put16(c, 20); put16(c, std::uint16_t(attrs.size()));
        put16(c, 0); put16(c, 0); put16(c, 0);                 // id/class/style index
        for (const Attr& a : attrs) {
            put32(c, kNone); put32(c, str(a.name));
            const std::uint32_t s = a.isString ? str(a.str) : kNone;
            put32(c, s);
            put16(c, 8); c.push_back(0); c.push_back(a.isString ? kTypeString : kTypeIntDec);
            put32(c, a.isString ? s : std::uint32_t(a.value));
        }
        patch32(c, 4, std::uint32_t(c.size()));
        body_.insert(body_.end(), c.begin(), c.end());
    }

    void end(const std::string& name) {
        std::vector<std::uint8_t> c;
        put16(c, 0x0103); put16(c, 16); put32(c, 24);
        put32(c, 1); put32(c, kNone); put32(c, kNone); put32(c, str(name));
        body_.insert(body_.end(), c.begin(), c.end());
    }

    std::vector<std::uint8_t> finish() const {
        // UTF-8 string pool
        std::vector<std::uint8_t> data;
        std::vector<std::uint32_t> offsets;
        for (const auto& s : strings_) {
            offsets.push_back(std::uint32_t(data.size()));
            data.push_back(std::uint8_t(s.size()));  // strings here are < 128 chars
            data.push_back(std::uint8_t(s.size()));
            data.insert(data.end(), s.begin(), s.end());
            data.push_back(0);
        }
        while (data.size() % 4) data.push_back(0);

        std::vector<std::uint8_t> pool;
        const std::uint32_t headerSize = 28, stringsStart = headerSize + 4 * std::uint32_t(strings_.size());
        put16(pool, 0x0001); put16(pool, std::uint16_t(headerSize));
        put32(pool, stringsStart + std::uint32_t(data.size()));
        put32(pool, std::uint32_t(strings_.size())); put32(pool, 0);  // string/style count
        put32(pool, 1u << 8); put32(pool, stringsStart); put32(pool, 0);  // UTF-8 flag
        for (std::uint32_t o : offsets) put32(pool, o);
        pool.insert(pool.end(), data.begin(), data.end());

        std::vector<std::uint8_t> out;
        put16(out, 0x0003); put16(out, 8);
        put32(out, std::uint32_t(8 + pool.size() + body_.size()));
        out.insert(out.end(), pool.begin(), pool.end());
        out.insert(out.end(), body_.begin(), body_.end());
        return out;
    }

private:
    std::uint32_t str(const std::string& s) {
        auto it = index_.find(s);
        if (it != index_.end()) return it->second;
        strings_.push_back(s);
        return index_[s] = std::uint32_t(strings_.size() - 1);
    }
    static void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
        v.push_back(std::uint8_t(x)); v.push_back(std::uint8_t(x >> 8));
    }
    static void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int i = 0; i < 4; ++i) v.push_back(std::uint8_t(x >> (8 * i)));
    }
    static void patch32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
        for (int i = 0; i < 4; ++i) v[at + std::size_t(i)] = std::uint8_t(x >> (8 * i));
    }

    std::vector<std::string> strings_;
    std::map<std::string, std::uint32_t> index_;
    std::vector<std::uint8_t> body_;
};

} // namespace

bool writeTestApk(const std::filesystem::path& file, const TestApk& apk) {
    using A = AxmlWriter::Attr;
    AxmlWriter x;
    std::vector<A> manifest = {{"package", true, apk.package, 0},
                               {"versionCode", false, {}, apk.versionCode},
                               {"versionName", true, apk.versionName, 0}};
    if (!apk.split.empty()) manifest.push_back({"split", true, apk.split, 0});
    x.start("manifest", manifest);
    x.start("uses-sdk", {{"minSdkVersion", false, {}, apk.minSdk}, {"targetSdkVersion", false, {}, apk.targetSdk}});
    x.end("uses-sdk");
    for (const auto& p : apk.permissions) {
        x.start("uses-permission", {{"name", true, p, 0}});
        x.end("uses-permission");
    }
    x.start("application", {{"label", true, apk.label, 0}});
    x.start("activity", {{"name", true, ".MainActivity", 0}});
    x.start("intent-filter", {});
    x.start("action", {{"name", true, "android.intent.action.MAIN", 0}});
    x.end("action");
    x.start("category", {{"name", true, "android.intent.category.LAUNCHER", 0}});
    x.end("category");
    x.end("intent-filter");
    x.end("activity");
    x.end("application");
    x.end("manifest");
    const auto manifestBytes = x.finish();

    std::error_code ec;
    std::filesystem::remove(file, ec);
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, file.string().c_str(), 0)) return false;
    bool ok = mz_zip_writer_add_mem(&zip, "AndroidManifest.xml", manifestBytes.data(), manifestBytes.size(),
                                    MZ_DEFAULT_COMPRESSION) &&
              mz_zip_writer_add_mem(&zip, "classes.dex", apk.payload.data(), apk.payload.size(),
                                    MZ_DEFAULT_COMPRESSION);
    for (const auto& abi : apk.abis) {
        const std::string name = "lib/" + abi + "/libtest.so";
        ok = ok && mz_zip_writer_add_mem(&zip, name.c_str(), "ELF", 3, MZ_NO_COMPRESSION);
    }
    ok = ok && mz_zip_writer_finalize_archive(&zip);
    return mz_zip_writer_end(&zip) && ok;
}
