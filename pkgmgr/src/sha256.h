#pragma once
// Minimal streaming SHA-256 (FIPS 180-4) for install integrity checks.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pkgmgr::detail {

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t size);
    std::array<std::uint8_t, 32> finish();
    std::string finishHex();

private:
    void block(const std::uint8_t* p);

    std::array<std::uint32_t, 8> h_;
    std::array<std::uint8_t, 64> buf_{};
    std::size_t bufLen_ = 0;
    std::uint64_t totalBytes_ = 0;
};

} // namespace pkgmgr::detail
