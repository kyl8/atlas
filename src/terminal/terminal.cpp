#include "atlas/terminal/terminal.hpp"
#include "atlas/platform/handle.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <io.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace atlas::terminal {
namespace {

bool environment_present(const wchar_t* name) {
    SetLastError(ERROR_SUCCESS);
    return GetEnvironmentVariableW(name, nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}

bool hosted_by_windows_terminal() {
    static const bool detected = [] {
        win::UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snapshot.get() == nullptr || snapshot.get() == INVALID_HANDLE_VALUE) return false;
        std::vector<PROCESSENTRY32W> processes;
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.get(), &entry)) return false;
        do processes.push_back(entry);
        while (Process32NextW(snapshot.get(), &entry));

        auto process = GetCurrentProcessId();
        for (std::size_t depth = 0; depth < 16; ++depth) {
            const auto found = std::ranges::find_if(processes, [process](const auto& candidate) {
                return candidate.th32ProcessID == process;
            });
            if (found == processes.end()) break;
            const std::wstring_view executable(found->szExeFile);
            if (_wcsicmp(executable.data(), L"WindowsTerminal.exe") == 0 ||
                _wcsicmp(executable.data(), L"WindowsTerminalPreview.exe") == 0) return true;
            if (found->th32ParentProcessID == 0 || found->th32ParentProcessID == process) break;
            process = found->th32ParentProcessID;
        }
        return false;
    }();
    return detected;
}

ColorLevel preferred_color_level() {
    wchar_t color[32]{};
    GetEnvironmentVariableW(L"COLORTERM", color, static_cast<DWORD>(std::size(color)));
    if (std::wstring_view(color) == L"truecolor" || std::wstring_view(color) == L"24bit") return ColorLevel::truecolor;
    if (environment_present(L"WT_SESSION")) return ColorLevel::truecolor;
    wchar_t term[64]{};
    GetEnvironmentVariableW(L"TERM", term, static_cast<DWORD>(std::size(term)));
    if (std::wstring_view(term) == L"ansi" || std::wstring_view(term) == L"vt100") return ColorLevel::basic;
    return ColorLevel::ansi256;
}

int distance(const Rgb left, const Rgb right) {
    const auto red = static_cast<int>(left.red) - right.red;
    const auto green = static_cast<int>(left.green) - right.green;
    const auto blue = static_cast<int>(left.blue) - right.blue;
    return red * red + green * green + blue * blue;
}

int ansi_basic(const Rgb color) {
    constexpr std::array<Rgb, 16> palette{
        Rgb{0, 0, 0}, Rgb{128, 0, 0}, Rgb{0, 128, 0}, Rgb{128, 128, 0},
        Rgb{0, 0, 128}, Rgb{128, 0, 128}, Rgb{0, 128, 128}, Rgb{192, 192, 192},
        Rgb{128, 128, 128}, Rgb{255, 0, 0}, Rgb{0, 255, 0}, Rgb{255, 255, 0},
        Rgb{0, 0, 255}, Rgb{255, 0, 255}, Rgb{0, 255, 255}, Rgb{255, 255, 255},
    };
    auto best = 0;
    auto best_distance = (std::numeric_limits<int>::max)();
    for (std::size_t index = 0; index < palette.size(); ++index) {
        const auto current = distance(color, palette[index]);
        if (current < best_distance) {
            best_distance = current;
            best = static_cast<int>(index);
        }
    }
    return best;
}

int ansi256(const Rgb color) {
    constexpr int levels[]{0, 95, 135, 175, 215, 255};
    const auto component = [&](const std::uint8_t value) {
        auto best = 0;
        for (int i = 1; i < 6; ++i) if (std::abs(levels[i] - value) < std::abs(levels[best] - value)) best = i;
        return best;
    };
    const auto red = component(color.red);
    const auto green = component(color.green);
    const auto blue = component(color.blue);
    const Rgb cube{static_cast<std::uint8_t>(levels[red]), static_cast<std::uint8_t>(levels[green]), static_cast<std::uint8_t>(levels[blue])};
    const auto gray_index = std::clamp((static_cast<int>(color.red) + color.green + color.blue) / 3 - 8, 0, 230) / 10;
    const auto gray = static_cast<std::uint8_t>(8 + gray_index * 10);
    return distance(color, Rgb{gray, gray, gray}) < distance(color, cube) ? 232 + gray_index : 16 + 36 * red + 6 * green + blue;
}

void append_code(std::string& sequence, const std::string& code) {
    if (!sequence.empty()) sequence.push_back(';');
    sequence += code;
}

std::string color_code(const Rgb color, const ColorLevel level, const bool background) {
    const auto prefix = background ? "48" : "38";
    if (level == ColorLevel::truecolor) {
        return std::string(prefix) + ";2;" + std::to_string(color.red) + ';' +
               std::to_string(color.green) + ';' + std::to_string(color.blue);
    }
    if (level == ColorLevel::ansi256) return std::string(prefix) + ";5;" + std::to_string(ansi256(color));
    const auto index = ansi_basic(color);
    const auto code = index < 8 ? (background ? 40 : 30) + index : (background ? 100 : 90) + index - 8;
    return std::to_string(code);
}

} // namespace

Terminal::Terminal(const ColorMode color_mode)
    : output_(detect(GetStdHandle(STD_OUTPUT_HANDLE), _fileno(stdout), color_mode)),
      error_(detect(GetStdHandle(STD_ERROR_HANDLE), _fileno(stderr), color_mode)) {}

Capabilities Terminal::detect(const HANDLE handle, const int descriptor, const ColorMode color_mode) {
    Capabilities result;
    DWORD console_mode = 0;
    const auto console = handle != nullptr && handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &console_mode) != 0;
    result.is_tty = console || _isatty(descriptor) != 0;
    result.unicode = GetConsoleOutputCP() == CP_UTF8 || environment_present(L"WT_SESSION") || environment_present(L"TERM");
    DWORD input_mode{};
    result.interactive_input = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode) != 0 && !environment_present(L"CI");

    if (console) {
        CONSOLE_SCREEN_BUFFER_INFO information{};
        if (GetConsoleScreenBufferInfo(handle, &information)) {
            result.width = static_cast<std::size_t>(information.srWindow.Right - information.srWindow.Left + 1);
            result.height = static_cast<std::size_t>(information.srWindow.Bottom - information.srWindow.Top + 1);
        }
        CONSOLE_FONT_INFOEX font{};
        font.cbSize = sizeof(font);
        if (GetCurrentConsoleFontEx(handle, FALSE, &font)) {
            if (font.dwFontSize.X > 0) result.cell_width = static_cast<std::size_t>(font.dwFontSize.X);
            if (font.dwFontSize.Y > 0) result.cell_height = static_cast<std::size_t>(font.dwFontSize.Y);
        }
    } else {
        wchar_t columns[16]{};
        const auto count = GetEnvironmentVariableW(L"COLUMNS", columns, static_cast<DWORD>(std::size(columns)));
        const auto parsed = count > 0 && count < std::size(columns) ? std::wcstoul(columns, nullptr, 10) : 0;
        if (parsed >= 20 && parsed <= 1000) result.width = parsed;
        wchar_t lines[16]{};
        const auto rows = GetEnvironmentVariableW(L"LINES", lines, static_cast<DWORD>(std::size(lines)));
        const auto height = rows > 0 && rows < std::size(lines) ? std::wcstoul(lines, nullptr, 10) : 0;
        if (height >= 10 && height <= 300) result.height = height;
    }
    result.vt = console ? SetConsoleMode(handle, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0
                        : color_mode == ColorMode::always;
    const auto no_color = environment_present(L"NO_COLOR") && color_mode == ColorMode::automatic;
    if (color_mode == ColorMode::never || no_color || (color_mode == ColorMode::automatic && !result.is_tty)) return result;
    if (console && !result.vt) return result;
    result.color = preferred_color_level();
    result.image_blocks = result.unicode && result.color != ColorLevel::none;
    // Windows Terminal 1.22+ supports Sixel. WT_SESSION is deliberately used as
    // a conservative capability signal; other terminals keep the block fallback.
    result.sixel = result.vt && result.is_tty && environment_present(L"WT_SESSION") &&
                   hosted_by_windows_terminal();
    return result;
}

const Capabilities& Terminal::capabilities(const Stream stream) const noexcept {
    return stream == Stream::output ? output_ : error_;
}

std::string Terminal::styled(const std::string_view text, const TextStyle& style, const Stream stream) const {
    const auto level = capabilities(stream).color;
    if (level == ColorLevel::none) return std::string(text);
    std::string codes;
    if (style.bold) append_code(codes, "1");
    if (style.dim) append_code(codes, "2");
    if (style.italic) append_code(codes, "3");
    if (style.underline) append_code(codes, "4");
    if (style.foreground) append_code(codes, color_code(*style.foreground, level, false));
    if (style.background) append_code(codes, color_code(*style.background, level, true));
    if (codes.empty()) return std::string(text);
    return "\x1b[" + codes + 'm' + std::string(text) + "\x1b[0m";
}

void Terminal::write(const Stream stream, const std::string_view text, const bool flush) {
    std::scoped_lock lock(mutex_);
    auto* file = stream == Stream::output ? stdout : stderr;
    std::fwrite(text.data(), 1, text.size(), file);
    if (flush) std::fflush(file);
}

void Terminal::clear_line(const Stream stream) {
    const auto& capability = capabilities(stream);
    if (!capability.is_tty) return;
    if (capability.vt) {
        write(stream, "\r\x1b[2K", true);
    } else {
        write(stream, "\r" + std::string(capability.width, ' ') + "\r", true);
    }
}

void Terminal::refresh_size() {
    for (const auto stream : {Stream::output, Stream::error}) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(GetStdHandle(stream == Stream::output ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE), &info)) {
            auto& capability = stream == Stream::output ? output_ : error_;
            capability.width = static_cast<std::size_t>(info.srWindow.Right - info.srWindow.Left + 1);
            capability.height = static_cast<std::size_t>(info.srWindow.Bottom - info.srWindow.Top + 1);
        }
    }
}

void Terminal::screen(const bool enter) {
    if (!output_.is_tty || !output_.vt) return;
    write(Stream::output, enter ? "\x1b[?1049h\x1b[?25l" : "\x1b[0m\x1b[?25h\x1b[?1049l", true);
}

void Terminal::replace_screen(const std::string_view frame) {
    if (!output_.is_tty || !output_.vt) return;
    write(Stream::output, "\x1b[H\x1b[2J" + std::string(frame), true);
}

} // namespace atlas::terminal
