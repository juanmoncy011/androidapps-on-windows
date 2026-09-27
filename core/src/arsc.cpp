#include "arsc.h"

#include <algorithm>
#include <climits>

namespace apkcore::detail {
namespace {

constexpr std::uint8_t kTypeFlagSparse = 0x01;
constexpr std::uint8_t kTypeFlagOffset16 = 0x02;
constexpr std::uint16_t kEntryFlagComplex = 0x0001;
constexpr std::uint16_t kEntryFlagCompact = 0x0008;

bool endsWith(const std::string& s, const char* suffix) {
    const std::string suf(suffix);
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

} // namespace

ResourceTable ResourceTable::parse(ByteView v) {
    if (v.u16(0) != chunk::Table) throw ParseError("not a resource table");
    const std::size_t end = std::min<std::size_t>(v.u32(4), v.size());

    ResourceTable t;
    std::size_t off = v.u16(2);
    while (off + 8 <= end) {
        const ChunkHeader h = readChunk(v, off);
        const ByteView c = v.sub(off, h.size);
        if (h.type == chunk::StringPool) t.globalStrings_ = StringPool(c);
        else if (h.type == chunk::TablePackage) t.parsePackage(c, h);
        off += h.size;
    }
    return t;
}

void ResourceTable::parsePackage(const ByteView& c, const ChunkHeader& h) {
    const std::uint32_t pkgId = c.u32(8);
    const std::uint32_t typeIdOffset = h.headerSize >= 288 ? c.u32(284) : 0;

    std::size_t off = h.headerSize;
    while (off + 8 <= h.size) {
        const ChunkHeader ch = readChunk(c, off);
        if (ch.type == chunk::TableType) {
            try {
                parseType(c.sub(off, ch.size), ch, pkgId, typeIdOffset);
            } catch (const ParseError&) {
                // skip a malformed type chunk, keep the rest
            }
        }
        off += ch.size;
    }
}

void ResourceTable::parseType(const ByteView& c, const ChunkHeader& h,
                              std::uint32_t pkgId, std::uint32_t typeIdOffset) {
    const std::uint8_t typeId = c.u8(8);
    const std::uint8_t flags = c.u8(9);
    const std::uint32_t entryCount = c.u32(12);
    const std::uint32_t entriesStart = c.u32(16);

    // ResTable_config starts at offset 20
    const std::size_t cfg = 20;
    const std::uint32_t cfgSize = c.u32(cfg);
    const bool defaultLocale = cfgSize < 12 || (c.u8(cfg + 8) == 0 && c.u8(cfg + 9) == 0);
    const std::uint16_t density = cfgSize >= 16 ? c.u16(cfg + 14) : 0;

    const std::uint32_t idBase = (pkgId << 24) | ((std::uint32_t(typeId) + typeIdOffset) << 16);

    auto addEntry = [&](std::uint32_t index, std::uint32_t entryOff) {
        try {
            const std::size_t e = std::size_t(entriesStart) + entryOff;
            const std::uint16_t sizeOrKey = c.u16(e);
            const std::uint16_t eflags = c.u16(e + 2);
            ResValue val;
            if (eflags & kEntryFlagCompact) {
                val.type = std::uint8_t(eflags >> 8);
                val.data = c.u32(e + 4);
            } else if (eflags & kEntryFlagComplex) {
                return;  // style/array/plural maps - not needed here
            } else {
                const std::size_t rv = e + sizeOrKey;
                val.type = c.u8(rv + 3);
                val.data = c.u32(rv + 4);
            }
            entries_[idBase | (index & 0xFFFF)].push_back({val, density, defaultLocale});
        } catch (const ParseError&) {
        }
    };

    const std::size_t offsets = h.headerSize;
    if (flags & kTypeFlagSparse) {
        for (std::uint32_t i = 0; i < entryCount; ++i)
            addEntry(c.u16(offsets + std::size_t(i) * 4), std::uint32_t(c.u16(offsets + std::size_t(i) * 4 + 2)) * 4);
    } else if (flags & kTypeFlagOffset16) {
        for (std::uint32_t i = 0; i < entryCount; ++i) {
            const std::uint16_t o = c.u16(offsets + std::size_t(i) * 2);
            if (o != 0xFFFF) addEntry(i, std::uint32_t(o) * 4);
        }
    } else {
        for (std::uint32_t i = 0; i < entryCount; ++i) {
            const std::uint32_t o = c.u32(offsets + std::size_t(i) * 4);
            if (o != NoIndex) addEntry(i, o);
        }
    }
}

int ResourceTable::score(const Candidate& c, bool wantFile) const {
    if (!wantFile) return c.defaultLocale ? 1 : 0;
    int s = 0;
    if (c.value.type == restype::String) {
        const std::string& p = globalStrings_.at(c.value.data);
        if (endsWith(p, ".png") || endsWith(p, ".webp") || endsWith(p, ".jpg")) s += 100000;
    }
    if (c.density > 0 && c.density < 0xFFFE) s += c.density;  // skip anydpi/nodpi markers
    return s;
}

std::optional<std::string> ResourceTable::resolve(std::uint32_t id, bool wantFile) const {
    for (int depth = 0; depth < 8; ++depth) {
        const auto it = entries_.find(id);
        if (it == entries_.end() || it->second.empty()) return std::nullopt;

        const Candidate* best = nullptr;
        int bestScore = INT_MIN;
        for (const auto& cand : it->second) {
            const int s = score(cand, wantFile);
            if (s > bestScore) { best = &cand; bestScore = s; }
        }
        if (best->value.type == restype::String) return globalStrings_.at(best->value.data);
        if (best->value.type == restype::Reference) { id = best->value.data; continue; }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> ResourceTable::resolveString(std::uint32_t resId) const {
    return resolve(resId, false);
}

std::optional<std::string> ResourceTable::resolveFile(std::uint32_t resId) const {
    return resolve(resId, true);
}

} // namespace apkcore::detail
