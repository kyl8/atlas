#include "atlas/terminal/ui.hpp"
#include "atlas/terminal/formatter.hpp"
#include "atlas/terminal/image.hpp"

#include <algorithm>
#include <fstream>

namespace atlas::terminal {
namespace {

std::vector<std::string> builtin_art() {
    return {
        "          .",
        "          /\\",
        "     .   /  \\   .",
        "        / /\\ \\",
        "   ----/ /--\\ \\----",
        "      /_/    \\_\\",
        "     .          .",
        "      A T L A S",
    };
}

struct RenderedArt {
    std::vector<std::string> lines;
    std::string sixel;
    std::size_t width{};
    std::size_t height{};
};

std::size_t automatic_art_width(const Ui& ui, const std::size_t columns) {
    return ui.width() > 54
        ? (std::min)((std::min)(std::size_t{56}, ui.width() - 50), columns)
        : (std::min)(std::size_t{32}, columns);
}

RenderedArt art_content(Ui& ui, const std::size_t columns,
                        const std::optional<std::size_t> matched_height = std::nullopt) {
    const auto& art = ui.theme().art;
    if (art.kind == ArtKind::none) return {};
    const auto automatic_width = automatic_art_width(ui, columns);
    const auto automatic_height = matched_height
        ? std::clamp(*matched_height, std::size_t{4}, std::size_t{32})
        : (std::min)(std::size_t{24}, (std::max)(std::size_t{6}, ui.height() * 3 / 5));
    const auto width = (std::min)(columns, art.automatic_size ? automatic_width : art.width);
    const auto height = art.automatic_size ? automatic_height :
        (art.height != 0 ? art.height : (std::min)(std::size_t{32},
            (std::max)(std::size_t{4}, ui.height() - 3)));
    if (art.kind == ArtKind::image) {
        ImageOptions options{width, height, std::nullopt, std::nullopt, art.cell_ratio, art.backend};
        if (art.automatic_size && matched_height) {
            options.height = height;
        } else if (!art.automatic_size) {
            if (art.width_configured) options.width = width;
            if (art.height_configured) options.height = art.height;
        }
        auto image = render_image(ui, art.source, options);
        if (image) return {std::move(image->lines), std::move(image->sixel), image->width, image->height};
        ui.warning(image.error().message + "; using the Atlas logo");
    }
    auto lines = art.kind == ArtKind::builtin || art.kind == ArtKind::image ? builtin_art() : art.lines;
    if (!art.source.empty() && art.kind != ArtKind::image) {
        std::error_code error;
        const auto size = std::filesystem::file_size(art.source, error);
        if (error || size > 64 * 1024) {
            ui.warning("Could not read art (limit: 64 KB); using the Atlas logo");
            lines = builtin_art();
        } else {
            std::ifstream input(art.source, std::ios::binary);
            std::string line;
            lines.clear();
            while (lines.size() < height && std::getline(input, line)) lines.push_back(std::move(line));
        }
    }
    if (art.kind == ArtKind::unicode && !ui.unicode()) lines = builtin_art();
    if (lines.size() > height) lines.resize(height);
    for (auto& line : lines) {
        line = ui.render_fragment(ellipsize(sanitize_text(line), width), ui.theme()[StyleToken::accent]);
    }
    std::size_t rendered_width = 0;
    for (const auto& line : lines) rendered_width = (std::max)(rendered_width, display_width(sanitize_text(line)));
    const auto rendered_height = lines.size();
    return {std::move(lines), {}, rendered_width, rendered_height};
}

} // namespace

std::string Ui::compose(const std::span<const Segment> segments) const {
    std::string result;
    for (const auto& segment : segments) result += render(segment.text, segment.style, Stream::output);
    return result;
}

std::string Ui::palette_line(const std::size_t available) const {
    if (!human() || !color() || theme_.palette.empty()) return {};
    const auto count = (std::min)(theme_.palette.size(), available / 2);
    const auto block_width = (std::min)(std::size_t{3}, available / (std::max)(count, std::size_t{1}));
    std::string output;
    for (std::size_t index = 0; index < count; ++index) {
        TextStyle style;
        style.background = theme_.palette[index];
        output += render_fragment(std::string(block_width, ' '), style);
    }
    return output;
}

void Ui::paragraph(const std::string_view text, const StyleToken style) {
    if (!human()) { data_line(text); return; }
    std::string output;
    for (const auto& line : wrap_text(text, width() > 4 ? width() - 4 : 1)) {
        output += "  " + render(line, style, Stream::output) + "\n";
    }
    data(output);
}

void Ui::section(const std::string_view title, const std::string_view detail) {
    if (!human()) { heading(title, detail); return; }
    std::string output = content_written_ ? "\n" : "";
    const auto visible = ellipsize(sanitize_text(title), width() > 8 ? width() - 8 : 1);
    output += "  " + render(visible, StyleToken::subheading, Stream::output);
    const auto remaining = width() > display_width(visible) + 6 ? width() - display_width(visible) - 6 : 0;
    if (!detail.empty() && remaining > 3) {
        output += "  " + render(ellipsize(detail, remaining), StyleToken::muted, Stream::output);
    } else if (remaining > 8) {
        output += "  " + render(std::string((std::min)(remaining, std::size_t{28}), '-'),
                                  StyleToken::table_border, Stream::output);
    }
    data(output + "\n\n");
}

void Ui::identity(const Identity& value) {
    if (!human()) {
        heading(value.title);
        paragraph(value.subtitle);
        for (const auto& [label, text] : value.facts) data_line(label + "\t" + text);
        return;
    }
    const auto requested_art_width = theme_.art.automatic_size ? std::size_t{24} : theme_.art.width;
    const bool room_for_art = width() >= requested_art_width + 46 || height() >= 32;
    const auto available_columns = width() > 6 ? width() - 6 : 1;
    const bool delayed_auto_image = room_for_art && theme_.art.kind == ArtKind::image &&
        theme_.art.automatic_size &&
        width() >= automatic_art_width(*this, available_columns) + 46;
    auto art = delayed_auto_image
        ? RenderedArt{{}, {}, automatic_art_width(*this, available_columns), 1}
        : room_for_art ? art_content(*this, available_columns) : RenderedArt{};
    auto art_width = art.width;
    auto side_by_side = art.height != 0 && width() >= art_width + 46;
    const auto content_margin = delayed_auto_image || !art.sixel.empty()
        ? std::size_t{7} : std::size_t{8};
    const auto content_width = side_by_side ? width() - art_width - content_margin : width() - 4;
    std::vector<std::string> content;
    content.push_back(render(ellipsize(value.title, content_width), StyleToken::heading, Stream::output));
    for (const auto& line : wrap_text(value.subtitle, content_width)) {
        content.push_back(render(line, StyleToken::secondary, Stream::output));
    }
    content.emplace_back();
    std::size_t label_width = 0;
    for (const auto& fact : value.facts) label_width = (std::max)(label_width, display_width(fact.first));
    label_width = (std::min)(label_width, content_width / 3);
    for (const auto& [label, text] : value.facts) {
        content.push_back(render(pad(label, label_width), StyleToken::muted, Stream::output) + "  " +
            render(ellipsize(sanitize_text(text), content_width - label_width - 2),
                   label == "Theme" ? StyleToken::highlight : StyleToken::primary, Stream::output));
    }
    const auto palette = value.show_palette ? palette_line(content_width) : std::string{};
    if (!palette.empty()) { content.emplace_back(); content.push_back(palette); }
    for (const auto& section : value.sections) {
        if (section.entries.empty() && section.body.empty()) continue;
        content.emplace_back();
        const auto title = ellipsize(sanitize_text(section.title), content_width);
        auto heading = render(title, StyleToken::subheading, Stream::output);
        const auto title_width = display_width(title);
        if (!section.detail.empty() && content_width > title_width + 2) {
            heading += "  " + render(ellipsize(sanitize_text(section.detail), content_width - title_width - 2),
                                     StyleToken::muted, Stream::output);
        }
        content.push_back(std::move(heading));
        std::size_t entry_width = 0;
        for (const auto& entry : section.entries) {
            entry_width = (std::max)(entry_width, display_width(entry.first));
        }
        entry_width = (std::min)(entry_width, content_width / 2);
        for (const auto& [label, text] : section.entries) {
            const auto remaining = content_width > entry_width + 2 ? content_width - entry_width - 2 : 1;
            content.push_back(render(pad(ellipsize(sanitize_text(label), entry_width), entry_width),
                                     StyleToken::command, Stream::output) + "  " +
                              render(ellipsize(sanitize_text(text), remaining),
                                     StyleToken::secondary, Stream::output));
        }
        for (const auto& line : wrap_text(section.body, content_width)) {
            content.push_back(render(line, section.body_style, Stream::output));
        }
    }
    if (delayed_auto_image) {
        std::size_t widest_content = 0;
        for (const auto& line : content) {
            widest_content = (std::max)(widest_content, display_width(sanitize_text(line)));
        }
        const auto art_columns = width() > widest_content + content_margin
            ? width() - widest_content - content_margin
            : art_width;
        auto matched = art_content(*this, art_columns, content.size());
        if (matched.height != 0 && width() >= matched.width + 46) {
            art = std::move(matched);
            art_width = art.width;
            side_by_side = true;
        }
    }
    std::string output = "\n";
    if (side_by_side) {
        const auto rows = (std::max)(art.height, content.size());
        // Prefer the lower middle row when the free height is odd. This avoids
        // the art looking top-heavy beside text with an odd number of lines.
        const auto top_padding = rows > art.height ? (rows - art.height + 1) / 2 : 0;
        constexpr auto art_indent = std::size_t{0};
        constexpr auto art_gap = std::size_t{4};
        if (!art.sixel.empty()) {
            output += "\x1b[s\x1b[3G" + art.sixel + "\x1b[u";
            for (std::size_t index = 0; index < rows; ++index) {
                output += "\x1b[" + std::to_string(art_width + 6) + 'G';
                if (index < content.size()) output += content[index];
                output += '\n';
            }
        } else {
            for (std::size_t index = 0; index < rows; ++index) {
                const auto art_index = index >= top_padding ? index - top_padding : art.lines.size();
                const auto left = art_index < art.lines.size() ? art.lines[art_index] : std::string{};
                output += "  " + std::string(art_indent, ' ') + left +
                          std::string(art_width - display_width(sanitize_text(left)) + art_gap, ' ');
                if (index < content.size()) output += content[index];
                output += '\n';
            }
        }
    } else {
        // Small consoles keep the information; a large illustration never dominates the screen.
        if (height() >= 32 && art.height != 0) {
            if (!art.sixel.empty()) {
                output += "\x1b[s  " + art.sixel + "\x1b[u";
                output.append(art.height, '\n');
            } else {
                for (const auto& line : art.lines) output += "  " + line + "\n";
            }
            output += '\n';
        }
        for (const auto& line : content) output += "  " + line + "\n";
    }
    data(output);
}

void Ui::summary(const std::size_t count, const std::string_view noun,
                 const std::chrono::nanoseconds elapsed) {
    if (!human() || quiet()) return;
    const Segment segments[]{{"  "}, {format_number(count), StyleToken::number},
        {" " + std::string(noun), StyleToken::secondary},
        {unicode() ? "  ·  " : "  /  ", StyleToken::subtle},
        {format_duration(elapsed), StyleToken::duration}};
    line(segments);
}

void Ui::progress_sample(const std::uint64_t current, const std::uint64_t total,
                         const std::string_view label) {
    if (total == 0) return;
    const auto ratio = (std::min)(1.0, static_cast<double>(current) / total);
    const auto length = (std::min)(std::size_t{28}, width() > 20 ? width() - 20 : std::size_t{4});
    const auto filled = static_cast<std::size_t>(ratio * length);
    std::string full, track;
    for (std::size_t i = 0; i < length; ++i) (i < filled ? full : track) += unicode() ? "━" : "-";
    const Segment segments[]{{"  "}, {full, StyleToken::progress_filled}, {track, StyleToken::progress_empty},
        {"  " + std::to_string(static_cast<int>(ratio * 100)) + "%", StyleToken::number}};
    line(segments);
    paragraph(label, StyleToken::muted);
}

} // namespace atlas::terminal
