#include "atlas/lua/runtime.hpp"

#include "atlas/platform/paths.hpp"
#include "atlas/platform/process.hpp"
#include "atlas/platform/shell.hpp"
#include "atlas/platform/utf.hpp"
#include "atlas/terminal/theme.hpp"
#include "atlas/terminal/ui.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace atlas::lua {
namespace {

using LuaResult = std::tuple<sol::object, sol::object>;

std::string error_text(const Error& error) {
    if (error.detail.empty()) return error.message;
    return error.message + ": " + error.detail;
}

template <typename Value>
LuaResult lua_success(const sol::state_view lua, Value&& value) {
    return {sol::make_object(lua, std::forward<Value>(value)), sol::make_object(lua, sol::nil)};
}

LuaResult lua_failure(const sol::state_view lua, const Error& error) {
    return {sol::make_object(lua, sol::nil), sol::make_object(lua, error_text(error))};
}

Result<std::vector<std::wstring>> process_arguments(const sol::optional<sol::table>& source) {
    std::vector<std::wstring> arguments;
    if (!source) return arguments;
    arguments.reserve(source->size());
    for (std::size_t index = 1; index <= source->size(); ++index) {
        const sol::object item = (*source)[index];
        if (!item.is<std::string>()) {
            return std::unexpected(Error{ErrorKind::lua, "Every process argument must be a string"});
        }
        auto wide = win::widen(item.as<std::string>());
        if (!wide) return std::unexpected(wide.error());
        arguments.push_back(std::move(*wide));
    }
    return arguments;
}

Result<std::filesystem::path> lua_path(const std::string_view path) {
    return win::resolve_path(path);
}

Result<std::string> path_utf8(const std::filesystem::path& path) {
    return win::narrow(path.native());
}

terminal::Ui& require_ui(terminal::Ui* ui) {
    if (ui == nullptr) throw std::runtime_error("Atlas UI is not available");
    return *ui;
}

cli::HelpEntry help_entry(const sol::object& source) {
    if (source.is<std::string>()) return {source.as<std::string>(), {}};
    if (!source.is<sol::table>()) throw std::runtime_error("command metadata entry must be a string or table");
    const auto table = source.as<sol::table>();
    cli::HelpEntry result;
    result.label = table.get_or("name", table.get_or(1, std::string{}));
    const auto short_name = table.get_or("short", std::string{});
    if (!short_name.empty()) result.label = short_name + ", " + result.label;
    const auto value_name = table.get_or("value_name", std::string{});
    if (!value_name.empty()) result.label += " <" + value_name + ">";
    result.description = table.get_or("description", table.get_or(2, std::string{}));
    sol::object completion = table["completion"];
    if (completion == sol::nil) completion = table["values"];
    if (completion.is<std::string>()) result.completion = completion.as<std::string>();
    else if (completion.is<sol::table>()) {
        const auto values = completion.as<sol::table>();
        for (std::size_t index = 1; index <= values.size(); ++index) {
            const sol::object value = values[index];
            if (value.is<std::string>()) result.values.emplace_back(value.as<std::string>(), std::string{});
            else if (value.is<sol::table>()) {
                const auto item = value.as<sol::table>();
                result.values.emplace_back(item.get_or("value", item.get_or(1, std::string{})),
                                           item.get_or("description", item.get_or(2, std::string{})));
            }
        }
    }
    return result;
}

void help_entries(const sol::table& metadata, const char* key, std::vector<cli::HelpEntry>& target) {
    const sol::optional<sol::table> values = metadata[key];
    if (!values) return;
    for (std::size_t index = 1; index <= values->size(); ++index) target.push_back(help_entry((*values)[index]));
}

std::string cell_text(const sol::object& value) {
    if (value == sol::nil) return {};
    if (!value.is<std::string>()) throw std::runtime_error("Table cell values must be strings or numbers");
    return value.as<std::string>();
}

terminal::Table ui_table(const sol::table& headers, const sol::table& rows) {
    terminal::Table table;
    const auto column_count = headers.size();
    table.columns.reserve(column_count);
    for (std::size_t column = 1; column <= column_count; ++column) {
        const sol::object heading = headers[column];
        if (!heading.is<std::string>()) throw std::runtime_error("Table headers must be strings");
        table.columns.emplace_back(heading.as<std::string>());
    }
    table.rows.reserve(rows.size());
    for (std::size_t row_index = 1; row_index <= rows.size(); ++row_index) {
        const sol::object source_row = rows[row_index];
        if (!source_row.is<sol::table>()) throw std::runtime_error("Every table row must be a table");
        const sol::table row_table = source_row.as<sol::table>();
        std::vector<terminal::TableCell> row;
        row.reserve(column_count);
        for (std::size_t column = 1; column <= column_count; ++column) {
            const sol::object source_cell = row_table[column];
            terminal::TableCell cell;
            if (source_cell.is<sol::table>()) {
                const sol::table styled = source_cell.as<sol::table>();
                cell.text = cell_text(styled["value"]);
                const sol::optional<std::string> style = styled["style"];
                if (style) cell.style = terminal::style_token(*style);
            } else {
                cell.text = cell_text(source_cell);
            }
            row.push_back(std::move(cell));
        }
        table.rows.push_back(std::move(row));
    }
    return table;
}

struct SpinnerHandle {
    std::unique_ptr<terminal::Spinner> value;
    explicit SpinnerHandle(std::unique_ptr<terminal::Spinner> spinner) : value(std::move(spinner)) {}
    SpinnerHandle(const SpinnerHandle&) = delete;
    SpinnerHandle& operator=(const SpinnerHandle&) = delete;
    SpinnerHandle(SpinnerHandle&&) noexcept = default;
    SpinnerHandle& operator=(SpinnerHandle&&) noexcept = default;

    void finish(const sol::optional<std::string>& message) {
        if (value) value->finish(message.value_or(""));
    }
};

struct ProgressHandle {
    std::unique_ptr<terminal::Progress> value;
    explicit ProgressHandle(std::unique_ptr<terminal::Progress> progress) : value(std::move(progress)) {}
    ProgressHandle(const ProgressHandle&) = delete;
    ProgressHandle& operator=(const ProgressHandle&) = delete;
    ProgressHandle(ProgressHandle&&) noexcept = default;
    ProgressHandle& operator=(ProgressHandle&&) noexcept = default;

    void set_total(const std::uint64_t total) const { if (value) value->set_total(total); }
    void set_current(const std::uint64_t current) const { if (value) value->set_current(current); }
    void set_speed(const double speed) const { if (value) value->set_speed(speed); }
    void finish(const sol::optional<std::string>& message) {
        if (value) value->finish(message.value_or(""));
    }
};

} // namespace

void Runtime::register_api() {
    auto atlas = state_->create_named_table("atlas");
    const auto add_command = [this](std::string name, sol::protected_function handler,
                                   const sol::optional<sol::table>& metadata) {
        cli::CommandHelp help;
        std::string description;
        std::string usage;
        if (metadata) {
            description = metadata->get_or("description", std::string{});
            usage = metadata->get_or("usage", std::string{});
            help.details = metadata->get_or("details", std::string{});
            if (help.details.empty()) help.details = metadata->get_or("long_description", std::string{});
            help.category = metadata->get_or("category", std::string("User commands"));
            help_entries(*metadata, "arguments", help.arguments);
            help_entries(*metadata, "options", help.options);
            help_entries(*metadata, "examples", help.examples);
            const sol::optional<sol::table> subcommands = (*metadata)["subcommands"];
            if (subcommands) for (std::size_t index = 1; index <= subcommands->size(); ++index) {
                const auto item = subcommands->get<sol::table>(index);
                cli::SubcommandHelp child;
                child.name = item.get<std::string>("name");
                child.description = item.get_or("description", std::string{});
                const sol::optional<sol::table> aliases = item["aliases"];
                if (aliases) for (std::size_t alias = 1; alias <= aliases->size(); ++alias)
                    child.aliases.push_back(aliases->get<std::string>(alias));
                help_entries(item, "arguments", child.arguments);
                help_entries(item, "options", child.options);
                help.subcommands.push_back(std::move(child));
            }
        }
        register_command(name, std::move(handler));
        auto& command = commands_.at(name);
        command.description = std::move(description);
        command.usage = std::move(usage);
        command.help = std::move(help);
        if (metadata) {
            const sol::optional<sol::table> aliases = (*metadata)["aliases"];
            if (aliases) for (std::size_t i = 1; i <= aliases->size(); ++i)
                command.aliases.push_back(aliases->get<std::string>(i));
        }
    };
    atlas.set_function("command", sol::overload(
        [add_command](std::string name, sol::protected_function handler) {
            add_command(std::move(name), std::move(handler), sol::nullopt);
        }, add_command,
        [add_command](const sol::table& metadata) {
            const sol::object handler = metadata["run"];
            if (!handler.is<sol::protected_function>()) throw std::runtime_error("atlas.command requires a run function");
            add_command(metadata.get<std::string>("name"), handler.as<sol::protected_function>(), metadata);
        }));

    auto completion_api = state_->create_table();
    atlas["completion"] = completion_api;
    completion_api.set_function("register", [this](std::string name, sol::protected_function handler) {
        register_completion_provider(std::move(name), std::move(handler));
    });

    atlas.set_function("exec", [this](const std::string& executable,
                                      const sol::optional<sol::table>& source_arguments,
                                      sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto wide_executable = win::widen(executable);
        if (!wide_executable) return lua_failure(lua, wide_executable.error());
        auto arguments = process_arguments(source_arguments);
        if (!arguments) return lua_failure(lua, arguments.error());
        auto activity = ui_ ? ui_->spinner("Running " + executable) : nullptr;
        auto result = win::execute(*wide_executable, *arguments);
        if (!result) return lua_failure(lua, result.error());
        if (activity) activity->finish();
        auto output = lua.create_table_with(
            "exit_code", result->exit_code,
            "stdout", result->stdout_text,
            "stderr", result->stderr_text);
        return lua_success(lua, std::move(output));
    });

    atlas.set_function("spawn", [](const std::string& executable,
                                    const sol::optional<sol::table>& source_arguments,
                                    const sol::optional<sol::object>& source_options,
                                    sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto wide_executable = win::widen(executable);
        if (!wide_executable) return lua_failure(lua, wide_executable.error());
        auto arguments = process_arguments(source_arguments);
        if (!arguments) return lua_failure(lua, arguments.error());
        win::SpawnOptions options;
        if (source_options && *source_options != sol::nil) {
            if (source_options->is<bool>()) {
                options.hidden = source_options->as<bool>();
            } else if (source_options->is<sol::table>()) {
                const auto table = source_options->as<sol::table>();
                options.hidden = table.get_or("hidden", false);
                options.new_console = table.get_or("new_console", false);
                const sol::optional<std::string> cwd = table["cwd"];
                if (cwd) {
                    auto path = lua_path(*cwd);
                    if (!path) return lua_failure(lua, path.error());
                    options.working_directory = std::move(*path);
                }
            } else {
                return lua_failure(lua, Error{ErrorKind::usage, "Spawn options must be a boolean or table"});
            }
        }
        auto result = win::spawn(*wide_executable, *arguments, options);
        if (!result) return lua_failure(lua, result.error());
        return lua_success(lua, *result);
    });

    atlas.set_function("open", [](const std::string& target, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        Result<std::wstring> wide = win::widen(target);
        if (target.find("://") == std::string::npos) {
            auto path = win::resolve_path(target);
            if (!path) return lua_failure(lua, path.error());
            wide = path->native();
        }
        if (!wide) return lua_failure(lua, wide.error());
        auto opened = win::shell_open(*wide);
        if (!opened) return lua_failure(lua, opened.error());
        return lua_success(lua, true);
    });

    atlas.set_function("getenv", [](const std::string& name, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto wide_name = win::widen(name);
        if (!wide_name) return lua_failure(lua, wide_name.error());
        const auto required = GetEnvironmentVariableW(wide_name->c_str(), nullptr, 0);
        if (required == 0) return lua_success(lua, sol::nil);
        std::wstring value(required, L'\0');
        const auto written = GetEnvironmentVariableW(wide_name->c_str(), value.data(), required);
        if (written == 0 || written >= required) {
            return lua_failure(lua, Error{ErrorKind::windows, "Could not read environment variable"});
        }
        value.resize(written);
        auto utf8 = win::narrow(value);
        if (!utf8) return lua_failure(lua, utf8.error());
        return lua_success(lua, std::move(*utf8));
    });

    atlas.set_function("setenv", [](const std::string& name, const sol::optional<std::string>& value,
                                    sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto wide_name = win::widen(name);
        if (!wide_name) return lua_failure(lua, wide_name.error());
        std::optional<std::wstring> wide_value;
        if (value) {
            auto converted = win::widen(*value);
            if (!converted) return lua_failure(lua, converted.error());
            wide_value = std::move(*converted);
        }
        if (!SetEnvironmentVariableW(wide_name->c_str(), wide_value ? wide_value->c_str() : nullptr)) {
            const auto code = GetLastError();
            return lua_failure(lua, Error{ErrorKind::windows, "Could not set environment variable",
                                          win::error_message(code), code});
        }
        return lua_success(lua, true);
    });

    auto fs = state_->create_table();
    atlas["fs"] = fs;
    auto cwd = [](sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        std::error_code error;
        const auto path = std::filesystem::current_path(error);
        if (error) return lua_failure(lua, Error{ErrorKind::io, "Could not read current directory", error.message()});
        auto value = path_utf8(path);
        if (!value) return lua_failure(lua, value.error());
        return lua_success(lua, std::move(*value));
    };
    auto cd = [](const std::string& input, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto path = lua_path(input);
        if (!path) return lua_failure(lua, path.error());
        std::error_code error;
        std::filesystem::current_path(*path, error);
        if (error) return lua_failure(lua, Error{ErrorKind::io, "Could not change directory", error.message()});
        return lua_success(lua, true);
    };
    auto exists = [](const std::string& input, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto path = lua_path(input);
        if (!path) return lua_failure(lua, path.error());
        std::error_code error;
        const auto found = std::filesystem::exists(*path, error);
        if (error) return lua_failure(lua, Error{ErrorKind::io, "Could not inspect path", error.message()});
        return lua_success(lua, found);
    };
    auto files = [](const std::string& input, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto directory = lua_path(input);
        if (!directory) return lua_failure(lua, directory.error());
        auto output = lua.create_table();
        std::error_code error;
        std::size_t index = 1;
        for (std::filesystem::directory_iterator iterator(*directory, error), end;
             !error && iterator != end; iterator.increment(error)) {
            auto path = path_utf8(iterator->path());
            auto name = path_utf8(iterator->path().filename());
            auto extension = path_utf8(iterator->path().extension());
            if (!path || !name || !extension) {
                return lua_failure(lua, Error{ErrorKind::windows, "Could not encode a file path"});
            }
            output[index++] = lua.create_table_with(
                "path", std::move(*path), "name", std::move(*name),
                "extension", std::move(*extension), "is_directory", iterator->is_directory(error));
        }
        if (error) return lua_failure(lua, Error{ErrorKind::io, "Could not enumerate directory", error.message()});
        return lua_success(lua, std::move(output));
    };
    auto glob = [](const std::string& input, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto pattern = lua_path(input);
        if (!pattern) return lua_failure(lua, pattern.error());
        auto output = lua.create_table();
        WIN32_FIND_DATAW data{};
        const auto search = FindFirstFileW(pattern->c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) return lua_success(lua, std::move(output));
            const auto code = GetLastError();
            return lua_failure(lua, Error{ErrorKind::windows, "Could not evaluate glob", win::error_message(code), code});
        }
        std::size_t index = 1;
        do {
            if (std::wstring_view(data.cFileName) == L"." || std::wstring_view(data.cFileName) == L"..") continue;
            auto value = path_utf8(pattern->parent_path() / data.cFileName);
            if (!value) {
                FindClose(search);
                return lua_failure(lua, value.error());
            }
            output[index++] = std::move(*value);
        } while (FindNextFileW(search, &data));
        const auto code = GetLastError();
        FindClose(search);
        if (code != ERROR_NO_MORE_FILES) {
            return lua_failure(lua, Error{ErrorKind::windows, "Could not evaluate glob", win::error_message(code), code});
        }
        return lua_success(lua, std::move(output));
    };
    auto read = [](const std::string& input, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto path = lua_path(input);
        if (!path) return lua_failure(lua, path.error());
        std::ifstream stream(*path, std::ios::binary);
        if (!stream) return lua_failure(lua, Error{ErrorKind::io, "Could not read file"});
        std::string contents(std::istreambuf_iterator<char>(stream), {});
        return lua_success(lua, std::move(contents));
    };
    auto write = [](const std::string& input, const std::string& contents,
                    sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto path = lua_path(input);
        if (!path) return lua_failure(lua, path.error());
        std::ofstream stream(*path, std::ios::binary | std::ios::trunc);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream) return lua_failure(lua, Error{ErrorKind::io, "Could not write file"});
        return lua_success(lua, true);
    };

    fs.set_function("cwd", cwd);
    fs.set_function("cd", cd);
    fs.set_function("exists", exists);
    fs.set_function("files", files);
    fs.set_function("glob", glob);
    fs.set_function("read", read);
    fs.set_function("write", write);
    fs.set_function("resolve", [](const std::string& input, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto path = lua_path(input);
        if (!path) return lua_failure(lua, path.error());
        auto value = path_utf8(*path);
        if (!value) return lua_failure(lua, value.error());
        return lua_success(lua, *value);
    });
    fs.set_function("is_dir", [](const std::string& input, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto path = lua_path(input);
        if (!path) return lua_failure(lua, path.error());
        std::error_code error;
        const auto found = std::filesystem::is_directory(*path, error);
        if (error == std::errc::no_such_file_or_directory) error.clear();
        if (error) return lua_failure(lua, Error{ErrorKind::io, "Could not inspect directory", error.message()});
        return lua_success(lua, found);
    });
    fs.set_function("mkdir", [](const std::string& input, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto path = lua_path(input);
        if (!path) return lua_failure(lua, path.error());
        std::error_code error;
        std::filesystem::create_directories(*path, error);
        if (error) return lua_failure(lua, Error{ErrorKind::io, "Could not create directory", error.message()});
        return lua_success(lua, true);
    });
    fs.set_function("write_atomic", [](const std::string& input, const std::string& contents,
                                         const std::string& expected, sol::this_state state) -> LuaResult {
        sol::state_view lua(state);
        auto path = lua_path(input);
        if (!path) return lua_failure(lua, path.error());
        auto saved = win::write_atomic(*path, contents, expected);
        if (!saved) return lua_failure(lua, saved.error());
        return lua_success(lua, true);
    });
    atlas.set_function("cwd", cwd);
    atlas.set_function("cd", cd);
    atlas.set_function("exists", exists);
    atlas.set_function("files", files);
    atlas.set_function("glob", glob);
    atlas.set_function("read", read);
    atlas.set_function("write", write);

    bind_ui();
}

void Runtime::bind_ui() {
    sol::table atlas = (*state_)["atlas"];
    ui_api_ = state_->create_table();
    auto ui = ui_api_;
    atlas["ui"] = ui;
    ui.set_function("human", [this]() { return require_ui(ui_).human(); });
    ui.set_function("json", [this]() { return require_ui(ui_).json(); });
    ui.set_function("data", [this](const std::string& text) { require_ui(ui_).data(text); });
    ui.new_usertype<SpinnerHandle>("Spinner", sol::no_constructor,
        "finish", &SpinnerHandle::finish);
    ui.new_usertype<ProgressHandle>("Progress", sol::no_constructor,
        "set_total", &ProgressHandle::set_total,
        "set_current", &ProgressHandle::set_current,
        "set_speed", &ProgressHandle::set_speed,
        "finish", &ProgressHandle::finish);
    ui.set_function("info", [this](const std::string& message) { require_ui(ui_).info(message); });
    ui.set_function("success", [this](const std::string& message) { require_ui(ui_).success(message); });
    ui.set_function("warning", [this](const std::string& message) { require_ui(ui_).warning(message); });
    ui.set_function("error", [this](const std::string& message, const sol::optional<std::string>& detail) {
        require_ui(ui_).error(message, detail.value_or(""));
    });
    ui.set_function("muted", [this](const std::string& message) { require_ui(ui_).muted(message); });
    ui.set_function("heading", [this](const std::string& title, const sol::optional<std::string>& subject) {
        require_ui(ui_).heading(title, subject.value_or(""));
    });
    ui.set_function("section", [this](const std::string& title, const sol::optional<std::string>& detail) {
        require_ui(ui_).section(title, detail.value_or(""));
    });
    ui.set_function("table", [this](const sol::table& headers, const sol::table& rows) {
        require_ui(ui_).table(ui_table(headers, rows));
    });
    ui.set_function("spinner", [this](const std::string& message) {
        return SpinnerHandle{require_ui(ui_).spinner(message)};
    });
    ui.set_function("progress", [this](const std::string& message) {
        return ProgressHandle{require_ui(ui_).progress(message)};
    });

    constexpr struct SemanticBinding { std::string_view name; terminal::StyleToken token; } semantic_bindings[] = {
        {"primary", terminal::StyleToken::primary}, {"secondary", terminal::StyleToken::secondary},
        {"path", terminal::StyleToken::path}, {"url", terminal::StyleToken::url},
        {"command", terminal::StyleToken::command}, {"number", terminal::StyleToken::number},
        {"date", terminal::StyleToken::date}, {"duration", terminal::StyleToken::duration},
        {"size", terminal::StyleToken::size}, {"highlight", terminal::StyleToken::highlight},
    };
    for (const auto& binding : semantic_bindings) {
        ui.set_function(std::string(binding.name), [this, token = binding.token](const std::string& message) {
            require_ui(ui_).semantic_line(token, message);
        });
    }
}

} // namespace atlas::lua
