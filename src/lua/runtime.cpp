#include "atlas/lua/runtime.hpp"

#include "atlas/lua/errors.hpp"
#include "atlas/cli/registry.hpp"
#include "atlas/lua/theme.hpp"
#include "atlas/platform/paths.hpp"
#include "atlas/platform/utf.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace atlas::lua {
namespace {

std::string path_utf8(const std::filesystem::path& path) {
    return win::narrow(path.native()).value_or("<invalid path>");
}

Error script_error(const std::filesystem::path& script, const std::string_view raw) {
    const auto parsed = parse_error(raw);
    std::string detail = "script: " + path_utf8(script);
    if (!parsed.line.empty()) detail += "\nline: " + parsed.line;
    detail += "\n\n" + parsed.message;
    Error error{ErrorKind::lua, "Lua script failed", std::move(detail)};
    error.debug_detail = parsed.traceback;
    return error;
}

Error command_error(const std::filesystem::path& script, const std::string_view command,
                    const std::string_view raw) {
    const auto parsed = parse_error(raw);
    const auto source = script.empty() ? std::string("<unknown>") : path_utf8(script.filename());
    std::string detail = "plugin: " + source + "\ncommand: " + std::string(command);
    if (!parsed.line.empty()) detail += "\nline: " + parsed.line;
    detail += "\n\n" + parsed.message;
    Error error{ErrorKind::lua, "Lua command '" + std::string(command) + "' failed", std::move(detail)};
    error.debug_detail = parsed.traceback;
    return error;
}

} // namespace

Runtime::Runtime(terminal::Ui* ui) : ui_(ui) {
    try {
        state_ = std::make_unique<sol::state>();
        state_->open_libraries(sol::lib::base, sol::lib::package, sol::lib::coroutine,
                               sol::lib::string, sol::lib::os, sol::lib::math,
                               sol::lib::table, sol::lib::io, sol::lib::utf8);
        register_api();
    } catch (const std::exception& exception) {
        commands_.clear();
        ui_api_ = sol::nil;
        state_.reset();
        initialization_error_ = Error{ErrorKind::lua, "Could not initialize Lua", exception.what()};
    }
}

bool Runtime::valid() const noexcept { return state_ != nullptr; }

const Error& Runtime::initialization_error() const noexcept { return initialization_error_; }

void Runtime::register_command(std::string name, sol::protected_function handler) {
    if (name.empty()) throw std::runtime_error("command name cannot be empty");
    if (commands_.contains(name)) throw std::runtime_error("command '" + name + "' is already registered");
    commands_.emplace(std::move(name), LuaCommand{std::move(handler), current_script_});
}

void Runtime::register_completion_provider(std::string name, sol::protected_function handler) {
    if (name.empty()) throw std::runtime_error("completion provider name cannot be empty");
    completion_providers_.insert_or_assign(std::move(name), std::move(handler));
}

Status Runtime::load_file(const std::filesystem::path& file) {
    if (!valid()) return std::unexpected(initialization_error_);
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return std::unexpected(Error{ErrorKind::io, "Could not read Lua file: " + path_utf8(file)});
    }
    const std::string source(std::istreambuf_iterator<char>(stream), {});
    const auto previous_script = std::exchange(current_script_, file);
    try {
        const auto chunk = "@" + path_utf8(file);
        auto result = state_->safe_script(source, sol::script_pass_on_error, chunk, sol::load_mode::text);
        current_script_ = previous_script;
        if (!result.valid()) {
            const sol::error error = result;
            return std::unexpected(script_error(file, error.what()));
        }
    } catch (const std::exception& exception) {
        current_script_ = previous_script;
        return std::unexpected(script_error(file, exception.what()));
    }
    return {};
}

Status Runtime::load_user_config() {
    if (config_loaded_) return {};
    config_loaded_ = true;
    auto directory = win::user_atlas_directory();
    if (!directory) return std::unexpected(directory.error());
    const auto config = *directory / L"config.lua";
    std::error_code error;
    if (!std::filesystem::exists(config, error)) return {};
    if (error) return std::unexpected(Error{ErrorKind::io, "Could not inspect Lua config", error.message()});
    auto loaded = load_file(config);
    const sol::object api = (*state_)["atlas"];
    if (!api.is<sol::table>()) return std::unexpected(Error{ErrorKind::lua,
        "config.lua must preserve the global atlas table"});
    sol::table atlas = api.as<sol::table>();
    const sol::object configured = atlas["ui"];
    if (configured.is<sol::table>()) {
        const auto values = configured.as<sol::table>();
        for (const auto& [key, value] : values) {
            if (!value.is<sol::function>()) ui_api_[key] = value;
        }
    }
    atlas["ui"] = ui_api_;
    return loaded;
}

Status Runtime::load_directory(const std::filesystem::path& directory, const bool best_effort) {
    std::error_code error;
    if (!std::filesystem::exists(directory, error)) return {};
    if (error) {
        return std::unexpected(Error{ErrorKind::io, "Could not inspect Lua directory: " + path_utf8(directory), error.message()});
    }
    std::vector<std::filesystem::path> scripts;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && iterator->path().extension() == L".lua") {
            scripts.push_back(iterator->path());
        }
    }
    if (error) {
        return std::unexpected(Error{ErrorKind::io, "Could not enumerate Lua directory: " + path_utf8(directory), error.message()});
    }
    std::ranges::sort(scripts);
    for (const auto& script : scripts) {
        if (auto loaded = load_file(script); !loaded && !best_effort) return loaded;
    }
    return {};
}

Status Runtime::load_user_extensions(const bool best_effort) {
    if (extensions_loaded_) return {};
    extensions_loaded_ = true;
    if (auto loaded = load_user_config(); !loaded && !best_effort) return loaded;
    auto directory = win::user_atlas_directory();
    if (!directory) return std::unexpected(directory.error());
    if (auto loaded = load_directory(*directory / L"commands", best_effort); !loaded) return loaded;
    return load_directory(*directory / L"plugins", best_effort);
}

Status Runtime::add_commands_to(cli::Registry& registry) {
    std::vector<std::string> names;
    names.reserve(commands_.size());
    for (const auto& [name, command] : commands_) {
        static_cast<void>(command);
        names.push_back(name);
    }
    std::ranges::sort(names);
    for (const auto& name : names) {
        if (registry.find(name) != nullptr) continue;
        const auto& command = commands_.at(name);
        const auto source = command.source.empty() ? std::string("Lua") : path_utf8(command.source.filename());
        cli::Command lua_command{name,
            command.description.empty() ? "Lua command from " + source : command.description,
            command.usage.empty() ? "atlas " + name + " [arguments]" : command.usage, {},
            [this, name](cli::CommandContext&, const std::span<const std::string> arguments) {
                return invoke(name, arguments);
            }, command.help};
        lua_command.user_defined = true;
        lua_command.aliases = command.aliases;
        lua_command.source = path_utf8(command.source);
        auto added = registry.add(std::move(lua_command));
        if (!added) return added;
    }
    return {};
}

Result<std::vector<completion::Item>> Runtime::complete(const std::string_view provider,
                                                        const std::string_view current,
                                                        const std::span<const std::string> completed) {
    const auto found = completion_providers_.find(std::string(provider));
    if (found == completion_providers_.end()) return std::vector<completion::Item>{};
    auto context = state_->create_table();
    context["current"] = current;
    auto words = state_->create_table(static_cast<int>(completed.size()), 0);
    for (std::size_t index = 0; index < completed.size(); ++index) words[index + 1] = completed[index];
    context["words"] = std::move(words);
    try {
        auto result = found->second(context);
        if (!result.valid()) {
            const sol::error error = result;
            return std::unexpected(Error{ErrorKind::lua, "Lua completion provider failed", error.what()});
        }
        if (result.return_count() == 0) return std::vector<completion::Item>{};
        const sol::object returned = result.get<sol::object>();
        if (!returned.is<sol::table>()) {
            return std::unexpected(Error{ErrorKind::lua, "Lua completion provider must return a table"});
        }
        std::vector<completion::Item> items;
        const auto values = returned.as<sol::table>();
        items.reserve(values.size());
        for (std::size_t index = 1; index <= values.size(); ++index) {
            const sol::object value = values[index];
            if (value.is<std::string>()) {
                items.push_back({value.as<std::string>(), {}, completion::Kind::value});
                continue;
            }
            if (!value.is<sol::table>()) continue;
            const auto item = value.as<sol::table>();
            const sol::optional<std::string> text = item["value"];
            if (!text) continue;
            auto kind = completion::Kind::value;
            const auto source_kind = item.get_or("kind", std::string{});
            if (source_kind == "project") kind = completion::Kind::project;
            else if (source_kind == "theme") kind = completion::Kind::theme;
            else if (source_kind == "tool") kind = completion::Kind::tool;
            else if (source_kind == "path") kind = completion::Kind::path;
            items.push_back({*text, item.get_or("description", std::string{}), kind,
                             item.get_or("score", 0)});
        }
        return items;
    } catch (const std::exception& exception) {
        return std::unexpected(Error{ErrorKind::lua, "Lua completion provider failed", exception.what()});
    }
}

bool Runtime::has_command(const std::string_view name) const {
    return commands_.contains(std::string(name));
}

Result<int> Runtime::invoke(const std::string_view name, const std::span<const std::string> arguments) {
    const auto found = commands_.find(std::string(name));
    if (found == commands_.end()) {
        return std::unexpected(Error{ErrorKind::not_found, "Lua command is not registered: " + std::string(name)});
    }
    auto values = state_->create_table(static_cast<int>(arguments.size()), 0);
    for (std::size_t index = 0; index < arguments.size(); ++index) values[index + 1] = arguments[index];
    try {
        auto result = found->second.handler(values);
        if (!result.valid()) {
            const sol::error error = result;
            return std::unexpected(command_error(found->second.source, name, error.what()));
        }
        if (result.return_count() == 0) return 0;
        const sol::object returned = result.get<sol::object>();
        if (returned.is<int>()) return returned.as<int>();
        return 0;
    } catch (const std::exception& exception) {
        return std::unexpected(command_error(found->second.source, name, exception.what()));
    }
}

Result<std::string> Runtime::configured_tool(const std::string_view name) {
    const sol::table atlas = (*state_)["atlas"];
    const sol::optional<sol::table> tools = atlas["tools"];
    if (tools) {
        const sol::optional<std::string> configured = (*tools)[std::string(name)];
        if (configured) return *configured;
    }
    return std::unexpected(Error{ErrorKind::not_found, "Tool is not configured: " + std::string(name)});
}

Result<std::string> Runtime::configured_tools_directory() {
    const sol::table atlas = (*state_)["atlas"];
    const sol::optional<std::string> configured = atlas["tools_dir"];
    if (configured) return *configured;
    return std::unexpected(Error{ErrorKind::not_found, "Tools directory is not configured"});
}

Result<std::vector<std::string>> Runtime::configured_tool_names() {
    const sol::table atlas = (*state_)["atlas"];
    const sol::object configured = atlas["tools"];
    std::vector<std::string> names;
    if (configured == sol::nil) return names;
    if (!configured.is<sol::table>()) {
        return std::unexpected(Error{ErrorKind::lua, "atlas.tools must be a table of executable paths"});
    }
    for (const auto& [key, value] : configured.as<sol::table>()) {
        if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
            return std::unexpected(Error{ErrorKind::lua, "atlas.tools requires string names and executable paths"});
        }
        names.push_back(key.as<std::string>());
    }
    return names;
}

Result<std::string> Runtime::configured_theme() {
    const sol::table atlas = (*state_)["atlas"];
    const sol::optional<std::string> configured = atlas["theme"];
    if (configured) return *configured;
    return std::unexpected(Error{ErrorKind::not_found, "Theme is not configured"});
}

void Runtime::apply_theme_overrides(terminal::Theme& theme, std::vector<std::string>& warnings) {
    const sol::table atlas = (*state_)["atlas"];
    const sol::object overrides = atlas["theme_overrides"];
    if (overrides == sol::nil) return;
    if (!overrides.is<sol::table>()) {
        warnings.emplace_back("atlas.theme_overrides must be a table");
        return;
    }
    merge_theme_styles(overrides.as<sol::table>(), theme, warnings, "config overrides");
}

void Runtime::apply_ui_config(terminal::Theme& theme, std::vector<std::string>& warnings) {
    auto directory = win::user_atlas_directory();
    if (!directory) return;
    const sol::object art = ui_api_["art"];
    const sol::object logo = ui_api_["logo"];
    merge_art(art != sol::nil ? art : logo, theme.art, warnings, *directory);
    theme.user_art_override = art != sol::nil || logo != sol::nil;

    const sol::object tagline = ui_api_["tagline"];
    const auto valid_text = [](const std::string_view value, const std::size_t maximum,
                               const bool allow_empty) {
        return (allow_empty || !value.empty()) && value.size() <= maximum &&
            std::ranges::none_of(value, [](const unsigned char character) {
                return character < 32 || character == 127;
            });
    };
    if (tagline.is<bool>() && !tagline.as<bool>()) {
        theme.tagline.clear();
    } else if (tagline.is<std::string>()) {
        auto value = tagline.as<std::string>();
        if (valid_text(value, 240, false)) theme.tagline = std::move(value);
        else warnings.emplace_back("atlas.ui.tagline must be one non-empty line of at most 240 bytes");
    } else if (tagline != sol::nil) {
        warnings.emplace_back("atlas.ui.tagline must be a string or false");
    }

    const sol::object home = ui_api_["home"];
    if (home.is<sol::table>()) {
        const auto values = home.as<sol::table>();
        const sol::object title = values["actions_title"];
        if (title.is<std::string>() && valid_text(title.as<std::string>(), 80, false)) {
            theme.home.actions_title = title.as<std::string>();
        } else if (title != sol::nil) {
            warnings.emplace_back("atlas.ui.home.actions_title must be one non-empty line");
        }
        const sol::object defaults = values["show_default_actions"];
        if (defaults.is<bool>()) theme.home.show_default_actions = defaults.as<bool>();
        else if (defaults != sol::nil) warnings.emplace_back("atlas.ui.home.show_default_actions must be boolean");

        const auto read_entries = [&](const std::string_view field,
                                      std::vector<terminal::HomeEntry>& destination,
                                      const std::string_view first_name,
                                      const std::string_view second_name) {
            const sol::object source = values[field];
            if (source == sol::nil) return;
            if (!source.is<sol::table>()) {
                warnings.emplace_back("atlas.ui.home." + std::string(field) + " must be a list");
                return;
            }
            destination.clear();
            const auto entries = source.as<sol::table>();
            for (std::size_t index = 1; index <= (std::min)(entries.size(), std::size_t{16}); ++index) {
                const sol::object item = entries[index];
                if (!item.is<sol::table>()) {
                    warnings.emplace_back("atlas.ui.home." + std::string(field) + " entries must be tables");
                    continue;
                }
                const auto entry = item.as<sol::table>();
                auto first = entry.get<sol::optional<std::string>>(std::string(first_name));
                auto second = entry.get<sol::optional<std::string>>(std::string(second_name));
                if (!first) first = entry.get<sol::optional<std::string>>(1);
                if (!second) second = entry.get<sol::optional<std::string>>(2);
                if (!first || !second || !valid_text(*first, 120, false) || !valid_text(*second, 240, false)) {
                    warnings.emplace_back("atlas.ui.home." + std::string(field) + " has an invalid entry");
                    continue;
                }
                destination.push_back({std::move(*first), std::move(*second)});
            }
        };
        read_entries("facts", theme.home.facts, "label", "value");
        read_entries("actions", theme.home.actions, "command", "description");
    } else if (home != sol::nil) {
        warnings.emplace_back("atlas.ui.home must be a table");
    }

    const sol::object identity = ui_api_["identity"];
    if (identity == sol::nil) return;
    if (!identity.is<sol::table>()) {
        warnings.emplace_back("atlas.ui.identity must be a table");
        return;
    }
    const auto values = identity.as<sol::table>();
    const auto text_field = [&](const std::string_view field, std::string& destination,
                                const std::size_t maximum, const bool allow_empty,
                                const bool allow_false = false) {
        const sol::object source = values[field];
        if (allow_false && source.is<bool>() && !source.as<bool>()) {
            destination.clear();
            return;
        }
        if (source == sol::nil) return;
        if (!source.is<std::string>() || !valid_text(source.as<std::string>(), maximum, allow_empty)) {
            warnings.emplace_back("atlas.ui.identity." + std::string(field) + " is invalid; using the default value");
            return;
        }
        destination = source.as<std::string>();
    };
    text_field("name", theme.identity.name, 80, false, true);
    text_field("separator", theme.identity.separator, 24, true);
    text_field("explorer", theme.identity.explorer, 80, true, true);

    const sol::object version = values["version"];
    if (version.is<bool>()) {
        theme.identity.show_version = version.as<bool>();
    } else if (version.is<std::string>()) {
        const auto format = version.as<std::string>();
        if (valid_text(format, 80, true)) {
            theme.identity.show_version = true;
            theme.identity.version = format;
        } else warnings.emplace_back("atlas.ui.identity.version is invalid; using the default value");
    } else if (version != sol::nil) {
        warnings.emplace_back("atlas.ui.identity.version must be true, false, or a string");
    }
    const sol::object palette = values["palette"];
    if (palette.is<bool>()) theme.identity.show_palette = palette.as<bool>();
    else if (palette != sol::nil) warnings.emplace_back("atlas.ui.identity.palette must be true or false");
}

} // namespace atlas::lua
