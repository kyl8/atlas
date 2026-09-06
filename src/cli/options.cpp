#include "atlas/cli/options.hpp"
#include "atlas/cli/parser.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace atlas::cli {
namespace {

std::vector<std::string> names(const std::string_view label) {
    std::vector<std::string> result;
    std::string current;
    for (const char character : label) {
        if (character == ',' || std::isspace(static_cast<unsigned char>(character)) ||
            character == '<' || character == '[') {
            if (current.starts_with('-')) result.push_back(std::move(current));
            current.clear();
            if (character == '<' || character == '[') break;
        } else current.push_back(character);
    }
    if (current.starts_with('-')) result.push_back(std::move(current));
    return result;
}

const GlobalOptionSpec* global(const std::string_view name) {
    for (const auto& option : global_options()) {
        if (name == option.long_name || (!option.short_name.empty() && name == option.short_name)) return &option;
    }
    return nullptr;
}

std::string generated_short(const std::string_view name, std::set<char>& used) {
    if (!name.starts_with('-') || name.size() <= 2) return {};
    const auto first = name.find_first_not_of('-');
    if (first == std::string_view::npos) return {};
    const auto claim = [&](const char character) -> std::string {
        if (!std::isalnum(static_cast<unsigned char>(character)) || used.contains(character)) return {};
        used.insert(character);
        return std::string{"-"} + character;
    };
    for (std::size_t index = first; index < name.size(); ++index) {
        if (auto result = claim(static_cast<char>(std::tolower(static_cast<unsigned char>(name[index])))); !result.empty())
            return result;
    }
    for (std::size_t index = first; index < name.size(); ++index) {
        if (auto result = claim(static_cast<char>(std::toupper(static_cast<unsigned char>(name[index])))); !result.empty())
            return result;
    }
    constexpr std::string_view fallbacks = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (const char character : fallbacks) if (auto result = claim(character); !result.empty()) return result;
    return {};
}

const OptionSpec* find(const std::span<const OptionSpec> options, const std::string_view token) {
    const auto separator = token.find('=');
    const auto key = token.substr(0, separator);
    for (const auto& option : options) {
        if (std::ranges::find(option.names, key) != option.names.end()) return &option;
    }
    return nullptr;
}

const SubcommandHelp* subcommand(const Command& command, const std::span<const std::string> arguments) {
    for (const auto& argument : arguments) {
        if (argument.starts_with('-')) continue;
        for (const auto& child : command.help.subcommands) {
            if (argument == child.name || std::ranges::find(child.aliases, argument) != child.aliases.end()) return &child;
        }
        break;
    }
    return nullptr;
}

std::set<char> reserved_shorts() {
    std::set<char> used;
    for (const auto& option : global_options()) if (option.short_name.size() == 2) {
        used.insert(option.short_name[1]);
    }
    return used;
}

void prepare(std::vector<HelpEntry>& entries, std::set<char>& used) {
    for (const auto& entry : entries) for (const auto& name : names(entry.label)) {
        if (name.size() == 2 && name.front() == '-') used.insert(name[1]);
    }
    for (auto& entry : entries) {
        auto parsed = names(entry.label);
        if (parsed.empty() || std::ranges::any_of(parsed, [](const auto& name) { return name.size() == 2; })) continue;
        auto long_name = std::ranges::find_if(parsed, [](const auto& name) { return name.starts_with("--"); });
        if (long_name == parsed.end()) {
            long_name = std::ranges::find_if(parsed, [](const auto& name) { return name.size() > 2 && name.starts_with('-'); });
        }
        if (long_name == parsed.end()) continue;
        if (const auto* global_option = global(*long_name)) {
            if (!global_option->short_name.empty()) entry.label = std::string(global_option->short_name) + ", " + entry.label;
            continue;
        }
        auto shorthand = generated_short(*long_name, used);
        if (!shorthand.empty()) entry.label = shorthand + ", " + entry.label;
    }
}

Status validate_scope(const std::string_view command_name,
                      const std::span<const HelpEntry> parent,
                      const std::span<const HelpEntry> local) {
    std::map<std::string, std::string, std::less<>> owners;
    const auto validate = [&](const std::span<const HelpEntry> entries) -> Status {
        for (const auto& entry : entries) {
            const auto parsed = names(entry.label);
            if (parsed.empty()) continue;
            const auto long_name = std::ranges::find_if(parsed, [](const auto& name) { return name.starts_with("--"); });
            const std::string canonical = long_name == parsed.end() ? parsed.front() : *long_name;
            for (const auto& name : parsed) {
                if (name.size() == 2) {
                    if (const auto* global_option = global(name);
                        global_option && std::ranges::find(parsed, global_option->long_name) == parsed.end()) {
                        return std::unexpected(Error{
                            ErrorKind::usage,
                            "Option " + name + " in command '" + std::string(command_name) +
                                "' conflicts with global " + std::string(global_option->long_name)});
                    }
                }
                const auto [existing, inserted] = owners.emplace(name, canonical);
                if (!inserted && existing->second != canonical) {
                    return std::unexpected(Error{
                        ErrorKind::usage,
                        "Option " + name + " is assigned more than once in command '" +
                            std::string(command_name) + "'"});
                }
            }
        }
        return {};
    };
    if (auto status = validate(parent); !status) return status;
    return validate(local);
}

} // namespace

std::vector<OptionSpec> option_specs(const std::span<const HelpEntry> entries, const bool automatic_short) {
    std::vector<HelpEntry> prepared(entries.begin(), entries.end());
    if (automatic_short) {
        auto used = reserved_shorts();
        prepare(prepared, used);
    }
    std::vector<OptionSpec> result;
    for (const auto& entry : prepared) {
        auto option_names = names(entry.label);
        if (option_names.empty()) continue;
        OptionSpec option;
        option.names = std::move(option_names);
        option.description = entry.description;
        option.completion = entry.completion;
        option.values = entry.values;
        option.takes_value = entry.label.find('<') != std::string::npos;
        const auto long_name = std::ranges::find_if(option.names, [](const auto& name) { return name.starts_with("--"); });
        if (long_name != option.names.end()) option.canonical = *long_name;
        else option.canonical = *std::ranges::max_element(option.names, {}, &std::string::size);
        result.push_back(std::move(option));
    }
    return result;
}

std::vector<std::string> normalize_options(const Command& command, const std::span<const std::string> arguments) {
    auto options = option_specs(command.help.options);
    if (const auto* child = subcommand(command, arguments)) {
        auto child_options = option_specs(child->options);
        options.insert(options.end(), child_options.begin(), child_options.end());
    }
    std::vector<std::string> result;
    result.reserve(arguments.size());
    bool parse = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto& argument = arguments[index];
        if (!parse || argument == "-") { result.push_back(argument); continue; }
        if (argument == "--") { parse = false; result.push_back(argument); continue; }
        if (const auto* option = find(options, argument)) {
            const auto separator = argument.find('=');
            result.push_back(separator == std::string::npos ? option->canonical :
                option->canonical + argument.substr(separator));
            continue;
        }
        if (argument.size() <= 2 || argument.front() != '-' || argument[1] == '-') {
            result.push_back(argument);
            continue;
        }
        std::vector<std::string> expanded;
        bool recognized = true;
        for (std::size_t position = 1; position < argument.size(); ++position) {
            const std::string shorthand{'-', argument[position]};
            const auto* option = find(options, shorthand);
            if (!option) { recognized = false; break; }
            expanded.push_back(option->canonical);
            if (option->takes_value) {
                if (position + 1 < argument.size()) expanded.push_back(argument.substr(position + 1));
                position = argument.size();
            }
        }
        if (recognized) result.insert(result.end(), expanded.begin(), expanded.end());
        else result.push_back(argument);
    }
    return result;
}

Status prepare_option_metadata(Command& command) {
    auto root_used = reserved_shorts();
    prepare(command.help.options, root_used);
    if (auto status = validate_scope(command.name, {}, command.help.options); !status) return status;

    for (auto& child : command.help.subcommands) {
        auto child_used = root_used;
        prepare(child.options, child_used);
        if (auto status = validate_scope(command.name, command.help.options, child.options); !status) return status;
    }
    const auto require_short = [&](const std::span<const HelpEntry> entries) -> Status {
        for (const auto& entry : entries) {
            const auto parsed = names(entry.label);
            if (parsed.empty() || std::ranges::any_of(parsed, [](const auto& name) { return name.size() == 2; })) continue;
            return std::unexpected(Error{ErrorKind::usage,
                "No short option is available for " + parsed.front() + " in command '" + command.name + "'"});
        }
        return {};
    };
    if (auto status = require_short(command.help.options); !status) return status;
    for (const auto& child : command.help.subcommands) {
        if (auto status = require_short(child.options); !status) return status;
    }
    return {};
}

} // namespace atlas::cli
