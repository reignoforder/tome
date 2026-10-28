/*
 * BLP texture guard (docs/known-bugs.md, "BLP texture loader";
 * docs/reverse-engineering.md).
 *
 * Warcraft III's BLP1 loader reads each mipmap's span, and a JPEG texture's
 * shared JPEG header, straight out of the loaded file using offsets and sizes
 * taken from the file's own header, without checking them against the file's
 * length. A crafted .blp -- in a downloaded map, or a mod's assets -- makes it
 * read past the end of the buffer: a crash, and the classic out-of-bounds read
 * primitive. Tome can't decode BLPs itself, and the bug is in the game's
 * decoder, so the guard validates the bytes the game loaded and, when they're
 * unsafe, makes the load fail (exactly as the game already does for a file
 * whose magic isn't "BLP1"): the texture shows as missing and the game carries
 * on, instead of decoding a malformed file.
 *
 * Mechanism: hook CBLPFile::Open (the one BLP1 header parser; every texture
 * load reaches it). Open loads the whole file, stores the buffer pointer and
 * its length in the object, copies the header and checks the magic. An AFTER
 * handler runs once it returns: if it reported success, read the buffer and
 * size out of the object (CBLPFile.buffer / .size) and run the portable
 * validator (core/blp.*) over them; if the file is unsafe to decode, overwrite
 * the result with 0 (failure), so the caller never runs the mip/JPEG decode.
 *
 * Built in as a patch, not a mod, so it's always in force (patches.h). The
 * hook is reserved: unlike the path sanitizer (whose threat is maps, which a
 * mod can't help), a mod that removed or shadowed this hook would reopen the
 * hole for every texture, so the mod-facing API refuses CBLPFile::Open.
 */
#include "patches.h"

#include "core/blp.h"
#include "hooks.h"
#include "log.h"
#include "patch.h"
#include "tome/tome.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

#pragma GCC diagnostic ignored "-Wattributes"

// The header-copy clamp (below). CBLPFile::Open copies a fixed 0x127 dwords
// (1180 bytes) of header out of the loaded file before it checks the magic,
// so a file shorter than that is over-read. An AFTER handler can't prevent
// it (the copy already ran), so the patch rewrites the copy site to call
// this detour, which copies only min(1180, size) bytes and zero-fills the
// rest. It runs mid-function with ebx = the CBLPFile (this), esi = the file
// buffer, edi = this+8 (the header destination); the size the loader stored
// is at this+size_offset, passed in through a variable so no offset is baked
// into the assembly. It clobbers only eax/ecx/edx/esi/edi (all dead after the
// copy) and preserves ebx; the game's `cmp [ebx+8], 'BLP1'` runs next.
// The CBLPFile.size offset the detour reads, and the detour itself. Both have
// C linkage so the assembly can name them without C++ mangling. Set at install.
extern "C" {
uint32_t tome_blp_size_offset = 0;
void tome_blp_header_copy();
}

asm(R"(
    .intel_syntax noprefix
    .text
    .globl _tome_blp_header_copy
_tome_blp_header_copy:
    cld
    mov ecx, [_tome_blp_size_offset]
    mov ecx, [ebx + ecx]        # ecx = file size in bytes
    cmp ecx, 1180
    jbe 1f
    mov ecx, 1180
1:
    mov edx, 1180
    sub edx, ecx                # edx = bytes to zero after the copy
    rep movsb                   # copy ecx bytes esi -> edi (this+8)
    mov ecx, edx
    xor eax, eax
    rep stosb                   # zero the remaining header bytes
    ret
    .att_syntax prefix
)");

namespace tome::patches {
namespace {

uint32_t g_buffer_offset = 0;
uint32_t g_size_offset = 0;
std::atomic<unsigned long> g_refused_total{0};  // textures may load off the main thread

// A map or mod could hand the game many bad textures; log the first few in
// full, then rarely, so tome.log can't be flooded.
constexpr unsigned long kLogFirst = 20;
constexpr unsigned long kLogEvery = 1000;

// AFTER CBLPFile::Open(this, name, context) -> int (1 on success). `this` is
// args[0] (thiscall). On success the object holds the loaded file at
// +buffer_offset and its length at +size_offset.
void TOME_CALL on_blp_open(tome_mod*, tome_hook_call* call, void*) {
    if (call->result == 0 || call->arg_count < 1) return;  // the load already failed
    const char* self = reinterpret_cast<const char*>(call->args[0]);
    if (!self) return;
    const char* buffer = *reinterpret_cast<const char* const*>(self + g_buffer_offset);
    uint32_t size = *reinterpret_cast<const uint32_t*>(self + g_size_offset);
    if (!buffer || size == 0) return;

    const char* reason = blp::reject_reason(std::string_view(buffer, size));
    if (!reason) return;

    call->result = 0;  // the game treats this exactly like a wrong-magic file
    const unsigned long total = ++g_refused_total;
    if (total <= kLogFirst || total % kLogEvery == 0) {
        TOME_WARN("blp-guard: refused a malformed texture (%s, %u bytes); it won't be decoded (total %lu)", reason,
                  size, total);
    }
}

// The header-copy site is `mov ecx,0x127; rep movsd` (7 bytes). Replace it
// with an absolute indirect call through g_detour_ptr (FF 15 <&ptr>, 6 bytes)
// plus a NOP, so the call reaches the detour whatever the distance between
// game.dll and tome.mix (a rel32 call might not). The detour returns to the
// NOP, then the game's magic check runs.
const uint8_t kCopyExpected[7] = {0xb9, 0x27, 0x01, 0x00, 0x00, 0xf3, 0xa5};
void* g_detour_ptr = reinterpret_cast<void*>(&tome_blp_header_copy);

// Clamps the fixed header copy to the file's length. Best-effort: if the
// mapping doesn't locate the site, or its bytes aren't what's expected, the
// AFTER guard still refuses malformed files (only the pre-magic over-read of
// a short file is left), so this never fails the patch.
std::string install_copy_clamp(uintptr_t base, const BuildMappings& build) {
    if (!build.blp.header_copy) return "header copy unclamped (site not mapped)";
    tome_blp_size_offset = g_size_offset;
    uint8_t replacement[7] = {0xff, 0x15};
    void* ptr = &g_detour_ptr;
    std::memcpy(replacement + 2, &ptr, 4);
    replacement[6] = 0x90;  // NOP: the call is 6 bytes, the site is 7
    std::string error;
    if (!patch_bytes(base + build.blp.header_copy, kCopyExpected, replacement, sizeof(replacement), error)) {
        return "header copy unclamped (" + error + ")";
    }
    return "";
}

bool install_blp_guard(uintptr_t base, const BuildMappings& build, std::string& report) {
    g_buffer_offset = build.blp.buffer_offset;
    g_size_offset = build.blp.size_offset;
    std::string error;
    hooks::Target* open = hooks::find_or_install("CBLPFile::Open", error);
    if (!open) {
        report = "CBLPFile::Open: " + error;
        return false;
    }
    hooks::Handler after;
    after.phase = TOME_HOOK_AFTER;
    after.priority = INT32_MIN;  // after any mod's handler; the guard has the last word
    after.fn = on_blp_open;
    hooks::add(open, after);
    std::string clamp = install_copy_clamp(base, build);
    report = "validates BLP textures before the game decodes them";
    if (!clamp.empty()) report += "; " + clamp;
    return true;
}

const char* const kSymbols[] = {"CBLPFile::Open", "CBLPFile.buffer", "CBLPFile.size", nullptr};
const char* const kReserve[] = {"CBLPFile::Open", nullptr};

} // namespace

extern const Patch kBlpGuard = {
    "blp-guard",
    "refuses malformed BLP textures so the game's loader can't read past them",
    nullptr,    // any build that maps the symbols below
    kSymbols,
    kReserve,   // a mod mustn't be able to drop the guard (file header)
    nullptr,    // needed everywhere
    install_blp_guard,
};

} // namespace tome::patches
