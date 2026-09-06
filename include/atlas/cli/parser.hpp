#pragma once

#include "atlas/error.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::cli {

enum class ColorPreference { automatic, always, never };

enum class GlobalOptionId { help, version, color, plain, json, raw, quiet, verbose, debug };

struct GlobalOptionSpec {
    GlobalOptionId id;
    std::string_view long_name;
    std::string_view short_name;
    std::string_view description;
    std::string_view value_name;
    std::span<const std::string_view> values;
};

struct Invocation {
    bool help{};
    bool version{};
    bool verbose{};
    bool debug{};
    bool plain{};
    bool json{};
    bool quiet{};
    bool raw{};
    ColorPreference color{ColorPreference::automatic};
    std::string command;
    std::vector<std::string> arguments;
};

Result<Invocation> parse(std::span<const std::string_view> arguments);
std::span<const GlobalOptionSpec> global_options() noexcept;

} // namespace atlas::cli
