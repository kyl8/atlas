#pragma once

#include "atlas/cli/registry.hpp"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::completion {

enum class Kind { command, alias, subcommand, option, value, path, file, directory, project, theme, tool };

struct Item {
    std::string value;
    std::string description;
    Kind kind{Kind::value};
    int score{};
};

struct Request {
    std::string_view line;
    std::size_t limit{100};
    bool allow_paths{true};
};

using DynamicProvider = std::function<Result<std::vector<Item>>(
    std::string_view provider, std::string_view current, std::span<const std::string> completed)>;

Result<std::vector<Item>> complete(const cli::Registry& registry, const Request& request,
                                   const DynamicProvider& dynamic = {});
std::string json(std::span<const Item> items);
std::string_view kind_name(Kind kind) noexcept;

} // namespace atlas::completion
