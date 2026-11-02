// SPDX-License-Identifier: GPL-3.0-or-later
// What a WebAssembly module passes Tome's imports (docs/wasm-mods.md): any
// pointer and length over any memory. The memory is a heap block of exactly
// its size, so ASan catches a single byte read or written outside it. Then
// handle table operations with handles the module made up, and the input as
// a module's sections (has_start_section).
#include "core/wasm_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace tome::wasm;

namespace {

struct Input {
    const uint8_t* data;
    size_t size;
    bool take(uint32_t& v) {
        if (size < 4) return false;
        std::memcpy(&v, data, 4);
        data += 4;
        size -= 4;
        return true;
    }
};

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // The input as a module's bytes too (exactly its size, for ASan).
    {
        std::unique_ptr<uint8_t[]> module(new uint8_t[size ? size : 1]);
        if (size) std::memcpy(module.get(), data, size);
        has_start_section(module.get(), size);
    }
    if (size < 2) return 0;
    const size_t memory_size = (data[0] | (data[1] << 8)) % 4096;
    Input in{data + 2, size - 2};
    std::unique_ptr<uint8_t[]> block(new uint8_t[memory_size ? memory_size : 1]);
    const size_t fill = in.size < memory_size ? in.size : memory_size;
    std::memset(block.get(), 'x', memory_size);
    std::memcpy(block.get(), in.data, fill);
    Memory memory{memory_size ? block.get() : nullptr, memory_size};

    HandleTable table;
    int objects[8];
    std::vector<uint32_t> handles;
    uint32_t op, a, b;
    while (in.take(op) && in.take(a) && in.take(b)) {
        std::string text;
        std::vector<uint8_t> bytes;
        uint32_t value = 0;
        switch (op % 9) {
        case 0:
            if (read_string(memory, a, b, 1 << 16, text) == StringError::None && text.size() != b) __builtin_trap();
            break;
        case 1: read_bytes(memory, a, b, 1 << 16, bytes); break;
        case 2: write_out(memory, a, b, "some text to copy out"); break;
        case 3: store_u32(memory, a, b); break;
        case 4: load_u32(memory, a, value); break;
        case 5: {
            const uint32_t h = table.add(static_cast<HandleKind>(1 + b % 6), &objects[a % 8]);
            if (h) handles.push_back(h);
            break;
        }
        case 6: {
            // A made-up handle, or one handed out (maybe since removed).
            const uint32_t h = handles.empty() || (b & 1) ? a : handles[a % handles.size()];
            void* object = table.get(h, static_cast<HandleKind>(1 + b % 6));
            if (object && (object < static_cast<void*>(objects) || object >= static_cast<void*>(objects + 8)))
                __builtin_trap();
            break;
        }
        case 7: table.remove(handles.empty() || (b & 1) ? a : handles[a % handles.size()]); break;
        case 8: table.remove_all(static_cast<HandleKind>(1 + b % 6)); break;
        }
    }
    return 0;
}
