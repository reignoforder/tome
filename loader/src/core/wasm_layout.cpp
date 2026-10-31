#include "wasm_layout.h"

#include <cstddef>
#include <cstring>

namespace tome::wasm {
namespace {

// A block in wasm32 layout: a fixed part (the structs), then a tail (what
// their pointers point to). Pointers are base + offset.
class Block {
public:
    Block(uint32_t base, uint32_t fixed) : base_(base), bytes_(fixed, '\0') {}

    void u32(uint32_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes_[at + i] = static_cast<char>(v >> (8 * i));
    }
    void i32(uint32_t at, int32_t v) { u32(at, static_cast<uint32_t>(v)); }
    void f64(uint32_t at, double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        u32(at, static_cast<uint32_t>(bits));
        u32(at + 4, static_cast<uint32_t>(bits >> 32));
    }
    // A NUL-terminated copy in the tail; its address at `at` (0 for null).
    void str(uint32_t at, const char* text) {
        if (!text) return u32(at, 0);
        u32(at, append(text, std::strlen(text) + 1, 1));
    }
    // Room for `size` bytes in the tail, aligned; its address.
    uint32_t reserve(uint32_t size, uint32_t align) {
        while (bytes_.size() % align) bytes_.push_back('\0');
        const uint32_t at = static_cast<uint32_t>(bytes_.size());
        bytes_.append(size, '\0');
        return at;
    }
    uint32_t address(uint32_t offset) const { return base_ + offset; }
    std::string take() { return std::move(bytes_); }

private:
    uint32_t append(const char* data, size_t size, uint32_t align) {
        const uint32_t at = reserve(static_cast<uint32_t>(size), align);
        std::memcpy(&bytes_[at], data, size);
        return address(at);
    }

    uint32_t base_;
    std::string bytes_;
};

// tome_wasm_unit at `at` (an offset in the block).
void put_unit(Block& b, uint32_t at, const tome_unit& u, const Handles& handles) {
    b.u32(at + 0, kUnitSize);
    b.u32(at + 4, handles(HandleKind::Unit, u.object));
    b.u32(at + 8, u.handle);
    b.u32(at + 12, u.type_id);
    b.str(at + 16, u.name);
    b.i32(at + 20, u.owner);
    b.u32(at + 24, u.flags);
}

// A unit pointer field: the unit after the fixed part, or 0.
void unit_field(Block& b, uint32_t at, const tome_unit* u, const Handles& handles) {
    if (!u) return b.u32(at, 0);
    const uint32_t offset = b.reserve(kUnitSize, 4);
    put_unit(b, offset, *u, handles);
    b.u32(at, b.address(offset));
}

// Whether `info` has `field`: its size says which API version filled it.
#define HAS(field) (info.size >= offsetof(tome_event_info, field) + sizeof(info.field))

} // namespace

std::string encode_event(const tome_event_info& info, const Handles& handles, uint32_t base) {
    Block b(base, kEventSize);
    b.u32(0, kEventSize);
    b.i32(4, info.event);
    b.str(8, info.map_name);
    b.str(12, info.map_path);
    b.f64(16, info.delta);
    b.u32(24, info.frame);
    b.f64(32, info.game_time);
    if (HAS(map_description)) b.str(40, info.map_description);
    if (HAS(leader)) {
        const uint32_t count = info.units ? info.unit_count : 0;
        if (count) {
            const uint32_t offset = b.reserve(kUnitSize * count, 4);
            for (uint32_t i = 0; i < count; ++i) put_unit(b, offset + i * kUnitSize, info.units[i], handles);
            b.u32(44, b.address(offset));
        }
        b.u32(48, count);
        b.i32(52, info.leader);
    }
    if (HAS(y)) {  // API version 8
        b.i32(56, info.player);
        b.str(60, info.player_name);
        b.str(64, info.text);
        b.i32(68, info.recipient);
        unit_field(b, 72, info.unit, handles);
        unit_field(b, 76, info.other, handles);
        b.u32(80, handles(HandleKind::Item, info.item));
        b.u32(84, info.object_id);
        b.str(88, info.object_name);
        b.i32(92, info.level);
        b.str(96, info.save_name);
        unit_field(b, 100, info.third, handles);
        b.u32(104, info.order_id);
        b.i32(108, info.order_type);
        b.f64(112, info.x);
        b.f64(120, info.y);
    }
    if (HAS(previous)) {  // API version 12
        b.f64(128, info.amount);
        b.u32(136, handles(HandleKind::Destructable, info.destructable));
        b.u32(140, info.item_handle);
        b.u32(144, info.destructable_handle);
        b.i32(148, info.key);
        b.i32(152, info.pressed);
        b.u32(156, info.dialog);
        b.u32(160, info.button);
        b.i32(164, info.resource);
        b.f64(168, info.previous);
    }
    if (HAS(script_shadows)) b.u32(176, info.script_shadows);  // API version 13
    return b.take();
}

#undef HAS

std::string encode_units(const tome_unit* units, uint32_t count, const Handles& handles, uint32_t base) {
    Block b(base, kUnitSize * count);
    for (uint32_t i = 0; i < count; ++i) put_unit(b, i * kUnitSize, units[i], handles);
    return b.take();
}

std::string encode_game_info(const char* build_name, const char* file_version, uint32_t base) {
    Block b(base, kGameInfoSize);
    b.u32(0, kGameInfoSize);
    b.str(4, build_name);
    b.str(8, file_version);
    return b.take();
}

std::string encode_shadows(const tome_script_shadow* shadows, uint32_t count, uint32_t base) {
    Block b(base, kShadowSize * count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t at = i * kShadowSize;
        b.u32(at, kShadowSize);
        b.str(at + 4, shadows[i].name);
        b.str(at + 8, shadows[i].local_type);
        b.str(at + 12, shadows[i].global_type);
        b.i32(at + 16, shadows[i].line);
    }
    return b.take();
}

bool parse_signature(std::string_view text, JassSignature& out) {
    out = {};
    if (text.empty() || text[0] != '(') return false;
    size_t i = 1;
    auto type = [&](JassType& t, bool result) -> bool {
        if (i >= text.size()) return false;
        t.kind = text[i++];
        switch (t.kind) {
        case 'I': case 'R': case 'B': case 'S': case 'C': return true;
        case 'V': return result;
        case 'H': {
            const size_t end = text.find(';', i);
            if (end == std::string_view::npos || end == i) return false;
            t.handle = std::string(text.substr(i, end - i));
            for (char c : t.handle) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
                    return false;
            }
            i = end + 1;
            return true;
        }
        default: return false;
        }
    };
    while (i < text.size() && text[i] != ')') {
        if (out.params.size() >= 32) return false;  // the engine's limit is lower still
        JassType t;
        if (!type(t, false)) return false;
        out.params.push_back(std::move(t));
    }
    if (i >= text.size()) return false;
    ++i;  // ')'
    if (!type(out.result, true)) return false;
    return i == text.size();
}

std::string encode_native_args(const JassSignature& sig, const std::vector<NativeArg>& args, uint32_t base) {
    const uint32_t count = static_cast<uint32_t>(sig.params.size());
    Block b(base, 4 * count);
    for (uint32_t i = 0; i < count && i < args.size(); ++i) {
        if (sig.params[i].kind == 'S') {
            b.str(4 * i, args[i].null ? nullptr : args[i].text.c_str());
        } else {
            b.u32(4 * i, args[i].word);
        }
    }
    return b.take();
}

bool valid_native_result(const JassSignature& sig, const std::vector<NativeArg>& args, uint32_t value) {
    switch (sig.result.kind) {
    case 'C': return false;
    case 'H':
        if (value == 0) return true;
        for (size_t i = 0; i < sig.params.size() && i < args.size(); ++i) {
            if (sig.params[i] == sig.result && args[i].word == value) return true;
        }
        return false;
    default: return true;  // integers, reals, booleans; strings are read by Tome
    }
}

} // namespace tome::wasm
