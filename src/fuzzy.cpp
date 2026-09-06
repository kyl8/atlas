#include "atlas/fuzzy.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <vector>

namespace atlas::fuzzy {
namespace {

// Scoring adapted from philj56/fuzzy-match, copyright (C) 2022 Philip Jones,
// used under the MIT License. The Atlas implementation computes the same
// bonuses with dynamic programming instead of recursive backtracking.
constexpr std::int32_t adjacency_bonus = 15;
constexpr std::int32_t separator_bonus = 30;
constexpr std::int32_t camel_bonus = 30;
constexpr std::int32_t first_letter_bonus = 15;
constexpr std::int32_t leading_letter_penalty = -5;
constexpr std::int32_t maximum_leading_penalty = -15;

unsigned char fold(const char value) noexcept {
    return static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(value)));
}

bool equal(const char left, const char right, const bool case_sensitive) noexcept {
    return case_sensitive ? left == right : fold(left) == fold(right);
}

std::int32_t position_score(const std::size_t jump, const bool first,
                            const std::string_view candidate, const std::size_t position) noexcept {
    std::int32_t result = 0;
    if (!first && jump == 0) result += adjacency_bonus;
    if ((!first || jump > 0) && position > 0) {
        const auto current = static_cast<unsigned char>(candidate[position]);
        const auto previous = static_cast<unsigned char>(candidate[position - 1]);
        if (std::isupper(current) != 0 && std::islower(previous) != 0) result += camel_bonus;
        if (std::isalnum(current) != 0 && std::isalnum(previous) == 0) result += separator_bonus;
    }
    if (first && jump == 0) result += first_letter_bonus;
    if (first) {
        const auto bounded = static_cast<std::int32_t>((std::min)(
            jump, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() / 5)));
        result += (std::max)(leading_letter_penalty * bounded, maximum_leading_penalty);
    }
    return result;
}

bool regex_special(const unsigned char value) noexcept {
    switch (value) {
        case '.': case '^': case '$': case '*': case '+': case '?':
        case '(': case ')': case '[': case ']': case '{': case '}':
        case '\\': case '|': return true;
        default: return false;
    }
}

} // namespace

std::optional<std::int32_t> score(const std::string_view pattern,
                                  const std::string_view candidate,
                                  const bool case_sensitive) {
    if (pattern.empty()) return 100;
    if (candidate.size() < pattern.size()) return std::nullopt;

    constexpr auto missing = std::numeric_limits<std::int32_t>::min();
    std::vector<std::int32_t> previous(candidate.size(), missing);
    std::vector<std::int32_t> current(candidate.size(), missing);

    for (std::size_t position = 0; position < candidate.size(); ++position) {
        if (equal(pattern.front(), candidate[position], case_sensitive)) {
            previous[position] = position_score(position, true, candidate, position);
        }
    }

    for (std::size_t pattern_index = 1; pattern_index < pattern.size(); ++pattern_index) {
        std::ranges::fill(current, missing);
        auto best_non_adjacent = missing;
        for (std::size_t position = 0; position < candidate.size(); ++position) {
            if (position >= 2) best_non_adjacent = (std::max)(best_non_adjacent, previous[position - 2]);
            if (!equal(pattern[pattern_index], candidate[position], case_sensitive)) continue;
            if (position > 0 && previous[position - 1] != missing) {
                current[position] = previous[position - 1] +
                    position_score(0, false, candidate, position);
            }
            if (best_non_adjacent != missing) {
                current[position] = (std::max)(current[position], best_non_adjacent +
                    position_score(1, false, candidate, position));
            }
        }
        previous.swap(current);
    }

    const auto best = *std::ranges::max_element(previous);
    if (best == missing) return std::nullopt;
    const auto unused = candidate.size() - pattern.size();
    const auto bounded = static_cast<std::int32_t>((std::min)(
        unused, static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max() - 100)));
    return 100 - bounded + best;
}

std::size_t edit_distance(const std::string_view left, const std::string_view right) {
    std::vector<std::size_t> row(right.size() + 1);
    for (std::size_t index = 0; index <= right.size(); ++index) row[index] = index;
    for (std::size_t left_index = 1; left_index <= left.size(); ++left_index) {
        auto diagonal = row[0];
        row[0] = left_index;
        for (std::size_t right_index = 1; right_index <= right.size(); ++right_index) {
            const auto previous = row[right_index];
            const auto substitution = diagonal +
                (fold(left[left_index - 1]) == fold(right[right_index - 1]) ? 0u : 1u);
            row[right_index] = (std::min)({row[right_index] + 1,
                                           row[right_index - 1] + 1, substitution});
            diagonal = previous;
        }
    }
    return row.back();
}

std::string subsequence_regex(const std::string_view pattern) {
    std::string result;
    result.reserve(pattern.size() * 4);
    bool first = true;
    for (std::size_t index = 0; index < pattern.size();) {
        const auto lead = static_cast<unsigned char>(pattern[index]);
        std::size_t length = 1;
        if ((lead & 0xe0) == 0xc0) length = 2;
        else if ((lead & 0xf0) == 0xe0) length = 3;
        else if ((lead & 0xf8) == 0xf0) length = 4;
        length = (std::min)(length, pattern.size() - index);
        if (!first) result += ".*";
        if (length == 1 && regex_special(lead)) result.push_back('\\');
        result.append(pattern.substr(index, length));
        first = false;
        index += length;
    }
    return result;
}

} // namespace atlas::fuzzy
