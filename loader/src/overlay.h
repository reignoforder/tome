#pragma once

// The archives Tome layers over the game's MPQs (docs/architecture.md, "Asset
// overlay"). Two of them, both in tome/cache and opened through Storm:
//
// - the generated overlay (priority 15): files Tome writes itself, i.e. the
//   mod menu's frame definitions and every FDF a mod loads with load_fdf.
//   Paths are Tome's own, so nothing in the game is overridden;
// - the mod assets archive (priority 12): the "assets" folders of the mods
//   whose assets are mounted, merged in load order. It sits above
//   War3Patch.mpq (8) and below campaign and map archives (13, 14, 16), so
//   mods override the game's files but a map's own files still win.
//
// Changing either one writes a new archive, opens it, then closes the old
// one. Storm keeps a closed archive alive until its last open file is
// closed, so files the game is still reading stay valid.

#include "core/mpq.h"
#include "paths.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tome::overlay {

// Deletes archives left in tome/cache by earlier sessions, except the last
// assets archive, which mount_assets may use again. Call once, before
// anything else here (DllMain).
void init(const Paths& paths);

// --- generated overlay ---------------------------------------------------------

// Adds or replaces files in the generated overlay and remounts it. False (and
// the previous overlay stays) if the archive couldn't be written or opened.
bool put_generated(const std::vector<mpq::File>& files);

// --- mod assets ------------------------------------------------------------------

struct AssetFile {
    std::string name;   // archive path (core/assets.h archive_path)
    std::wstring path;  // on disk
    uint32_t size = 0;
};

// Lists a mod's asset folder. `fingerprint` covers every name, size and write
// time, so it changes whenever the folder does (0 if there are no files).
// Unusable names and files over 2 GiB are skipped with a warning.
void scan_assets(const std::wstring& dir, std::vector<AssetFile>& files, uint64_t& fingerprint,
                 std::vector<std::string>& warnings);

struct ModSource {
    std::string id;
    uint64_t fingerprint = 0;  // scan_assets: the files this mod's part of the archive was built from
    // Copy this mod's files from the archive mounted now instead of from disk
    // (a mod whose newer files have to wait, see core/assets.h).
    bool keep_previous = false;
    std::vector<AssetFile> files;
};

// Rebuilds the assets archive from `mods`, lowest precedence first, and swaps
// it in; an empty list unmounts it. At the first call of a session, the
// archive the last session built is mounted again if it was built from the
// same mods and fingerprints (tome/cache/assets.json), instead of copying
// every file again. `summary` describes the result for the
// log, including files that couldn't be added. False (and the previous
// archive stays) on failure.
bool mount_assets(const std::vector<ModSource>& mods, std::string& summary);

// TOME_DEBUG_ASSET=<archive path> (developer builds, TOME_DEV): logs the size and FNV-1a hash of that file
// as the game sees it now (tests). Call after logging has started.
void log_probe();

} // namespace tome::overlay
