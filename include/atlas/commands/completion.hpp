#pragma once

#include "atlas/error.hpp"

namespace atlas::cli { class Registry; }

namespace atlas::commands {

Status register_completion_command(cli::Registry& registry);

} // namespace atlas::commands
