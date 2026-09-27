#pragma once
// Android binary XML (AXML) parser - enough for AndroidManifest.xml.
#include "binary.h"
#include <string>
#include <string_view>
#include <vector>

namespace apkcore::detail {

struct XmlAttribute {
    std::string name;           // e.g. "versionCode" (android: prefix dropped)
    std::uint32_t resId = 0;    // android attribute resource id, if known
    bool hasString = false;
    std::string str;
    ResValue value;

    bool isReference() const { return !hasString && value.type == restype::Reference; }
    std::string text() const;
    int asInt(int fallback = 0) const;
};

struct XmlElement {
    std::string name;
    int parent = -1;            // index into XmlDocument::elements
    std::vector<XmlAttribute> attrs;

    const XmlAttribute* attr(std::string_view n) const {
        for (const auto& a : attrs)
            if (a.name == n) return &a;
        return nullptr;
    }
};

struct XmlDocument {
    std::vector<XmlElement> elements;  // document order
    static XmlDocument parse(ByteView data);
};

} // namespace apkcore::detail
