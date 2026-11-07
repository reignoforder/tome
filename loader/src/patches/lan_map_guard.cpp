// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * LAN map guard (docs/known-bugs.md, "Map downloads from a host").
 *
 * When a player joins a game whose map they may not have, the host sends the
 * map's path as it has it (e.g. "Maps\\FrozenThrone\\(2)EchoIsles.w3x"), its
 * size and its checksums. The client passes the path to the map transfer's
 * own name check (MapTransfer::IsValidName) and, if that accepts it, first
 * looks the map up at that path under the game folder, then by its base
 * name anywhere under Maps\, and downloads it into Maps\Download\<base name>
 * only if neither matches.
 *
 * The game's check refuses an absolute path, the characters * / : > < | &
 * + ^ ? and " (so no drive, no alternate data stream), an extension other than .w3m and
 * .w3x, and a base name that is a DOS device, compared up to its last dot.
 * It lets '..' components through, though, so a host can point the client's
 * lookup anywhere relative to the game folder (and with enough of them, at
 * any folder on the drive): the client opens and checksums a .w3m/.w3x file
 * there, tells the host whether it has it, and plays it if it matches. And a
 * device name with a second dot ("com1.x.w3x") isn't recognised as one,
 * while Windows before 11 still opens the device for it: a port, the console
 * or a printer, both for the lookup (a read that may never return) and for
 * the download's write.
 *
 * Fix: an AFTER handler on IsValidName refuses, on top of what the game
 * refuses, any name that path_guard::is_unsafe refuses (its ".." component,
 * device-name and length rules: the same rules as the path sanitizer's). The
 * transfer is then never made, as for a name the game refuses itself: the
 * client can't get the map and leaves the lobby. A host's own maps have
 * plain relative paths, so a real game is never refused. The download's
 * file name was already safe without this (the base name only, through the
 * same check), so the guard closes the lookup and the device-name gap.
 *
 * Reserved: a mod mustn't be able to drop the check (the hook would re-open
 * the lookup for every host the player joins).
 */
#include "patches.h"

#include "core/log_guard.h"
#include "core/path_guard.h"
#include "hooks.h"
#include "log.h"
#include "tome/tome.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

namespace tome::patches {
namespace {

std::atomic<unsigned long> g_refused{0};  // the transfer runs on the network thread

// A host could send many: logged like the path sanitizer's (log::Throttle).
constexpr size_t kLogChars = 160;

// AFTER MapTransfer::IsValidName(this, name) -> int (1 if the name is fine).
void TOME_CALL on_is_valid_name(tome_mod*, tome_hook_call* call, void*) {
    if (call->result == 0 || call->arg_count < 2) return;  // the game refused it already
    const char* name = reinterpret_cast<const char*>(call->args[1]);
    if (!name) return;
    // Bounded: the game copies the name into MAX_PATH buffers.
    const size_t len = strnlen(name, path_guard::kMaxPathLen + 1);
    const std::string_view view(name, len);
    if (!path_guard::is_unsafe(view)) return;
    call->result = 0;
    const unsigned long total = ++g_refused;
    if (log::Throttle::logs(total)) {
        TOME_WARN("lan-map-guard: refused the map path \"%s\" from a game's host -- %s; the map isn't looked up "
                  "or downloaded (total %lu)",
                  log::printable(view, kLogChars).c_str(),
                  len > path_guard::kMaxPathLen ? "too long" : "escapes the Maps folder or names a device", total);
    }
}

bool install(uintptr_t, const BuildMappings&, const Variant&, std::string& report) {
    std::string error;
    hooks::Target* target = hooks::find_or_install("MapTransfer::IsValidName", error);
    if (!target) {
        report = "MapTransfer::IsValidName: " + error;
        return false;
    }
    hooks::Handler after;
    after.phase = TOME_HOOK_AFTER;
    after.priority = INT32_MIN;  // after any mod's handler: the guard has the last word
    after.fn = on_is_valid_name;
    hooks::add(target, after);
    report = "refuses map paths from a host that leave the Maps folder or name a device";
    return true;
}

const char* const kSymbols[] = {"MapTransfer::IsValidName", nullptr};
const char* const kReserve[] = {"MapTransfer::IsValidName", nullptr};

} // namespace

extern const Patch kLanMapGuard = {
    "lan-map-guard",
    "refuses a host's map path that would make the client open a file outside Maps or a device",
    {},
    kSymbols,
    kReserve,
    nullptr,  // needed everywhere
    install,
};

} // namespace tome::patches
