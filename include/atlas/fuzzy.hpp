#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace atlas::fuzzy {

// Higher scores are better. A missing value means the pattern is not a
// case-insensitive subsequence of the candidate.
[[nodiscard]] std::optional<std::int32_t> score(std::string_view pattern,
                                                std::string_view candidate,
                                                bool case_sensitive = false);

[[nodiscard]] std::size_t edit_distance(std::string_view left, std::string_view right);

// Produces an Everything-compatible regular expression which finds a bounded
// candidate set for local fuzzy ranking.
[[nodiscard]] std::string subsequence_regex(std::string_view pattern);

} // namespace atlas::fuzzy
