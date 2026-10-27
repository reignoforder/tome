#pragma once

#include <string>

namespace tome {

// SHA-256 of a file as lowercase hex, using the OS's BCrypt provider.
// Returns an empty string on failure.
std::string sha256_file(const std::wstring& path);

} // namespace tome
