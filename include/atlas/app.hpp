#pragma once

#include <span>
#include <string_view>

namespace atlas {

int run(std::span<const std::string_view> arguments);

} // namespace atlas
