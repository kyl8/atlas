#include "atlas/terminal/ui.hpp"

#include "atlas/terminal/formatter.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
#include <condition_variable>

namespace atlas::terminal {
namespace {

std::size_t table_width(const std::vector<std::size_t>& widths, const std::vector<bool>& active) {
    std::size_t width = 0;
    std::size_t count = 0;
    for (std::size_t index = 0; index < widths.size(); ++index) {
        if (!active[index]) continue;
        width += widths[index];
        ++count;
    }
    return width + (count > 0 ? (count - 1) * 2 : 0);
}

std::string indent_lines(const std::string_view text) {
    std::string result = "  ";
    result.reserve(text.size() + 16);
    for (std::size_t index = 0; index < text.size(); ++index) {
        result.push_back(text[index]);
        if (text[index] == '\n' && index + 1 < text.size()) result += "  ";
    }
    return result;
}

} // namespace

TableColumn::TableColumn(std::string label, const StyleToken token)
    : heading(std::move(label)), style(token) {
    switch (token) {
        case StyleToken::path: case StyleToken::directory:
            minimum_width = 16; maximum_width = 240; break;
        case StyleToken::filename:
            minimum_width = 12; maximum_width = 36; break;
        case StyleToken::command:
            minimum_width = 8; maximum_width = 32; break;
        case StyleToken::date:
            minimum_width = 12; maximum_width = 12; priority = 40; flexible = false; break;
        case StyleToken::size:
            minimum_width = 8; maximum_width = 10; priority = 60; flexible = false; right_aligned = true; break;
        case StyleToken::muted:
            minimum_width = 6; maximum_width = 10; priority = 20; flexible = false; break;
        case StyleToken::secondary:
            minimum_width = 8; maximum_width = 20; priority = 30; break;
        case StyleToken::success: case StyleToken::warning: case StyleToken::error:
            minimum_width = 6; maximum_width = 12; flexible = false; break;
        default: break;
    }
}

Ui::Ui(OutputOptions options, Theme theme)
    : options_(options), theme_(std::move(theme)), terminal_(
        options.mode == OutputMode::automatic && !options.raw ? options.color : ColorMode::never) {}

bool Ui::human() const noexcept {
    return options_.mode == OutputMode::automatic && !options_.raw &&
           (terminal_.capabilities(Stream::output).is_tty || options_.color == ColorMode::always);
}

bool Ui::json() const noexcept { return options_.mode == OutputMode::json; }
bool Ui::raw() const noexcept { return options_.raw; }
bool Ui::quiet() const noexcept { return options_.quiet; }
bool Ui::interactive_progress() const noexcept {
    return human() && !options_.quiet && terminal_.capabilities(Stream::output).is_tty &&
           terminal_.capabilities(Stream::error).is_tty;
}
std::size_t Ui::width() const noexcept { return terminal_.capabilities(Stream::output).width; }
bool Ui::unicode() const noexcept { return terminal_.capabilities(Stream::output).unicode; }
bool Ui::color() const noexcept { return terminal_.capabilities(Stream::output).color != ColorLevel::none; }
std::size_t Ui::height() const noexcept { return terminal_.capabilities(Stream::output).height; }
const Capabilities& Ui::capabilities() const noexcept { return terminal_.capabilities(Stream::output); }
bool Ui::interactive() const noexcept {
    const auto& caps = terminal_.capabilities(Stream::output);
    return human() && !quiet() && caps.is_tty && caps.vt && caps.interactive_input;
}
void Ui::refresh_size() { terminal_.refresh_size(); }
void Ui::screen(const bool enter) { terminal_.screen(enter); }
void Ui::replace_screen(const std::string_view frame) { terminal_.replace_screen(frame); }
void Ui::set_theme(Theme theme) { theme_ = std::move(theme); }

std::string Ui::render(const std::string_view text, const StyleToken token, const Stream stream) const {
    return terminal_.styled(sanitize_text(text), theme_[token], stream);
}

std::string Ui::symbol(const std::string_view unicode_value, const std::string_view fallback) const {
    return unicode() ? std::string(unicode_value) : std::string(fallback);
}

void Ui::data(const std::string_view text) { content_written_ = true; terminal_.write(Stream::output, text); }
void Ui::data_line(const std::string_view text) {
    data(std::string(text) + '\n');
}

std::string Ui::render_fragment(const std::string_view text, const TextStyle& style) const {
    return terminal_.styled(text, style, Stream::output);
}

void Ui::line(const std::span<const Segment> segments, const Stream stream) {
    std::string output;
    for (const auto& segment : segments) output += render(segment.text, segment.style, stream);
    output.push_back('\n');
    terminal_.write(stream, output);
    if (stream == Stream::output) content_written_ = true;
}

void Ui::heading(const std::string_view title, const std::string_view subject) {
    if (quiet()) return;
    if (!human()) {
        data_line(subject.empty() ? std::string(title) : std::string(title) + "  " + std::string(subject));
        data_line("");
        return;
    }
    std::string line = content_written_ ? "\n" : "";
    const auto available = width() > 4 ? width() - 4 : 1;
    const auto title_text = ellipsize(sanitize_text(title), subject.empty() ? available : available / 2);
    line += "  " + render(title_text, StyleToken::heading, Stream::output);
    if (!subject.empty()) line += "  " + render(ellipsize(sanitize_text(subject),
        available > display_width(title_text) + 2 ? available - display_width(title_text) - 2 : 1),
        StyleToken::highlight, Stream::output);
    line += "\n\n";
    terminal_.write(Stream::output, line);
    content_written_ = true;
}

void Ui::status(const StyleToken token, const std::string_view unicode_symbol,
                const std::string_view fallback_symbol, const std::string_view message) {
    if (quiet()) return;
    const auto marker = terminal_.capabilities(Stream::error).unicode ? unicode_symbol : fallback_symbol;
    const auto& caps = terminal_.capabilities(Stream::error);
    const auto lines = caps.is_tty ? wrap_text(message, caps.width > 4 ? caps.width - 4 : 1)
                                  : std::vector<std::string>{sanitize_text(message)};
    std::string output;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        output += i == 0 ? render(marker, token, Stream::error) + " " : "  ";
        output += render(lines[i], StyleToken::primary, Stream::error) + "\n";
    }
    terminal_.write(Stream::error, output);
}

void Ui::info(const std::string_view message) { status(StyleToken::info, "→", ">", message); }
void Ui::success(const std::string_view message) { status(StyleToken::success, "✓", "+", message); }
void Ui::warning(const std::string_view message) { status(StyleToken::warning, "!", "!", message); }

void Ui::error(const std::string_view message, const std::string_view detail) {
    const auto marker = terminal_.capabilities(Stream::error).unicode ? "✗" : "x";
    const auto& caps = terminal_.capabilities(Stream::error);
    const auto lines = caps.is_tty ? wrap_text(message, caps.width > 4 ? caps.width - 4 : 1)
                                  : std::vector<std::string>{sanitize_text(message)};
    std::string output;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        output += i == 0 ? render(marker, StyleToken::error, Stream::error) + " " : "  ";
        output += render(lines[i], StyleToken::primary, Stream::error) + "\n";
    }
    terminal_.write(Stream::error, output);
    if (!detail.empty()) {
        terminal_.write(Stream::error, "\n" + render(indent_lines(detail), StyleToken::muted, Stream::error) + "\n");
    }
}

void Ui::diagnostic(const std::string_view message) {
    if (!options_.verbose || quiet()) return;
    terminal_.write(Stream::error, render("  " + std::string(message), StyleToken::muted, Stream::error) + "\n");
}

void Ui::muted(const std::string_view message) {
    if (!human() || quiet()) return;
    terminal_.write(Stream::error, render(message, StyleToken::muted, Stream::error) + "\n");
}

void Ui::semantic_line(const StyleToken token, const std::string_view message) {
    data_line(human() ? render(message, token, Stream::output) : std::string(message));
}

void Ui::table(const Table& value) {
    if (value.rows.empty()) {
        if (human() && !quiet()) data_line("  " + render(value.empty_message, StyleToken::muted, Stream::output));
        return;
    }
    if (!human()) {
        for (const auto& row : value.rows) {
            std::string line;
            for (std::size_t column = 0; column < row.size(); ++column) {
                if (column != 0) line.push_back('\t');
                line += row[column].text;
            }
            data_line(line);
        }
        return;
    }

    const auto column_count = value.columns.size();
    if (column_count == 0) return;
    if (width() < 72 && value.rows.front().size() > 1) {
        const auto primary = value.compact_primary.value_or(0);
        for (const auto& row : value.rows) {
            if (primary >= row.size()) continue;
            semantic_line(row[primary].style.value_or(StyleToken::filename), "  " + ellipsize(sanitize_text(row[primary].text), width() - 4));
            for (std::size_t column = 0; column < row.size() && column < column_count; ++column) {
                if (column == primary || value.columns[column].priority < 60) continue;
                const auto style = row[column].style.value_or(value.columns[column].style);
                const auto prefix = value.columns[column].heading + "  ";
                const auto available = width() > 4 + display_width(prefix)
                    ? width() - 4 - display_width(prefix) : std::size_t{1};
                const auto clean = sanitize_text(row[column].text);
                const auto visible = style == StyleToken::path || style == StyleToken::directory
                    ? truncate_path(clean, available) : ellipsize(clean, available);
                semantic_line(style, "    " + prefix + visible);
            }
            data_line("");
        }
        return;
    }
    std::vector<bool> active(column_count, true);
    std::vector<std::size_t> widths(column_count);
    for (std::size_t column = 0; column < column_count; ++column) {
        widths[column] = (std::max)(value.columns[column].minimum_width,
                                   (std::min)(value.columns[column].maximum_width,
                                              display_width(value.columns[column].heading)));
        for (const auto& row : value.rows) {
            if (column < row.size()) widths[column] = (std::max)(widths[column],
                (std::min)(value.columns[column].maximum_width, display_width(row[column].text)));
        }
    }
    const auto available = width() > 4 ? width() - 4 : width();
    const auto minimum_table_width = [&] {
        std::vector<std::size_t> minimums(column_count);
        for (std::size_t column = 0; column < column_count; ++column) {
            minimums[column] = value.columns[column].minimum_width;
        }
        return table_width(minimums, active);
    };
    while (minimum_table_width() > available) {
        std::optional<std::size_t> removable;
        for (std::size_t column = 0; column < column_count; ++column) {
            if (active[column] && value.columns[column].priority < 100 &&
                (!removable || value.columns[column].priority < value.columns[*removable].priority)) {
                removable = column;
            }
        }
        if (!removable) break;
        active[*removable] = false;
    }
    while (table_width(widths, active) > available) {
        std::optional<std::size_t> widest;
        for (std::size_t column = 0; column < column_count; ++column) {
            if (active[column] && value.columns[column].flexible &&
                widths[column] > value.columns[column].minimum_width &&
                (!widest || widths[column] > widths[*widest])) widest = column;
        }
        if (widest) {
            --widths[*widest];
            continue;
        }
        std::optional<std::size_t> removable;
        for (std::size_t column = 0; column < column_count; ++column) {
            if (active[column] && value.columns[column].priority < 100 &&
                (!removable || value.columns[column].priority < value.columns[*removable].priority)) {
                removable = column;
            }
        }
        if (!removable) break;
        active[*removable] = false;
    }

    std::string output;
    auto append_row = [&](const std::vector<TableCell>& row, const bool header) {
        output += "  ";
        bool first = true;
        for (std::size_t column = 0; column < column_count; ++column) {
            if (!active[column]) continue;
            if (!first) output += "  ";
            const auto text = header ? value.columns[column].heading :
                (column < row.size() ? row[column].text : std::string{});
            const auto style = header ? StyleToken::table_header :
                (column < row.size() && row[column].style ? *row[column].style : value.columns[column].style);
            const auto clean = sanitize_text(text);
            const auto visible = style == StyleToken::path || style == StyleToken::directory
                ? truncate_path(clean, widths[column]) : clean;
            output += render(pad(visible, widths[column], value.columns[column].right_aligned), style, Stream::output);
            first = false;
        }
        while (!output.empty() && output.back() == ' ') output.pop_back();
        output.push_back('\n');
    };
    append_row({}, true);
    output += '\n';
    for (const auto& row : value.rows) append_row(row, false);
    data(output);
}

void Ui::tree(const Tree& value) {
    if (!human()) {
        std::string output;
        const auto append = [&](const auto& self, const TreeNode& node) -> void {
            output += node.path;
            output.push_back('\n');
            for (const auto& child : node.children) self(self, child);
        };
        for (const auto& node : value.children) append(append, node);
        data(output);
        return;
    }
    semantic_line(StyleToken::path, "  " + truncate_path(value.root, width() > 4 ? width() - 4 : 1));
    std::string output;
    std::vector<bool> branches;
    const auto append = [&](const auto& self, const TreeNode& node, const bool last) -> void {
        output += "  ";
        for (const auto continued : branches) output += continued ? (unicode() ? "│  " : "|  ") : "   ";
        output += unicode() ? (last ? "└─ " : "├─ ") : (last ? "`- " : "|- ");
        const auto prefix_width = std::size_t{5} + branches.size() * 3;
        const auto detail = !node.directory && node.size ? format_filesize(*node.size) : std::string{};
        const auto detail_width = detail.empty() ? std::size_t{} : display_width(detail) + 2;
        const auto name_width = width() > prefix_width + detail_width ? width() - prefix_width - detail_width : 1;
        output += render(ellipsize(node.name, name_width),
                         node.directory ? StyleToken::directory : StyleToken::filename, Stream::output);
        if (!detail.empty() && width() >= 64) {
            const auto used = prefix_width + (std::min)(display_width(node.name), name_width) + detail_width;
            if (used < width()) output.append(width() - used, ' ');
            output += "  " + render(detail, StyleToken::size, Stream::output);
        }
        output.push_back('\n');
        branches.push_back(!last);
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            self(self, node.children[index], index + 1 == node.children.size());
        }
        branches.pop_back();
    };
    for (std::size_t index = 0; index < value.children.size(); ++index) {
        append(append, value.children[index], index + 1 == value.children.size());
    }
    data(output);
}

void Ui::list(const std::span<const std::vector<TableCell>> rows, std::size_t label_width) {
    if (label_width == 0) {
        for (const auto& row : rows) if (!row.empty()) label_width = (std::max)(label_width, display_width(row.front().text));
    }
    if (human()) label_width = (std::min)(label_width, width() / 2);
    std::string output;
    for (const auto& row : rows) {
        if (row.empty()) continue;
        if (human() && row.size() > 1) {
            const auto narrow = width() < label_width + 26 || display_width(row[0].text) > label_width;
            const auto label = sanitize_text(row[0].text);
            std::string description;
            for (std::size_t i = 1; i < row.size(); ++i) { if (i > 1) description += "  "; description += row[i].text; }
            const auto indent = narrow ? std::size_t{4} : label_width + 4;
            const auto wrapped = wrap_text(description, width() > indent + 1 ? width() - indent - 1 : 10);
            if (narrow) {
                for (const auto& line : wrap_text(label, width() - 4)) output += "  " + render(line, row[0].style.value_or(StyleToken::command), Stream::output) + "\n";
            } else output += "  " + render(pad(label, label_width), row[0].style.value_or(StyleToken::command), Stream::output) + "  ";
            for (std::size_t i = 0; i < wrapped.size(); ++i) {
                if (narrow || i > 0) output.append(indent, ' ');
                output += render(wrapped[i], row[1].style.value_or(StyleToken::secondary), Stream::output) + "\n";
            }
            if (narrow) output += '\n';
            continue;
        }
        output += human() ? "  " + render(pad(row[0].text, label_width), row[0].style.value_or(StyleToken::primary), Stream::output)
                          : row[0].text;
        for (std::size_t index = 1; index < row.size(); ++index) {
            output += human() ? "  " + render(row[index].text, row[index].style.value_or(StyleToken::secondary), Stream::output)
                              : "\t" + row[index].text;
        }
        output.push_back('\n');
    }
    data(output);
}

void Ui::swatches(const std::span<const Rgb> colors) {
    if (colors.empty() || !human() || !color()) return;
    std::string output = human() ? "  " : "";
    for (std::size_t index = 0; index < colors.size(); ++index) {
        const auto color = colors[index];
        TextStyle style;
        style.foreground = color;
        style.background = color;
        if (human()) {
            output += terminal_.styled("  ", style, Stream::output);
        } else {
            output += "[]";
            if (index + 1 < colors.size()) output.push_back('\t');
        }
    }
    output.push_back('\n');
    terminal_.write(Stream::output, output);
    content_written_ = true;
}

void Ui::error(const Error& value) {
    std::string detail;
    if (options_.debug || value.kind == ErrorKind::lua) detail = value.detail;
    if (options_.debug && !value.debug_detail.empty() && value.debug_detail != value.detail) {
        if (!detail.empty()) detail += "\n\n";
        detail += value.debug_detail;
    }
    if (options_.debug && value.native_code != 0) {
        if (!detail.empty()) detail += " · ";
        detail += "native code " + std::to_string(value.native_code);
    }
    error(value.message, detail);
}

std::optional<std::string> Ui::prompt(const std::string_view label) {
    terminal_.write(Stream::output, render(label, StyleToken::secondary, Stream::output) +
                    render(" › ", StyleToken::accent, Stream::output), true);
    std::string value;
    if (!std::getline(std::cin, value)) return std::nullopt;
    return value;
}

std::unique_ptr<Spinner> Ui::spinner(std::string message) {
    return std::make_unique<Spinner>(*this, std::move(message));
}

std::unique_ptr<Progress> Ui::progress(std::string message) {
    return std::make_unique<Progress>(*this, std::move(message));
}

void Ui::progress_frame(const std::string_view marker, const std::string_view message) {
    if (!interactive_progress()) return;
    const auto message_width = width() > display_width(marker) + 2
        ? width() - display_width(marker) - 2
        : std::size_t{1};
    const auto visible_message = ellipsize(message, message_width);
    terminal_.clear_line(Stream::error);
    terminal_.write(Stream::error, render(marker, StyleToken::spinner, Stream::error) + " " +
                    render(visible_message, StyleToken::primary, Stream::error), true);
}

void Ui::clear_progress() { if (interactive_progress()) terminal_.clear_line(Stream::error); }

struct Spinner::State {
    Ui& ui;
    std::string message;
    std::jthread thread;
    std::atomic_bool displayed{};
    std::atomic_bool finished{};

    State(Ui& target, std::string text) : ui(target), message(std::move(text)) {
        if (!ui.interactive_progress()) return;
        thread = std::jthread([this](const std::stop_token stop) {
            constexpr std::string_view frames[]{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            wake.wait_for(lock, stop, std::chrono::milliseconds(140), [] { return false; });
            std::size_t frame = 0;
            while (!stop.stop_requested()) {
                displayed = true;
                ui.progress_frame(ui.unicode() ? frames[frame++ % std::size(frames)] : "-", message);
                wake.wait_for(lock, stop, std::chrono::milliseconds(80), [] { return false; });
            }
        });
    }
};

Spinner::Spinner(Ui& ui, std::string message) : state_(std::make_unique<State>(ui, std::move(message))) {}
Spinner::~Spinner() { finish(); }

void Spinner::finish(const std::string_view message) {
    if (!state_ || state_->finished.exchange(true)) return;
    state_->thread.request_stop();
    if (state_->thread.joinable()) state_->thread.join();
    if (state_->displayed) state_->ui.clear_progress();
    if (!message.empty()) state_->ui.success(message);
}

Progress::Progress(Ui& ui, std::string message) : ui_(ui), message_(std::move(message)) {}
Progress::~Progress() { if (!finished_) ui_.clear_progress(); }
void Progress::set_total(const std::uint64_t total) { total_ = total; render(false); }
void Progress::set_current(const std::uint64_t current) { current_ = current; render(false); }
void Progress::set_speed(const double bytes_per_second) {
    speed_ = std::isfinite(bytes_per_second) && bytes_per_second > 0 && bytes_per_second < 1e18
        ? bytes_per_second : 0;
    render(false);
}

void Progress::render(const bool force) {
    if (!ui_.interactive_progress() || finished_) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && last_render_ != std::chrono::steady_clock::time_point{} &&
        now - last_render_ < std::chrono::milliseconds(80)) return;
    last_render_ = now;
    if (total_ == 0) {
        ui_.progress_frame("-", message_);
        return;
    }
    const auto ratio = (std::min)(1.0, static_cast<double>(current_) / static_cast<double>(total_));
    const auto available = ui_.width() > 4 ? ui_.width() - 4 : 1;
    const auto label = ellipsize(sanitize_text(message_), (std::min)(std::size_t{22}, available / 3));
    const auto bar_width = (std::min)(std::size_t{24}, available > display_width(label) + 10
        ? available - display_width(label) - 10 : std::size_t{1});
    const auto filled = static_cast<std::size_t>(std::round(ratio * bar_width));
    const auto full = ui_.unicode() ? "█" : "#";
    const auto empty = ui_.unicode() ? "░" : "-";
    std::string bar;
    for (std::size_t index = 0; index < filled; ++index) bar += full;
    std::string remainder;
    for (std::size_t index = filled; index < bar_width; ++index) remainder += empty;
    std::string line = ui_.render(label, StyleToken::primary, Stream::error) + "  " +
        ui_.render(bar, StyleToken::progress_filled, Stream::error) +
        ui_.render(remainder, StyleToken::progress_empty, Stream::error) + "  " +
        ui_.render(std::to_string(static_cast<int>(ratio * 100.0)) + "%", StyleToken::number, Stream::error);
    auto used = display_width(label) + bar_width + 9;
    const auto append = [&](const std::string& text) {
        const auto length = display_width(text) + 2;
        if (used + length <= available) { line += "  " + text; used += length; }
    };
    append(format_filesize(current_) + " / " + format_filesize(total_));
    if (speed_ > 0.0) {
        append(format_filesize(static_cast<std::uint64_t>(speed_)) + "/s");
        if (current_ < total_) {
            const auto eta = std::chrono::duration<double>((total_ - current_) / speed_);
            if (eta.count() < 1e8) append("ETA " + format_duration(std::chrono::duration_cast<std::chrono::nanoseconds>(eta)));
        }
    }
    ui_.terminal_.clear_line(Stream::error);
    ui_.terminal_.write(Stream::error, line, true);
}

void Progress::finish(const std::string_view message) {
    if (finished_) return;
    render(true);
    finished_ = true;
    ui_.clear_progress();
    if (!message.empty()) ui_.success(message);
}

} // namespace atlas::terminal
