#pragma once

#include <string>
#include <string_view>

namespace atlas::lua {

struct ErrorText {
    std::string message;
    std::string line;
    std::string traceback;
};

ErrorText parse_error(std::string_view raw);

} // namespace atlas::lua
