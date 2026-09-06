#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::terminal {
std::string json_escape(std::string_view value);
std::string sanitize_text(std::string_view text);
std::vector<std::string> wrap_text(std::string_view text, std::size_t width);
std::string truncate_path(std::string_view text, std::size_t width);
std::string format_relative_time(std::time_t timestamp, std::time_t now);

std::string format_filesize(std::uint64_t bytes);
std::string format_duration(std::chrono::nanoseconds duration);
std::string format_number(std::uint64_t number);
std::string format_timestamp(std::time_t timestamp);
std::size_t display_width(std::string_view utf8);
std::string ellipsize(std::string_view utf8, std::size_t width);
std::string pad(std::string_view utf8, std::size_t width, bool right_aligned = false);

} // namespace atlas::terminal
