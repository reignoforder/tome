// A .tome package: the central directory, every entry unpacked, and the
// package checks, with small limits so a run doesn't stall on huge sizes.
#include "core/package.h"
#include "core/zip.h"
#include "names.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view archive(reinterpret_cast<const char*>(data), size);
    std::vector<tome::zip::Entry> entries;
    std::string error;
    tome::zip::Limits limits;
    limits.max_total = 16u << 20;
    limits.max_entries = 1000;
    if (!tome::zip::read_directory(archive, entries, error, limits)) return 0;
    tome::package::check_entries(entries, error);
    for (const tome::zip::Entry& entry : entries) {
        check_safe_name(entry.name);
        if (entry.directory()) continue;
        std::string out;
        tome::zip::extract(archive, entry, out, error);
    }
    return 0;
}
