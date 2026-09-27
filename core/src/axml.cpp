#include "axml.h"
#include "string_pool.h"

#include <algorithm>
#include <cstdio>

namespace apkcore::detail {
namespace {

// Resource IDs of framework attributes we care about. Used when obfuscators
// strip or rename the attribute name strings - the ID is authoritative.
const char* knownAttrName(std::uint32_t id) {
    switch (id) {
    case 0x01010001: return "label";
    case 0x01010002: return "icon";
    case 0x01010003: return "name";
    case 0x0101020c: return "minSdkVersion";
    case 0x0101021b: return "versionCode";
    case 0x0101021c: return "versionName";
    case 0x01010270: return "targetSdkVersion";
    case 0x01010271: return "maxSdkVersion";
    default: return nullptr;
    }
}

} // namespace

std::string XmlAttribute::text() const {
    if (hasString) return str;
    char buf[32];
    switch (value.type) {
    case restype::IntDec: return std::to_string(std::int32_t(value.data));
    case restype::IntHex: std::snprintf(buf, sizeof buf, "0x%x", unsigned(value.data)); return buf;
    case restype::IntBoolean: return value.data ? "true" : "false";
    case restype::Reference: std::snprintf(buf, sizeof buf, "@0x%08x", unsigned(value.data)); return buf;
    default: std::snprintf(buf, sizeof buf, "0x%08x", unsigned(value.data)); return buf;
    }
}

int XmlAttribute::asInt(int fallback) const {
    if (value.type == restype::IntDec || value.type == restype::IntHex)
        return std::int32_t(value.data);
    if (hasString) {
        try { return std::stoi(str); } catch (...) {}
    }
    return fallback;
}

XmlDocument XmlDocument::parse(ByteView v) {
    if (v.u16(0) != chunk::Xml) throw ParseError("not a binary XML document");
    const std::size_t end = std::min<std::size_t>(v.u32(4), v.size());

    XmlDocument doc;
    StringPool pool;
    std::vector<std::uint32_t> resMap;
    std::vector<int> stack;

    std::size_t off = v.u16(2);
    while (off + 8 <= end) {
        const ChunkHeader h = readChunk(v, off);
        const ByteView c = v.sub(off, h.size);

        switch (h.type) {
        case chunk::StringPool:
            pool = StringPool(c);
            break;

        case chunk::XmlResourceMap:
            resMap.clear();
            for (std::size_t i = h.headerSize; i + 4 <= h.size; i += 4) resMap.push_back(c.u32(i));
            break;

        case chunk::XmlStartElement: {
            const std::size_t ext = h.headerSize;
            XmlElement el;
            el.name = pool.at(c.u32(ext + 4));
            el.parent = stack.empty() ? -1 : stack.back();

            const std::uint16_t attrStart = c.u16(ext + 8);
            std::uint16_t attrSize = c.u16(ext + 10);
            const std::uint16_t attrCount = c.u16(ext + 12);
            if (attrSize < 20) attrSize = 20;

            for (std::uint16_t i = 0; i < attrCount; ++i) {
                const std::size_t a = ext + attrStart + std::size_t(i) * attrSize;
                XmlAttribute at;
                const std::uint32_t nameIdx = c.u32(a + 4);
                const std::uint32_t raw = c.u32(a + 8);
                at.value.type = c.u8(a + 15);
                at.value.data = c.u32(a + 16);
                at.resId = nameIdx < resMap.size() ? resMap[nameIdx] : 0;

                const char* known = knownAttrName(at.resId);
                at.name = known ? known : pool.at(nameIdx);

                if (raw != NoIndex) {
                    at.str = pool.at(raw);
                    at.hasString = true;
                } else if (at.value.type == restype::String) {
                    at.str = pool.at(at.value.data);
                    at.hasString = true;
                }
                el.attrs.push_back(std::move(at));
            }
            stack.push_back(int(doc.elements.size()));
            doc.elements.push_back(std::move(el));
            break;
        }

        case chunk::XmlEndElement:
            if (!stack.empty()) stack.pop_back();
            break;

        default:
            break;  // namespaces, CDATA - not needed
        }
        off += h.size;
    }
    return doc;
}

} // namespace apkcore::detail
