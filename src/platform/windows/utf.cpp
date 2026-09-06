#include "atlas/platform/utf.hpp"

#include <Windows.h>

#include <limits>
#include <vector>

namespace atlas::win {
namespace {

template <typename Char>
bool too_large(std::basic_string_view<Char> text) {
    return text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)());
}

} // namespace

Result<std::wstring> widen(const std::string_view utf8) {
    if (utf8.empty()) {
        return std::wstring{};
    }
    if (too_large(utf8)) {
        return std::unexpected(Error{ErrorKind::usage, "UTF-8 input is too large"});
    }

    const auto size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size == 0) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Invalid UTF-8 text", error_message(code), code});
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), result.data(), size) == 0) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not convert UTF-8 text", error_message(code), code});
    }
    return result;
}

Result<std::string> narrow(const std::wstring_view utf16) {
    if (utf16.empty()) {
        return std::string{};
    }
    if (too_large(utf16)) {
        return std::unexpected(Error{ErrorKind::usage, "UTF-16 input is too large"});
    }

    const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(),
                                          static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Invalid UTF-16 text", error_message(code), code});
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(),
                            static_cast<int>(utf16.size()), result.data(), size, nullptr, nullptr) == 0) {
        const auto code = GetLastError();
        return std::unexpected(Error{ErrorKind::windows, "Could not convert UTF-16 text", error_message(code), code});
    }
    return result;
}

std::string error_message(const unsigned long code) {
    wchar_t* buffer = nullptr;
    const auto length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return "Windows error " + std::to_string(code);
    }

    std::wstring_view message(buffer, length);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
        message.remove_suffix(1);
    }
    auto converted = narrow(message);
    LocalFree(buffer);
    return converted ? *converted : "Windows error " + std::to_string(code);
}

} // namespace atlas::win
