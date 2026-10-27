#pragma once

// The game's Storm.dll file API. Storm exports by ordinal only; these are the
// ordinals game.dll itself uses (checked against its call sites).

#include <string>

namespace tome::storm {

bool init();

// Reads a file through the game's open archives (highest priority first).
bool read_file(const char* name, std::string& out);

// Opens an MPQ for the game. Higher priority wins: the game uses 0-3 for its
// base archives, 8 for War3Patch.mpq and 13, 14 and 16 for campaigns and maps.
// Returns the archive handle, or null.
void* open_archive(const std::wstring& path, unsigned priority);

// Removes an archive from the search order. Storm counts the files still open
// in it and only releases it after the last one closes, so this is safe while
// the game is still reading (or streaming) one of its files.
void close_archive(void* archive);

} // namespace tome::storm
