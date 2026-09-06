#pragma once

#include "atlas/error.hpp"

#include <filesystem>
#include <string_view>

namespace atlas::win {

Result<std::filesystem::path> resolve_path(std::string_view input);
Result<std::filesystem::path> user_atlas_directory();

// Writes through a flushed sibling file and replaces the destination while
// holding the per-file lock. `expected` protects concurrent edits.
Status write_atomic(const std::filesystem::path& path, std::string_view contents, std::string_view expected);

} // namespace atlas::win
