#include "atlas/completion.hpp"

#include "atlas/cli/parser.hpp"
#include "atlas/cli/options.hpp"
#include "atlas/fuzzy.hpp"
#include "atlas/platform/utf.hpp"
#include "atlas/terminal/theme.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>

namespace atlas::completion {
namespace {

struct PartialLine {
    std::vector<std::string> completed;
    std::string current;
};

using Option = cli::OptionSpec;

std::string lower(std::string_view value) {
    std::string result(value);
    for (auto& character : result) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return result;
}

bool equal(const std::string_view left, const std::string_view right) { return lower(left) == lower(right); }

PartialLine tokenize(const std::string_view line) {
    std::vector<std::string> tokens;
    std::string token;
    char quote{};
    bool escaped = false;
    bool separated = true;
    for (const char character : line) {
        if (escaped) { token.push_back(character); escaped = false; separated = false; continue; }
        if (character == '`') { escaped = true; separated = false; continue; }
        if (quote != 0) {
            if (character == quote) quote = 0;
            else token.push_back(character);
            separated = false;
            continue;
        }
        if (character == '\'' || character == '"') { quote = character; separated = false; continue; }
        if (std::isspace(static_cast<unsigned char>(character))) {
            if (!separated) { tokens.push_back(std::move(token)); token.clear(); }
            separated = true;
            continue;
        }
        token.push_back(character);
        separated = false;
    }
    if (escaped) token.push_back('`');
    PartialLine result;
    if (separated) result.completed = std::move(tokens);
    else {
        result.completed = std::move(tokens);
        result.current = std::move(token);
    }
    if (!result.completed.empty()) {
        auto executable = lower(std::filesystem::path(result.completed.front()).filename().string());
        if (executable == "atlas" || executable == "atlas.exe") result.completed.erase(result.completed.begin());
    } else if (!result.current.empty()) {
        auto executable = lower(std::filesystem::path(result.current).filename().string());
        if (executable == "atlas" || executable == "atlas.exe") result.current.clear();
    }
    return result;
}

std::optional<int> match_cost(const std::string_view candidate, const std::string_view query,
                              const bool case_sensitive = false) {
    if (query.empty()) return 50;
    const auto matched = fuzzy::score(query, candidate, case_sensitive);
    if (!matched) return std::nullopt;
    return -*matched;
}

void candidate(std::vector<Item>& output, Item item, const std::string_view query) {
    const auto score = match_cost(item.value, query, item.kind == Kind::option);
    if (!score) return;
    item.score += *score;
    output.push_back(std::move(item));
}

std::vector<Option> global_options() {
    std::vector<Option> result;
    for (const auto& spec : cli::global_options()) {
        Option option;
        option.names.emplace_back(spec.long_name);
        if (!spec.short_name.empty()) option.names.emplace_back(spec.short_name);
        option.description = spec.description;
        option.canonical = spec.long_name;
        option.takes_value = !spec.value_name.empty();
        for (const auto value : spec.values) option.values.emplace_back(value, std::string{});
        result.push_back(std::move(option));
    }
    return result;
}

const Option* find_option(const std::vector<Option>& values, const std::string_view name) {
    const auto separator = name.find('=');
    const auto key = name.substr(0, separator);
    for (const auto& option : values) {
        if (std::ranges::find(option.names, key) != option.names.end()) return &option;
    }
    return nullptr;
}

const cli::Command* find_command(const cli::Registry& registry, const std::string_view name) {
    for (const auto& command : registry.commands()) {
        if (equal(command.name, name) || std::ranges::any_of(command.aliases,
                [&](const auto& alias) { return equal(alias, name); })) return &command;
    }
    return nullptr;
}

const cli::SubcommandHelp* find_subcommand(const cli::Command& command, const std::string_view name) {
    for (const auto& child : command.help.subcommands) {
        if (equal(child.name, name) || std::ranges::any_of(child.aliases,
                [&](const auto& alias) { return equal(alias, name); })) return &child;
    }
    return nullptr;
}

Result<std::vector<Item>> paths(const std::string_view current, const std::string_view type) {
    auto wide = win::widen(current);
    if (!wide) return std::unexpected(wide.error());
    std::filesystem::path typed(*wide);
    auto directory = typed.parent_path();
    const auto prefix = typed.filename().native();
    if (directory.empty()) directory = L".";
    std::error_code error;
    std::vector<Item> result;
    for (std::filesystem::directory_iterator iterator(directory,
             std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && iterator != end && result.size() < 500; iterator.increment(error)) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().native();
        if (!prefix.empty() && _wcsnicmp(name.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
        const auto directory_entry = entry.is_directory(error);
        if (error) { error.clear(); continue; }
        if (type == "directory" && !directory_entry) continue;
        if (type == "file" && directory_entry) continue;
        auto completion = typed.parent_path() / entry.path().filename();
        auto value = win::narrow(completion.native());
        if (!value) continue;
        if (directory_entry) value->push_back('\\');
        result.push_back({std::move(*value), directory_entry ? "Directory" : "File",
                          directory_entry ? Kind::directory : Kind::file});
    }
    return result;
}

void add_values(std::vector<Item>& result, const Option& option, const std::string_view query,
                const std::string_view equal_prefix = {}) {
    for (const auto& [value, description] : option.values) {
        Item item{std::string(equal_prefix) + value, description, Kind::value};
        candidate(result, std::move(item), equal_prefix.empty() ? query : std::string(equal_prefix) + std::string(query));
    }
}

Result<std::vector<Item>> provider(const std::string_view name, const std::string_view current,
                                   const std::span<const std::string> completed,
                                   const DynamicProvider& dynamic, const bool allow_paths) {
    if (name == "path" || name == "file" || name == "directory") {
        if (!allow_paths) return std::vector<Item>{};
        return paths(current, name);
    }
    if (name == "themes") {
        auto names = terminal::available_themes();
        if (!names) return std::unexpected(names.error());
        std::vector<Item> result;
        for (auto& value : *names) {
            std::string description = "Compiled default theme";
            if (value != "default") {
                auto file = terminal::find_theme_file(value);
                description = file ? win::narrow(file->native()).value_or("Theme") : "Theme";
            }
            result.push_back({std::move(value), std::move(description), Kind::theme});
        }
        return result;
    }
    if (dynamic) return dynamic(name, current, completed);
    return std::vector<Item>{};
}

void sort_and_limit(std::vector<Item>& result, const std::size_t limit) {
    std::ranges::sort(result, [](const Item& left, const Item& right) {
        if (left.score != right.score) return left.score < right.score;
        const auto left_folded = lower(left.value);
        const auto right_folded = lower(right.value);
        return left_folded == right_folded ? left.value < right.value : left_folded < right_folded;
    });
    std::set<std::string> seen;
    std::erase_if(result, [&](const Item& item) {
        const auto key = item.kind == Kind::option ? item.value : lower(item.value);
        return !seen.insert(key).second;
    });
    if (!result.empty() && result.front().score < 30) {
        std::erase_if(result, [](const Item& item) { return item.score >= 30; });
    }
    if (result.size() > limit) result.resize(limit);
}

} // namespace

Result<std::vector<Item>> complete(const cli::Registry& registry, const Request& request,
                                   const DynamicProvider& dynamic) {
    auto input = tokenize(request.line);
    std::vector<Item> result;
    const auto globals = global_options();

    std::size_t command_index = 0;
    while (command_index < input.completed.size()) {
        const auto* global = find_option(globals, input.completed[command_index]);
        if (!global) break;
        if (global->takes_value) {
            if (command_index + 1 == input.completed.size()) {
                add_values(result, *global, input.current);
                sort_and_limit(result, request.limit);
                return result;
            }
            command_index += 2;
        } else ++command_index;
    }

    if (command_index == input.completed.size()) {
        const auto equal_at = input.current.find('=');
        if (equal_at != std::string::npos) {
            if (const auto* global = find_option(globals, input.current); global && global->takes_value) {
                const auto prefix = input.current.substr(0, equal_at + 1);
                add_values(result, *global, input.current.substr(equal_at + 1), prefix);
                sort_and_limit(result, request.limit);
                return result;
            }
        }
        if (input.current.starts_with('-')) {
            for (const auto& option : globals) for (const auto& name : option.names)
                candidate(result, {name, option.description, Kind::option}, input.current);
        } else {
            for (const auto& command : registry.commands()) {
                candidate(result, {command.name, command.description, Kind::command}, input.current);
                for (const auto& alias : command.aliases)
                    candidate(result, {alias, "Alias for " + command.name, Kind::alias, 20}, input.current);
            }
        }
        sort_and_limit(result, request.limit);
        return result;
    }

    const auto* command = find_command(registry, input.completed[command_index]);
    if (!command) return result;
    std::vector<std::string> completed(input.completed.begin() + static_cast<std::ptrdiff_t>(command_index + 1),
                                       input.completed.end());
    auto command_options = cli::option_specs(command->help.options);
    const cli::SubcommandHelp* active_subcommand = completed.empty() ? nullptr : find_subcommand(*command, completed.front());
    if (active_subcommand) {
        auto child_options = cli::option_specs(active_subcommand->options);
        command_options.insert(command_options.end(), child_options.begin(), child_options.end());
    }
    command_options.insert(command_options.end(), globals.begin(), globals.end());

    const Option* waiting = nullptr;
    if (!completed.empty()) waiting = find_option(command_options, completed.back());
    const auto equal_at = input.current.find('=');
    if (equal_at != std::string::npos) {
        if (const auto* option = find_option(command_options, input.current); option && option->takes_value) {
            const auto prefix = input.current.substr(0, equal_at + 1);
            add_values(result, *option, input.current.substr(equal_at + 1), prefix);
            if (!option->completion.empty()) {
                auto values = provider(option->completion, input.current.substr(equal_at + 1), completed,
                                       dynamic, request.allow_paths);
                if (values) for (auto& item : *values) {
                    item.value = std::string(prefix) + item.value;
                    candidate(result, std::move(item), input.current);
                }
            }
            sort_and_limit(result, request.limit);
            return result;
        }
    }
    if (waiting && waiting->takes_value) {
        add_values(result, *waiting, input.current);
        if (!waiting->completion.empty()) {
            auto values = provider(waiting->completion, input.current, completed, dynamic, request.allow_paths);
            if (values) for (auto& item : *values) candidate(result, std::move(item), input.current);
        }
        sort_and_limit(result, request.limit);
        return result;
    }

    if (input.current.starts_with('-')) {
        std::set<std::string> used;
        for (const auto& token : completed) if (token.starts_with('-')) {
            if (const auto* option = find_option(command_options, token)) {
                for (const auto& name : option->names) used.insert(name);
            } else used.insert(token.substr(0, token.find('=')));
        }
        for (const auto& option : command_options) for (const auto& name : option.names) {
            if (used.contains(name)) continue;
            candidate(result, {name, option.description, Kind::option}, input.current);
        }
        sort_and_limit(result, request.limit);
        return result;
    }

    std::vector<std::string> positional;
    for (std::size_t index = 0; index < completed.size(); ++index) {
        if (const auto* option = find_option(command_options, completed[index])) {
            if (option->takes_value && index + 1 < completed.size()) ++index;
            continue;
        }
        positional.push_back(completed[index]);
    }
    const cli::SubcommandHelp* subcommand = positional.empty() ? nullptr : find_subcommand(*command, positional.front());
    if (!subcommand && positional.empty()) {
        for (const auto& child : command->help.subcommands) {
            candidate(result, {child.name, child.description, Kind::subcommand}, input.current);
            for (const auto& alias : child.aliases)
                candidate(result, {alias, "Alias for " + child.name, Kind::alias, 20}, input.current);
        }
    }
    const auto& arguments = subcommand ? subcommand->arguments : command->help.arguments;
    const auto argument_index = positional.size() - (subcommand ? 1u : 0u);
    if (argument_index < arguments.size()) {
        const auto& argument = arguments[argument_index];
        for (const auto& [value, description] : argument.values)
            candidate(result, {value, description, Kind::value}, input.current);
        if (!argument.completion.empty()) {
            auto values = provider(argument.completion, input.current, completed, dynamic, request.allow_paths);
            if (values) for (auto& item : *values) candidate(result, std::move(item), input.current);
        }
    }
    sort_and_limit(result, request.limit);
    return result;
}

std::string_view kind_name(const Kind kind) noexcept {
    switch (kind) {
        case Kind::command: return "command"; case Kind::alias: return "alias";
        case Kind::subcommand: return "subcommand"; case Kind::option: return "option";
        case Kind::value: return "value"; case Kind::path: return "path"; case Kind::file: return "file";
        case Kind::directory: return "directory"; case Kind::project: return "project";
        case Kind::theme: return "theme"; case Kind::tool: return "tool";
    }
    return "value";
}

std::string json(const std::span<const Item> items) {
    const auto escape = [](const std::string_view text) {
        std::string output;
        for (const unsigned char character : text) {
            switch (character) {
                case '"': output += "\\\""; break; case '\\': output += "\\\\"; break;
                case '\b': output += "\\b"; break; case '\f': output += "\\f"; break;
                case '\n': output += "\\n"; break; case '\r': output += "\\r"; break;
                case '\t': output += "\\t"; break;
                default:
                    if (character < 0x20) {
                        constexpr char hex[] = "0123456789abcdef";
                        output += "\\u00"; output += hex[character >> 4]; output += hex[character & 15];
                    } else output.push_back(static_cast<char>(character));
            }
        }
        return output;
    };
    std::string output = "[";
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index) output.push_back(',');
        output += "{\"value\":\"" + escape(items[index].value) + "\",\"description\":\"" +
            escape(items[index].description) + "\",\"kind\":\"" + std::string(kind_name(items[index].kind)) + "\"}";
    }
    return output + "]\n";
}

} // namespace atlas::completion
