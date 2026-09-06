#include "atlas/lua/theme.hpp"
#include "atlas/platform/paths.hpp"
#include "atlas/platform/utf.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>

namespace atlas::lua {
namespace {

constexpr terminal::Rgb rgb(const std::uint32_t value) {
    return terminal::Rgb{static_cast<std::uint8_t>((value >> 16) & 0xff),
                         static_cast<std::uint8_t>((value >> 8) & 0xff),
                         static_cast<std::uint8_t>(value & 0xff)};
}

std::optional<terminal::Rgb> parse_color(const std::string_view value) {
    if (value.size() != 7 || value.front() != '#') return std::nullopt;
    std::uint32_t color = 0;
    const auto parsed = std::from_chars(value.data() + 1, value.data() + value.size(), color, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
    return rgb(color);
}

void invalid_value(std::vector<std::string>& warnings, const std::string_view source,
                   const std::string_view field, const std::string_view value) {
    warnings.emplace_back("Invalid " + std::string(field) + " '" + std::string(value) +
                          "' in theme '" + std::string(source) + "'; using the default value");
}

void merge_style(const sol::table& source_style, terminal::TextStyle& style,
                 std::vector<std::string>& warnings, const std::string_view source,
                 const std::string_view token, const terminal::Theme& theme) {
    for (const auto field : {"fg", "bg"}) {
        const sol::object value = source_style[field];
        if (value == sol::nil) continue;
        if (!value.is<std::string>()) {
            invalid_value(warnings, source, std::string(token) + "." + field, "<non-string>");
            continue;
        }
        const auto text = value.as<std::string>();
        auto color = parse_color(text);
        if (!color) {
            const auto found = std::ranges::find(theme.named_palette, text, &std::pair<std::string, terminal::Rgb>::first);
            if (found != theme.named_palette.end()) color = found->second;
        }
        if (!color) {
            invalid_value(warnings, source, std::string(token) + "." + field, text);
            continue;
        }
        if (field == std::string_view("fg")) style.foreground = *color;
        else style.background = *color;
    }
    struct Attribute { std::string_view name; bool terminal::TextStyle::*member; };
    constexpr Attribute attributes[]{
        {"bold", &terminal::TextStyle::bold}, {"dim", &terminal::TextStyle::dim},
        {"italic", &terminal::TextStyle::italic}, {"underline", &terminal::TextStyle::underline},
    };
    for (const auto& attribute : attributes) {
        const sol::object value = source_style[attribute.name];
        if (value == sol::nil) continue;
        if (!value.is<bool>()) {
            invalid_value(warnings, source, std::string(token) + "." + std::string(attribute.name), "<non-boolean>");
            continue;
        }
        style.*(attribute.member) = value.as<bool>();
    }
}

} // namespace

void merge_theme_styles(const sol::table& styles, terminal::Theme& theme,
                        std::vector<std::string>& warnings, const std::string_view source) {
    for (const auto& [key, unused] : styles) {
        static_cast<void>(unused);
        if (key.is<std::string>()) {
            const auto name = key.as<std::string>();
            if (!terminal::style_token(name)) {
                warnings.emplace_back("Unknown style token '" + name + "' in theme '" + std::string(source) + "'");
            }
        }
    }
    for (std::size_t index = 0; index < static_cast<std::size_t>(terminal::StyleToken::count); ++index) {
        const auto token = static_cast<terminal::StyleToken>(index);
        const auto name = std::string(terminal::style_token_name(token));
        sol::object value = styles[name];
        if (value == sol::nil && token == terminal::StyleToken::progress_filled) value = styles["progress_fill"];
        if (value == sol::nil && token == terminal::StyleToken::progress_empty) value = styles["progress_track"];
        if (value == sol::nil && token == terminal::StyleToken::badge) value = styles["tag"];
        if (value == sol::nil) continue;
        if (!value.is<sol::table>()) {
            warnings.emplace_back("Style '" + name + "' in theme '" + std::string(source) + "' must be a table");
            continue;
        }
        merge_style(value.as<sol::table>(), theme[token], warnings, source, name, theme);
        theme.custom_styles[index] = true;
    }
    using enum terminal::StyleToken;
    for (const auto& [target, parent] : {std::pair{heading, accent}, {subheading, secondary},
            {directory, path}, {filename, primary}, {selection, highlight}, {keybind, command}, {badge, accent}}) {
        if (!theme.custom_styles[static_cast<std::size_t>(target)]) theme[target] = theme[parent];
    }
    if (!theme.custom_styles[static_cast<std::size_t>(heading)]) theme[heading].bold = true;
    if (!theme.custom_styles[static_cast<std::size_t>(subheading)]) theme[subheading].bold = true;
    if (!theme.custom_styles[static_cast<std::size_t>(filename)]) theme[filename].bold = true;
}

void merge_theme_art(const sol::table& art, terminal::Theme& theme,
                     std::vector<std::string>& warnings, const std::string_view source) {
    const sol::object logo = art["logo"];
    if (logo.is<sol::table>()) {
        theme.art.kind = terminal::ArtKind::ascii;
        theme.art.lines.clear();
        const auto lines = logo.as<sol::table>();
        for (std::size_t index = 1; index <= lines.size(); ++index) {
            const sol::object line = lines[index];
            if (!line.is<std::string>()) {
                warnings.emplace_back("Theme '" + std::string(source) + "' art.logo entries must be strings");
                continue;
            }
            if (theme.art.lines.size() < 40) theme.art.lines.push_back(line.as<std::string>().substr(0, 4096));
        }
    } else if (logo != sol::nil) {
        warnings.emplace_back("Theme '" + std::string(source) + "' field 'art.logo' must be a table");
    }

    const sol::object palette = art["palette"];
    if (palette.is<sol::table>()) {
        theme.palette.clear();
        const auto colors = palette.as<sol::table>();
        for (std::size_t index = 1; index <= colors.size(); ++index) {
            const sol::object color = colors[index];
            if (!color.is<std::string>()) {
                warnings.emplace_back("Theme '" + std::string(source) + "' art.palette entries must be colors");
                continue;
            }
            const auto text = color.as<std::string>();
            if (const auto parsed = parse_color(text)) theme.palette.push_back(*parsed);
            else invalid_value(warnings, source, "art.palette", text);
        }
    } else if (palette != sol::nil) {
        warnings.emplace_back("Theme '" + std::string(source) + "' field 'art.palette' must be a table");
    }

    // Legacy art.image remains inert. Images require the explicit type="image".
    merge_art(sol::make_object(art.lua_state(), art), theme.art, warnings, theme.source.parent_path());
}

void merge_art(const sol::object& definition, terminal::ArtOptions& art,
               std::vector<std::string>& warnings, const std::filesystem::path& base) {
    if (definition == sol::nil) return;
    if (definition.is<bool>() && !definition.as<bool>()) { art.kind = terminal::ArtKind::none; return; }
    if (!definition.is<sol::table>()) { warnings.emplace_back("UI art must be a table or false"); return; }
    const auto table = definition.as<sol::table>();
    const sol::optional<bool> enabled = table["enabled"];
    if (enabled && !*enabled) { art.kind = terminal::ArtKind::none; return; }
    if (const sol::optional<std::string> type = table["type"]) {
        if (*type == "ascii") art.kind = terminal::ArtKind::ascii;
        else if (*type == "unicode") art.kind = terminal::ArtKind::unicode;
        else if (*type == "image") art.kind = terminal::ArtKind::image;
        else if (*type == "builtin") art.kind = terminal::ArtKind::builtin;
        else if (*type == "none") art.kind = terminal::ArtKind::none;
        else warnings.emplace_back("Unknown art type '" + *type + "'; using fallback");
    }
    auto source = table.get<sol::optional<std::string>>("source");
    if (!source) source = table.get<sol::optional<std::string>>("path");
    if (source) {
        auto wide = win::widen(*source);
        if (wide) {
            auto path = std::filesystem::path(*wide);
            if (!source->starts_with('~') && source->find('%') == std::string::npos && path.is_relative()) path = base / path;
            auto resolved = win::resolve_path(win::narrow(path.native()).value_or(*source));
            if (resolved) { art.source = *resolved; art.lines.clear(); }
            else warnings.push_back(resolved.error().message);
        }
    }
    const sol::object lines = table["inline"];
    if (lines.is<std::string>()) {
        art.lines.clear();
        const auto text = lines.as<std::string>();
        std::size_t start = 0;
        while (start <= text.size() && art.lines.size() < 40) {
            const auto end = text.find('\n', start);
            auto line = text.substr(start, end == std::string::npos ? text.size() - start : end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            art.lines.push_back(line.substr(0, 4096));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        art.source.clear();
    } else if (lines.is<sol::table>()) {
        art.lines.clear();
        const auto values = lines.as<sol::table>();
        for (std::size_t i = 1; i <= (std::min)(values.size(), std::size_t{40}); ++i) {
            const sol::optional<std::string> line = values[i];
            if (line) art.lines.push_back(line->substr(0, 4096));
        }
        art.source.clear();
    } else if (lines != sol::nil) {
        warnings.emplace_back("art.inline must be a string or a table of lines");
    }
    const sol::object size = table["size"];
    if (size.is<std::string>()) {
        if (size.as<std::string>() == "auto") {
            art.automatic_size = true;
            art.width_configured = false;
            art.height_configured = false;
        }
        else warnings.emplace_back("art.size must be 'auto'; use width and height for manual sizing");
    } else if (size != sol::nil) {
        warnings.emplace_back("art.size must be 'auto'");
    }
    const sol::optional<int> width = table["width"];
    const sol::optional<int> height = table["height"];
    if (width || height) {
        art.automatic_size = false;
        art.width_configured = false;
        art.height_configured = false;
    }
    if (width) {
        art.width = static_cast<std::size_t>(std::clamp(*width, 8, 64));
        art.width_configured = true;
    }
    if (height) {
        art.height = static_cast<std::size_t>(std::clamp(*height, 4, 32));
        art.height_configured = true;
    }
    if (const sol::optional<double> ratio = table["cell_ratio"]) {
        if (*ratio >= 0.25 && *ratio <= 1.0) art.cell_ratio = *ratio;
        else warnings.emplace_back("art.cell_ratio must be between 0.25 and 1.0");
    }
    if (const sol::optional<std::string> backend = table["backend"]) {
        if (*backend == "auto") art.backend = terminal::ImageBackend::automatic;
        else if (*backend == "sixel") art.backend = terminal::ImageBackend::sixel;
        else if (*backend == "blocks") art.backend = terminal::ImageBackend::blocks;
        else warnings.emplace_back("art.backend must be 'auto', 'sixel', or 'blocks'");
    }
}

void merge_theme_definition(const sol::table& definition, terminal::Theme& theme,
                            std::vector<std::string>& warnings, const std::string_view source) {
    if (const sol::optional<std::string> name = definition["name"]) theme.name = *name;
    if (const sol::optional<std::string> author = definition["author"]) theme.author = *author;
    const sol::object palette = definition["palette"];
    if (palette.is<sol::table>()) {
        theme.named_palette.clear();
        for (const auto& [key, value] : palette.as<sol::table>()) {
            if (key.get_type() != sol::type::string || value.get_type() != sol::type::string) {
                warnings.emplace_back("palette requires named #RRGGBB colors"); continue;
            }
            if (const auto color = parse_color(value.as<std::string>())) theme.named_palette.emplace_back(key.as<std::string>(), *color);
            else invalid_value(warnings, source, "palette." + key.as<std::string>(), value.as<std::string>());
        }
        std::ranges::sort(theme.named_palette, {}, &std::pair<std::string, terminal::Rgb>::first);
        if (!theme.named_palette.empty()) {
            theme.palette.clear();
            for (const auto& [key, color] : theme.named_palette) { static_cast<void>(key); theme.palette.push_back(color); }
        }
    }
    const sol::object art = definition["art"];
    if (art.is<sol::table>()) merge_theme_art(art.as<sol::table>(), theme, warnings, source);
    else if (art != sol::nil) merge_art(art, theme.art, warnings, theme.source.parent_path());

    const sol::object colors = definition["colors"];
    if (colors.is<sol::table>()) {
        const auto legacy = colors.as<sol::table>();
        for (std::size_t index = 0; index < static_cast<std::size_t>(terminal::StyleToken::count); ++index) {
            const auto token = static_cast<terminal::StyleToken>(index);
            const auto name = std::string(terminal::style_token_name(token));
            const sol::object value = legacy[name];
            if (value == sol::nil) continue;
            if (!value.is<std::string>()) {
                invalid_value(warnings, source, name + ".fg", "<non-string>");
                continue;
            }
            const auto text = value.as<std::string>();
            if (const auto color = parse_color(text)) theme[token].foreground = *color;
            else invalid_value(warnings, source, name + ".fg", text);
        }
    }
    const sol::object styles = definition["styles"];
    if (styles == sol::nil) return;
    if (!styles.is<sol::table>()) {
        warnings.emplace_back("Theme '" + std::string(source) + "' field 'styles' must be a table");
        return;
    }
    merge_theme_styles(styles.as<sol::table>(), theme, warnings, source);
}

} // namespace atlas::lua
