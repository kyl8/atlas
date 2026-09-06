#include "atlas/platform/paths.hpp"

#include "atlas/platform/utf.hpp"
#include "atlas/platform/handle.hpp"

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <iterator>
#include <algorithm>

namespace atlas::win {
namespace {

Result<std::wstring> expand_environment(const std::wstring_view input) {
    std::wstring owned(input);
    const auto required = ExpandEnvironmentStringsW(owned.c_str(), nullptr, 0);
    if (required == 0) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not expand environment variables", error_message(code), code});
    }
    std::wstring expanded(required, L'\0');
    if (ExpandEnvironmentStringsW(owned.c_str(), expanded.data(), required) == 0) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not expand environment variables", error_message(code), code});
    }
    expanded.resize(required - 1);
    return expanded;
}

Result<std::wstring> home_directory() {
    const auto required = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
    if (required == 0) {
        return std::unexpected(Error{ErrorKind::not_found, "USERPROFILE is not defined"});
    }
    std::wstring value(required, L'\0');
    const auto written = GetEnvironmentVariableW(L"USERPROFILE", value.data(), required);
    if (written == 0 || written >= required) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not read USERPROFILE", error_message(code), code});
    }
    value.resize(written);
    return value;
}

} // namespace

Result<std::filesystem::path> resolve_path(const std::string_view input) {
    auto wide = widen(input);
    if (!wide) {
        return std::unexpected(wide.error());
    }
    auto expanded = expand_environment(*wide);
    if (!expanded) {
        return std::unexpected(expanded.error());
    }

    if (*expanded == L"~" || expanded->starts_with(L"~/") || expanded->starts_with(L"~\\")) {
        auto home = home_directory();
        if (!home) {
            return std::unexpected(home.error());
        }
        *expanded = *home + expanded->substr(1);
    }

    std::error_code error;
    auto path = std::filesystem::absolute(std::filesystem::path(*expanded), error).lexically_normal();
    if (error) {
        return std::unexpected(Error{ErrorKind::io, "Could not resolve path: " + std::string(input), error.message()});
    }
    return path;
}

Result<std::filesystem::path> user_atlas_directory() {
    auto home = home_directory();
    if (!home) {
        return std::unexpected(home.error());
    }
    return std::filesystem::path(*home) / L".atlas";
}

Status write_atomic(const std::filesystem::path& path, const std::string_view contents,
                    const std::string_view expected) {
    const auto lock_path = path.native() + L".lock";
    UniqueHandle lock(CreateFileW(lock_path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
                                  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr));
    if (lock.get() == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::io, "Could not lock data file; retry the command", error_message(code), code});
    }
    // Read the current value under the lock so concurrent edits are not lost.
    std::error_code error;
    const auto exists = std::filesystem::exists(path, error);
    if (error) return std::unexpected(Error{ErrorKind::io, "Could not inspect data file", error.message()});
    std::string previous;
    if (exists) {
        std::ifstream input(path, std::ios::binary);
        if (!input) return std::unexpected(Error{ErrorKind::io, "Could not read data file"});
        previous.assign(std::istreambuf_iterator<char>(input), {});
        if (input.bad()) return std::unexpected(Error{ErrorKind::io, "Could not finish reading data file"});
    }
    if (previous != expected) {
        return std::unexpected(Error{ErrorKind::io, "Data changed in another process; retry the command"});
    }

    const auto temporary = path.native() + L".tmp";
    UniqueHandle output(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (output.get() == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::io, "Could not create temporary data file (check the .tmp sibling)", error_message(code), code});
    }
    struct TemporaryCleanup {
        const std::wstring& path;
        UniqueHandle& file;
        bool installed{};
        ~TemporaryCleanup() {
            file.reset();
            if (!installed) DeleteFileW(path.c_str());
        }
    } cleanup{temporary, output};
    for (std::size_t offset = 0; offset < contents.size();) {
        const auto count = static_cast<DWORD>((std::min)(contents.size() - offset, std::size_t{1'048'576}));
        DWORD written = 0;
        if (!WriteFile(output.get(), contents.data() + offset, count, &written, nullptr) || written == 0) {
            const auto code = GetLastError();
            return std::unexpected(Error{ErrorKind::io, "Could not write temporary data file", error_message(code), code});
        }
        offset += written;
    }
    if (!FlushFileBuffers(output.get())) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::io, "Could not flush data file", error_message(code), code});
    }
    output.reset();
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        auto code = GetLastError();
        // A watcher may deny delete/rename sharing. Keep the atomic path for the
        // normal case and copy over the file only for that Windows limitation.
        if ((code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION) &&
            CopyFileW(temporary.c_str(), path.c_str(), FALSE)) {
            cleanup.installed = true;
            DeleteFileW(temporary.c_str());
            return {};
        }
        code = GetLastError();
        return std::unexpected(Error{ErrorKind::io, "Could not replace data file", error_message(code), code});
    }
    cleanup.installed = true;
    return {};
}

} // namespace atlas::win
