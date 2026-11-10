// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Command card button positions (docs/known-bugs.md, "Command card button
 * positions"; core/command_card.h), 1.28's fix ported.
 *
 * A command button's position (CCommandButtonData.x and .y) comes from game
 * data a map can set (Buttonpos in CommandFunc.txt, its object data), and
 * CCommandCard::PlaceButton indexes the card's 4x3 grid with it unchecked.
 * 1.26a stores any value, so a bad position reads outside the grid and
 * crashes. 1.27a made the setters drop a coordinate outside the grid, which
 * also drops the two positions maps use to hide a button, (0, -11) and
 * (INT_MIN, INT_MIN): on 1.27b such a button shows up.
 *
 * The fix, in two parts:
 *   - PlaceButton (BEFORE, every build): a button whose position is outside
 *     the grid isn't placed, which is how the card leaves out a button it
 *     doesn't show (its caller skips the call for those). On 1.26a that is
 *     every bad position, hidden instead of read past the grid; on 1.27b only
 *     the two hiding positions get there.
 *   - 1.27b's setters keep the two hiding positions (core/command_card.h
 *     stored()): their store sequences are rewritten into a call to
 *     set_position, through a pointer (ff 15, which reaches tome.mix
 *     whatever its distance from game.dll), with NOPs after it. The three
 *     sites are the variant's: CCommandCard::AddButton.pos_check (abilities'
 *     buttons, CommandFunc.txt) and both paths of
 *     CGameUI::SetCommandButtonPos. 0x373A90 to 0x373AF0, single-coordinate
 *     setters with the same checks, have no callers and are left alone.
 *
 * Built in as a patch, not a mod, so it's always in force (patches.h). The
 * card only changes what this player sees; nothing goes over the network.
 */
#include "patches.h"

#include "core/command_card.h"
#include "core/log_guard.h"
#include "hooks.h"
#include "log.h"
#include "patch.h"
#include "tome/tome.h"

#include <cstdint>
#include <cstring>
#include <string>

#pragma GCC diagnostic ignored "-Wattributes"

namespace tome::patches {
namespace {

uint32_t g_x_offset = 0;
uint32_t g_y_offset = 0;
unsigned long g_left_out = 0;  // main thread (the game UI)

// Called from the rewritten setters: stores what 1.28 would.
void __cdecl set_position(uint8_t* data, uint32_t x, uint32_t y) {
    if (!data) return;
    const command_card::Stored s = command_card::stored(x, y);
    if (s.x) std::memcpy(data + g_x_offset, &x, sizeof x);
    if (s.y) std::memcpy(data + g_y_offset, &y, sizeof y);
}
void* g_set_position = reinterpret_cast<void*>(&set_position);

// BEFORE CCommandCard::PlaceButton(this, data): leaves out a button whose
// position isn't a cell.
void TOME_CALL on_place_button(tome_mod*, tome_hook_call* call, void*) {
    if (call->arg_count < 2 || !call->args[1]) return;
    const auto* data = reinterpret_cast<const uint8_t*>(call->args[1]);
    uint32_t x = 0, y = 0;
    std::memcpy(&x, data + g_x_offset, sizeof x);
    std::memcpy(&y, data + g_y_offset, sizeof y);
    if (command_card::in_grid(x, y)) return;
    call->skip_original = 1;
    call->result = 0;
    if (command_card::hides(x, y)) return;  // what the map asked for: nothing to say
    const unsigned long total = ++g_left_out;
    if (log::Throttle::logs(total)) {
        TOME_WARN("command-card-guard: left out a command button at (%d, %d), outside the card (total %lu)",
                  static_cast<int>(x), static_cast<int>(y), total);
    }
}

// One setter's store sequence and what replaces it. In `code`, kPtr marks
// the 4 bytes of the `call [ptr]` operand, filled in at install; the rest of
// the site is padded with NOPs.
constexpr int16_t kPtr = -1;
struct Site {
    const char* name;  // its mapping
    const uint8_t* expected;
    size_t size;
    const int16_t* code;
    size_t code_size;
};

// 1.27b, CCommandCard::AddButton.pos_check: data in ecx, x and y at
// [ebp+0x1C] and [ebp+0x20]; ecx is used after it, so it's kept.
const uint8_t kAddExpected[] = {
    0x8b, 0x45, 0x1c,                    // mov eax, [ebp+0x1C]
    0x83, 0xf8, 0x03,                    // cmp eax, 3
    0x77, 0x06,                          // ja +6
    0x89, 0x81, 0x9c, 0x05, 0x00, 0x00,  // mov [ecx+0x59C], eax
    0x8b, 0x45, 0x20,                    // mov eax, [ebp+0x20]
    0x83, 0xf8, 0x02,                    // cmp eax, 2
    0x77, 0x06,                          // ja +6
    0x89, 0x81, 0xa0, 0x05, 0x00, 0x00,  // mov [ecx+0x5A0], eax
};
const int16_t kAddCode[] = {
    0x51,                    // push ecx
    0xff, 0x75, 0x20,        // push [ebp+0x20]   y
    0xff, 0x75, 0x1c,        // push [ebp+0x1C]   x
    0x51,                    // push ecx          data
    0xff, 0x15, kPtr, kPtr, kPtr, kPtr,  // call [g_set_position]
    0x83, 0xc4, 0x0c,        // add esp, 12
    0x59,                    // pop ecx
};

// 1.27b, CGameUI::SetCommandButtonPos.pos_check_button: the button in eax,
// its data at +0x190, [ebp+0x10] points to {x, y}. Up to the final pop ebp,
// which stays; the pop esi before it is moved to the end. eax ends as the
// data, as the path that stores both coordinates leaves it.
const uint8_t kMoveButtonExpected[] = {
    0x8b, 0x55, 0x10,                    // mov edx, [ebp+0x10]
    0x8b, 0x32,                          // mov esi, [edx]
    0x83, 0xfe, 0x03,                    // cmp esi, 3
    0x77, 0x0c,                          // ja +12
    0x8b, 0x88, 0x90, 0x01, 0x00, 0x00,  // mov ecx, [eax+0x190]
    0x89, 0xb1, 0x9c, 0x05, 0x00, 0x00,  // mov [ecx+0x59C], esi
    0x8b, 0x4a, 0x04,                    // mov ecx, [edx+4]
    0x83, 0xf9, 0x02,                    // cmp ecx, 2
    0x77, 0x44,                          // ja (the shared pop esi / pop ebp / ret)
    0x8b, 0x80, 0x90, 0x01, 0x00, 0x00,  // mov eax, [eax+0x190]
    0x5e,                                // pop esi
    0x89, 0x88, 0xa0, 0x05, 0x00, 0x00,  // mov [eax+0x5A0], ecx
};
const int16_t kMoveButtonCode[] = {
    0x8b, 0x80, 0x90, 0x01, 0x00, 0x00,  // mov eax, [eax+0x190]   data
    0x50,                                // push eax
    0x8b, 0x55, 0x10,                    // mov edx, [ebp+0x10]
    0xff, 0x72, 0x04,                    // push [edx+4]   y
    0xff, 0x32,                          // push [edx]     x
    0x50,                                // push eax       data
    0xff, 0x15, kPtr, kPtr, kPtr, kPtr,  // call [g_set_position]
    0x83, 0xc4, 0x0c,                    // add esp, 12
    0x58,                                // pop eax
    0x5e,                                // pop esi
};

// 1.27b, CGameUI::SetCommandButtonPos.pos_check_data: the data in eax,
// [ebp+0x10] points to {x, y}; the function's pop esi / pop ebp / ret
// follow.
const uint8_t kMoveDataExpected[] = {
    0x8b, 0x55, 0x10,                    // mov edx, [ebp+0x10]
    0x8b, 0x0a,                          // mov ecx, [edx]
    0x83, 0xf9, 0x03,                    // cmp ecx, 3
    0x77, 0x06,                          // ja +6
    0x89, 0x88, 0x9c, 0x05, 0x00, 0x00,  // mov [eax+0x59C], ecx
    0x8b, 0x4a, 0x04,                    // mov ecx, [edx+4]
    0x83, 0xf9, 0x02,                    // cmp ecx, 2
    0x77, 0x06,                          // ja +6
    0x89, 0x88, 0xa0, 0x05, 0x00, 0x00,  // mov [eax+0x5A0], ecx
};
const int16_t kMoveDataCode[] = {
    0x50,                                // push eax
    0x8b, 0x55, 0x10,                    // mov edx, [ebp+0x10]
    0xff, 0x72, 0x04,                    // push [edx+4]   y
    0xff, 0x32,                          // push [edx]     x
    0x50,                                // push eax       data
    0xff, 0x15, kPtr, kPtr, kPtr, kPtr,  // call [g_set_position]
    0x83, 0xc4, 0x0c,                    // add esp, 12
    0x58,                                // pop eax
};

#define TOME_SITE(name, expected, code) {name, expected, sizeof expected, code, sizeof code / sizeof code[0]}
// In the order of BuildMappings::command_card's add_check, move_check_button
// and move_check_data.
const Site k127bSites[] = {
    TOME_SITE("CCommandCard::AddButton.pos_check", kAddExpected, kAddCode),
    TOME_SITE("CGameUI::SetCommandButtonPos.pos_check_button", kMoveButtonExpected, kMoveButtonCode),
    TOME_SITE("CGameUI::SetCommandButtonPos.pos_check_data", kMoveDataExpected, kMoveDataCode),
};
#undef TOME_SITE
constexpr size_t kSites = sizeof k127bSites / sizeof k127bSites[0];

// What a build's setters need: the three sites, or none (1.26a's setters
// keep any value; PlaceButton's check is enough there).
struct Setters {
    bool rewrite;
};
const Setters kRewrite{true};
const Setters kKeep{false};

// The bytes that replace one site, in `code`; false if they don't fit.
constexpr size_t kMaxSite = 64;
bool replacement_for(const Site& site, uint8_t (&code)[kMaxSite]) {
    if (site.code_size > site.size || site.size > kMaxSite) return false;
    std::memset(code, 0x90, site.size);
    void* ptr = &g_set_position;
    uint32_t ptr_value = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr));
    for (size_t i = 0, p = 0; i < site.code_size; ++i) {
        if (site.code[i] == kPtr) {
            code[i] = static_cast<uint8_t>(ptr_value >> (8 * p++));
        } else {
            code[i] = static_cast<uint8_t>(site.code[i]);
        }
    }
    return true;
}

bool install_command_card_guard(uintptr_t base, const BuildMappings& build, const Variant& variant,
                                std::string& report) {
    g_x_offset = build.command_card.x_offset;
    g_y_offset = build.command_card.y_offset;
    const Setters& setters = *static_cast<const Setters*>(variant.params);
    std::string error;
    hooks::Target* place = hooks::find_or_install("CCommandCard::PlaceButton", error);
    if (!place) {
        report = "CCommandCard::PlaceButton: " + error;
        return false;
    }
    hooks::Handler before;
    before.phase = TOME_HOOK_BEFORE;
    before.priority = INT32_MAX;  // before any mod's handler: nothing may index the grid with a bad position
    before.fn = on_place_button;
    hooks::add(place, before);
    report = "leaves out command buttons placed outside the card";
    if (!setters.rewrite) return true;

    // All three or none: a setter left as it was would drop a hiding
    // position the others keep. Checked first, so a mismatch changes nothing.
    const uint32_t rvas[kSites] = {build.command_card.add_check, build.command_card.move_check_button,
                                   build.command_card.move_check_data};
    uint8_t codes[kSites][kMaxSite];
    for (size_t i = 0; i < kSites; ++i) {
        const Site& site = k127bSites[i];
        const uintptr_t at = base + rvas[i];
        if (!readable(at, site.size) || std::memcmp(reinterpret_cast<const void*>(at), site.expected, site.size) != 0) {
            report += "; hiding positions not kept (" + std::string(site.name) + " isn't as expected)";
            return true;
        }
        if (!replacement_for(site, codes[i])) {
            report += "; hiding positions not kept (" + std::string(site.name) + ": too short)";
            return true;
        }
    }
    for (size_t i = 0; i < kSites; ++i) {
        const Site& site = k127bSites[i];
        std::string error;
        if (!patch_bytes(base + rvas[i], site.expected, codes[i], site.size, error)) {
            // Only if the write itself fails (under Shade, patch_bytes always
            // does, at the first site): the bytes were checked above. The
            // sites already rewritten are put back, so all three stay as the
            // game had them.
            for (size_t j = i; j-- > 0;) {
                std::string ignored;
                patch_bytes(base + rvas[j], codes[j], k127bSites[j].expected, k127bSites[j].size, ignored);
            }
            report += "; hiding positions not kept (" + std::string(site.name) + ": " + error + ")";
            return true;
        }
    }
    report += "; keeps the hiding positions (0, -11) and (INT_MIN, INT_MIN)";
    return true;
}

const char* const kSymbols[] = {"CCommandCard::PlaceButton", "CCommandButtonData.x", "CCommandButtonData.y", nullptr};
const char* const kSetterSymbols[] = {"CCommandCard::AddButton.pos_check",
                                      "CGameUI::SetCommandButtonPos.pos_check_button",
                                      "CGameUI::SetCommandButtonPos.pos_check_data", nullptr};
const char* const kReserve[] = {"CCommandCard::PlaceButton", nullptr};
const char* const k126a[] = {"1.26a", nullptr};
const Variant kVariants[] = {
    {k126a, nullptr, nullptr, &kKeep},  // its setters keep any value
    {nullptr, kSetterSymbols, nullptr, &kRewrite},
};

} // namespace

extern const Patch kCommandCardGuard = {
    "command-card-guard",
    "keeps command buttons placed outside the card off it, and lets maps hide buttons as on 1.28",
    kVariants,  // whether the setters are rewritten
    kSymbols,
    kReserve,   // a mod mustn't place a button the guard left out
    nullptr,    // needed everywhere
    install_command_card_guard,
};

} // namespace tome::patches
