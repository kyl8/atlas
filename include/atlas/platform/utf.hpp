#pragma once

#include "atlas/error.hpp"

#include <string>
#include <string_view>

namespace atlas::win {

Result<std::wstring> widen(std::string_view utf8);
Result<std::string> narrow(std::wstring_view utf16);
std::string error_message(unsigned long code);

} // namespace atlas::win
