#include "atlas/terminal/screens.hpp"
#include "atlas/cli/parser.hpp"
#include "atlas/terminal/formatter.hpp"
#include "atlas/platform/utf.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace atlas::terminal {
namespace {

using enum StyleToken;

std::vector<cli::HelpEntry> global_help() {
    std::vector<cli::HelpEntry> result;
    for (const auto& option : cli::global_options()) {
        std::string label;
        if (!option.short_name.empty()) label = std::string(option.short_name) + ", ";
        label += option.long_name;
        if (!option.value_name.empty()) label += " <" + std::string(option.value_name) + ">";
        result.push_back({std::move(label), std::string(option.description)});
    }
    return result;
}

std::string origin(const cli::Command& command) {
    if (!command.user_defined) return "built-in";
    return command.source.find("plugins/") != std::string::npos ||
           command.source.find("plugins\\") != std::string::npos ? "plugin" : "Lua";
}

std::string category(const cli::Command& command) {
    return command.help.category == "General" && command.user_defined
        ? "User commands" : command.help.category;
}

void entries(Ui& ui, const std::string_view title, const std::span<const cli::HelpEntry> values) {
    if (values.empty()) return;
    ui.section(title);
    std::vector<std::vector<TableCell>> rows;
    for (const auto& value : values) rows.push_back({{value.label, command}, {value.description, secondary}});
    ui.list(rows);
}

std::string aliases(const cli::Command& value) {
    std::string result;
    for (const auto& alias : value.aliases) {
        if (!result.empty()) result += ", ";
        result += alias;
    }
    return result;
}

std::string json_quote(const std::string_view text) { return "\"" + json_escape(text) + "\""; }

std::string identity_title(const Theme& theme, const std::string_view version) {
    std::string result = theme.identity.name;
    if (!theme.identity.show_version) return result;
    auto label = theme.identity.version;
    constexpr std::string_view placeholder = "{version}";
    for (auto position = label.find(placeholder); position != std::string::npos;
         position = label.find(placeholder, position + version.size())) {
        label.replace(position, placeholder.size(), version);
    }
    if (result.empty()) return label;
    if (!label.empty()) result += theme.identity.separator + label;
    return result;
}

void registry_json(Ui& ui, const cli::Registry& registry) {
    std::string result = "[";
    for (const auto& item : registry.commands()) {
        if (result.size() > 1) result += ',';
        result += "{\"name\":" + json_quote(item.name) + ",\"description\":" + json_quote(item.description) +
            ",\"category\":" + json_quote(category(item)) + ",\"usage\":" + json_quote(item.usage) +
            ",\"source\":" + json_quote(item.source) + ",\"origin\":" + json_quote(origin(item)) + ",\"aliases\":[";
        for (std::size_t i = 0; i < item.aliases.size(); ++i) {
            if (i) result += ',';
            result += json_quote(item.aliases[i]);
        }
        result += "]}";
    }
    ui.data_line(result + "]");
}

std::string lowercase(std::string value) {
    for (auto& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

struct HelpInput {
    Ui& ui;
    HANDLE input{GetStdHandle(STD_INPUT_HANDLE)};
    DWORD mode{};
    bool active{};
    explicit HelpInput(Ui& target) : ui(target) {
        active = GetConsoleMode(input, &mode) &&
            SetConsoleMode(input, (mode | ENABLE_WINDOW_INPUT) &
                ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE));
        if (active) ui.screen(true);
    }
    ~HelpInput() {
        if (active) { ui.screen(false); SetConsoleMode(input, mode); }
    }
};

// The browser stays small; command names and details come from the registry.
bool browse_help(Ui& ui, const cli::Registry& registry) {
    HelpInput session(ui);
    if (!session.active) return false;
    const auto global_options = global_help();
    std::string query;
    std::size_t selected = 0, offset = 0;
    bool searching = false, details = false, options_view = false;
    for (;;) {
        ui.refresh_size();
        const auto width = ui.width() > 6 ? ui.width() - 6 : 1;
        const auto rows = ui.height() > 11 ? (ui.height() - 10) / 2 * 2 : 2;
        std::vector<const cli::Command*> matches;
        for (const auto& item : registry.commands()) {
            const auto text = lowercase(item.name + " " + item.description + " " + category(item) + " " + aliases(item));
            if (text.find(lowercase(query)) != std::string::npos) matches.push_back(&item);
        }
        if (!query.empty()) {
            const auto needle = lowercase(query);
            std::ranges::stable_sort(matches, [&](const cli::Command* left, const cli::Command* right) {
                const auto score = [&](const cli::Command* item) {
                    if (lowercase(item->name) == needle || std::ranges::any_of(item->aliases,
                            [&](const auto& alias) { return lowercase(alias) == needle; })) return 0;
                    if (lowercase(item->name).starts_with(needle)) return 1;
                    return 2;
                };
                return score(left) < score(right);
            });
        }
        if (matches.empty()) selected = 0;
        else selected = (std::min)(selected, matches.size() - 1);
        std::vector<Line> content;
        const auto append = [&](const std::string_view text, const StyleToken style) {
            for (auto& wrapped : wrap_text(text, width)) content.push_back({{std::move(wrapped), style}});
        };
        if (options_view) {
            append("Usage", heading);
            append("atlas <command> [arguments]", command);
            content.push_back({});
            append("Global options", subheading);
            for (const auto& option : global_options) {
                append(option.label, command);
                append("  " + option.description, secondary);
            }
        } else if (details && !matches.empty()) {
            const auto& item = *matches[selected];
            append(item.name + "  /  " + category(item), heading);
            append(item.description, primary);
            if (!item.help.details.empty()) append(item.help.details, secondary);
            content.push_back({});
            append(item.usage, command);
            if (!item.aliases.empty()) append("Aliases: " + aliases(item), muted);
            const auto add_entries = [&](const std::string_view label, const std::span<const cli::HelpEntry> values) {
                if (values.empty()) return;
                content.push_back({});
                append(label, subheading);
                for (const auto& entry : values) {
                    append(entry.label, command);
                    append("  " + entry.description, secondary);
                }
            };
            add_entries("Arguments", item.help.arguments);
            add_entries("Options", item.help.options);
            add_entries("Examples", item.help.examples);
        } else {
            for (std::size_t i = 0; i < matches.size(); ++i) {
                const auto& item = *matches[i];
                const auto suffix = "  " + category(item) + " / " + origin(item);
                const auto label = item.name + (item.aliases.empty() ? "" : " (" + aliases(item) + ")");
                content.push_back({{i == selected ? "> " : "  ", i == selected ? selection : primary},
                    {pad(label, (std::min)(std::size_t{18}, width / 2)), i == selected ? selection : command},
                    {ellipsize(suffix, width > 22 ? width - 22 : 1), muted}});
                content.push_back({{"  " + ellipsize(item.description, width - 2), secondary}});
            }
        }
        if (!details && !options_view) offset = matches.empty() ? 0 : (selected * 2 / rows) * rows;
        offset = (std::min)(offset, content.empty() ? 0 : content.size() - 1);
        std::string frame = "\n";
        Line title{{"  " + ui.theme().identity.name, heading}};
        if (!ui.theme().identity.explorer.empty()) {
            title.push_back({ui.theme().identity.separator + ui.theme().identity.explorer, secondary});
        }
        frame += ui.compose(title) + "\n";
        const auto theme_name = ellipsize(ui.theme().name, (std::min)(std::size_t{24}, width / 2));
        const Line theme_line{{"  Theme ", muted}, {theme_name, highlight}};
        frame += ui.compose(theme_line) + "  " + ui.palette_line(width > display_width(theme_name) + 10
            ? width - display_width(theme_name) - 10 : 0) + "\n";
        const Line filter{{"  " + std::string(searching ? "/ " : "Search: "), keybind},
                          {query.empty() ? "all commands" : ellipsize(query, width - 8), highlight}};
        frame += ui.compose(filter) + "\n\n";
        for (std::size_t row = 0; row < rows; ++row) {
            if (offset + row < content.size()) frame += "  " + ui.compose(content[offset + row]);
            else if (row == 0 && matches.empty()) frame += "  No matching commands";
            frame += '\n';
        }
        const auto page_count = (std::max)(std::size_t{1}, (content.size() + rows - 1) / rows);
        const Line pages{{"  Page " + std::to_string(offset / rows + 1) + " / " + std::to_string(page_count), muted}};
        frame += "\n" + ui.compose(pages) + "\n";
        const auto controls = details || options_view ? "Up/Down scroll  Esc back  q quit" :
            (searching ? "Type to search  Enter done  Esc clear" :
                (width < 65 ? "Arrows / search Enter open ? flags q quit" :
                    "Arrows move  / search  Enter details  ? options  q quit"));
        const Line footer{{"  " + ellipsize(controls, width), keybind}};
        frame += ui.compose(footer);
        ui.replace_screen(frame);
        INPUT_RECORD event{};
        DWORD read = 0;
        if (!ReadConsoleInputW(session.input, &event, 1, &read)) return true;
        if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;
        const auto key = event.Event.KeyEvent.wVirtualKeyCode;
        const auto ch = event.Event.KeyEvent.uChar.UnicodeChar;
        if (ch == 3 || (!searching && ch == L'q')) return true;
        if (searching) {
            if (key == VK_ESCAPE) { query.clear(); searching = false; }
            else if (key == VK_RETURN) searching = false;
            else if (key == VK_BACK && !query.empty()) {
                auto start = query.size() - 1;
                while (start && (static_cast<unsigned char>(query[start]) & 0xc0) == 0x80) --start;
                query.resize(start);
            } else if (ch >= 32 && query.size() < 128) {
                if (auto utf8 = win::narrow(std::wstring(1, ch)); utf8) query += *utf8;
            }
            selected = 0; offset = 0;
            continue;
        }
        if (key == VK_ESCAPE) { if (!details && !options_view) return true; details = false; options_view = false; offset = 0; }
        else if (ch == L'?') { options_view = !options_view; details = false; offset = 0; }
        else if (ch == L'/') { searching = true; details = false; options_view = false; }
        else if (key == VK_RETURN && !matches.empty()) { details = true; offset = 0; }
        else if (details || options_view) {
            if (key == VK_DOWN && offset + rows < content.size()) ++offset;
            if (key == VK_UP && offset) --offset;
            if (key == VK_NEXT || key == VK_RIGHT) offset = (std::min)(offset + rows, content.empty() ? 0 : content.size() - 1);
            if (key == VK_PRIOR || key == VK_LEFT) offset = offset > rows ? offset - rows : 0;
        } else {
            const auto step = key == VK_RIGHT || key == VK_NEXT || key == VK_LEFT || key == VK_PRIOR
                ? (std::max)(std::size_t{1}, rows / 2) : 1;
            if ((key == VK_DOWN || key == VK_RIGHT || key == VK_NEXT) && !matches.empty())
                selected = (std::min)(selected + step, matches.size() - 1);
            if (key == VK_UP || key == VK_LEFT || key == VK_PRIOR) selected = selected > step ? selected - step : 0;
        }
    }
}

} // namespace

void show_home(Ui& ui, const cli::Registry& registry, const std::string_view version,
               const std::string_view config, const std::string_view tools_directory) {
    if (ui.json()) { registry_json(ui, registry); return; }
    std::size_t builtin = 0, lua = 0;
    std::set<std::string> plugins;
    for (const auto& item : registry.commands()) {
        if (item.user_defined) ++lua; else ++builtin;
        if (origin(item) == "plugin") plugins.insert(item.source);
    }
    Identity identity{identity_title(ui.theme(), version), ui.theme().tagline, {
        {"Theme", ui.theme().name},
        {"Commands", std::to_string(builtin) + " built-in / " + std::to_string(lua) + " Lua"},
        {"Plugins", std::to_string(plugins.size()) + " loaded"},
        {"Config", std::string(config)},
    }, ui.theme().identity.show_palette};
    if (!tools_directory.empty()) identity.facts.emplace_back("Tools", tools_directory);
    for (const auto& entry : ui.theme().home.facts) {
        identity.facts.emplace_back(entry.label, entry.value);
    }
    Identity::Section actions{ui.theme().home.actions_title, {}};
    if (ui.theme().home.show_default_actions) {
        actions.entries.emplace_back("atlas help", "Explore commands and examples");
        actions.entries.emplace_back("atlas help <command>", "Learn what a command does");
    }
    for (const auto& entry : ui.theme().home.actions) {
        actions.entries.emplace_back(entry.label, entry.value);
    }
    if (!actions.entries.empty()) identity.sections.push_back(std::move(actions));
    std::string names;
    for (const auto& item : registry.commands()) {
        if (!names.empty()) names += "   ";
        names += item.name;
    }
    identity.sections.push_back({"Your commands", {},
        std::to_string(registry.commands().size()) + " available", std::move(names), StyleToken::command});
    ui.identity(identity);
    if (ui.human()) ui.data_line("");
}

void show_help(Ui& ui, const cli::Registry& registry, const std::string_view version, const bool interactive) {
    if (ui.json()) { registry_json(ui, registry); return; }
    if (interactive && ui.interactive() && browse_help(ui, registry)) return;
    ui.identity({identity_title(ui.theme(), version), ui.theme().tagline,
                 {{"Theme", ui.theme().name}, {"Commands", std::to_string(registry.commands().size()) + " available"}},
                 ui.theme().identity.show_palette});
    ui.section("Usage");
    ui.paragraph("atlas <command> [arguments]\natlas help <command>", command);
    for (const bool user_defined : {false, true}) {
        std::vector<std::string> categories;
        std::size_t label_width = 0;
        for (const auto& item : registry.commands()) {
            if (item.user_defined != user_defined) continue;
            const auto label = category(item);
            const auto command_label = item.name + (item.aliases.empty() ? "" : " (" + aliases(item) + ")");
            label_width = (std::max)(label_width, display_width(command_label));
            if (std::ranges::find(categories, label) == categories.end()) categories.push_back(label);
        }
        if (categories.empty()) continue;
        ui.section(user_defined ? "User commands" : "Built-in commands");
        for (const auto& group : categories) {
            if (group != "General" && group != "User commands") ui.paragraph(group, badge);
            std::vector<std::vector<TableCell>> rows;
            for (const auto& item : registry.commands()) {
                if (item.user_defined != user_defined || category(item) != group) continue;
                auto name = item.name;
                if (!item.aliases.empty()) name += " (" + aliases(item) + ")";
                rows.push_back({{name, command}, {item.description, secondary}});
            }
            ui.list(rows, label_width);
            if (ui.human()) ui.data_line("");
        }
    }
    const auto options = global_help();
    entries(ui, "Global options", options);
    if (ui.human()) ui.paragraph("\nUse atlas help for keyboard navigation. --plain prints everything.", muted);
}

void show_command_help(Ui& ui, const cli::Command& value) {
    if (ui.json()) {
        cli::Registry registry;
        static_cast<void>(registry.add(value));
        registry_json(ui, registry);
        return;
    }
    ui.heading(value.name, category(value));
    ui.paragraph(value.description, primary);
    if (!value.help.details.empty()) ui.paragraph(value.help.details);
    ui.section("Usage");
    ui.paragraph(value.usage, command);
    if (!value.aliases.empty()) ui.paragraph("Aliases: " + aliases(value), muted);
    entries(ui, "Arguments", value.help.arguments);
    entries(ui, "Options", value.help.options);
    entries(ui, "Examples", value.help.examples);
    if (!value.help.powered_by.empty()) {
        ui.section("Powered by");
        for (const auto& tool : value.help.powered_by) ui.paragraph(tool, secondary);
    }
    if (!value.source.empty()) {
        ui.section("Source", origin(value));
        ui.paragraph(value.source, path);
    }
}

void show_theme_preview(Ui& ui) {
    const auto& theme = ui.theme();
    if (ui.json()) {
        const auto color = [](const Rgb rgb) {
            constexpr std::string_view hex = "0123456789ABCDEF";
            std::string text = "#";
            for (const auto channel : {rgb.red, rgb.green, rgb.blue}) {
                text += hex[channel >> 4]; text += hex[channel & 15];
            }
            return json_quote(text);
        };
        std::string result = "{\"name\":" + json_quote(theme.name) + ",\"author\":" + json_quote(theme.author) +
            ",\"source\":" + json_quote(win::narrow(theme.source.native()).value_or("")) + ",\"palette\":[";
        for (std::size_t i = 0; i < theme.palette.size(); ++i) {
            if (i) result += ',';
            result += color(theme.palette[i]);
        }
        result += "],\"styles\":{";
        for (std::size_t i = 0; i < static_cast<std::size_t>(StyleToken::count); ++i) {
            if (i) result += ',';
            const auto token = static_cast<StyleToken>(i);
            const auto& style = theme[token];
            result += json_quote(style_token_name(token)) + ":{\"fg\":" +
                (style.foreground ? color(*style.foreground) : "null") + ",\"bg\":" +
                (style.background ? color(*style.background) : "null") +
                ",\"bold\":" + (style.bold ? "true" : "false") +
                ",\"dim\":" + (style.dim ? "true" : "false") +
                ",\"italic\":" + (style.italic ? "true" : "false") +
                ",\"underline\":" + (style.underline ? "true" : "false") + "}";
        }
        ui.data_line(result + "}}");
        return;
    }
    ui.identity({theme.name, "Theme preview / Atlas design system", {
        {"Author", theme.author}, {"Source", theme.source.empty() ? "Compiled default" :
            win::narrow(theme.source.native()).value_or("<invalid path>")}
    }, false});
    ui.section("Palette", std::to_string(theme.palette.size()) + " colors");
    if (ui.color()) ui.data_line("  " + ui.palette_line(ui.width() - 4));
    else ui.paragraph("Color swatches are available in a color terminal.", muted);
    const auto samples = [&](const std::string_view title, const std::span<const std::pair<StyleToken, std::string_view>> values) {
        ui.section(title);
        std::vector<std::vector<TableCell>> rows;
        for (const auto& [token, text] : values) {
            rows.push_back({{std::string(style_token_name(token)), muted}, {std::string(text), token}});
        }
        ui.list(rows, 17);
    };
    const std::pair<StyleToken, std::string_view> typography[]{
        {heading, "Atlas / your terminal toolkit"}, {primary, "Main content"},
        {secondary, "Supporting information"}, {muted, "Additional context"},
        {accent, "An accent with purpose"}, {subheading, "A new section"}, {subtle, "Quiet separators"}};
    samples("Typography", typography);
    const std::pair<StyleToken, std::string_view> semantics[]{
        {success, "Success / Operation completed"}, {warning, "Warning / Check the destination"},
        {error, "Error / Tool not found"}, {info, "Info / Ready when you are"},
        {selection, "> Selected command"}, {keybind, "Enter / details"}, {badge, "Lua"}, {highlight, "Matched text"}};
    samples("Semantic states", semantics);
    const std::pair<StyleToken, std::string_view> values[]{
        {filename, "main.cpp"}, {directory, "D:\\Dev\\Atlas"}, {path, "D:\\Dev\\Atlas\\src\\main.cpp"},
        {url, "https://example.com"}, {command, "atlas search waluna"},
        {number, "1,284 files"}, {date, "Sep 04 02:14"}, {duration, "318 ms"}, {size, "42.8 MB"},
        {table_header, "NAME  LOCATION  SIZE"}, {table_border, "--------------------"},
        {spinner, "Working / indeterminate"}};
    samples("Values", values);
    ui.section("Progress", "static sample");
    ui.progress_sample(72, 100, "72 / 100 items / demonstration only");
}

} // namespace atlas::terminal
