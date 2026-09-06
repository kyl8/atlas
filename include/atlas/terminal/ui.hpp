#pragma once

#include "atlas/error.hpp"
#include "atlas/terminal/terminal.hpp"

#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::terminal {

enum class OutputMode { automatic, plain, json };

struct OutputOptions {
    OutputMode mode{OutputMode::automatic};
    ColorMode color{ColorMode::automatic};
    bool quiet{};
    bool raw{};
    bool verbose{};
    bool debug{};
};

struct Segment {
    std::string text;
    StyleToken style{StyleToken::primary};
};
using Line = std::vector<Segment>;

struct Identity {
    std::string title;
    std::string subtitle;
    std::vector<std::pair<std::string, std::string>> facts;
    bool show_palette{true};
    struct Section {
        std::string title;
        std::vector<std::pair<std::string, std::string>> entries;
        std::string detail;
        std::string body;
        StyleToken body_style{StyleToken::primary};
    };
    std::vector<Section> sections;
};

struct TableColumn {
    TableColumn(std::string heading, StyleToken style = StyleToken::primary);
    std::string heading;
    StyleToken style{StyleToken::primary};
    std::size_t minimum_width{3};
    std::size_t maximum_width{80};
    int priority{100};
    bool flexible{true};
    bool right_aligned{};
};

struct TableCell {
    std::string text;
    std::optional<StyleToken> style;
};

struct Table {
    std::vector<TableColumn> columns;
    std::vector<std::vector<TableCell>> rows;
    std::string empty_message{"No results"};
    std::optional<std::size_t> compact_primary;
};

struct TreeNode {
    std::string name;
    std::string path;
    bool directory{};
    std::optional<std::uint64_t> size;
    std::optional<std::time_t> modified;
    std::vector<TreeNode> children;
};

struct Tree {
    std::string root;
    std::vector<TreeNode> children;
};

class Spinner;
class Progress;

class Ui {
public:
    explicit Ui(OutputOptions options, Theme theme = default_theme());

    [[nodiscard]] bool human() const noexcept;
    [[nodiscard]] bool json() const noexcept;
    [[nodiscard]] bool raw() const noexcept;
    [[nodiscard]] bool quiet() const noexcept;
    [[nodiscard]] bool interactive_progress() const noexcept;
    [[nodiscard]] std::size_t width() const noexcept;
    [[nodiscard]] bool unicode() const noexcept;
    [[nodiscard]] bool interactive() const noexcept;
    [[nodiscard]] std::size_t height() const noexcept;
    [[nodiscard]] bool color() const noexcept;
    [[nodiscard]] const Capabilities& capabilities() const noexcept;
    [[nodiscard]] const Theme& theme() const noexcept { return theme_; }
    [[nodiscard]] const OutputOptions& options() const noexcept { return options_; }
    void set_theme(Theme theme);

    void data(std::string_view text);
    void data_line(std::string_view text);
    [[nodiscard]] std::string render_fragment(std::string_view text, const TextStyle& style) const;
    void line(std::span<const Segment> segments, Stream stream = Stream::output);
    void heading(std::string_view title, std::string_view subject = {});
    void section(std::string_view title, std::string_view detail = {});
    void identity(const Identity& value);
    void paragraph(std::string_view text, StyleToken style = StyleToken::secondary);
    void summary(std::size_t count, std::string_view noun, std::chrono::nanoseconds elapsed);
    void progress_sample(std::uint64_t current, std::uint64_t total, std::string_view label);
    [[nodiscard]] std::string compose(std::span<const Segment> segments) const;
    [[nodiscard]] std::string palette_line(std::size_t available) const;
    void refresh_size();
    void screen(bool enter);
    void replace_screen(std::string_view frame);
    void info(std::string_view message);
    void success(std::string_view message);
    void warning(std::string_view message);
    void error(std::string_view message, std::string_view detail = {});
    void diagnostic(std::string_view message);
    void muted(std::string_view message);
    void semantic_line(StyleToken token, std::string_view message);
    void table(const Table& value);
    void tree(const Tree& value);
    void list(std::span<const std::vector<TableCell>> rows, std::size_t label_width = 0);
    void swatches(std::span<const Rgb> colors);
    void error(const Error& value);
    std::optional<std::string> prompt(std::string_view label);
    [[nodiscard]] std::unique_ptr<Spinner> spinner(std::string message);
    [[nodiscard]] std::unique_ptr<Progress> progress(std::string message);

    void progress_frame(std::string_view symbol, std::string_view message);
    void clear_progress();

private:
    friend class Progress;
    std::string render(std::string_view text, StyleToken token, Stream stream) const;
    std::string symbol(std::string_view unicode_value, std::string_view fallback) const;
    void status(StyleToken token, std::string_view unicode_symbol,
                std::string_view fallback_symbol, std::string_view message);

    OutputOptions options_;
    Theme theme_;
    Terminal terminal_;
    bool content_written_{};
};

class Spinner {
public:
    Spinner(Ui& ui, std::string message);
    ~Spinner();
    Spinner(const Spinner&) = delete;
    Spinner& operator=(const Spinner&) = delete;
    void finish(std::string_view message = {});

private:
    struct State;
    std::unique_ptr<State> state_;
};

class Progress {
public:
    Progress(Ui& ui, std::string message);
    ~Progress();
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;
    void set_total(std::uint64_t total);
    void set_current(std::uint64_t current);
    void set_speed(double bytes_per_second);
    void finish(std::string_view message = {});

private:
    void render(bool force);
    Ui& ui_;
    std::string message_;
    std::uint64_t total_{};
    std::uint64_t current_{};
    double speed_{};
    bool finished_{};
    std::chrono::steady_clock::time_point last_render_{};
};

} // namespace atlas::terminal
