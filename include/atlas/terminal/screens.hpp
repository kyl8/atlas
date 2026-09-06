#pragma once

#include "atlas/cli/registry.hpp"
#include "atlas/terminal/ui.hpp"

namespace atlas::terminal {

void show_home(Ui& ui, const cli::Registry& registry, std::string_view version,
               std::string_view config, std::string_view tools_directory);
void show_help(Ui& ui, const cli::Registry& registry, std::string_view version, bool interactive = true);
void show_command_help(Ui& ui, const cli::Command& command);
void show_theme_preview(Ui& ui);

} // namespace atlas::terminal
