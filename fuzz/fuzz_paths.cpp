// File names a map passes to natives, TOC files, mods' FDF paths, and
// names of files in a package.
#include "core/frames.h"
#include "core/path_guard.h"
#include "names.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string_view input(reinterpret_cast<const char*>(data), size);
    tome::path_guard::is_unsafe(input);
    if (tome::zip::safe_name(input)) check_safe_name(input);
    tome::path_guard::toc_is_unsafe(input);
    if (tome::ui::is_valid_mod_fdf_path(input)) {
        std::string archive = tome::ui::mod_fdf_archive_path("mod", input);
        tome::ui::toc_for(archive);
    }
    return 0;
}
