#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace tome::log {

enum class Level { Debug, Info, Warn, Error };

// Opens (truncates) the log file. Messages written before this are only sent
// to OutputDebugString, which winedbg/DebugView can still show.
bool open(const std::wstring& path);

// From now on every line is also kept in memory, for find_kept
// (TOME_DEBUG_CLICKS's wait steps), or no longer (and the kept lines
// dropped). Off unless asked for. The input driver's own lines (messages
// starting "Debug ", which repeat what the script sent) aren't kept, as
// the e2e test's checks skip them.
void keep_lines(bool keep);

// Whether a line kept since offset `from` contains `text`. If so, `from`
// moves past the end of that line.
bool find_kept(size_t& from, std::string_view text);

// The end of the kept lines: a find_kept from here sees only later lines.
size_t kept_size();

void write(Level level, const char* fmt, ...) __attribute__((format(gnu_printf, 2, 3)));

// Writes `message` as is, unless another thread is writing right now: for
// the crash handler, which mustn't wait for a lock the crashed thread may
// hold (it gives up after about 100 ms). False if it wasn't written to the file.
bool try_write(Level level, const char* message);

#define TOME_DEBUG(...) ::tome::log::write(::tome::log::Level::Debug, __VA_ARGS__)
#define TOME_INFO(...)  ::tome::log::write(::tome::log::Level::Info, __VA_ARGS__)
#define TOME_WARN(...)  ::tome::log::write(::tome::log::Level::Warn, __VA_ARGS__)
#define TOME_ERROR(...) ::tome::log::write(::tome::log::Level::Error, __VA_ARGS__)

} // namespace tome::log
