#include "atlas/tools.hpp"

#include "atlas/platform/process.hpp"
#include "atlas/platform/utf.hpp"

#include <algorithm>
#include <vector>

namespace atlas {
namespace {

Result<std::filesystem::path> validate_configured_path(const std::filesystem::path& path) {
    auto resolved = win::find_executable(path.native());
    if (!resolved) {
        auto display = win::narrow(path.native()).value_or("<invalid path>");
        return std::unexpected(Error{ErrorKind::not_found,
            "Configured tool was not found: " + display,
            resolved.error().detail,
            resolved.error().native_code});
    }
    return *resolved;
}

bool regular_file(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

} // namespace

Result<std::vector<std::filesystem::path>> discover_tools(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> paths;
    std::vector<std::filesystem::path> directories{directory};
    std::error_code error;
    for (std::size_t index = 0; index < directories.size(); ++index) {
        const auto current = directories[index];
        for (std::filesystem::directory_iterator iterator(current, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (iterator->is_directory(error)) {
                if (index == 0) directories.push_back(iterator->path());
            } else if (!error && iterator->is_regular_file(error)) {
                const auto extension = iterator->path().extension().native();
                if (_wcsicmp(extension.c_str(), L".exe") != 0) continue;
                paths.push_back(iterator->path());
            }
        }
        if (error) return std::unexpected(Error{ErrorKind::io,
            "Could not list tools in " + win::narrow(current.native()).value_or("<invalid path>"), error.message()});
    }
    std::ranges::sort(paths);
    paths.erase(std::ranges::unique(paths).begin(), paths.end());
    return paths;
}

Result<std::filesystem::path> locate_tool(const std::string_view name, const ToolLocationOptions& options) {
    if (name.empty() || name.find_first_of("/\\") != std::string_view::npos) {
        return std::unexpected(Error{ErrorKind::usage, "Tool name must be a filename, not a path"});
    }
    auto wide_name = win::widen(name);
    if (!wide_name) return std::unexpected(wide_name.error());
    std::filesystem::path filename(*wide_name);
    const auto extension = filename.extension().native();
    if (_wcsicmp(extension.c_str(), L".exe") != 0 && _wcsicmp(extension.c_str(), L".com") != 0 &&
        _wcsicmp(extension.c_str(), L".cmd") != 0 && _wcsicmp(extension.c_str(), L".bat") != 0) filename += L".exe";

    if (options.configured_path) return validate_configured_path(*options.configured_path);

    if (auto from_path = win::find_executable(filename.native()); from_path) return *from_path;

    if (!options.tools_directory) {
        return std::unexpected(Error{ErrorKind::not_found,
            "Tool was not found: " + std::string(name),
            "It is not configured, available in PATH, or associated with a tools directory"});
    }

    std::error_code error;
    const auto root = std::filesystem::absolute(*options.tools_directory, error).lexically_normal();
    if (error || !std::filesystem::is_directory(root, error)) {
        auto display = win::narrow(options.tools_directory->native()).value_or("<invalid path>");
        return std::unexpected(Error{ErrorKind::not_found, "Tools directory was not found: " + display, error.message()});
    }

    std::vector<std::filesystem::path> matches;
    const auto direct = root / filename;
    if (regular_file(direct)) matches.push_back(direct);

    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (!iterator->is_directory(error)) continue;
        const auto candidate = iterator->path() / filename;
        if (regular_file(candidate)) matches.push_back(candidate);
    }
    if (error) {
        auto display = win::narrow(root.native()).value_or("<invalid path>");
        return std::unexpected(Error{ErrorKind::io, "Could not inspect tools directory: " + display, error.message()});
    }
    std::ranges::sort(matches);
    matches.erase(std::ranges::unique(matches).begin(), matches.end());
    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1) {
        std::string detail = "Multiple matches:";
        for (const auto& match : matches) {
            detail += "\n  " + win::narrow(match.native()).value_or("<invalid path>");
        }
        return std::unexpected(Error{ErrorKind::usage,
            "Tool resolution is ambiguous: " + std::string(name) +
                ". Configure atlas.tools." + std::string(name) + " explicitly",
            std::move(detail)});
    }
    return std::unexpected(Error{ErrorKind::not_found,
        "Tool was not found in PATH or the configured tools directory: " + std::string(name)});
}

} // namespace atlas
