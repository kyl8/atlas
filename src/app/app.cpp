#include "atlas/app.hpp"

#include "atlas/cli/parser.hpp"
#include "atlas/cli/options.hpp"
#include "atlas/cli/registry.hpp"
#include "atlas/completion.hpp"
#include "atlas/commands/builtins.hpp"
#include "atlas/lua/runtime.hpp"
#include "atlas/platform/paths.hpp"
#include "atlas/terminal/theme.hpp"
#include "atlas/terminal/ui.hpp"
#include "atlas/terminal/screens.hpp"
#include "atlas/platform/utf.hpp"
#include "atlas/tools.hpp"

#include <filesystem>
#include <algorithm>
#include <memory>
#include <iostream>
#include <set>

namespace atlas {
namespace {

constexpr std::string_view version = "1.0";

terminal::OutputOptions output_options(const cli::Invocation& invocation) {
    terminal::OutputOptions options;
    if (invocation.plain) options.mode = terminal::OutputMode::plain;
    if (invocation.json) options.mode = terminal::OutputMode::json;
    switch (invocation.color) {
        case cli::ColorPreference::automatic: options.color = terminal::ColorMode::automatic; break;
        case cli::ColorPreference::always: options.color = terminal::ColorMode::always; break;
        case cli::ColorPreference::never: options.color = terminal::ColorMode::never; break;
    }
    options.quiet = invocation.quiet;
    options.raw = invocation.raw;
    options.verbose = invocation.verbose;
    options.debug = invocation.debug;
    return options;
}

std::unique_ptr<lua::Runtime> apply_user_theme(terminal::Ui& ui) {
    if (!ui.human()) return nullptr;
    auto atlas_directory = win::user_atlas_directory();
    if (!atlas_directory) return nullptr;
    const auto config = *atlas_directory / L"config.lua";
    std::error_code error;
    if (!std::filesystem::exists(config, error) || error) return nullptr;

    auto runtime = std::make_unique<lua::Runtime>(&ui);
    if (!runtime->valid()) {
        ui.warning("Could not initialize Lua for theme configuration; using the default theme");
        return nullptr;
    }
    if (auto loaded = runtime->load_user_config(); !loaded) {
        ui.warning("Could not load theme configuration; using the default theme");
        if (ui.options().debug) ui.diagnostic(loaded.error().detail);
        return nullptr;
    }

    auto theme = terminal::default_theme();
    std::vector<std::string> warnings;
    const auto configured = runtime->configured_theme();
    const auto name = configured ? *configured : std::string("default");
    if (name != "default") {
        if (!terminal::valid_theme_name(name)) {
            warnings.emplace_back("Invalid theme name '" + name + "'; using the default theme");
        } else {
            auto file = terminal::find_theme_file(name);
            auto loaded = file ? terminal::load_theme_file(*file, theme) : Result<terminal::ThemeLoad>(std::unexpected(file.error()));
            if (loaded) {
                theme = std::move(loaded->theme);
                warnings.insert(warnings.end(), loaded->warnings.begin(), loaded->warnings.end());
            } else {
                warnings.emplace_back(loaded.error().message + "; using the default theme");
            }
        }
    }
    runtime->apply_theme_overrides(theme, warnings);
    runtime->apply_ui_config(theme, warnings);
    ui.set_theme(std::move(theme));
    for (const auto& warning : warnings) ui.warning(warning);
    return runtime;
}


void print_unknown_command(terminal::Ui& ui, const cli::Registry& registry, const std::string_view name) {
    ui.error("Unknown command: " + std::string(name));
    const auto suggestions = registry.suggestions(name);
    if (!suggestions.empty()) {
        ui.muted("Did you mean:");
        for (const auto suggestion : suggestions) {
            ui.semantic_line(terminal::StyleToken::command, "  " + std::string(suggestion));
        }
    }
}

int run_command(const cli::Command& command, const cli::Invocation& invocation, terminal::Ui& ui) {
    ui.diagnostic("Running command '" + command.name + "'");
    cli::CommandContext context{ui};
    const auto arguments = cli::normalize_options(command, invocation.arguments);
    auto result = command.handler(context, arguments);
    if (!result) {
        ui.error(result.error());
        return result.error().kind == ErrorKind::usage ? 2 : 1;
    }
    return *result;
}

} // namespace

int run(const std::span<const std::string_view> arguments) {
    if (!arguments.empty() && arguments.front() == "__complete") {
        if (arguments.size() != 3) { std::cout << "[]\n"; return 0; }
        cli::Registry registry;
        if (auto registered = commands::register_builtins(registry); !registered) { std::cout << "[]\n"; return 0; }
        lua::Runtime runtime;
        if (runtime.valid()) {
            static_cast<void>(runtime.load_user_extensions(true));
            static_cast<void>(runtime.add_commands_to(registry));
        }
        const completion::DynamicProvider provider = [&](const std::string_view name,
                const std::string_view current, const std::span<const std::string> completed)
                -> Result<std::vector<completion::Item>> {
            if (!runtime.valid()) return std::vector<completion::Item>{};
            if (name == "tools") {
                std::set<std::string> names;
                const auto configured = runtime.configured_tool_names();
                if (configured) names.insert(configured->begin(), configured->end());
                const auto directory = runtime.configured_tools_directory();
                if (directory) {
                    auto root = win::resolve_path(*directory);
                    auto tools = root ? discover_tools(*root) : Result<std::vector<std::filesystem::path>>(std::unexpected(root.error()));
                    if (tools) for (const auto& path : *tools) {
                        auto stem = win::narrow(path.stem().native());
                        if (stem) names.insert(*stem);
                    }
                }
                std::vector<completion::Item> result;
                for (const auto& value : names) result.push_back({value, "External tool", completion::Kind::tool});
                return result;
            }
            return runtime.complete(name, current, completed);
        };
        if (arguments[1] == "snapshot") {
            const auto clean = [](std::string value) {
                std::ranges::replace(value, '\n', ' '); std::ranges::replace(value, '\r', ' ');
                std::ranges::replace(value, '\t', ' ');
                return value;
            };
            const auto emit = [&](const std::string& query, const std::string_view cache_key = {}) {
                auto values = completion::complete(registry, {query, 500, false}, provider);
                if (!values) return;
                const auto context = cache_key.empty() ? std::string_view(query) : cache_key;
                for (const auto& item : *values) {
                    std::cout << context << '\t' << clean(item.value) << '\t'
                              << completion::kind_name(item.kind) << '\t' << clean(item.description) << '\n';
                }
            };
            emit("atlas ");
            emit("atlas -", "atlas ");
            for (const auto& option : cli::global_options()) {
                if (!option.value_name.empty()) emit("atlas " + std::string(option.long_name) + " ");
            }
            for (const auto& command : registry.commands()) {
                const auto base = "atlas " + command.name + " ";
                emit(base);
                emit(base + "-", base);
                for (const auto& option : cli::option_specs(command.help.options)) {
                    if (option.takes_value) emit(base + option.names.front() + " ");
                }
                for (const auto& child : command.help.subcommands) {
                    const auto child_base = base + child.name + " ";
                    emit(child_base);
                    emit(child_base + "-", child_base);
                    for (const auto& option : cli::option_specs(child.options)) {
                        if (option.takes_value) emit(child_base + option.names.front() + " ");
                    }
                }
            }
            return 0;
        }
        auto result = completion::complete(registry, {arguments[2], arguments[1] == "json" ? 100u : 500u}, provider);
        if (!result) { std::cout << (arguments[1] == "json" ? "[]\n" : ""); return 0; }
        if (arguments[1] == "json") std::cout << completion::json(*result);
        else for (const auto& item : *result) {
            std::string value = item.value;
            std::string description = item.description;
            std::ranges::replace(value, '\n', ' '); std::ranges::replace(value, '\r', ' ');
            std::ranges::replace(value, '\t', ' ');
            std::ranges::replace(description, '\n', ' '); std::ranges::replace(description, '\r', ' ');
            std::ranges::replace(description, '\t', ' ');
            if (arguments[1] == "records") {
                std::cout << value << '\t' << completion::kind_name(item.kind) << '\t' << description << '\n';
            } else if (arguments[1] == "fish") std::cout << value << '\t' << description << '\n';
            else std::cout << value << '\n';
        }
        return 0;
    }
    auto invocation = cli::parse(arguments);
    if (!invocation) {
        terminal::Ui ui({});
        ui.error(invocation.error());
        return 2;
    }

    terminal::Ui ui(output_options(*invocation));
    if (invocation->version) {
        const terminal::Segment line[]{{"atlas ", terminal::StyleToken::secondary},
                                       {std::string(version), terminal::StyleToken::accent}};
        ui.line(line);
        return 0;
    }
    auto lua_runtime = apply_user_theme(ui);

    cli::Registry registry;
    if (auto registered = commands::register_builtins(registry); !registered) {
        ui.error(registered.error());
        return 1;
    }

    const auto load_lua_commands = [&]() -> Status {
        if (!lua_runtime) lua_runtime = std::make_unique<lua::Runtime>(&ui);
        if (!lua_runtime->valid()) return std::unexpected(lua_runtime->initialization_error());
        ui.diagnostic("Loading Lua extensions");
        auto loaded = lua_runtime->load_user_extensions();
        auto registered = lua_runtime->add_commands_to(registry);
        if (!loaded) return loaded;
        return registered;
    };

    if (invocation->command.empty()) {
        if (auto loaded = load_lua_commands(); !loaded) {
            ui.warning("Could not load user commands: " + loaded.error().message);
            if (ui.options().debug) ui.diagnostic(loaded.error().detail);
        }
        if (invocation->help) terminal::show_help(ui, registry, version, false);
        else {
            const auto directory = win::user_atlas_directory();
            const auto tools = lua_runtime ? lua_runtime->configured_tools_directory().value_or("") : "";
            terminal::show_home(ui, registry, version,
                directory ? win::narrow(directory->native()).value_or("~/.atlas") : "~/.atlas", tools);
        }
        return 0;
    }
    if (invocation->command == "help") {
        if (invocation->arguments.empty()) {
            if (auto loaded = load_lua_commands(); !loaded) {
                ui.warning("Could not load user commands: " + loaded.error().message);
                if (ui.options().debug) ui.diagnostic(loaded.error().detail);
            }
            terminal::show_help(ui, registry, version);
            return 0;
        }
        if (invocation->arguments.size() != 1) {
            ui.error(Error{ErrorKind::usage, "Usage: atlas help <command>"});
            return 2;
        }
        if (const auto* command = registry.find(invocation->arguments.front())) {
            terminal::show_command_help(ui, *command);
            return 0;
        }
        if (auto loaded = load_lua_commands(); !loaded) {
            ui.error(loaded.error());
            return 1;
        }
        if (const auto* command = registry.find(invocation->arguments.front())) {
            terminal::show_command_help(ui, *command);
            return 0;
        }
        print_unknown_command(ui, registry, invocation->arguments.front());
        return 2;
    }
    if (invocation->help) {
        if (const auto* command = registry.find(invocation->command)) {
            terminal::show_command_help(ui, *command);
            return 0;
        }
        if (auto loaded = load_lua_commands(); !loaded) {
            ui.error(loaded.error());
            return 1;
        }
        if (const auto* command = registry.find(invocation->command)) {
            terminal::show_command_help(ui, *command);
            return 0;
        }
        print_unknown_command(ui, registry, invocation->command);
        return 2;
    }
    if (const auto* command = registry.find(invocation->command)) {
        return run_command(*command, *invocation, ui);
    }

    if (auto loaded = load_lua_commands(); !loaded) {
        ui.error(loaded.error());
        return 1;
    }
    if (const auto* command = registry.find(invocation->command)) {
        return run_command(*command, *invocation, ui);
    }

    print_unknown_command(ui, registry, invocation->command);
    return 2;
}

} // namespace atlas
