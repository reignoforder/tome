#pragma once

// The checks between a WebAssembly mod and Tome (docs/wasm-mods.md): what an
// import may read from or write to the module's linear memory, and the
// handles that stand in for game pointers. Interpreter-independent: an
// embedder only reports where the memory is and how large it is.
//
// Everything a module passes is untrusted: a pointer and a length are two
// u32 the module chose. Nothing here touches memory outside [base, base +
// size), whatever they are.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace tome::wasm {

// A module's linear memory as the interpreter reports it. Valid until the
// module runs again (memory.grow may move it): take it again after every
// call into the module.
struct Memory {
    uint8_t* base = nullptr;
    uint64_t size = 0;
};

// Whether [ptr, ptr + len) lies inside memory. A zero length is in bounds
// anywhere up to the end, including at `size`.
bool in_bounds(const Memory& memory, uint32_t ptr, uint32_t len);

enum class StringError {
    None,
    OutOfBounds,
    TooLong,      // longer than the import allows
    InvalidUtf8,
    Nul,          // a NUL byte: Tome's strings end there, so it would cut them short
};

const char* describe(StringError error);

// A string argument (ptr, len): in bounds, at most `max` bytes, valid UTF-8,
// no NUL. `out` gets a copy (so a later memory.grow can't change it).
StringError read_string(const Memory& memory, uint32_t ptr, uint32_t len, size_t max, std::string& out);

// A byte argument (ptr, len): in bounds and at most `max` bytes, copied.
bool read_bytes(const Memory& memory, uint32_t ptr, uint32_t len, size_t max, std::vector<uint8_t>& out);

// An out-parameter (ptr, capacity) the way the C API's are: copies as much
// of `data` as fits and returns data's full size, so the module can retry
// with a larger buffer; capacity 0 only asks for the size. Strings get no
// terminator: the module has the length. Returns UINT32_MAX (and writes
// nothing) if the buffer is out of bounds, or data is larger than a u32.
uint32_t write_out(const Memory& memory, uint32_t ptr, uint32_t capacity, std::string_view data);

// A small value (an i32, a struct Tome lays out) at `ptr`: false if it
// doesn't fit. Little-endian, unaligned, as Wasm memory is.
bool store_u32(const Memory& memory, uint32_t ptr, uint32_t value);
bool load_u32(const Memory& memory, uint32_t ptr, uint32_t& value);

// Whether a module's bytes have a start section (id 8): a function the
// interpreter runs while it instantiates the module, before Tome has the
// instance (so before an import could find its mod) and without the
// instruction budget. False if there's none, or if the bytes aren't a
// module header and a well-formed sequence of sections (the loader refuses
// those anyway).
bool has_start_section(const uint8_t* bytes, size_t size);

// A file name a module may use in its config folder (config_read,
// config_write): 1-64 characters of A-Z a-z 0-9 . _ -, not starting with a
// dot, so no folders, no "..", no device names' tricks (and Windows' reserved
// names like "con" or "nul" with any extension are refused too).
bool valid_file_name(std::string_view name);
// The most a module may keep in one such file.
constexpr size_t kMaxFileSize = 1u << 20;

// Handles: what a module gets instead of a game pointer (a frame, a unit, a
// JASS value). A u32: the slot's index in the low 20 bits, the slot's
// generation in the high 12, so a handle a module keeps after its object is
// gone (and its slot reused) is refused instead of naming something else. 0
// is never a handle. One table per module, so modules can't use each
// other's handles.
enum class HandleKind : uint8_t {
    Frame = 1,
    Unit,
    JassString,
    JassCode,
    Item,
    Destructable,
};

class HandleTable {
public:
    static constexpr uint32_t kIndexBits = 20;
    static constexpr uint32_t kMaxHandles = (1u << kIndexBits) - 1;  // slot 0 is never used

    // The handle for `object` (one handle per object and kind: the same one
    // if it already has one). 0 if the table is full or object is null.
    uint32_t add(HandleKind kind, void* object);
    // The object, if `handle` is live and of `kind`; nullptr otherwise.
    void* get(uint32_t handle, HandleKind kind) const;
    // Frees the handle (the object is gone); later uses of it get nullptr.
    void remove(uint32_t handle);
    // Frees every handle of `kind` (e.g. JASS values when the map ends).
    void remove_all(HandleKind kind);
    size_t live() const { return live_; }

private:
    struct Slot {
        void* object = nullptr;
        HandleKind kind{};
        uint16_t generation = 1;  // 12 bits used
        bool used = false;
    };
    static uint32_t make(uint32_t index, uint16_t generation) {
        return (uint32_t(generation) << kIndexBits) | index;
    }
    std::vector<Slot> slots_{Slot{}};  // slot 0: never handed out
    std::vector<uint32_t> free_;
    std::map<std::pair<void*, HandleKind>, uint32_t> index_of_;  // live objects' slots
    size_t live_ = 0;
};

} // namespace tome::wasm
