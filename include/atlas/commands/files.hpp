#pragma once

#include "atlas/cli/registry.hpp"

namespace atlas::commands {

Result<int> list_directory(cli::CommandContext& context, std::span<const std::string> arguments);
Result<int> show_tree(cli::CommandContext& context, std::span<const std::string> arguments);

} // namespace atlas::commands
