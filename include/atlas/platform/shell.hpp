#pragma once

#include "atlas/error.hpp"

#include <string_view>

namespace atlas::win {

Status shell_open(std::wstring_view target);

} // namespace atlas::win
