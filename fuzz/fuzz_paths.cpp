// SPDX-License-Identifier: GPL-3.0-or-later
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
    const bool unsafe = tome::path_guard::is_unsafe(input);
    // Preload's argument: the same rules, only no length limit.
    const bool unsafe_any_length = tome::path_guard::is_unsafe_any_length(input);
    if (size <= tome::path_guard::kMaxPathLen && unsafe != unsafe_any_length) __builtin_trap();
    if (unsafe_any_length && !unsafe) __builtin_trap();
    // A name that fits after a prefix this long (plus ".pld") stays within kMaxPathLen.
    if (const size_t n = tome::path_guard::max_name_after(size); n && size + n + 4 > tome::path_guard::kMaxPathLen) {
        __builtin_trap();
    }
    if (tome::zip::safe_name(input)) check_safe_name(input);
    tome::path_guard::toc_is_unsafe(input);
    if (tome::ui::is_valid_mod_fdf_path(input)) {
        std::string archive = tome::ui::mod_fdf_archive_path("mod", input);
        tome::ui::toc_for(archive);
    }
    return 0;
}
