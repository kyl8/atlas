#include "atlas/platform/shell.hpp"

#include "atlas/platform/utf.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <string>

namespace atlas::win {

Status shell_open(const std::wstring_view target) {
    std::wstring owned(target);
    const auto result = reinterpret_cast<std::intptr_t>(
        ShellExecuteW(nullptr, L"open", owned.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
        const auto code = static_cast<unsigned long>(result);
        return std::unexpected(Error{ErrorKind::windows, "Could not open the target", error_message(code), code});
    }
    return {};
}

} // namespace atlas::win
