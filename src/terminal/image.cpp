#include "atlas/terminal/image.hpp"

#include "atlas/platform/paths.hpp"
#include "atlas/platform/utf.hpp"
#include "atlas/terminal/ui.hpp"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace atlas::terminal {
namespace {

template <typename Interface>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (value_) value_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    Interface** receive() noexcept { return &value_; }
    Interface* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }
private:
    Interface* value_{};
};

struct Pixel { std::uint8_t blue{}, green{}, red{}, alpha{}; };

std::string image_error(const char* action, const HRESULT result) {
    return std::string("Could not ") + action + " (HRESULT 0x" + [] (const HRESULT value) {
        char buffer[9]{};
        sprintf_s(buffer, "%08lX", static_cast<unsigned long>(value));
        return std::string(buffer);
    }(result) + ")";
}

void append_run(std::string& output, const char value, const std::size_t count) {
    if (count >= 4) output += '!' + std::to_string(count) + value;
    else output.append(count, value);
}

std::string encode_sixel(const std::span<const std::uint8_t> pixels, const UINT width,
                         const UINT height, const std::span<const WICColor> colors) {
    std::string output;
    output.reserve(static_cast<std::size_t>(width) * height * 2);
    output += "\x1bP0;1;0q\"1;1;" + std::to_string(width) + ';' + std::to_string(height);
    for (std::size_t index = 0; index < colors.size(); ++index) {
        const auto color = colors[index];
        const auto red = (color >> 16) & 0xff;
        const auto green = (color >> 8) & 0xff;
        const auto blue = color & 0xff;
        output += '#' + std::to_string(index) + ";2;" + std::to_string(red * 100 / 255) + ';' +
                  std::to_string(green * 100 / 255) + ';' + std::to_string(blue * 100 / 255);
    }

    std::vector<std::uint8_t> used;
    used.reserve(colors.size());
    std::vector<char> values(width);
    for (UINT top = 0; top < height; top += 6) {
        used.clear();
        std::vector<bool> present(colors.size());
        for (UINT y = top; y < (std::min)(height, top + 6); ++y) {
            for (UINT x = 0; x < width; ++x) {
                const auto color = pixels[static_cast<std::size_t>(y) * width + x];
                if (color < present.size()) present[color] = true;
            }
        }
        for (std::size_t index = 0; index < present.size(); ++index) {
            if (present[index]) used.push_back(static_cast<std::uint8_t>(index));
        }
        for (std::size_t color_index = 0; color_index < used.size(); ++color_index) {
            const auto color = used[color_index];
            std::size_t last = 0;
            bool any = false;
            for (UINT x = 0; x < width; ++x) {
                unsigned char bits = 0;
                for (UINT bit = 0; bit < 6 && top + bit < height; ++bit) {
                    if (pixels[static_cast<std::size_t>(top + bit) * width + x] == color) {
                        bits |= static_cast<unsigned char>(1u << bit);
                    }
                }
                values[x] = static_cast<char>('?' + bits);
                if (bits != 0) { last = x; any = true; }
            }
            if (!any) continue;
            output += '#' + std::to_string(color);
            for (std::size_t x = 0; x <= last;) {
                auto end = x + 1;
                while (end <= last && values[end] == values[x]) ++end;
                append_run(output, values[x], end - x);
                x = end;
            }
            if (color_index + 1 < used.size()) output.push_back('$');
        }
        if (top + 6 < height) output.push_back('-');
    }
    output += "\x1b\\";
    return output;
}

} // namespace

Result<RenderedImage> render_image(const Ui& ui, const std::filesystem::path& path,
                                  const ImageOptions options) {
    if (!ui.human()) return RenderedImage{};
    const auto display = win::narrow(path.native()).value_or("<invalid image path>");
    const auto resolved = win::resolve_path(display);
    if (!resolved) return std::unexpected(resolved.error());

    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize = SUCCEEDED(initialized);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        return std::unexpected(Error{ErrorKind::windows, image_error("initialize image support", initialized)});
    }
    struct Uninitializer { bool active; ~Uninitializer() { if (active) CoUninitialize(); } } uninitializer{uninitialize};

    ComPtr<IWICImagingFactory> factory;
    auto result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.receive()));
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::windows, image_error("create image decoder", result)});
    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(resolved->c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, decoder.receive());
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::not_found, "Could not load image: " + display});
    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, frame.receive());
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, image_error("read image frame", result)});
    UINT source_width = 0;
    UINT source_height = 0;
    result = frame->GetSize(&source_width, &source_height);
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, "Could not read image dimensions"});
    if (source_width == 0 || source_height == 0) return std::unexpected(Error{ErrorKind::io, "Image has no pixels: " + display});

    if (static_cast<std::uint64_t>(source_width) * source_height > 64'000'000) {
        return std::unexpected(Error{ErrorKind::io, "Image exceeds the 64 megapixel limit"});
    }
    const auto& capabilities = ui.capabilities();
    const bool sixel = ui.color() && options.backend != ImageBackend::blocks &&
        (capabilities.sixel || (options.backend == ImageBackend::sixel &&
                               capabilities.vt && capabilities.is_tty));
    const bool blocks = !sixel && ui.color() && ui.unicode();
    const double detected_ratio = static_cast<double>(capabilities.cell_width) /
                                  (std::max)(capabilities.cell_height, std::size_t{1});
    const double ratio = sixel ? std::clamp(detected_ratio, 0.25, 1.0)
                               : std::clamp(options.cell_ratio, 0.25, 1.0);
    const auto max_width = std::clamp(options.max_width, std::size_t{1}, std::size_t{64});
    const auto max_rows = std::clamp(options.max_height, std::size_t{1}, std::size_t{32});
    const double aspect = static_cast<double>(source_height) / source_width;
    UINT output_width = 1;
    UINT rows = 1;
    if (options.width && options.height) {
        output_width = static_cast<UINT>(std::clamp(*options.width, std::size_t{1}, std::size_t{64}));
        rows = static_cast<UINT>(std::clamp(*options.height, std::size_t{1}, std::size_t{32}));
    } else if (options.width) {
        output_width = static_cast<UINT>(std::clamp(*options.width, std::size_t{1}, std::size_t{64}));
        rows = (std::max)(1u, static_cast<UINT>(std::round(output_width * aspect * ratio)));
        rows = (std::min)(rows, 32u);
    } else if (options.height) {
        rows = static_cast<UINT>(std::clamp(*options.height, std::size_t{1}, std::size_t{32}));
        output_width = (std::max)(1u, static_cast<UINT>(std::round(rows / (aspect * ratio))));
        const auto bounded_width = static_cast<UINT>((std::min)(max_width, std::size_t{64}));
        if (output_width > bounded_width) {
            output_width = bounded_width;
            rows = (std::max)(1u, static_cast<UINT>(std::round(output_width * aspect * ratio)));
        }
    } else {
        output_width = static_cast<UINT>((std::max)(1.0,
            (std::min)(static_cast<double>(max_width), max_rows / (aspect * ratio))));
        rows = (std::max)(1u, static_cast<UINT>(std::round(output_width * aspect * ratio)));
    }
    // Windows Terminal reports legacy console font metrics, while Sixel pixels
    // are mapped to the pane's physical grid. Compensate for that difference so
    // a requested cell box occupies the same visual area as adjacent text.
    constexpr double sixel_pixel_scale = 1.25;
    const auto output_height = sixel
        ? static_cast<UINT>(std::round(rows *
              static_cast<double>((std::max)(capabilities.cell_height, std::size_t{1})) *
              sixel_pixel_scale))
        : rows * (blocks ? 2u : 1u);
    const auto pixel_width = sixel
        ? static_cast<UINT>(std::round(output_width *
              static_cast<double>((std::max)(capabilities.cell_width, std::size_t{1})) *
              sixel_pixel_scale))
        : output_width;
    ComPtr<IWICBitmapScaler> scaler;
    result = factory->CreateBitmapScaler(scaler.receive());
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::windows, image_error("create image scaler", result)});
    result = scaler->Initialize(frame.operator->(), pixel_width, output_height, WICBitmapInterpolationModeFant);
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, image_error("scale image", result)});

    if (sixel) {
        ComPtr<IWICPalette> palette;
        result = factory->CreatePalette(palette.receive());
        if (FAILED(result)) return std::unexpected(Error{ErrorKind::windows, image_error("create image palette", result)});
        // Sixel addresses colors through a palette. Use the full indexed range and
        // diffuse quantization error so photographs keep their gradients and skin
        // tones instead of becoming posterized by a small, nearest-color palette.
        constexpr UINT sixel_palette_size = 256;
        result = palette->InitializeFromBitmap(scaler.operator->(), sixel_palette_size, FALSE);
        if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, image_error("quantize image", result)});
        ComPtr<IWICFormatConverter> indexed;
        result = factory->CreateFormatConverter(indexed.receive());
        if (FAILED(result)) return std::unexpected(Error{ErrorKind::windows, image_error("create image converter", result)});
        result = indexed->Initialize(scaler.operator->(), GUID_WICPixelFormat8bppIndexed,
                                     WICBitmapDitherTypeErrorDiffusion, palette.operator->(), 0.0,
                                     WICBitmapPaletteTypeCustom);
        if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, image_error("convert image", result)});
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(pixel_width) * output_height);
        result = indexed->CopyPixels(nullptr, pixel_width, static_cast<UINT>(pixels.size()), pixels.data());
        if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, image_error("read image pixels", result)});
        UINT count = 0;
        result = palette->GetColorCount(&count);
        if (FAILED(result) || count == 0) return std::unexpected(Error{ErrorKind::io, "Could not read image palette"});
        std::vector<WICColor> colors(count);
        UINT actual = 0;
        result = palette->GetColors(count, colors.data(), &actual);
        if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, image_error("read image palette", result)});
        colors.resize(actual);
        return RenderedImage{{}, encode_sixel(pixels, pixel_width, output_height, colors), output_width, rows};
    }

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(converter.receive());
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::windows, image_error("create image converter", result)});
    result = converter->Initialize(scaler.operator->(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut);
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, image_error("convert image", result)});
    std::vector<Pixel> pixels(static_cast<std::size_t>(output_width) * output_height);
    result = converter->CopyPixels(nullptr, pixel_width * sizeof(Pixel),
                                   static_cast<UINT>(pixels.size() * sizeof(Pixel)), reinterpret_cast<BYTE*>(pixels.data()));
    if (FAILED(result)) return std::unexpected(Error{ErrorKind::io, image_error("read image pixels", result)});

    std::vector<std::string> lines;
    lines.reserve(rows);
    const auto background = ui.theme()[StyleToken::primary].background.value_or(Rgb{});
    const auto flatten = [background](const Pixel& pixel) {
        const auto blend = [a = pixel.alpha](const std::uint8_t fg, const std::uint8_t bg) {
            return static_cast<std::uint8_t>((fg * a + bg * (255 - a)) / 255);
        };
        return Rgb{blend(pixel.red, background.red), blend(pixel.green, background.green),
                   blend(pixel.blue, background.blue)};
    };
    for (UINT row = 0; row < output_height; row += blocks ? 2u : 1u) {
        std::string line;
        for (UINT column = 0; column < output_width; ++column) {
            const auto top = flatten(pixels[static_cast<std::size_t>(row) * output_width + column]);
            if (blocks) {
                const auto bottom = flatten(pixels[static_cast<std::size_t>(row + 1) * output_width + column]);
                TextStyle style;
                style.foreground = top;
                style.background = bottom;
                line += ui.render_fragment("▀", style);
            } else {
                constexpr std::string_view shades = " .:-=+*#%@";
                const auto luminance = (top.red * 54u + top.green * 183u + top.blue * 19u) / 256u;
                line += shades[luminance * (shades.size() - 1) / 255u];
            }
        }
        lines.push_back(std::move(line));
    }
    return RenderedImage{std::move(lines), {}, output_width, rows};
}

} // namespace atlas::terminal
