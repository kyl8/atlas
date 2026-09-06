#include "atlas/cli/parser.hpp"

#include <array>
#include <algorithm>

namespace atlas::cli {
namespace {

constexpr std::array color_values{std::string_view{"auto"}, std::string_view{"always"}, std::string_view{"never"}};
constexpr std::array<GlobalOptionSpec, 9> option_specs{{
    {GlobalOptionId::help, "--help", "-?", "Show global or command-specific help", {}, {}},
    {GlobalOptionId::version, "--version", "-V", "Show the installed Atlas version", {}, {}},
    {GlobalOptionId::color, "--color", "-c", "Set colors to auto, always, or never", "mode", color_values},
    {GlobalOptionId::plain, "--plain", "-p", "Emit simple output suitable for scripts and pipes", {}, {}},
    {GlobalOptionId::json, "--json", "-j", "Emit structured JSON when supported", {}, {}},
    {GlobalOptionId::raw, "--raw", "-r", "Forward the underlying tool output without formatting", {}, {}},
    {GlobalOptionId::quiet, "--quiet", "-q", "Suppress non-essential status output", {}, {}},
    {GlobalOptionId::verbose, "--verbose", "-v", "Show additional diagnostics on stderr", {}, {}},
    {GlobalOptionId::debug, "--debug", "-x", "Show technical error details", {}, {}},
}};

const GlobalOptionSpec* option(const std::string_view argument) {
    const auto separator = argument.find('=');
    const auto name = argument.substr(0, separator);
    for (const auto& candidate : option_specs) {
        if (name == candidate.long_name || (!candidate.short_name.empty() && name == candidate.short_name)) return &candidate;
    }
    return nullptr;
}

} // namespace

std::span<const GlobalOptionSpec> global_options() noexcept { return option_specs; }

Result<Invocation> parse(const std::span<const std::string_view> arguments) {
    Invocation result;
    const auto apply = [&](const GlobalOptionId id) {
        switch (id) {
            case GlobalOptionId::help: result.help = true; break;
            case GlobalOptionId::version: result.version = true; break;
            case GlobalOptionId::plain: result.plain = true; break;
            case GlobalOptionId::json: result.json = true; break;
            case GlobalOptionId::raw: result.raw = true; break;
            case GlobalOptionId::quiet: result.quiet = true; break;
            case GlobalOptionId::verbose: result.verbose = true; break;
            case GlobalOptionId::debug: result.debug = true; result.verbose = true; break;
            case GlobalOptionId::color: break;
        }
    };
    bool options = true;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (options && argument == "--") { options = false; continue; }
        if (options && argument.size() > 2 && argument.front() == '-' && argument[1] != '-' && option(argument) == nullptr) {
            std::vector<const GlobalOptionSpec*> clustered;
            std::string_view attached_value;
            bool recognized = true;
            for (std::size_t position = 1; position < argument.size(); ++position) {
                const std::array name{'-', argument[position]};
                const auto* candidate = option(std::string_view(name.data(), name.size()));
                if (!candidate) { recognized = false; break; }
                clustered.push_back(candidate);
                if (!candidate->value_name.empty()) {
                    attached_value = argument.substr(position + 1);
                    break;
                }
            }
            if (recognized) {
                for (const auto* candidate : clustered) apply(candidate->id);
                const auto color = std::ranges::find(clustered, GlobalOptionId::color, &GlobalOptionSpec::id);
                if (color != clustered.end()) {
                    auto value = attached_value;
                    if (value.empty() && ++index < arguments.size()) value = arguments[index];
                    if (value == "auto") result.color = ColorPreference::automatic;
                    else if (value == "always") result.color = ColorPreference::always;
                    else if (value == "never") result.color = ColorPreference::never;
                    else return std::unexpected(Error{ErrorKind::usage, "Invalid -c/--color value: " + std::string(value)});
                }
                continue;
            }
        }
        const auto* global = options ? option(argument) : nullptr;
        if (global && global->id == GlobalOptionId::color) {
            std::string_view value;
            if (!argument.contains('=')) {
                if (++index >= arguments.size()) {
                    return std::unexpected(Error{ErrorKind::usage, "--color requires auto, always, or never"});
                }
                value = arguments[index];
            } else {
                value = argument.substr(argument.find('=') + 1);
            }
            if (value == "auto") result.color = ColorPreference::automatic;
            else if (value == "always") result.color = ColorPreference::always;
            else if (value == "never") result.color = ColorPreference::never;
            else return std::unexpected(Error{ErrorKind::usage, "Invalid --color value: " + std::string(value)});
            continue;
        }
        if (global) {
            apply(global->id);
            continue;
        }
        if (result.command.empty() && options && argument.starts_with('-')) {
            return std::unexpected(Error{ErrorKind::usage, "Unknown global option: " + std::string(argument)});
        }
        if (result.command.empty()) result.command = argument;
        else result.arguments.emplace_back(argument);
    }

    if (result.version && (!result.command.empty() || result.help)) {
        return std::unexpected(Error{ErrorKind::usage, "--version cannot be combined with a command or --help"});
    }
    if (result.plain && result.json) {
        return std::unexpected(Error{ErrorKind::usage, "--plain and --json cannot be combined"});
    }
    if (result.raw && result.json) {
        return std::unexpected(Error{ErrorKind::usage, "--raw and --json cannot be combined"});
    }
    return result;
}

} // namespace atlas::cli
