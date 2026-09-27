#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace apkcore {

// Read-only ZIP access (APKs are ZIP files). Unicode paths are supported.
class ZipReader {
public:
    explicit ZipReader(const std::filesystem::path& path);
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool isOpen() const noexcept;
    std::vector<std::string> entries() const;
    // Extracts an entry into memory; nullopt if missing, corrupt or larger than maxSize.
    std::optional<std::vector<std::uint8_t>> read(const std::string& name,
                                                  std::size_t maxSize = 64u << 20) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace apkcore
