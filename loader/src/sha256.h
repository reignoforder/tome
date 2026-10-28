#pragma once

#include <string>
#include <string_view>

namespace tome {

// SHA-256 of a file as lowercase hex, using the OS's BCrypt provider.
// Returns an empty string on failure.
std::string sha256_file(const std::wstring& path);

// The same for bytes in memory.
std::string sha256_bytes(std::string_view data);

} // namespace tome
