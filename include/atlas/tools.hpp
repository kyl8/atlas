#pragma once

#include "atlas/error.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <string>
#include <vector>

namespace atlas {

struct ToolLocationOptions {
    std::optional<std::filesystem::path> configured_path;
    std::optional<std::filesystem::path> tools_directory;
};

Result<std::filesystem::path> locate_tool(
    std::string_view name,
    const ToolLocationOptions& options = {});

// Same directory depth as locate_tool; discovery never executes tools.
Result<std::vector<std::filesystem::path>> discover_tools(const std::filesystem::path& directory);

} // namespace atlas
