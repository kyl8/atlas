#pragma once

#include "atlas/cli/registry.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::cli {

struct OptionSpec {
    std::vector<std::string> names;
    std::string canonical;
    std::string description;
    std::string completion;
    std::vector<std::pair<std::string, std::string>> values;
    bool takes_value{};
};

std::vector<OptionSpec> option_specs(std::span<const HelpEntry> entries, bool automatic_short = true);
std::vector<std::string> normalize_options(const Command& command, std::span<const std::string> arguments);
Status prepare_option_metadata(Command& command);

} // namespace atlas::cli
