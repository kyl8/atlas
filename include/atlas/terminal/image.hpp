#pragma once

#include "atlas/error.hpp"
#include "atlas/terminal/theme.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace atlas::terminal {

struct ImageOptions {
    std::size_t max_width{48};
    std::size_t max_height{16};
    std::optional<std::size_t> width;
    std::optional<std::size_t> height;
    double cell_ratio{0.5};
    ImageBackend backend{ImageBackend::automatic};
};

struct RenderedImage {
    std::vector<std::string> lines;
    std::string sixel;
    std::size_t width{};
    std::size_t height{};

    [[nodiscard]] bool native() const noexcept { return !sixel.empty(); }
};

class Ui;
Result<RenderedImage> render_image(const Ui& ui, const std::filesystem::path& path,
                                  ImageOptions options = {});

} // namespace atlas::terminal
