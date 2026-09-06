#pragma once

#include "atlas/terminal/theme.hpp"

#include <Windows.h>

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>

namespace atlas::terminal {

enum class ColorMode { automatic, always, never };
enum class ColorLevel { none, basic, ansi256, truecolor };
enum class Stream { output, error };

struct Capabilities {
    bool is_tty{};
    bool unicode{};
    bool vt{};
    bool interactive_input{};
    bool image_blocks{};
    bool sixel{};
    ColorLevel color{ColorLevel::none};
    std::size_t width{80};
    std::size_t height{30};
    std::size_t cell_width{8};
    std::size_t cell_height{16};
};

class Terminal {
public:
    explicit Terminal(ColorMode color_mode = ColorMode::automatic);

    [[nodiscard]] const Capabilities& capabilities(Stream stream) const noexcept;
    [[nodiscard]] std::string styled(std::string_view text, const TextStyle& style, Stream stream) const;
    void write(Stream stream, std::string_view text, bool flush = false);
    void clear_line(Stream stream);
    void refresh_size();
    void screen(bool enter);
    void replace_screen(std::string_view frame);

private:
    static Capabilities detect(HANDLE handle, int descriptor, ColorMode color_mode);

    Capabilities output_;
    Capabilities error_;
    mutable std::mutex mutex_;
};

} // namespace atlas::terminal
