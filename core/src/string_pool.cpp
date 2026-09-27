#include "string_pool.h"

namespace apkcore::detail {
namespace {

constexpr std::uint32_t kUtf8Flag = 1u << 8;

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

std::string decodeUtf8(const ByteView& c, std::size_t off) {
    auto readLen = [&](std::size_t& o) -> std::uint32_t {
        std::uint32_t n = c.u8(o++);
        if (n & 0x80) n = ((n & 0x7F) << 8) | c.u8(o++);
        return n;
    };
    readLen(off);                       // UTF-16 length (unused)
    const std::uint32_t bytes = readLen(off);
    ByteView s = c.sub(off, bytes);
    return std::string(reinterpret_cast<const char*>(s.data()), bytes);
}

std::string decodeUtf16(const ByteView& c, std::size_t off) {
    std::uint32_t n = c.u16(off);
    off += 2;
    if (n & 0x8000) {
        n = ((n & 0x7FFF) << 16) | c.u16(off);
        off += 2;
    }
    std::string out;
    out.reserve(n);
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint32_t u = c.u16(off + std::size_t(i) * 2);
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            const std::uint32_t lo = c.u16(off + std::size_t(i + 1) * 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        appendUtf8(out, u);
    }
    return out;
}

} // namespace

StringPool::StringPool(ByteView c) {
    const ChunkHeader h = readChunk(c, 0);
    if (h.type != chunk::StringPool) throw ParseError("expected string pool");

    const std::uint32_t count = c.u32(8);
    const std::uint32_t flags = c.u32(16);
    const std::uint32_t stringsStart = c.u32(20);
    const bool utf8 = (flags & kUtf8Flag) != 0;

    strings_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        try {
            const std::size_t off = std::size_t(stringsStart) + c.u32(h.headerSize + std::size_t(i) * 4);
            strings_.push_back(utf8 ? decodeUtf8(c, off) : decodeUtf16(c, off));
        } catch (const ParseError&) {
            strings_.emplace_back();  // keep indices aligned even if one string is corrupt
        }
    }
}

const std::string& StringPool::at(std::uint32_t index) const {
    static const std::string empty;
    return index < strings_.size() ? strings_[index] : empty;
}

} // namespace apkcore::detail
