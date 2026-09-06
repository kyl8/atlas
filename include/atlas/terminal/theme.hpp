#pragma once

#include "atlas/error.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::terminal {

enum class StyleToken : std::uint8_t {
    primary,
    secondary,
    accent,
    success,
    warning,
    error,
    info,
    muted,
    subtle,
    path,
    url,
    command,
    number,
    date,
    duration,
    size,
    highlight,
    table_header,
    table_border,
    progress_filled,
    progress_empty,
    spinner,
    heading,
    subheading,
    directory,
    filename,
    selection,
    keybind,
    badge,
    count,
};

struct Rgb {
    std::uint8_t red{};
    std::uint8_t green{};
    std::uint8_t blue{};
    friend bool operator==(const Rgb&, const Rgb&) = default;
};

struct TextStyle {
    std::optional<Rgb> foreground;
    std::optional<Rgb> background;
    bool bold{};
    bool dim{};
    bool italic{};
    bool underline{};
};

enum class ArtKind { builtin, ascii, unicode, image, none };
enum class ImageBackend { automatic, sixel, blocks };

struct ArtOptions {
    ArtKind kind{ArtKind::builtin};
    std::filesystem::path source;
    std::vector<std::string> lines;
    std::size_t width{24};
    std::size_t height{};
    bool automatic_size{};
    bool width_configured{};
    bool height_configured{};
    double cell_ratio{0.5};
    ImageBackend backend{ImageBackend::automatic};
};

struct HomeEntry {
    std::string label;
    std::string value;
};

struct HomeOptions {
    std::string actions_title{"Start here"};
    bool show_default_actions{true};
    std::vector<HomeEntry> facts;
    std::vector<HomeEntry> actions;
};

struct IdentityOptions {
    std::string name{"ATLAS"};
    std::string separator{"  /  "};
    bool show_version{true};
    std::string version{"{version}"};
    std::string explorer{"Command explorer"};
    bool show_palette{true};
};

struct Theme {
    std::array<TextStyle, static_cast<std::size_t>(StyleToken::count)> styles;
    std::array<bool, static_cast<std::size_t>(StyleToken::count)> custom_styles{};
    std::string name{"default"};
    std::string author{"Atlas"};
    std::string tagline{"Personal automation. Windows-native."};
    IdentityOptions identity;
    std::filesystem::path source;
    ArtOptions art;
    HomeOptions home;
    bool user_art_override{};
    std::vector<Rgb> palette;
    std::vector<std::pair<std::string, Rgb>> named_palette;

    [[nodiscard]] const TextStyle& operator[](StyleToken token) const noexcept;
    [[nodiscard]] TextStyle& operator[](StyleToken token) noexcept;
};

struct ThemeLoad {
    Theme theme;
    std::vector<std::string> warnings;
};

Theme default_theme();
std::optional<StyleToken> style_token(std::string_view name);
std::string_view style_token_name(StyleToken token);
bool valid_theme_name(std::string_view name);
Result<ThemeLoad> load_theme_file(const std::filesystem::path& file, Theme base);
Result<std::filesystem::path> find_theme_file(std::string_view name);
Result<std::vector<std::string>> available_themes();

} // namespace atlas::terminal
