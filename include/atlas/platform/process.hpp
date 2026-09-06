#pragma once

#include "atlas/error.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace atlas::win {

struct ProcessOptions {
    bool capture_output{true};
    bool hidden{};
    std::optional<std::filesystem::path> working_directory;
    std::stop_token stop_token{};
    std::function<void(std::string_view)> on_stdout;
    std::function<void(std::string_view)> on_stderr;
    // Use callbacks without retaining potentially unbounded stdout in memory.
    bool collect_stdout{true};
};

struct ProcessResult {
    std::uint32_t exit_code{};
    std::string stdout_text;
    std::string stderr_text;
};

struct SpawnOptions {
    bool hidden{};
    bool new_console{};
    std::optional<std::filesystem::path> working_directory;
};

std::wstring quote_argument(std::wstring_view argument);
Result<std::filesystem::path> find_executable(std::wstring_view executable);
Result<ProcessResult> execute(
    std::wstring_view executable,
    std::span<const std::wstring> arguments,
    const ProcessOptions& options = {});
Result<std::uint32_t> spawn(
    std::wstring_view executable,
    std::span<const std::wstring> arguments,
    bool hidden = false);
Result<std::uint32_t> spawn(
    std::wstring_view executable,
    std::span<const std::wstring> arguments,
    const SpawnOptions& options);

} // namespace atlas::win
