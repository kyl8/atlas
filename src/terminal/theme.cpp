#include "atlas/terminal/theme.hpp"

#include "atlas/lua/errors.hpp"
#include "atlas/lua/theme.hpp"
#include "atlas/platform/paths.hpp"
#include "atlas/platform/utf.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <iterator>
#include <set>

namespace atlas::terminal {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(StyleToken::count)> token_names{
    "primary", "secondary", "accent", "success", "warning", "error", "info", "muted", "subtle",
    "path", "url", "command", "number", "date", "duration", "size", "highlight", "table_header",
    "table_border", "progress_filled", "progress_empty", "spinner",
    "heading", "subheading", "directory", "filename", "selection", "keybind", "badge",
};

constexpr Rgb rgb(const std::uint32_t value) {
    return Rgb{static_cast<std::uint8_t>((value >> 16) & 0xff),
               static_cast<std::uint8_t>((value >> 8) & 0xff),
               static_cast<std::uint8_t>(value & 0xff)};
}

void instruction_limit(lua_State* state, lua_Debug*) {
    luaL_error(state, "theme exceeded the instruction limit");
}

Error theme_error(const std::string_view raw) {
    const auto parsed = lua::parse_error(raw);
    std::string detail;
    if (!parsed.line.empty()) detail = "line: " + parsed.line + "\n\n";
    detail += parsed.message;
    Error error{ErrorKind::lua, "Theme evaluation failed", std::move(detail)};
    error.debug_detail = parsed.traceback;
    return error;
}

Result<std::filesystem::path> executable_directory() {
    std::wstring buffer(512, L'\0');
    for (;;) {
        const auto written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            const auto code = GetLastError();
            return std::unexpected(Error{ErrorKind::windows, "Could not locate Atlas executable",
                                         win::error_message(code), code});
        }
        if (written < buffer.size() - 1) {
            buffer.resize(written);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

Result<std::vector<std::filesystem::path>> theme_directories() {
    std::vector<std::filesystem::path> result;
    auto user = win::user_atlas_directory();
    if (!user) return std::unexpected(user.error());
    result.push_back(*user / L"themes");
    auto executable = executable_directory();
    if (executable) result.push_back(executable->parent_path() / L"share" / L"atlas" / L"themes");
    return result;
}

} // namespace

const TextStyle& Theme::operator[](const StyleToken token) const noexcept {
    return styles[static_cast<std::size_t>(token)];
}

TextStyle& Theme::operator[](const StyleToken token) noexcept {
    return styles[static_cast<std::size_t>(token)];
}

Theme default_theme() {
    Theme theme;
    theme.name = "default";
    theme[StyleToken::primary].foreground = rgb(0xe6edf3);
    theme[StyleToken::secondary].foreground = rgb(0x9da7b3);
    theme[StyleToken::accent] = TextStyle{rgb(0x58a6ff), {}, true};
    theme[StyleToken::success] = TextStyle{rgb(0x3fb950), {}, true};
    theme[StyleToken::warning] = TextStyle{rgb(0xd29922), {}, true};
    theme[StyleToken::error] = TextStyle{rgb(0xf85149), {}, true};
    theme[StyleToken::info].foreground = rgb(0x58a6ff);
    theme[StyleToken::muted].foreground = rgb(0x8b949e);
    theme[StyleToken::subtle].foreground = rgb(0x484f58);
    theme[StyleToken::path].foreground = rgb(0x56d4dd);
    theme[StyleToken::url] = TextStyle{rgb(0x58a6ff), {}, false, false, false, true};
    theme[StyleToken::command].foreground = rgb(0xbc8cff);
    theme[StyleToken::number].foreground = rgb(0xbc8cff);
    theme[StyleToken::date].foreground = rgb(0x8b949e);
    theme[StyleToken::duration].foreground = rgb(0xd29922);
    theme[StyleToken::size].foreground = rgb(0x79c0ff);
    theme[StyleToken::highlight] = TextStyle{rgb(0xf2cc60), {}, true};
    theme[StyleToken::table_header] = TextStyle{rgb(0x9da7b3), {}, true};
    theme[StyleToken::table_border].foreground = rgb(0x484f58);
    theme[StyleToken::progress_filled].foreground = rgb(0x3fb950);
    theme[StyleToken::progress_empty].foreground = rgb(0x484f58);
    theme[StyleToken::spinner].foreground = rgb(0x58a6ff);
    theme[StyleToken::heading] = theme[StyleToken::accent];
    theme[StyleToken::subheading] = theme[StyleToken::secondary];
    theme[StyleToken::subheading].bold = true;
    theme[StyleToken::directory] = theme[StyleToken::path];
    theme[StyleToken::filename] = theme[StyleToken::primary];
    theme[StyleToken::filename].bold = true;
    theme[StyleToken::selection] = theme[StyleToken::highlight];
    theme[StyleToken::keybind] = theme[StyleToken::command];
    theme[StyleToken::badge] = theme[StyleToken::accent];
    theme.palette = {rgb(0x58a6ff), rgb(0x56d4dd), rgb(0x3fb950), rgb(0xf2cc60), rgb(0xbc8cff), rgb(0xe6edf3)};
    return theme;
}

std::optional<StyleToken> style_token(const std::string_view name) {
    if (name == "progress_fill") return StyleToken::progress_filled;
    if (name == "progress_track") return StyleToken::progress_empty;
    if (name == "tag") return StyleToken::badge;
    const auto found = std::ranges::find(token_names, name);
    if (found == token_names.end()) return std::nullopt;
    return static_cast<StyleToken>(std::distance(token_names.begin(), found));
}

std::string_view style_token_name(const StyleToken token) {
    return token_names[static_cast<std::size_t>(token)];
}

bool valid_theme_name(const std::string_view name) {
    if (name.empty()) return false;
    for (const auto character : name) {
        if (!((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
              (character >= '0' && character <= '9') || character == '-' || character == '_')) return false;
    }
    return true;
}

Result<ThemeLoad> load_theme_file(const std::filesystem::path& file, Theme base) {
    std::error_code size_error;
    if (std::filesystem::file_size(file, size_error) > 256 * 1024 && !size_error) {
        return std::unexpected(Error{ErrorKind::lua, "Theme file exceeds 256 KB"});
    }
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        return std::unexpected(Error{ErrorKind::not_found,
            "Theme file was not found: " + win::narrow(file.native()).value_or("<invalid path>")});
    }
    const std::string source(std::istreambuf_iterator<char>(stream), {});
    const auto display = win::narrow(file.native()).value_or("theme.lua");
    try {
        sol::state state;
        lua_sethook(state.lua_state(), instruction_limit, LUA_MASKCOUNT, 100'000);
        auto evaluated = state.safe_script(source, sol::script_pass_on_error,
                                           "@" + display, sol::load_mode::text);
        if (!evaluated.valid()) {
            const sol::error error = evaluated;
            return std::unexpected(theme_error(error.what()));
        }
        if (evaluated.return_count() == 0) {
            return std::unexpected(Error{ErrorKind::lua, "Theme must return a table"});
        }
        const sol::object definition = evaluated.get<sol::object>();
        if (!definition.is<sol::table>()) {
            return std::unexpected(Error{ErrorKind::lua, "Theme must return a table"});
        }
        ThemeLoad result{std::move(base), {}};
        result.theme.source = file;
        const auto name = win::narrow(file.stem().native()).value_or("custom");
        result.theme.name = name;
        lua::merge_theme_definition(definition.as<sol::table>(), result.theme, result.warnings, name);
        return result;
    } catch (const std::exception& exception) {
        return std::unexpected(Error{ErrorKind::lua, "Theme loading failed", exception.what()});
    }
}

Result<std::filesystem::path> find_theme_file(const std::string_view name) {
    if (!valid_theme_name(name)) {
        return std::unexpected(Error{ErrorKind::usage, "Invalid theme name: " + std::string(name)});
    }
    auto wide = win::widen(name);
    if (!wide) return std::unexpected(wide.error());
    auto directories = theme_directories();
    if (!directories) return std::unexpected(directories.error());
    std::error_code error;
    for (const auto& directory : *directories) {
        const auto candidate = directory / (*wide + L".lua");
        if (std::filesystem::is_regular_file(candidate, error)) return candidate;
        error.clear();
    }
    return std::unexpected(Error{ErrorKind::not_found, "Theme was not found: " + std::string(name)});
}

Result<std::vector<std::string>> available_themes() {
    std::set<std::string> names{"default"};
    auto directories = theme_directories();
    if (!directories) return std::unexpected(directories.error());
    for (const auto& directory : *directories) {
        std::error_code error;
        if (!std::filesystem::exists(directory, error)) continue;
        for (std::filesystem::directory_iterator iterator(directory, error), end;
             !error && iterator != end; iterator.increment(error)) {
            if (iterator->is_regular_file(error) && iterator->path().extension() == L".lua") {
                if (auto name = win::narrow(iterator->path().stem().native());
                    name && valid_theme_name(*name)) names.insert(*name);
            }
        }
        if (error) return std::unexpected(Error{ErrorKind::io, "Could not list themes", error.message()});
    }
    return std::vector<std::string>(names.begin(), names.end());
}

} // namespace atlas::terminal
