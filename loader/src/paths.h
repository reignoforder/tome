#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace tome {

struct Paths {
    std::wstring game_dir;  // folder containing tome.mix (the Warcraft III install)
    // <home> is the game directory unless the TOME_HOME environment variable is set.
    std::wstring tome_dir;  // <home>/tome: log, state, cache
    std::wstring mods_dir;  // <home>/mods
    std::wstring host_exe;  // file name of the process Tome was loaded into, e.g. war3.exe
};

// Resolves all paths from tome.mix's own location and creates tome_dir.
bool init_paths(HMODULE self, Paths& out);

std::string narrow(const std::wstring& wide);
std::wstring widen(const std::string& utf8);

// Reads a whole file (up to 16 MB). Other processes may keep it open.
bool read_file(const std::wstring& path, std::string& out);

struct FileStamp {
    uint64_t size = 0;
    uint64_t write_time = 0;

    bool operator==(const FileStamp&) const = default;
};

// Size and last write time, without opening the file; false if it's missing.
bool file_stamp(const std::wstring& path, FileStamp& out);

// Writes to a temp file, then renames it over `path`, so a crash mid-write
// never leaves a truncated file.
bool write_file_atomic(const std::wstring& path, const std::string& data);

} // namespace tome
