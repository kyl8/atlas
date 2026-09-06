#include "atlas/commands/files.hpp"

#include "atlas/platform/paths.hpp"
#include "atlas/platform/utf.hpp"
#include "atlas/terminal/formatter.hpp"
#include "atlas/terminal/ui.hpp"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <optional>

namespace atlas::commands {
namespace {

struct BrowseOptions {
    std::filesystem::path path;
    std::size_t depth{3};
    std::size_t limit{2'000};
    bool all{};
    bool directories_only{};
    bool files_only{};
};

struct ScanState {
    std::size_t count{};
    std::size_t limit{};
    bool truncated{};
    bool all{};
    bool directories_only{};
    bool files_only{};
};

Result<std::size_t> number(std::string_view value, std::string_view option, std::size_t maximum) {
    std::size_t parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed > maximum) {
        return std::unexpected(Error{ErrorKind::usage,
            std::string(option) + " requires an integer from 0 to " + std::to_string(maximum)});
    }
    return parsed;
}

Result<BrowseOptions> browse_options(const std::span<const std::string> arguments, const bool recursive) {
    BrowseOptions options;
    options.depth = recursive ? 3 : 0;
    std::optional<std::string> source_path;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "-a" || argument == "--all") options.all = true;
        else if (argument == "--dirs-only") options.directories_only = true;
        else if (argument == "--files-only") options.files_only = true;
        else if (argument == "-d" || argument == "--depth") {
            if (!recursive) return std::unexpected(Error{ErrorKind::usage, "--depth is only valid for atlas tree"});
            if (++index >= arguments.size()) return std::unexpected(Error{ErrorKind::usage, "--depth requires a value"});
            auto parsed = number(arguments[index], "--depth", 32);
            if (!parsed) return std::unexpected(parsed.error());
            options.depth = *parsed;
        } else if (argument == "-n" || argument == "--limit") {
            if (++index >= arguments.size()) return std::unexpected(Error{ErrorKind::usage, "--limit requires a value"});
            auto parsed = number(arguments[index], "--limit", 100'000);
            if (!parsed || *parsed == 0) return std::unexpected(parsed ? Error{ErrorKind::usage, "--limit must be greater than zero"} : parsed.error());
            options.limit = *parsed;
        } else if (!argument.empty() && argument.front() == '-') {
            return std::unexpected(Error{ErrorKind::usage, "Unknown directory option: " + argument});
        } else if (source_path) {
            return std::unexpected(Error{ErrorKind::usage, recursive ? "Usage: atlas tree [path] [options]" : "Usage: atlas ls [path] [options]"});
        } else source_path = argument;
    }
    if (options.directories_only && options.files_only) {
        return std::unexpected(Error{ErrorKind::usage, "--dirs-only and --files-only cannot be combined"});
    }
    auto resolved = win::resolve_path(source_path.value_or("."));
    if (!resolved) return std::unexpected(resolved.error());
    std::error_code error;
    if (!std::filesystem::is_directory(*resolved, error)) {
        return std::unexpected(Error{error ? ErrorKind::io : ErrorKind::not_found,
            "Directory was not found: " + source_path.value_or("."), error.message()});
    }
    options.path = std::move(*resolved);
    return options;
}

std::optional<std::time_t> modified_time(const std::filesystem::directory_entry& entry) {
    std::error_code error;
    const auto value = entry.last_write_time(error);
    if (error) return std::nullopt;
    return std::chrono::system_clock::to_time_t(std::chrono::clock_cast<std::chrono::system_clock>(value));
}

std::string mode(const DWORD attributes) {
    std::string value = "------";
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) value[0] = 'd';
    if ((attributes & FILE_ATTRIBUTE_ARCHIVE) != 0) value[1] = 'a';
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) value[2] = 'r';
    if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) value[3] = 'h';
    if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0) value[4] = 's';
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) value[5] = 'l';
    return value;
}

bool hidden(const std::filesystem::directory_entry& entry, const DWORD attributes) {
    const auto name = entry.path().filename().native();
    return (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0 ||
           (!name.empty() && name.front() == L'.');
}

Result<std::vector<terminal::TreeNode>> scan_directory(const std::filesystem::path& directory,
                                                       const std::size_t remaining_depth,
                                                       ScanState& state) {
    std::error_code error;
    std::vector<std::filesystem::directory_entry> entries;
    for (std::filesystem::directory_iterator iterator(directory,
             std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && iterator != end; iterator.increment(error)) {
        entries.push_back(*iterator);
    }
    if (error && error != std::errc::permission_denied) {
        return std::unexpected(Error{ErrorKind::io,
            "Could not list directory: " + win::narrow(directory.native()).value_or("<invalid path>"), error.message()});
    }
    std::ranges::sort(entries, [](const auto& left, const auto& right) {
        std::error_code left_error, right_error;
        const auto left_directory = left.is_directory(left_error);
        const auto right_directory = right.is_directory(right_error);
        if (left_directory != right_directory) return left_directory;
        return _wcsicmp(left.path().filename().c_str(), right.path().filename().c_str()) < 0;
    });

    std::vector<terminal::TreeNode> nodes;
    for (const auto& entry : entries) {
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        if (!state.all && hidden(entry, attributes)) continue;
        const auto is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (state.directories_only && !is_directory) continue;
        if (state.files_only && is_directory) {
            if (remaining_depth == 0 || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
            auto children = scan_directory(entry.path(), remaining_depth - 1, state);
            if (!children) return std::unexpected(children.error());
            if (children->empty()) continue;
            auto name = win::narrow(entry.path().filename().native());
            auto path = win::narrow(entry.path().native());
            if (!name || !path) return std::unexpected(name ? path.error() : name.error());
            terminal::TreeNode node{*name, *path, true};
            node.modified = modified_time(entry);
            node.children = std::move(*children);
            nodes.push_back(std::move(node));
            if (state.truncated) break;
            continue;
        }
        if (state.count >= state.limit) { state.truncated = true; break; }
        auto name = win::narrow(entry.path().filename().native());
        auto path = win::narrow(entry.path().native());
        if (!name || !path) return std::unexpected(name ? path.error() : name.error());
        terminal::TreeNode node{*name, *path, is_directory};
        if (!is_directory) {
            const auto bytes = entry.file_size(error);
            if (!error) node.size = bytes;
            error.clear();
        }
        node.modified = modified_time(entry);
        ++state.count;
        if (is_directory && remaining_depth > 0 && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
            auto children = scan_directory(entry.path(), remaining_depth - 1, state);
            if (!children) return std::unexpected(children.error());
            node.children = std::move(*children);
        }
        nodes.push_back(std::move(node));
        if (state.truncated) break;
    }
    return nodes;
}

void append_json_node(std::string& json, const terminal::TreeNode& node) {
    json += "{\"name\":\"" + terminal::json_escape(node.name) + "\",\"path\":\"" +
        terminal::json_escape(node.path) + "\",\"type\":\"" + (node.directory ? "directory" : "file") +
        "\",\"size\":" + (node.size ? std::to_string(*node.size) : "null") + ",\"modified\":" +
        (node.modified ? std::to_string(*node.modified) : "null") + ",\"children\":[";
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (index != 0) json.push_back(',');
        append_json_node(json, node.children[index]);
    }
    json += "]}";
}

Result<std::pair<terminal::Tree, bool>> directory_tree(const BrowseOptions& options) {
    ScanState state{0, options.limit, false, options.all, options.directories_only, options.files_only};
    auto children = scan_directory(options.path, options.depth, state);
    if (!children) return std::unexpected(children.error());
    auto root = win::narrow(options.path.native());
    if (!root) return std::unexpected(root.error());
    return std::pair{terminal::Tree{*root, std::move(*children)}, state.truncated};
}

void emit_json(cli::CommandContext& context, const terminal::Tree& tree, const bool truncated) {
    std::string json = "{\"root\":\"" + terminal::json_escape(tree.root) + "\",\"truncated\":" +
        (truncated ? "true" : "false") + ",\"entries\":[";
    for (std::size_t index = 0; index < tree.children.size(); ++index) {
        if (index != 0) json.push_back(',');
        append_json_node(json, tree.children[index]);
    }
    context.ui.data(json + "]}\n");
}

} // namespace

Result<int> list_directory(cli::CommandContext& context, const std::span<const std::string> arguments) {
    auto options = browse_options(arguments, false);
    if (!options) return std::unexpected(options.error());
    auto result = directory_tree(*options);
    if (!result) return std::unexpected(result.error());
    auto& [tree, truncated] = *result;
    if (context.ui.json()) emit_json(context, tree, truncated);
    else if (!context.ui.human()) context.ui.tree(tree);
    else {
        context.ui.heading("Directory", tree.root);
        terminal::Table table;
        table.columns = {{"MODE", terminal::StyleToken::muted}, {"NAME", terminal::StyleToken::filename},
                         {"SIZE", terminal::StyleToken::size}, {"MODIFIED", terminal::StyleToken::date}};
        table.compact_primary = 1;
        for (const auto& node : tree.children) {
            const auto attributes = GetFileAttributesW(win::widen(node.path).value_or(L"").c_str());
            table.rows.push_back({{mode(attributes == INVALID_FILE_ATTRIBUTES ? 0 : attributes), terminal::StyleToken::muted},
                {node.name, node.directory ? terminal::StyleToken::directory : terminal::StyleToken::filename},
                {node.size ? terminal::format_filesize(*node.size) : "-", node.size ? terminal::StyleToken::size : terminal::StyleToken::muted},
                {node.modified ? terminal::format_timestamp(*node.modified) : "-", node.modified ? terminal::StyleToken::date : terminal::StyleToken::muted}});
        }
        context.ui.table(table);
    }
    if (truncated) context.ui.warning("Directory output reached --limit " + std::to_string(options->limit));
    return 0;
}

Result<int> show_tree(cli::CommandContext& context, const std::span<const std::string> arguments) {
    auto options = browse_options(arguments, true);
    if (!options) return std::unexpected(options.error());
    auto result = directory_tree(*options);
    if (!result) return std::unexpected(result.error());
    auto& [tree, truncated] = *result;
    if (context.ui.json()) emit_json(context, tree, truncated);
    else {
        if (context.ui.human()) context.ui.heading("Tree");
        context.ui.tree(tree);
    }
    if (truncated) context.ui.warning("Tree output reached --limit " + std::to_string(options->limit));
    return 0;
}

} // namespace atlas::commands
