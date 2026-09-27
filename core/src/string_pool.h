#pragma once
#include "binary.h"
#include <string>
#include <vector>

namespace apkcore::detail {

// ResStringPool chunk decoded eagerly into UTF-8 strings.
class StringPool {
public:
    StringPool() = default;
    explicit StringPool(ByteView chunkData);

    std::size_t size() const { return strings_.size(); }
    const std::string& at(std::uint32_t index) const;  // empty string if out of range

private:
    std::vector<std::string> strings_;
};

} // namespace apkcore::detail
