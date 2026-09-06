#pragma once

#include "atlas/error.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::terminal { class Ui; }

namespace atlas::cli {

struct CommandContext {
    terminal::Ui& ui;
};

using CommandHandler = std::function<Result<int>(CommandContext&, std::span<const std::string>)>;

struct HelpEntry {
    std::string label;
    std::string description;
    std::string completion;
    std::vector<std::pair<std::string, std::string>> values;
};

struct SubcommandHelp {
    std::string name;
    std::string description;
    std::vector<std::string> aliases;
    std::vector<HelpEntry> arguments;
    std::vector<HelpEntry> options;
};

struct CommandHelp {
    std::string details;
    std::vector<HelpEntry> arguments;
    std::vector<HelpEntry> options;
    std::vector<HelpEntry> examples;
    std::string category{"General"};
    std::vector<std::string> powered_by;
    std::vector<SubcommandHelp> subcommands;
};

struct Command {
    std::string name;
    std::string description;
    std::string usage;
    std::vector<std::string> aliases;
    CommandHandler handler;
    CommandHelp help;
    bool user_defined{};
    std::string source;
};

class Registry {
public:
    Status add(Command command);
    [[nodiscard]] const Command* find(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<std::string_view> suggestions(std::string_view name) const;
    [[nodiscard]] const std::vector<Command>& commands() const noexcept { return commands_; }

private:
    std::vector<Command> commands_;
};

} // namespace atlas::cli
