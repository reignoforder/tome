// BLP1 textures (downloaded maps, mods' assets): the guard that keeps the
// game's loader from reading past a crafted file (core/blp.*).
#include "core/blp.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    tome::blp::reject_reason(std::string_view(reinterpret_cast<const char*>(data), size));
    return 0;
}
