#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace atlas {

enum class ErrorKind {
    usage,
    not_found,
    io,
    process,
    lua,
    windows,
};

struct Error {
    ErrorKind kind{};
    std::string message;
    std::string detail;
    std::uint32_t native_code{};
    std::string debug_detail;
};

template <typename T>
using Result = std::expected<T, Error>;

using Status = Result<void>;

} // namespace atlas
