#include <apkcore/zip_reader.h>

#include <miniz.h>

#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace apkcore {

struct ZipReader::Impl {
    std::ifstream file;
    mz_zip_archive zip{};
    bool open = false;

    // miniz read callback over std::ifstream so wide (Unicode) paths work on Windows.
    static size_t readCallback(void* opaque, mz_uint64 offset, void* buf, size_t n) {
        auto* self = static_cast<Impl*>(opaque);
        self->file.clear();
        self->file.seekg(static_cast<std::streamoff>(offset));
        if (!self->file) return 0;
        self->file.read(static_cast<char*>(buf), static_cast<std::streamsize>(n));
        return static_cast<size_t>(self->file.gcount());
    }
};

ZipReader::ZipReader(const fs::path& path) : impl_(std::make_unique<Impl>()) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return;
    impl_->file.open(path, std::ios::binary);
    if (!impl_->file) return;
    impl_->zip.m_pRead = &Impl::readCallback;
    impl_->zip.m_pIO_opaque = impl_.get();
    impl_->open = mz_zip_reader_init(&impl_->zip, size, 0) != 0;
}

ZipReader::~ZipReader() {
    if (impl_ && impl_->open) mz_zip_reader_end(&impl_->zip);
}

bool ZipReader::isOpen() const noexcept { return impl_ && impl_->open; }

std::vector<std::string> ZipReader::entries() const {
    std::vector<std::string> out;
    if (!isOpen()) return out;
    const mz_uint count = mz_zip_reader_get_num_files(&impl_->zip);
    out.reserve(count);
    char name[1024];
    for (mz_uint i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(&impl_->zip, i)) continue;
        if (mz_zip_reader_get_filename(&impl_->zip, i, name, sizeof name) > 0) out.emplace_back(name);
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> ZipReader::read(const std::string& name, std::size_t maxSize) const {
    if (!isOpen()) return std::nullopt;
    const int index = mz_zip_reader_locate_file(&impl_->zip, name.c_str(), nullptr, 0);
    if (index < 0) return std::nullopt;

    mz_zip_archive_file_stat st;
    if (!mz_zip_reader_file_stat(&impl_->zip, mz_uint(index), &st) || st.m_uncomp_size > maxSize)
        return std::nullopt;

    std::vector<std::uint8_t> out(static_cast<std::size_t>(st.m_uncomp_size));
    if (out.empty()) return out;
    if (!mz_zip_reader_extract_to_mem(&impl_->zip, mz_uint(index), out.data(), out.size(), 0))
        return std::nullopt;
    return out;
}

} // namespace apkcore
