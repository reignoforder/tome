#include "pe.h"

#include <cstring>

namespace tome::pe {
namespace {

bool read_u16(const std::vector<uint8_t>& d, size_t at, uint16_t& out) {
    if (at > d.size() || d.size() - at < 2) return false;
    out = static_cast<uint16_t>(d[at] | d[at + 1] << 8);
    return true;
}

bool read_u32(const std::vector<uint8_t>& d, size_t at, uint32_t& out) {
    if (at > d.size() || d.size() - at < 4) return false;
    std::memcpy(&out, d.data() + at, 4);
    return true;
}

} // namespace

bool map_image(const std::vector<uint8_t>& file, Image& out, std::string& error) {
    out = Image{};
    uint16_t mz = 0, machine = 0, section_count = 0, optional_size = 0;
    uint32_t nt = 0, signature = 0, headers_size = 0;
    if (!read_u16(file, 0, mz) || mz != 0x5A4D || !read_u32(file, 0x3C, nt) || !read_u32(file, nt, signature) ||
        signature != 0x00004550) {
        error = "not a PE file";
        return false;
    }
    const size_t optional = size_t(nt) + 24;
    if (!read_u16(file, nt + 4, machine) || !read_u16(file, nt + 6, section_count) ||
        !read_u32(file, nt + 8, out.timestamp) || !read_u16(file, nt + 20, optional_size) ||
        !read_u32(file, optional + 56, out.size_of_image) || !read_u32(file, optional + 60, headers_size)) {
        error = "truncated PE header";
        return false;
    }
    if (machine != 0x14C) {
        error = "not a 32-bit x86 image";
        return false;
    }
    if (out.size_of_image == 0 || out.size_of_image > 256u << 20 || headers_size > out.size_of_image) {
        error = "implausible SizeOfImage";
        return false;
    }

    out.bytes.assign(out.size_of_image, 0);
    std::memcpy(out.bytes.data(), file.data(), headers_size < file.size() ? headers_size : file.size());
    size_t header = optional + optional_size;
    for (uint16_t i = 0; i < section_count; ++i, header += 40) {
        uint32_t virtual_size = 0, rva = 0, raw_size = 0, raw_offset = 0;
        if (!read_u32(file, header + 8, virtual_size) || !read_u32(file, header + 12, rva) ||
            !read_u32(file, header + 16, raw_size) || !read_u32(file, header + 20, raw_offset)) {
            error = "truncated section table";
            return false;
        }
        // The loader maps min(raw, virtual) bytes from the file and zero-fills the rest.
        uint32_t copy = raw_size < virtual_size || virtual_size == 0 ? raw_size : virtual_size;
        if (raw_offset > file.size() || copy > file.size() - raw_offset || rva > out.size_of_image ||
            copy > out.size_of_image - rva) {
            error = "section " + std::to_string(i) + " lies outside the file or the image";
            return false;
        }
        std::memcpy(out.bytes.data() + rva, file.data() + raw_offset, copy);
    }
    return true;
}

} // namespace tome::pe
