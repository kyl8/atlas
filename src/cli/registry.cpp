#include "atlas/cli/registry.hpp"
#include "atlas/cli/options.hpp"
#include "atlas/fuzzy.hpp"

#include <algorithm>
#include <limits>

namespace atlas::cli {
Status Registry::add(Command command) {
    if (command.name.empty() || !command.handler) {
        return std::unexpected(Error{ErrorKind::usage, "A command requires a name and handler"});
    }
    if (find(command.name) != nullptr) {
        return std::unexpected(Error{ErrorKind::usage, "Command is already registered: " + command.name});
    }
    for (const auto& alias : command.aliases) {
        if (find(alias) != nullptr) {
            return std::unexpected(Error{ErrorKind::usage, "Command alias is already registered: " + alias});
        }
    }
    if (auto prepared = prepare_option_metadata(command); !prepared) return prepared;
    commands_.push_back(std::move(command));
    return {};
}

const Command* Registry::find(const std::string_view name) const noexcept {
    for (const auto& command : commands_) {
        if (command.name == name || std::ranges::find(command.aliases, name) != command.aliases.end()) {
            return &command;
        }
    }
    return nullptr;
}

std::vector<std::string_view> Registry::suggestions(const std::string_view name) const {
    struct Candidate { int rank; std::size_t distance; std::string_view name; };
    std::vector<Candidate> candidates;
    for (const auto& command : commands_) {
        const auto distance = fuzzy::edit_distance(name, command.name);
        const auto matched = fuzzy::score(name, command.name);
        candidates.push_back({matched ? -*matched : 1'000 + static_cast<int>(distance), distance, command.name});
    }
    std::ranges::sort(candidates, [](const Candidate& left, const Candidate& right) {
        return left.rank == right.rank ? left.distance < right.distance : left.rank < right.rank;
    });
    std::vector<std::string_view> result;
    for (const auto& candidate : candidates) {
        const auto threshold = (std::max)(std::size_t{2}, name.size() / 3);
        if ((candidate.rank < 1'000 || candidate.distance <= threshold) && result.size() < 3) {
            result.push_back(candidate.name);
        }
    }
    return result;
}

} // namespace atlas::cli
