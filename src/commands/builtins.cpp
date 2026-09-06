#include "atlas/commands/builtins.hpp"
#include "atlas/commands/files.hpp"
#include "atlas/commands/completion.hpp"

#include "atlas/fuzzy.hpp"
#include "atlas/lua/runtime.hpp"
#include "atlas/platform/paths.hpp"
#include "atlas/platform/process.hpp"
#include "atlas/platform/service.hpp"
#include "atlas/platform/shell.hpp"
#include "atlas/platform/utf.hpp"
#include "atlas/tools.hpp"
#include "atlas/terminal/formatter.hpp"
#include "atlas/terminal/ui.hpp"
#include "atlas/terminal/screens.hpp"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <thread>

namespace atlas::commands {
namespace {

struct SearchItem {
    std::string name;
    std::string directory;
    std::optional<std::uint64_t> size;
    std::optional<std::time_t> modified;
    std::uint32_t attributes{};
    std::int32_t fuzzy_score{};
};

bool simple_fuzzy_query(const std::string_view query) {
    std::size_t characters = 0;
    for (std::size_t index = 0; index < query.size();) {
        const auto value = static_cast<unsigned char>(query[index]);
        if (value < 0x80 && std::string_view{"*?|!<>:\"/\\[]{}()^$+-"}.contains(static_cast<char>(value))) {
            return false;
        }
        auto length = std::size_t{1};
        if ((value & 0xe0) == 0xc0) length = 2;
        else if ((value & 0xf0) == 0xe0) length = 3;
        else if ((value & 0xf8) == 0xf0) length = 4;
        index += (std::min)(length, query.size() - index);
        ++characters;
    }
    return characters >= 3 && characters <= 96;
}

std::string search_path(const SearchItem& item) {
    const auto full = std::filesystem::path(win::widen(item.directory).value_or(L"")) /
                      win::widen(item.name).value_or(L"");
    return win::narrow(full.native()).value_or(item.directory + "\\" + item.name);
}

std::string everything_literal(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        if (character == ' ') result += "&sp:";
        else if (character == '&') result += "&#x26:";
        else result.push_back(character);
    }
    return result;
}

std::optional<std::int32_t> search_fuzzy_score(const std::string_view query,
                                               const std::string_view candidate) {
    const auto matched = fuzzy::score(query, candidate);
    if (!matched) return std::nullopt;
    const auto distance = static_cast<std::int32_t>((std::min)(
        fuzzy::edit_distance(query, candidate), std::size_t{10'000}));
    return *matched - distance * 5;
}

std::vector<std::string> parse_csv_row(const std::string_view line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const auto character = line[index];
        if (character == '"') {
            if (quoted && index + 1 < line.size() && line[index + 1] == '"') {
                field.push_back('"');
                ++index;
            } else {
                quoted = !quoted;
            }
        } else if (character == ',' && !quoted) {
            fields.push_back(std::move(field));
            field.clear();
        } else if (character != '\r') {
            field.push_back(character);
        }
    }
    fields.push_back(std::move(field));
    return fields;
}

std::optional<std::time_t> parse_es_time(const std::string_view value) {
    std::uint64_t ticks = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), ticks);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || ticks == 0) return std::nullopt;
    // ES -date-format 2 is a UTC Windows FILETIME, independent of the user locale.
    return static_cast<std::time_t>(ticks / 10'000'000) - 11'644'473'600LL;
}

Result<std::vector<SearchItem>> parse_search_results(const std::string_view output) {
    std::vector<SearchItem> items;
    for (std::size_t start = 0; start < output.size();) {
        const auto end = output.find('\n', start);
        auto line = output.substr(start, end == std::string_view::npos ? output.size() - start : end - start);
        start = end == std::string_view::npos ? output.size() : end + 1;
        if (line.empty() || line == "\r") continue;
        auto fields = parse_csv_row(line);
        if (fields.size() != 5) {
            return std::unexpected(Error{ErrorKind::process, "Everything returned an unexpected result format", std::string(line)});
        }
        SearchItem item{std::move(fields[0]), std::move(fields[1])};
        if (item.directory.size() == 2 && item.directory[1] == ':' &&
            ((item.directory[0] >= 'A' && item.directory[0] <= 'Z') ||
             (item.directory[0] >= 'a' && item.directory[0] <= 'z'))) {
            item.directory.push_back('\\');
        }
        if (!fields[2].empty()) {
            std::uint64_t size = 0;
            if (const auto parsed = std::from_chars(fields[2].data(), fields[2].data() + fields[2].size(), size);
                parsed.ec == std::errc{} && parsed.ptr == fields[2].data() + fields[2].size()) item.size = size;
        }
        item.modified = parse_es_time(fields[3]);
        std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), item.attributes);
        if ((item.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) item.size.reset();
        items.push_back(std::move(item));
    }
    return items;
}

std::string file_mode(const std::uint32_t attributes) {
    std::string mode = "------";
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) mode[0] = 'd';
    if ((attributes & FILE_ATTRIBUTE_ARCHIVE) != 0) mode[1] = 'a';
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) mode[2] = 'r';
    if ((attributes & FILE_ATTRIBUTE_HIDDEN) != 0) mode[3] = 'h';
    if ((attributes & FILE_ATTRIBUTE_SYSTEM) != 0) mode[4] = 's';
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) mode[5] = 'l';
    return mode;
}

using terminal::json_escape;

Result<int> run_script(cli::CommandContext& context, const std::span<const std::string> arguments) {
    if (arguments.size() != 1) {
        return std::unexpected(Error{ErrorKind::usage, "Usage: atlas run <file.lua>"});
    }
    auto path = win::resolve_path(arguments.front());
    if (!path) return std::unexpected(path.error());
    std::error_code error;
    if (!std::filesystem::is_regular_file(*path, error)) {
        return std::unexpected(Error{ErrorKind::not_found, "Lua script was not found: " + arguments.front()});
    }
    lua::Runtime runtime(&context.ui);
    if (!runtime.valid()) return std::unexpected(runtime.initialization_error());
    auto loaded = runtime.load_file(*path);
    if (!loaded) return std::unexpected(loaded.error());
    return 0;
}

Result<int> open_target(cli::CommandContext& context, const std::span<const std::string> arguments) {
    if (arguments.size() != 1) {
        return std::unexpected(Error{ErrorKind::usage, "Usage: atlas open <path-or-url>"});
    }
    Result<std::wstring> target = win::widen(arguments.front());
    if (arguments.front().find("://") == std::string::npos) {
        auto path = win::resolve_path(arguments.front());
        if (!path) return std::unexpected(path.error());
        target = path->native();
    }
    if (!target) return std::unexpected(target.error());
    auto opened = win::shell_open(*target);
    if (!opened) {
        opened.error().message += ": " + arguments.front();
        return std::unexpected(opened.error());
    }
    context.ui.success("Opened " + arguments.front());
    return 0;
}

std::optional<std::wstring> environment_value(const std::wstring_view name) {
    const std::wstring owned(name);
    const auto required = GetEnvironmentVariableW(owned.c_str(), nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    const auto written = GetEnvironmentVariableW(owned.c_str(), value.data(), required);
    if (written == 0 || written >= required) return std::nullopt;
    value.resize(written);
    return value;
}

std::wstring tool_environment_name(const std::string_view name) {
    std::wstring result = L"ATLAS_";
    for (const auto character : name) {
        if (character >= 'a' && character <= 'z') result.push_back(static_cast<wchar_t>(character - 'a' + 'A'));
        else if ((character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9')) result.push_back(static_cast<wchar_t>(character));
        else result.push_back(L'_');
    }
    return result;
}

Result<ToolLocationOptions> tool_options(const std::string_view name, lua::Runtime* runtime = nullptr) {
    ToolLocationOptions options;
    if (auto explicit_path = environment_value(tool_environment_name(name))) {
        options.configured_path = std::filesystem::path(*explicit_path);
    }
    if (auto tools_directory = environment_value(L"ATLAS_TOOLS")) {
        options.tools_directory = std::filesystem::path(*tools_directory);
    }

    auto atlas_directory = win::user_atlas_directory();
    if (!atlas_directory) return std::unexpected(atlas_directory.error());
    std::error_code config_error;
    const auto config = *atlas_directory / L"config.lua";
    const auto has_config = std::filesystem::exists(config, config_error);
    if (config_error) {
        return std::unexpected(Error{ErrorKind::io, "Could not inspect ~/.atlas/config.lua", config_error.message()});
    }
    if (has_config && !std::filesystem::is_regular_file(config, config_error)) {
        return std::unexpected(Error{ErrorKind::io, "~/.atlas/config.lua is not a regular file", config_error.message()});
    }
    if (!has_config || (options.configured_path && options.tools_directory)) return options;

    std::unique_ptr<lua::Runtime> owned_runtime;
    if (!runtime) {
        owned_runtime = std::make_unique<lua::Runtime>();
        runtime = owned_runtime.get();
        if (!runtime->valid()) return std::unexpected(runtime->initialization_error());
        if (auto loaded = runtime->load_user_config(); !loaded) return std::unexpected(loaded.error());
    }
    if (!options.configured_path) {
        if (auto configured = runtime->configured_tool(name)) {
            auto path = win::resolve_path(*configured);
            if (!path) return std::unexpected(path.error());
            options.configured_path = std::move(*path);
        }
    }
    if (!options.tools_directory) {
        if (auto configured = runtime->configured_tools_directory()) {
            auto path = win::resolve_path(*configured);
            if (!path) return std::unexpected(path.error());
            options.tools_directory = std::move(*path);
        }
    }
    return options;
}

Result<std::filesystem::path> resolve_tool(const std::string_view name) {
    auto options = tool_options(name);
    if (!options) return std::unexpected(options.error());
    return locate_tool(name, *options);
}

Status check_search_index(const std::filesystem::path& executable,
                          const std::span<const std::wstring> arguments) {
    // Only empty searches need this extra IPC query. Never enumerate the disk.
    std::vector<std::wstring> probe{L"-get-result-count", L"-no-digit-grouping", L"-timeout", L"1000"};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument.empty() || (argument.front() != L'-' && argument.front() != L'/')) continue;
        auto option = argument.substr(1, argument.find(L'=') - 1);
        for (auto& character : option) {
            if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
        }
        if (option != L"instance") continue;
        probe.push_back(argument);
        if (argument.find(L'=') == std::wstring::npos && index + 1 < arguments.size()) {
            probe.push_back(arguments[++index]);
        }
    }
    auto result = win::execute(executable.native(), probe);
    if (!result) return std::unexpected(result.error());
    if (result->exit_code != 0) {
        return std::unexpected(Error{ErrorKind::process,
            "Could not check the Everything index after an empty search. Check that Everything is running and its index has loaded.",
            result->stdout_text + result->stderr_text, result->exit_code});
    }
    std::string_view count_text = result->stdout_text;
    const auto first = count_text.find_first_not_of(" \t\r\n");
    if (first != std::string_view::npos) {
        count_text = count_text.substr(first, count_text.find_last_not_of(" \t\r\n") - first + 1);
    }
    std::uint64_t count{};
    const auto parsed = std::from_chars(count_text.data(), count_text.data() + count_text.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != count_text.data() + count_text.size()) {
        return std::unexpected(Error{ErrorKind::process,
            "Everything returned an invalid index count. Check your ES version and settings.", result->stdout_text});
    }
    if (count == 0) {
        return std::unexpected(Error{ErrorKind::process,
            "Everything is running, but its index is empty. Open Everything > Tools > Options and configure indexing. "
            "For NTFS disks, enable Everything Service, check the indexed volumes and wait for indexing to finish."});
    }
    return {};
}

std::string trimmed(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value.erase(value.find_last_not_of(" \t\r\n") + 1);
    value.erase(0, first);
    return value;
}

Result<std::uint64_t> everything_index_count(const std::filesystem::path& es) {
    const std::vector<std::wstring> arguments{L"-get-result-count", L"-no-digit-grouping", L"-timeout", L"1000"};
    auto result = win::execute(es.native(), arguments);
    if (!result) return std::unexpected(result.error());
    if (result->exit_code != 0) {
        return std::unexpected(Error{ErrorKind::process,
            result->exit_code == 8 ? "Everything is not running" : "Could not read the Everything index",
            result->stdout_text + result->stderr_text, result->exit_code});
    }
    const auto output = trimmed(std::move(result->stdout_text));
    std::uint64_t count{};
    const auto parsed = std::from_chars(output.data(), output.data() + output.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != output.data() + output.size()) {
        return std::unexpected(Error{ErrorKind::process, "Everything returned an invalid index count", output});
    }
    return count;
}

Status start_everything_for_search(const std::filesystem::path& es) {
    auto everything = resolve_tool("Everything");
    if (!everything) {
        return std::unexpected(Error{ErrorKind::not_found,
            "Everything is not running and 'Everything.exe' could not be started. "
            "Place it in the Atlas tools directory or configure atlas.tools.Everything.",
            everything.error().message + (everything.error().detail.empty() ? "" : "\n" + everything.error().detail)});
    }
    const std::vector<std::wstring> arguments{L"-startup"};
    if (auto started = win::spawn(everything->native(), arguments, true); !started) {
        return std::unexpected(started.error());
    }

    // The IPC window appears before a fresh index is necessarily ready. Wait a
    // short bounded period so the first search behaves like every later search.
    bool ipc_ready = false;
    for (int attempt = 0; attempt < 75; ++attempt) {
        auto count = everything_index_count(es);
        if (count) {
            ipc_ready = true;
            if (*count != 0) return {};
        } else if (count.error().native_code != 8) {
            return std::unexpected(count.error());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (ipc_ready) return {};
    return std::unexpected(Error{ErrorKind::process,
        "Everything was started, but its search interface did not become ready in time. Try the command again."});
}

Result<int> search_status(cli::CommandContext& context) {
    const auto es = resolve_tool("es");
    const auto everything = resolve_tool("Everything");
    const auto service = win::query_service(L"Everything");
    std::string version;
    std::optional<std::uint64_t> count;
    bool application_running = false;
    if (es) {
        const std::vector<std::wstring> version_arguments{L"-get-everything-version", L"-timeout", L"1000"};
        if (auto result = win::execute(es->native(), version_arguments);
            result && result->exit_code == 0) {
            application_running = true;
            version = trimmed(std::move(result->stdout_text));
            if (auto indexed = everything_index_count(*es)) count = *indexed;
        }
    }
    const auto ready = es.has_value() && application_running && count.value_or(0) != 0;
    const auto path = [](const Result<std::filesystem::path>& result) {
        return result ? win::narrow(result->native()).value_or("") : std::string("not found");
    };
    const auto service_status = !service ? std::string("error") : !service->installed ? std::string("not installed") :
        service->running ? std::string("running") : std::string("stopped");
    const auto index_status = !application_running ? std::string("unavailable") :
        count.value_or(0) == 0 ? std::string("empty") : std::string("ready");

    if (context.ui.json()) {
        context.ui.data("{\"ready\":" + std::string(ready ? "true" : "false") +
            ",\"es\":{\"status\":\"" + (es ? "found" : "missing") + "\",\"path\":\"" +
            json_escape(path(es)) + "\"},\"application\":{\"status\":\"" +
            (application_running ? "running" : "stopped") + "\",\"version\":" +
            (version.empty() ? "null" : "\"" + json_escape(version) + "\"") + ",\"path\":\"" +
            json_escape(path(everything)) + "\"},\"service\":{\"status\":\"" + service_status +
            "\",\"automatic\":" + std::string(service && service->automatic ? "true" : "false") +
            "},\"index\":{\"status\":\"" + index_status + "\",\"items\":" +
            (count ? std::to_string(*count) : "null") + "}}\n");
        return ready ? 0 : 1;
    }
    if (context.ui.human()) context.ui.heading("Search", ready ? "ready" : "not ready");
    terminal::Table table;
    table.columns = {{"COMPONENT", terminal::StyleToken::primary},
                     {"STATUS", terminal::StyleToken::success},
                     {"DETAIL", terminal::StyleToken::secondary}};
    const auto add = [&](std::string component, std::string status, std::string detail) {
        const auto style = status == "ready" || status == "running" || status == "found"
            ? terminal::StyleToken::success : terminal::StyleToken::warning;
        table.rows.push_back({{std::move(component), terminal::StyleToken::primary},
                              {std::move(status), style}, {std::move(detail), terminal::StyleToken::secondary}});
    };
    add("ES CLI", es ? "found" : "missing", path(es));
    add("Everything app", application_running ? "running" : "stopped",
        version.empty() ? path(everything) : version + " · " + path(everything));
    add("Everything service", service_status,
        service && service->automatic ? "automatic startup" : service && service->installed ? "manual startup" : "-");
    add("Index", index_status, count ? terminal::format_number(*count) + " items" : "-");
    context.ui.table(table);
    return ready ? 0 : 1;
}

Result<int> search(cli::CommandContext& context, const std::span<const std::string> arguments) {
    if (arguments.size() == 1 && arguments.front() == "--status") return search_status(context);
    if (arguments.empty()) return show_tree(context, {});

    bool here = false;
    bool tree = false;
    bool has_query = false;
    bool browsing_only_option = false;
    bool explicit_limit = false;
    bool fuzzy_requested = false;
    bool exact_requested = false;
    std::string limit_value;
    std::vector<std::string> normalized_arguments;
    std::vector<std::string> tree_arguments;
    normalized_arguments.reserve(arguments.size() + 2);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (argument == "--status") {
            return std::unexpected(Error{ErrorKind::usage, "--status cannot be combined with a search query or other options"});
        }
        if (argument == "--here") { here = true; continue; }
        if (argument == "--tree") { tree = true; continue; }
        if (argument == "--fuzzy") { fuzzy_requested = true; continue; }
        if (argument == "--exact") { exact_requested = true; continue; }
        if (argument == "--files") { normalized_arguments.emplace_back("/a-d"); tree_arguments.emplace_back("--files-only"); continue; }
        if (argument == "--dirs") { normalized_arguments.emplace_back("/ad"); tree_arguments.emplace_back("--dirs-only"); continue; }
        if (argument == "--limit") {
            if (++index >= arguments.size()) return std::unexpected(Error{ErrorKind::usage, "--limit requires a value"});
            explicit_limit = true;
            limit_value = arguments[index];
            tree_arguments.emplace_back("--limit");
            tree_arguments.push_back(arguments[index]);
            continue;
        }
        if (argument == "--depth" || argument == "-d" || argument == "--all") {
            browsing_only_option = true;
            tree_arguments.push_back(argument);
            if (argument != "--all") {
                if (++index >= arguments.size()) return std::unexpected(Error{ErrorKind::usage, argument + " requires a value"});
                tree_arguments.push_back(arguments[index]);
            }
            continue;
        }
        if (!argument.empty() && argument.front() != '-' && argument.front() != '/') has_query = true;
        normalized_arguments.push_back(argument);
    }
    if (!has_query) {
        if (fuzzy_requested || exact_requested) {
            return std::unexpected(Error{ErrorKind::usage, "--fuzzy and --exact require a search query"});
        }
        if (!here && !tree && !normalized_arguments.empty()) {
            return std::unexpected(Error{ErrorKind::usage, "Usage: atlas search <query> [options], or atlas search --here"});
        }
        return show_tree(context, tree_arguments);
    }
    if (browsing_only_option) {
        return std::unexpected(Error{ErrorKind::usage, "--depth and --all are only valid when browsing without a query"});
    }
    if (tree && !here) {
        return std::unexpected(Error{ErrorKind::usage, "--tree requires --here so results have a single root"});
    }
    if (fuzzy_requested && exact_requested) {
        return std::unexpected(Error{ErrorKind::usage, "--fuzzy and --exact cannot be combined"});
    }
    if (fuzzy_requested && context.ui.raw()) {
        return std::unexpected(Error{ErrorKind::usage, "--fuzzy cannot be combined with --raw"});
    }

    std::optional<std::size_t> requested_limit;
    if (explicit_limit) {
        std::uint64_t parsed = 0;
        const auto converted = std::from_chars(limit_value.data(), limit_value.data() + limit_value.size(), parsed);
        if (converted.ec != std::errc{} || converted.ptr != limit_value.data() + limit_value.size() ||
            parsed == 0 || parsed > 10'000) {
            return std::unexpected(Error{ErrorKind::usage, "--limit must be between 1 and 10,000"});
        }
        requested_limit = static_cast<std::size_t>(parsed);
    }

    const auto query_value = std::ranges::find_if(normalized_arguments, [](const auto& value) {
        return !value.empty() && value.front() != '-' && value.front() != '/';
    });
    std::string query = query_value == normalized_arguments.end() ? std::string("query") : *query_value;
    const auto query_index = static_cast<std::size_t>(std::distance(normalized_arguments.begin(), query_value));
    const auto automatic_fuzzy = query_index == 0 && simple_fuzzy_query(query) &&
        std::ranges::all_of(normalized_arguments | std::views::drop(1), [](const auto& value) {
            return !value.empty() && (value.front() == '-' || value.front() == '/');
        });
    const bool fuzzy = !context.ui.raw() && !exact_requested && (fuzzy_requested || automatic_fuzzy);

    auto executable = resolve_tool("es");
    if (!executable) {
        if (executable.error().kind != ErrorKind::not_found) return std::unexpected(executable.error());
        return std::unexpected(Error{ErrorKind::not_found,
            "'es.exe' was not found. Run 'atlas setup <tools-directory>', add it to PATH, "
            "set ATLAS_ES, or configure atlas.tools.es",
            executable.error().message + (executable.error().detail.empty() ? "" : "\n" + executable.error().detail),
            executable.error().native_code});
    }

    auto process_arguments = normalized_arguments;
    if (fuzzy) {
        process_arguments.erase(process_arguments.begin() + static_cast<std::ptrdiff_t>(query_index));
        const auto pattern = fuzzy::subsequence_regex(query);
        if (fuzzy_requested) {
            process_arguments.insert(process_arguments.begin() + static_cast<std::ptrdiff_t>(query_index),
                                     {"-regex", pattern});
        } else {
            // Keep native substring hits while adding a filename-start fuzzy
            // branch. The anchored branch avoids filling the candidate pool
            // with unrelated paths before relevant names reach Atlas.
            process_arguments.insert(process_arguments.begin() + static_cast<std::ptrdiff_t>(query_index),
                                     "<regex:^" + pattern + '|' + everything_literal(query) + '>');
        }
    }

    constexpr std::size_t visual_result_limit = 100;
    const auto result_limit = requested_limit.value_or(visual_result_limit);
    const auto candidate_limit = fuzzy
        ? fuzzy_requested ? std::size_t{10'000}
                          : (std::min)(std::size_t{10'000}, (std::max)(std::size_t{1'000}, result_limit * 10))
        : result_limit;
    if (fuzzy || explicit_limit || context.ui.human()) {
        process_arguments.emplace_back("-n");
        process_arguments.push_back(std::to_string(candidate_limit));
    }

    std::vector<std::wstring> wide_arguments;
    wide_arguments.reserve(process_arguments.size() + 10);
    for (const auto& argument : process_arguments) {
        auto wide = win::widen(argument);
        if (!wide) return std::unexpected(wide.error());
        wide_arguments.push_back(std::move(*wide));
    }
    const bool visually_limited = context.ui.human() && !explicit_limit;
    std::filesystem::path here_path;
    if (here) {
        auto current = win::resolve_path(".");
        if (!current) return std::unexpected(current.error());
        here_path = std::move(*current);
        wide_arguments.emplace_back(L"-path");
        wide_arguments.push_back(here_path.native());
    }
    const auto structured = fuzzy || context.ui.human() || context.ui.json();
    if (structured) {
        constexpr std::wstring_view metadata[]{
            L"-csv", L"-no-header", L"-name", L"-path-column", L"-size",
            L"-dm", L"-attributes", L"-size-format", L"1", L"-no-digit-grouping", L"-date-format", L"2",
        };
        for (const auto option : metadata) wide_arguments.emplace_back(option);
    }
    win::ProcessOptions options;
    options.capture_output = !context.ui.raw();
    options.collect_stdout = structured;
    std::string plain_prefix;
    bool streaming = false;
    if (!structured && !context.ui.raw()) {
        options.on_stdout = [&](const std::string_view chunk) {
            if (streaming) { context.ui.data(chunk); return; }
            // ES writes failures to stdout. Recognize its error prefix before
            // forwarding paths, then stream successful results with bounded memory.
            if (plain_prefix.starts_with("Error ")) {
                if (plain_prefix.size() < 16 * 1024) plain_prefix.append(chunk.substr(0, 16 * 1024 - plain_prefix.size()));
                return;
            }
            plain_prefix.append(chunk);
            if (plain_prefix.size() < 6) return;
            if (plain_prefix.starts_with("Error ")) return;
            context.ui.data(plain_prefix);
            plain_prefix.clear();
            streaming = true;
        };
    }
    if (context.ui.human()) context.ui.heading("Search", query);
    auto spinner = context.ui.spinner("Searching");
    const auto started = std::chrono::steady_clock::now();
    if (context.ui.raw()) {
        auto probe = everything_index_count(*executable);
        if (!probe && probe.error().native_code == 8) {
            if (auto started_backend = start_everything_for_search(*executable); !started_backend) {
                spinner->finish();
                return std::unexpected(started_backend.error());
            }
        }
    }
    auto result = win::execute(executable->native(), wide_arguments, options);
    if (result && result->exit_code == 8 && !context.ui.raw()) {
        plain_prefix.clear();
        streaming = false;
        if (auto launched = start_everything_for_search(*executable); !launched) {
            spinner->finish();
            return std::unexpected(launched.error());
        }
        result = win::execute(executable->native(), wide_arguments, options);
    }
    Status index_status;
    if (result && result->exit_code == 0 && !context.ui.raw()) {
        const auto empty = structured ? result->stdout_text.find_first_not_of(" \t\r\n") == std::string::npos
                                      : !streaming && plain_prefix.empty();
        if (empty) index_status = check_search_index(*executable, wide_arguments);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    spinner->finish();
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!index_status) return std::unexpected(index_status.error());
    if (result->exit_code != 0) {
        std::string diagnostic = result->stderr_text;
        if (!diagnostic.empty()) diagnostic.push_back('\n');
        diagnostic += structured ? result->stdout_text : plain_prefix;
        const auto message = result->exit_code == 8
            ? "Everything is not running. Start Everything.exe (the search application), then retry. es.exe alone cannot search."
            : "Everything search failed (exit " + std::to_string(result->exit_code) + ")";
        context.ui.error(message, context.ui.raw() ? std::string_view{} : std::string_view(diagnostic));
        return static_cast<int>(result->exit_code);
    }
    if (!structured) {
        if (!plain_prefix.empty()) context.ui.data(plain_prefix);
        if (!result->stderr_text.empty()) context.ui.warning(result->stderr_text);
        return 0;
    }

    auto items = parse_search_results(result->stdout_text);
    if (!items) return std::unexpected(items.error());
    bool additional_matches = false;
    if (fuzzy) {
        for (auto& item : *items) {
            const auto name_score = search_fuzzy_score(query, item.name);
            const auto path_score = name_score ? std::optional<std::int32_t>{}
                                               : search_fuzzy_score(query, search_path(item));
            item.fuzzy_score = name_score ? *name_score : path_score ? *path_score - 25
                                                                      : std::numeric_limits<std::int32_t>::min();
        }
        std::erase_if(*items, [](const SearchItem& item) {
            return item.fuzzy_score == std::numeric_limits<std::int32_t>::min();
        });
        std::ranges::stable_sort(*items, [](const SearchItem& left, const SearchItem& right) {
            if (left.fuzzy_score != right.fuzzy_score) return left.fuzzy_score > right.fuzzy_score;
            if (left.name.size() != right.name.size()) return left.name.size() < right.name.size();
            return left.name < right.name;
        });
        additional_matches = items->size() > result_limit || items->size() == candidate_limit;
        if (items->size() > result_limit) items->resize(result_limit);
    }
    if (!result->stderr_text.empty()) context.ui.warning(result->stderr_text);
    if (!context.ui.human() && !context.ui.json()) {
        for (const auto& item : *items) context.ui.data_line(search_path(item));
        return 0;
    }
    if (context.ui.json()) {
        std::string json = "[\n";
        for (std::size_t index = 0; index < items->size(); ++index) {
            const auto& item = (*items)[index];
            const auto full_utf8 = search_path(item);
            json += "  {\"name\":\"" + json_escape(item.name) + "\",\"path\":\"" + json_escape(full_utf8) +
                    "\",\"directory\":\"" + json_escape(item.directory) + "\",\"mode\":\"" +
                    file_mode(item.attributes) + "\",\"size\":";
            json += item.size ? std::to_string(*item.size) : "null";
            json += ",\"modified\":";
            json += item.modified ? std::to_string(*item.modified) : "null";
            json += index + 1 == items->size() ? "}\n" : "},\n";
        }
        json += "]\n";
        context.ui.data(json);
        return 0;
    }

    if (tree) {
        terminal::Tree result_tree{win::narrow(here_path.native()).value_or(".")};
        const auto insert = [&](const auto& self, std::vector<terminal::TreeNode>& nodes,
                                const std::vector<std::filesystem::path>& parts, const std::size_t part_index,
                                const SearchItem& item,
                                const std::filesystem::path& accumulated) -> void {
            if (part_index >= parts.size()) return;
            const auto& part = parts[part_index];
            const auto leaf = part_index + 1 == parts.size();
            const auto name = win::narrow(part.native()).value_or("?");
            auto found = std::ranges::find(nodes, name, &terminal::TreeNode::name);
            if (found == nodes.end()) {
                const auto path = here_path / accumulated / part;
                nodes.push_back({name, win::narrow(path.native()).value_or(name), !leaf ||
                    (item.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
                found = std::prev(nodes.end());
            }
            if (leaf) {
                found->directory = (item.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                found->size = item.size;
                found->modified = item.modified;
            } else self(self, found->children, parts, part_index + 1, item, accumulated / part);
        };
        for (const auto& item : *items) {
            const auto full = std::filesystem::path(win::widen(item.directory).value_or(L"")) /
                              win::widen(item.name).value_or(L"");
            const auto relative = full.lexically_relative(here_path);
            if (!relative.empty() && !relative.native().starts_with(L"..")) {
                std::vector<std::filesystem::path> parts;
                for (const auto& part : relative) parts.push_back(part);
                insert(insert, result_tree.children, parts, 0, item, {});
            }
        }
        context.ui.summary(items->size(), items->size() == 1 ? "result" : "results", elapsed);
        context.ui.data_line("");
        context.ui.tree(result_tree);
        return 0;
    }

    context.ui.summary(items->size(), items->size() == 1 ? "result" : "results", elapsed);
    context.ui.data_line("");
    terminal::Table table;
    auto mode = terminal::TableColumn{"MODE", terminal::StyleToken::muted};
    mode.minimum_width = 6;
    mode.maximum_width = 6;
    mode.priority = 20;
    mode.flexible = false;

    auto name = terminal::TableColumn{"NAME", terminal::StyleToken::filename};
    name.minimum_width = 16;
    name.maximum_width = 32;

    auto path = terminal::TableColumn{"PATH", terminal::StyleToken::path};
    path.minimum_width = 48;
    path.maximum_width = 120;

    auto size = terminal::TableColumn{"SIZE", terminal::StyleToken::size};
    size.minimum_width = 8;
    size.maximum_width = 10;
    size.priority = 50;
    size.flexible = false;
    size.right_aligned = true;

    auto modified = terminal::TableColumn{"MODIFIED", terminal::StyleToken::date};
    modified.minimum_width = 12;
    modified.maximum_width = 12;
    modified.priority = 40;
    modified.flexible = false;

    table.columns = {std::move(mode), std::move(name), std::move(path),
                     std::move(size), std::move(modified)};
    table.rows.reserve(items->size());
    table.compact_primary = 1;
    for (const auto& item : *items) {
        table.rows.push_back({
            {file_mode(item.attributes), terminal::StyleToken::muted},
            {item.name, terminal::StyleToken::filename},
            {item.directory, terminal::StyleToken::path},
            {item.size ? terminal::format_filesize(*item.size) : "-", item.size ? terminal::StyleToken::size : terminal::StyleToken::muted},
            {item.modified ? terminal::format_timestamp(*item.modified) : "-", item.modified ? terminal::StyleToken::date : terminal::StyleToken::muted},
        });
    }
    context.ui.table(table);
    if (fuzzy && additional_matches) {
        context.ui.muted("Showing the best " + std::to_string(result_limit) +
                         " fuzzy matches. Use --limit <count> to change the limit.");
    } else if (visually_limited && items->size() == visual_result_limit) {
        context.ui.warning("Showing the first 100 results. Use --limit <count> to change the limit.");
    }
    return static_cast<int>(result->exit_code);
}

std::string tool_key(std::string name) {
    for (auto& character : name) {
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    }
    if (name.ends_with(".exe")) name.resize(name.size() - 4);
    return name;
}

Result<int> list_tools(cli::CommandContext& context) {
    lua::Runtime runtime;
    if (!runtime.valid()) return std::unexpected(runtime.initialization_error());
    if (auto loaded = runtime.load_user_config(); !loaded) return std::unexpected(loaded.error());
    auto configured = runtime.configured_tool_names();
    if (!configured) return std::unexpected(configured.error());
    auto defaults = tool_options("es", &runtime);
    if (!defaults) return std::unexpected(defaults.error());

    struct Entry { std::string name; std::string use; std::vector<std::filesystem::path> candidates; };
    std::vector<Entry> entries;
    const auto add = [&](const std::string& name, const std::string_view use,
                         const std::filesystem::path& candidate = {}) {
        const auto key = tool_key(name);
        const auto found = std::ranges::find_if(entries, [&](const auto& entry) { return tool_key(entry.name) == key; });
        if (found == entries.end()) entries.push_back({name, std::string(use), candidate.empty()
            ? std::vector<std::filesystem::path>{} : std::vector{candidate}});
        else {
            if (use != "repository") found->use = use;
            if (!candidate.empty()) found->candidates.push_back(candidate);
        }
    };
    for (const auto& name : *configured) add(name, "configured");
    add("es", "search");
    add("Everything", "search backend");
    add("wt", "project -g");
    if (defaults->tools_directory) {
        auto discovered = discover_tools(*defaults->tools_directory);
        if (!discovered) return std::unexpected(discovered.error());
        for (const auto& candidate : *discovered) {
            auto name = win::narrow(candidate.stem().native());
            if (!name) return std::unexpected(name.error());
            add(*name, "repository", candidate);
        }
    }
    std::ranges::sort(entries, [](const auto& left, const auto& right) { return tool_key(left.name) < tool_key(right.name); });

    if (context.ui.human()) {
        context.ui.heading("Tools", defaults->tools_directory
            ? win::narrow(defaults->tools_directory->native()).value_or("") : "PATH / config");
    }
    terminal::Table table;
    table.columns = {{"TOOL", terminal::StyleToken::command},
                     {"STATUS", terminal::StyleToken::success},
                     {"PATH", terminal::StyleToken::path},
                     {"USE", terminal::StyleToken::secondary}};
    std::string json = "[\n";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& entry = entries[index];
        auto options = tool_options(entry.name, &runtime);
        if (!options) return std::unexpected(options.error());
        auto resolved = locate_tool(entry.name, *options);
        const std::string status = resolved ? "found" : resolved.error().kind == ErrorKind::not_found ? "missing" :
            resolved.error().kind == ErrorKind::usage ? "ambiguous" : "error";
        const auto path = resolved ? win::narrow(resolved->native()).value_or("") :
            options->configured_path ? win::narrow(options->configured_path->native()).value_or("") : std::string{};
        if (context.ui.json()) {
            const auto directory = resolved ? win::narrow(resolved->parent_path().native()).value_or("") : std::string{};
            json += "  {\"name\":\"" + json_escape(entry.name) + "\",\"status\":\"" + status + "\",\"path\":" +
                (path.empty() ? "null" : "\"" + json_escape(path) + "\"") + ",\"directory\":" +
                (directory.empty() ? "null" : "\"" + json_escape(directory) + "\"") + ",\"use\":\"" +
                json_escape(entry.use) + "\",\"error\":" +
                (resolved ? "null" : "\"" + json_escape(resolved.error().message + "\n" + resolved.error().detail) + "\"") + ",\"candidates\":[";
            for (std::size_t candidate = 0; candidate < entry.candidates.size(); ++candidate) {
                if (candidate != 0) json += ",";
                json += "\"" + json_escape(win::narrow(entry.candidates[candidate].native()).value_or("")) + "\"";
            }
            json += "]}";
            json += index + 1 == entries.size() ? "\n" : ",\n";
        } else {
            const auto row = [&](const std::string& state, const std::string& location) {
                table.rows.push_back({{entry.name, terminal::StyleToken::command},
                    {state, state == "found" ? terminal::StyleToken::success : terminal::StyleToken::warning},
                    {location.empty() ? "-" : location, terminal::StyleToken::path}, {entry.use, terminal::StyleToken::secondary}});
            };
            if (status != "ambiguous" || entry.candidates.empty()) row(status, path);
            for (const auto& candidate : entry.candidates) {
                const auto display = win::narrow(candidate.native()).value_or("");
                if (resolved && _wcsicmp(resolved->c_str(), candidate.c_str()) == 0) continue;
                row(status == "ambiguous" ? status : "shadowed", display);
            }
        }
        if (!resolved) context.ui.diagnostic(resolved.error().message + "\n" + resolved.error().detail);
    }
    if (context.ui.json()) context.ui.data(json + "]\n");
    else context.ui.table(table);
    return 0;
}

Result<int> show_tool(cli::CommandContext& context, const std::span<const std::string> arguments) {
    if (arguments.empty() || (arguments.size() == 1 && arguments.front() == "list")) return list_tools(context);
    if (arguments.size() != 1) {
        return std::unexpected(Error{ErrorKind::usage, "Usage: atlas tools [list|<name>]"});
    }
    auto path = resolve_tool(arguments.front());
    if (!path) return std::unexpected(path.error());
    auto display = win::narrow(path->native());
    if (!display) return std::unexpected(display.error());
    context.ui.semantic_line(terminal::StyleToken::path, *display);
    return 0;
}

std::string lua_string(std::string value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const auto character : value) {
        if (character == '\\') escaped.push_back('/');
        else {
            if (character == '"') escaped.push_back('\\');
            escaped.push_back(character);
        }
    }
    escaped.push_back('"');
    return escaped;
}

Result<int> setup(cli::CommandContext& context, const std::span<const std::string> arguments) {
    if (arguments.size() > 1) {
        return std::unexpected(Error{ErrorKind::usage, "Usage: atlas setup [tools-directory]"});
    }
    std::string input;
    if (arguments.empty()) {
        if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_CHAR) {
            return std::unexpected(Error{ErrorKind::usage, "A tools directory is required when stdin is redirected"});
        }
        auto response = context.ui.prompt("Tools directory");
        if (!response || response->empty()) {
            return std::unexpected(Error{ErrorKind::usage, "Tools directory cannot be empty"});
        }
        input = std::move(*response);
    } else {
        input = arguments.front();
    }

    auto tools_directory = win::resolve_path(input);
    if (!tools_directory) return std::unexpected(tools_directory.error());
    std::error_code error;
    std::filesystem::create_directories(*tools_directory, error);
    if (error) return std::unexpected(Error{ErrorKind::io, "Could not create tools directory", error.message()});

    auto atlas_directory = win::user_atlas_directory();
    if (!atlas_directory) return std::unexpected(atlas_directory.error());
    std::filesystem::create_directories(*atlas_directory / L"commands", error);
    if (!error) std::filesystem::create_directories(*atlas_directory / L"plugins", error);
    if (!error) std::filesystem::create_directories(*atlas_directory / L"themes", error);
    if (!error) std::filesystem::create_directories(*atlas_directory / L"data", error);
    if (error) return std::unexpected(Error{ErrorKind::io, "Could not create ~/.atlas directories", error.message()});

    const auto config = *atlas_directory / L"config.lua";
    if (std::filesystem::exists(config, error)) {
        lua::Runtime runtime;
        if (!runtime.valid()) return std::unexpected(runtime.initialization_error());
        if (auto loaded = runtime.load_user_config(); !loaded) return std::unexpected(loaded.error());
        if (auto configured = runtime.configured_tools_directory()) {
            auto configured_path = win::resolve_path(*configured);
            if (configured_path && std::filesystem::equivalent(*configured_path, *tools_directory, error) && !error) {
                context.ui.success("Atlas is already configured");
                context.ui.semantic_line(terminal::StyleToken::path,
                    win::narrow(tools_directory->native()).value_or(input));
                return 0;
            }
        }
        return std::unexpected(Error{ErrorKind::usage,
            "~/.atlas/config.lua already exists; it was not overwritten\n"
            "Add this line manually:\n  atlas.tools_dir = " +
                lua_string(win::narrow(tools_directory->native()).value_or(input))});
    }
    if (error) return std::unexpected(Error{ErrorKind::io, "Could not inspect ~/.atlas/config.lua", error.message()});
    const auto tools_path = win::narrow(tools_directory->native()).value_or(input);
    const auto initial_config =
        "atlas.tools_dir = " + lua_string(tools_path) + "\n"
        "atlas.tools = atlas.tools or {}\n";
    if (auto saved = win::write_atomic(config, initial_config, ""); !saved) {
        return std::unexpected(saved.error());
    }
    context.ui.success("Atlas configured");
    context.ui.semantic_line(terminal::StyleToken::path,
        tools_path);
    return 0;
}

Result<std::string> active_theme_name() {
    auto directory = win::user_atlas_directory();
    if (!directory) return std::unexpected(directory.error());
    std::error_code error;
    if (!std::filesystem::exists(*directory / L"config.lua", error)) return std::string("default");
    if (error) return std::unexpected(Error{ErrorKind::io, "Could not inspect theme configuration", error.message()});
    lua::Runtime runtime;
    if (!runtime.valid()) return std::unexpected(runtime.initialization_error());
    if (auto loaded = runtime.load_user_config(); !loaded) return std::unexpected(loaded.error());
    auto configured = runtime.configured_theme();
    return configured ? *configured : std::string("default");
}

Result<terminal::ThemeLoad> named_theme(const std::string_view name) {
    if (name == "default") return terminal::ThemeLoad{terminal::default_theme(), {}};
    if (!terminal::valid_theme_name(name)) {
        return std::unexpected(Error{ErrorKind::usage, "Invalid theme name: " + std::string(name)});
    }
    auto file = terminal::find_theme_file(name);
    if (!file) return std::unexpected(file.error());
    return terminal::load_theme_file(*file, terminal::default_theme());
}

bool theme_assignment(const std::string_view line) {
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos || !line.substr(first).starts_with("atlas.theme")) return false;
    auto rest = line.substr(first + std::string_view("atlas.theme").size());
    const auto non_space = rest.find_first_not_of(" \t");
    return non_space != std::string_view::npos && rest[non_space] == '=';
}

Status persist_theme(const std::string_view name) {
    auto directory = win::user_atlas_directory();
    if (!directory) return std::unexpected(directory.error());
    std::error_code error;
    std::filesystem::create_directories(*directory / L"themes", error);
    if (error) return std::unexpected(Error{ErrorKind::io, "Could not create themes directory", error.message()});
    const auto config = *directory / L"config.lua";
    std::string source;
    if (std::filesystem::exists(config, error)) {
        std::ifstream input(config, std::ios::binary);
        if (!input) return std::unexpected(Error{ErrorKind::io, "Could not read ~/.atlas/config.lua"});
        source.assign(std::istreambuf_iterator<char>(input), {});
    }
    std::string updated;
    bool replaced = false;
    for (std::size_t start = 0; start < source.size();) {
        const auto end = source.find('\n', start);
        const auto line = source.substr(start, end == std::string::npos ? source.size() - start : end - start);
        if (theme_assignment(line)) {
            if (!replaced) updated += "atlas.theme = " + lua_string(std::string(name)) + "\n";
            replaced = true;
        } else {
            updated += line;
            updated.push_back('\n');
        }
        start = end == std::string::npos ? source.size() : end + 1;
    }
    if (!replaced) updated += "atlas.theme = " + lua_string(std::string(name)) + "\n";
    return win::write_atomic(config, updated, source);
}

Result<int> theme_command(cli::CommandContext& context, const std::span<const std::string> arguments) {
    const auto action = arguments.empty() ? std::string_view{} : std::string_view(arguments[0]);
    if (action.empty() || action == "list") {
        if (action == "list" && arguments.size() != 1) {
            return std::unexpected(Error{ErrorKind::usage, "Usage: atlas theme list"});
        }
        auto active = active_theme_name();
        if (!active) {
            context.ui.warning("Could not read the selected theme; showing available themes with the default fallback");
            active = std::string("default");
        }
        auto names = terminal::available_themes();
        if (!names) return std::unexpected(names.error());
        if (context.ui.json()) {
            std::string json = "[";
            for (const auto& name : *names) {
                if (json.size() > 1) json += ',';
                json += "{\"name\":\"" + json_escape(name) + "\",\"active\":" + (name == *active ? "true" : "false") + "}";
            }
            context.ui.data_line(json + "]");
            return 0;
        }
        context.ui.heading("Themes");
        terminal::Table table;
        table.columns = {{"THEME", terminal::StyleToken::primary},
                         {"STATUS", terminal::StyleToken::success}};
        for (const auto& name : *names) table.rows.push_back({{name, terminal::StyleToken::primary},
                                                             {name == *active ? "active" : "", terminal::StyleToken::success}});
        context.ui.table(table);
        return 0;
    }
    if (action == "set") {
        if (arguments.size() != 2) return std::unexpected(Error{ErrorKind::usage, "Usage: atlas theme set <name>"});
        auto loaded = named_theme(arguments[1]);
        if (!loaded) return std::unexpected(loaded.error());
        if (auto saved = persist_theme(arguments[1]); !saved) return std::unexpected(saved.error());
        for (const auto& warning : loaded->warnings) context.ui.warning(warning);
        context.ui.success("Theme set to '" + arguments[1] + "'");
        return 0;
    }
    if (action == "preview") {
        if (arguments.size() > 2) return std::unexpected(Error{ErrorKind::usage, "Usage: atlas theme preview [name]"});
        auto name = arguments.size() == 2 ? Result<std::string>(arguments[1]) : active_theme_name();
        if (!name) return std::unexpected(name.error());
        auto loaded = named_theme(*name);
        if (!loaded) return std::unexpected(loaded.error());
        if (context.ui.theme().user_art_override) loaded->theme.art = context.ui.theme().art;
        context.ui.set_theme(std::move(loaded->theme));
        for (const auto& warning : loaded->warnings) context.ui.warning(warning);
        terminal::show_theme_preview(context.ui);
        return 0;
    }
    if (action == "reload" && arguments.size() == 1) {
        context.ui.success("Theme reloaded for this invocation");
        return 0;
    }
    return std::unexpected(Error{ErrorKind::usage, "Usage: atlas theme [list|set <name>|preview [name]|reload]"});
}

} // namespace

Status register_builtins(cli::Registry& registry) {
    if (auto status = register_completion_command(registry); !status) return status;
    if (auto status = registry.add({
            "run",
            "Run a Lua script with the Atlas API available",
            "atlas run <file.lua>",
            {},
            run_script,
            {
                "Executes the file in a fresh Lua 5.4 runtime and reports syntax or runtime errors safely.",
                {{"<file.lua>", "Path to the Lua script; relative paths, spaces, Unicode, and ~ are supported.", "file"}},
                {},
                {
                    {"atlas run hello.lua", "Run hello.lua from the current directory."},
                    {"atlas run \"D:\\My Scripts\\cleanup.lua\"", "Run a script whose path contains spaces."},
                },
                "Automation",
            }}); !status) return status;
    if (auto status = registry.add({
            "ls",
            "List the contents of a directory with useful metadata",
            "atlas ls [path] [--all] [--dirs-only] [--limit <count>]",
            {"dir"},
            list_directory,
            {
                "Lists one directory without starting Everything. Rich terminals get an adaptive table; pipes receive full paths.",
                {{"[path]", "Directory to list; defaults to the current directory.", "directory"}},
                {{"-a, --all", "Include hidden/system and dot-prefixed entries."},
                 {"-D, --dirs-only", "Show directories only."},
                 {"-f, --files-only", "Show files only."},
                 {"-n, --limit <count>", "Stop after this many entries (default: 2,000)."}},
                {{"atlas ls", "List the current directory."}, {"atlas ls D:\\Dev --all", "Include hidden entries in D:\\Dev."},
                 {"atlas ls --plain | fzf", "Pipe complete paths into another tool."}},
                "Navigation",
            }}); !status) return status;
    if (auto status = registry.add({
            "tree",
            "Explore a directory as a compact recursive tree",
            "atlas tree [path] [--depth <levels>] [--all] [--dirs-only] [--limit <count>]",
            {},
            show_tree,
            {
                "Builds a bounded filesystem tree without following reparse-point directories. Defaults to three levels and 2,000 entries.",
                {{"[path]", "Root directory; defaults to the current directory.", "directory"}},
                {{"-d, --depth <levels>", "Maximum recursion depth from 0 through 32 (default: 3)."},
                 {"-a, --all", "Include hidden/system and dot-prefixed entries."},
                 {"-D, --dirs-only", "Show directory branches only."},
                 {"-f, --files-only", "Show files while retaining their directory branches."},
                 {"-n, --limit <count>", "Bound the total entries (default: 2,000)."}},
                {{"atlas tree", "Explore the current directory."}, {"atlas tree src --depth 2", "Show two levels below src."},
                 {"atlas search --here", "Shortcut to browse the current directory as a tree."}},
                "Navigation",
            }}); !status) return status;
    if (auto status = registry.add({
            "open",
            "Open a file, folder, or URL in its default Windows application",
            "atlas open <path-or-url>",
            {},
            open_target,
            {
                "Uses Windows Shell, like double-clicking the target in File Explorer.",
                {{"<path-or-url>", "File, directory, or web address to open. Relative paths use the current directory.", "path"}},
                {},
                {
                    {"atlas open .", "Open the current directory in File Explorer."},
                    {"atlas open \"D:\\Dev\\Waluna\"", "Open a directory, including paths with spaces."},
                    {"atlas open README.md", "Open a file with its associated application."},
                    {"atlas open https://example.com", "Open a website in the default browser."},
                },
                "Navigation",
            }}); !status) return status;
    if (auto status = registry.add({
            "search",
            "Find indexed files and folders instantly through Everything",
            "atlas search [<query>] [options] [es-options]",
            {"find"},
            search,
            {
                "With a simple query, asks Everything for fuzzy candidates and ranks the best filename matches locally. "
                "Everything expressions such as *.cpp and ext:cpp retain their native behavior. Without a query, explores the current directory as a bounded tree.\n"
                "If the Everything application is closed, Atlas starts it hidden on demand. Use --status to inspect every search component.",
                {
                    {"[<query>]", "Everything search expression to find; omit it to browse the current directory."},
                    {"[es-options]", "Additional Everything CLI options forwarded unchanged to es.exe."},
                },
                {
                    {"-h, --here", "Restrict a query to the current directory; without a query, browse it."},
                    {"-t, --tree", "Render local query results as a directory tree."},
                    {"-F, --fuzzy", "Force fuzzy subsequence matching and relevance ranking for a literal query."},
                    {"-e, --exact", "Disable automatic fuzzy matching and use the native Everything expression unchanged."},
                    {"-f, --files", "Return files only."},
                    {"-D, --dirs", "Return directories only."},
                    {"-n, --limit <count>", "Limit results or local tree entries; -n <count> is the compact form. Rich search defaults to 100."},
                    {"-d, --depth <levels>", "Set local browsing depth when no query is supplied."},
                    {"-a, --all", "Include hidden and system entries during local browsing. Boolean short flags may be grouped, for example -hf."},
                    {"-path <directory>", "Restrict the search to a directory."},
                    {"-sort <field>", "Sort by name, path, size, date-modified, or another ES field."},
                    {"-s, --status", "Show ES, Everything application, service, and index health."},
                    {"--plain", "Print complete paths without the human table."},
                    {"--json", "Return structured name, path, mode, size, and modified fields."},
                    {"--raw", "Forward the original es.exe output without parsing or decoration."},
                },
                {
                    {"atlas search naruto", "Search all indexed files and folders for naruto."},
                    {"atlas search wlna", "Fuzzy-match names such as Waluna and rank the closest results first."},
                    {"atlas search main --exact", "Use main as an unchanged Everything substring query."},
                    {"atlas search", "Explore the current directory as a compact tree."},
                    {"atlas search waluna --here", "Search only below the current directory."},
                    {"atlas search main --here --tree", "Show matching local paths as a tree."},
                    {"atlas search \"*.cpp\" -n 20", "Return at most 20 C++ source files."},
                    {"atlas search waluna /ad", "Return only matching directories."},
                    {"atlas search \"*.cpp\" --plain | fzf", "Pipe complete paths into another terminal tool."},
                },
                "Discovery", {"Everything / ES"},
            }}); !status) return status;
    if (auto status = registry.add({
            "tools",
            "List external tools, availability, and executable paths",
            "atlas tools [list|<name>]",
            {"tool"},
            show_tool,
            {
                "Lists command dependencies, atlas.tools entries, and executables in the tools directory (root plus one folder level).\n"
                "Checks explicit configuration, PATH, and the tools repository. Listed repository files are discoverable tools, not new Atlas commands.",
                {{"[name]", "Omit to list all tools; provide an executable name to print its exact full path.", "tools"}},
                {{"--json", "Include full paths, directories, status, use, and resolution errors."},
                 {"--plain", "Print name, status, full path, and use as tab-separated data."}},
                {
                    {"atlas tools", "List all integrated, configured, and repository tools."},
                    {"atlas tool es", "Show the es.exe used by the search command."},
                    {"atlas tool ffmpeg", "Verify that Atlas can locate ffmpeg.exe."},
                },
                "Configuration",
            }}); !status) return status;
    if (auto status = registry.add({
            "setup",
            "Create the Atlas user directories and configure where external tools live",
            "atlas setup [tools-directory]",
            {},
            setup,
            {
                "Creates ~/.atlas and saves atlas.tools_dir in config.lua without overwriting existing configuration.",
                {{"[tools-directory]", "Root directory containing tools directly or in one nested folder; prompted when omitted.", "directory"}},
                {},
                {
                    {"atlas setup D:\\Tools", "Use D:\\Tools as the external tools repository."},
                    {"atlas setup", "Choose the tools directory interactively."},
                },
                "Configuration",
            }}); !status) return status;
    return registry.add({
        "theme",
        "Inspect, select, and preview Atlas terminal themes",
        "atlas theme [list|set <name>|preview [name]|reload]",
        {},
        theme_command,
        {
            "Themes are Lua files in ~/.atlas/themes and apply to both C++ and Lua commands.",
            {{"[action]", "Omit to list every available theme, or use set, preview, or reload."}},
            {
                {"list", "List installed themes and identify the active one."},
                {"set <name>", "Persist a theme selection in ~/.atlas/config.lua."},
                {"preview [name]", "Render every semantic style without changing the saved theme."},
                {"reload", "Reload the current theme for this Atlas invocation."},
            },
            {
                {"atlas theme", "List every available theme and mark the active one."},
                {"atlas theme list", "List the default theme and user theme files."},
                {"atlas theme set monokai", "Make monokai the active theme."},
                {"atlas theme preview gruvbox", "Preview gruvbox without selecting it."},
            },
            "Configuration", {},
            {
                {"list", "List installed themes"},
                {"set", "Change the active theme", {}, {{"<name>", "Theme to activate", "themes"}}},
                {"preview", "Preview a theme", {}, {{"[name]", "Theme to preview", "themes"}}},
                {"reload", "Reload the active theme"},
            },
        }});
}

} // namespace atlas::commands
