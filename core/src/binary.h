#pragma once
// Bounds-checked little-endian reader + Android resource chunk constants.
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace apkcore::detail {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class ByteView {
public:
    ByteView() = default;
    ByteView(const std::uint8_t* d, std::size_t n) : data_(d), size_(n) {}
    explicit ByteView(const std::vector<std::uint8_t>& v) : data_(v.data()), size_(v.size()) {}

    std::size_t size() const { return size_; }
    const std::uint8_t* data() const { return data_; }
    bool has(std::size_t off, std::size_t n) const { return off <= size_ && n <= size_ - off; }

    std::uint8_t u8(std::size_t off) const { check(off, 1); return data_[off]; }
    std::uint16_t u16(std::size_t off) const {
        check(off, 2);
        return std::uint16_t(data_[off] | (data_[off + 1] << 8));
    }
    std::uint32_t u32(std::size_t off) const {
        check(off, 4);
        return std::uint32_t(data_[off]) | (std::uint32_t(data_[off + 1]) << 8) |
               (std::uint32_t(data_[off + 2]) << 16) | (std::uint32_t(data_[off + 3]) << 24);
    }
    ByteView sub(std::size_t off, std::size_t n) const { check(off, n); return {data_ + off, n}; }

private:
    void check(std::size_t off, std::size_t n) const {
        if (!has(off, n)) throw ParseError("read out of bounds");
    }
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

struct ChunkHeader {
    std::uint16_t type;
    std::uint16_t headerSize;
    std::uint32_t size;
};

inline ChunkHeader readChunk(const ByteView& v, std::size_t off) {
    ChunkHeader h{v.u16(off), v.u16(off + 2), v.u32(off + 4)};
    if (h.headerSize < 8 || h.size < h.headerSize || !v.has(off, h.size))
        throw ParseError("bad chunk header");
    return h;
}

namespace chunk {
constexpr std::uint16_t StringPool = 0x0001, Table = 0x0002, Xml = 0x0003;
constexpr std::uint16_t XmlStartNs = 0x0100, XmlEndNs = 0x0101, XmlStartElement = 0x0102,
                        XmlEndElement = 0x0103, XmlCData = 0x0104, XmlResourceMap = 0x0180;
constexpr std::uint16_t TablePackage = 0x0200, TableType = 0x0201, TableTypeSpec = 0x0202;
} // namespace chunk

namespace restype {
constexpr std::uint8_t Null = 0x00, Reference = 0x01, Attribute = 0x02, String = 0x03,
                       Float = 0x04, IntDec = 0x10, IntHex = 0x11, IntBoolean = 0x12;
} // namespace restype

struct ResValue {
    std::uint8_t type = restype::Null;
    std::uint32_t data = 0;
};

constexpr std::uint32_t NoIndex = 0xFFFFFFFFu;

} // namespace apkcore::detail
