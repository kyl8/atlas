#include "atlas/lua/errors.hpp"

#include <cctype>

namespace atlas::lua {

ErrorText parse_error(const std::string_view raw) {
    ErrorText result;
    result.traceback = std::string(raw);
    const auto trace = raw.find("\nstack traceback:");
    const auto first_line = raw.substr(0, trace == std::string_view::npos ? raw.size() : trace);
    for (std::size_t index = 0; index + 2 < first_line.size(); ++index) {
        if (first_line[index] != ':' || !std::isdigit(static_cast<unsigned char>(first_line[index + 1]))) continue;
        auto end = index + 1;
        while (end < first_line.size() && std::isdigit(static_cast<unsigned char>(first_line[end]))) ++end;
        if (end >= first_line.size() || first_line[end] != ':') continue;
        result.line = std::string(first_line.substr(index + 1, end - index - 1));
        auto message = end + 1;
        while (message < first_line.size() && first_line[message] == ' ') ++message;
        result.message = std::string(first_line.substr(message));
        return result;
    }
    result.message = std::string(first_line);
    return result;
}

} // namespace atlas::lua
