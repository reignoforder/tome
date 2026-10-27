#pragma once

#include <string>

namespace tome::log {

enum class Level { Debug, Info, Warn, Error };

// Opens (truncates) the log file. Messages written before this are only sent
// to OutputDebugString, which winedbg/DebugView can still show.
bool open(const std::wstring& path);

void write(Level level, const char* fmt, ...) __attribute__((format(gnu_printf, 2, 3)));

#define TOME_DEBUG(...) ::tome::log::write(::tome::log::Level::Debug, __VA_ARGS__)
#define TOME_INFO(...)  ::tome::log::write(::tome::log::Level::Info, __VA_ARGS__)
#define TOME_WARN(...)  ::tome::log::write(::tome::log::Level::Warn, __VA_ARGS__)
#define TOME_ERROR(...) ::tome::log::write(::tome::log::Level::Error, __VA_ARGS__)

} // namespace tome::log
