#pragma once

#include "atlas/error.hpp"

#include <string_view>

namespace atlas::win {

struct ServiceInfo {
    bool installed{};
    bool running{};
    bool automatic{};
};

Result<ServiceInfo> query_service(std::wstring_view name);

} // namespace atlas::win
