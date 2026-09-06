#include "atlas/terminal/formatter.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace atlas::terminal {
std::string json_escape(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const unsigned char character : value) {
        switch (character) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (character < 0x20) {
                    constexpr char digits[] = "0123456789abcdef";
                    result += "\\u00";
                    result.push_back(digits[character >> 4]);
                    result.push_back(digits[character & 0x0f]);
                } else result.push_back(static_cast<char>(character));
        }
    }
    return result;
}
namespace {

struct Codepoint {
    std::size_t bytes{1};
    std::uint32_t value{};
};

Codepoint decode(const std::string_view text, const std::size_t offset) {
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) return {1, first};
    std::size_t count = 0;
    std::uint32_t value = 0;
    if ((first & 0xe0) == 0xc0) { count = 2; value = first & 0x1f; }
    else if ((first & 0xf0) == 0xe0) { count = 3; value = first & 0x0f; }
    else if ((first & 0xf8) == 0xf0) { count = 4; value = first & 0x07; }
    else return {1, 0xfffd};
    if (offset + count > text.size()) return {1, 0xfffd};
    for (std::size_t index = 1; index < count; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if ((next & 0xc0) != 0x80) return {1, 0xfffd};
        value = (value << 6) | (next & 0x3f);
    }
    if ((count == 2 && value < 0x80) || (count == 3 && value < 0x800) ||
        (count == 4 && value < 0x10000) || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
        return {1, 0xfffd};
    return {count, value};
}

std::size_t codepoint_width(const std::uint32_t value) {
    if (value == 0x200d || (value >= 0x0300 && value <= 0x036f) || (value >= 0xfe00 && value <= 0xfe0f)) return 0;
    if ((value >= 0x1100 && value <= 0x115f) || (value >= 0x2e80 && value <= 0xa4cf) ||
        (value >= 0xac00 && value <= 0xd7a3) || (value >= 0xf900 && value <= 0xfaff) ||
        (value >= 0x1f300 && value <= 0x1faff)) return 2;
    return 1;
}

} // namespace

std::string sanitize_text(const std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto ch = static_cast<unsigned char>(text[i]);
        if (ch == 27) {
            ++i;
            if (i < text.size() && text[i] == '[') {
                ++i;
                while (i < text.size() && !(text[i] >= '@' && text[i] <= '~')) ++i;
                if (i < text.size()) ++i;
            } else if (i < text.size() && text[i] == ']') {
                ++i;
                while (i < text.size() && text[i] != '\a' && text[i] != 27) ++i;
                if (i < text.size()) ++i;
                if (i < text.size() && text[i] == '\\') ++i;
            }
            continue;
        }
        if (ch == '\t') { clean += "  "; ++i; continue; }
        if ((ch < 32 && ch != '\n') || ch == 127) { ++i; continue; }
        const auto point = decode(text, i);
        if (point.value >= 0x80 && point.value <= 0x9f) { i += point.bytes; continue; }
        if (point.value == 0xfffd) clean += "?";
        else clean.append(text.substr(i, point.bytes));
        i += point.bytes;
    }
    return clean;
}

std::vector<std::string> wrap_text(const std::string_view text, std::size_t width) {
    width = (std::max)(std::size_t{2}, width);
    const auto clean = sanitize_text(text);
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < clean.size()) {
        std::size_t end = start, used = 0, space = std::string::npos;
        while (end < clean.size() && clean[end] != '\n') {
            const auto point = decode(clean, end);
            if (used + codepoint_width(point.value) > width) break;
            if (clean[end] == ' ') space = end;
            used += codepoint_width(point.value);
            end += point.bytes;
        }
        if (end < clean.size() && clean[end] != '\n' && space != std::string::npos && space > start) end = space;
        auto line = clean.substr(start, end - start);
        while (!line.empty() && line.back() == ' ') line.pop_back();
        lines.push_back(std::move(line));
        start = end;
        if (start < clean.size() && clean[start] == '\n') ++start;
        else while (start < clean.size() && clean[start] == ' ') ++start;
    }
    if (lines.empty()) lines.emplace_back();
    return lines;
}

std::string truncate_path(const std::string_view text, const std::size_t width) {
    if (display_width(text) <= width) return std::string(text);
    const auto separator = text.find_last_of("/\\");
    if (separator != std::string_view::npos) {
        const auto tail = text.substr(separator);
        const auto tail_width = display_width(tail);
        if (width > tail_width + 4) return ellipsize(text.substr(0, separator), width - tail_width) + std::string(tail);
    }
    return ellipsize(text, width);
}

std::string format_relative_time(const std::time_t timestamp, const std::time_t now) {
    const auto elapsed = now - timestamp;
    if (elapsed < 0) return format_timestamp(timestamp);
    if (elapsed < 60) return "just now";
    if (elapsed < 3600) return std::to_string(elapsed / 60) + " min ago";
    if (elapsed < 86400) return std::to_string(elapsed / 3600) + " h ago";
    if (elapsed < 7 * 86400) return std::to_string(elapsed / 86400) + " days ago";
    return format_timestamp(timestamp);
}

std::string format_filesize(const std::uint64_t bytes) {
    constexpr std::array units{"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < units.size()) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64]{};
    if (unit == 0) std::snprintf(buffer, sizeof(buffer), "%llu %s", static_cast<unsigned long long>(bytes), units[unit]);
    else std::snprintf(buffer, sizeof(buffer), value >= 100.0 ? "%.0f %s" : "%.1f %s", value, units[unit]);
    return buffer;
}

std::string format_duration(const std::chrono::nanoseconds duration) {
    const auto microseconds = std::chrono::duration<double, std::micro>(duration).count();
    char buffer[64]{};
    if (microseconds < 1000.0) std::snprintf(buffer, sizeof(buffer), "%.0f µs", microseconds);
    else if (microseconds < 1'000'000.0) std::snprintf(buffer, sizeof(buffer), "%.0f ms", microseconds / 1000.0);
    else if (microseconds < 60'000'000.0) std::snprintf(buffer, sizeof(buffer), "%.1f s", microseconds / 1'000'000.0);
    else std::snprintf(buffer, sizeof(buffer), "%.1f min", microseconds / 60'000'000.0);
    return buffer;
}

std::string format_number(const std::uint64_t number) {
    auto value = std::to_string(number);
    for (std::ptrdiff_t position = static_cast<std::ptrdiff_t>(value.size()) - 3; position > 0; position -= 3) {
        value.insert(static_cast<std::size_t>(position), 1, ',');
    }
    return value;
}

std::string format_timestamp(const std::time_t timestamp) {
    std::tm local{};
    if (localtime_s(&local, &timestamp) != 0) return "-";
    constexpr std::array months{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%s %02d %02d:%02d", months[static_cast<std::size_t>(local.tm_mon)],
                  local.tm_mday, local.tm_hour, local.tm_min);
    return buffer;
}

std::size_t display_width(const std::string_view utf8) {
    std::size_t result = 0;
    for (std::size_t offset = 0; offset < utf8.size();) {
        const auto point = decode(utf8, offset);
        result += codepoint_width(point.value);
        offset += point.bytes;
    }
    return result;
}

std::string ellipsize(const std::string_view utf8, const std::size_t width) {
    if (display_width(utf8) <= width) return std::string(utf8);
    if (width == 0) return {};
    if (width == 1) return "…";
    std::size_t used = 0;
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        const auto point = decode(utf8, offset);
        const auto point_width = codepoint_width(point.value);
        if (used + point_width + 1 > width) break;
        used += point_width;
        offset += point.bytes;
    }
    return std::string(utf8.substr(0, offset)) + "…";
}

std::string pad(const std::string_view utf8, const std::size_t width, const bool right_aligned) {
    auto value = ellipsize(utf8, width);
    const auto padding = width > display_width(value) ? width - display_width(value) : 0;
    if (right_aligned) return std::string(padding, ' ') + value;
    value.append(padding, ' ');
    return value;
}

} // namespace atlas::terminal
