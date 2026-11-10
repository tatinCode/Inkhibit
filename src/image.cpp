#include "inkhibit/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_FAILURE_USERMSG
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

namespace inkhibit::image {

    namespace {

        // Limits protect the 512 MB Raspberry Pi before any pixel decoding.
        constexpr int max_side = 8192;
        constexpr long long max_pixels = 33'000'000;
        constexpr std::uintmax_t max_file_bytes = 64'000'000;

        // Initial palette references, subject to tuning after a physical run.
        struct PaletteEntry {
            framebuffer::Color color;
            std::uint8_t rgb[3];
        };

        constexpr std::array<PaletteEntry, 6> palette{{
            {framebuffer::Color::black,  {20,  20,  20}},
                {framebuffer::Color::white,  {240, 240, 240}},
                {framebuffer::Color::yellow, {255, 214, 0}},
                {framebuffer::Color::red,    {200, 34, 39}},
                {framebuffer::Color::blue,   {35, 70, 147}},
                {framebuffer::Color::green,  {52, 137, 63}}
        }};


        const PaletteEntry& closest_palette_entry(
                float red,
                float green,
                float blue) {
            std::size_t best = 0;
            float best_distance = std::numeric_limits<float>::max();

            for (std::size_t i = 0; i < palette.size(); ++i) {
                const float dr = red - palette[i].rgb[0];
                const float dg = green - palette[i].rgb[1];
                const float db = blue - palette[i].rgb[2];

                const float distance =
                    dr * dr + dg * dg + db * db;

                // Strict comparison preserves the earlier entry on ties.
                if (distance < best_distance) {
                    best_distance = distance;
                    best = i;
                }
            }

            return palette[best];
        }

        bool within_limits(int width, int height) {
            return width >= 1 && width <= max_side
                && height >= 1 && height <= max_side
                && static_cast<long long>(width) * height <= max_pixels;
        }

        std::size_t pixel_bytes(int width, int height) {
            return static_cast<std::size_t>(width)
                * static_cast<std::size_t>(height) * 3;
        }

        std::string failure(const char* prefix) {
            const char* reason = stbi_failure_reason();
            return std::string(prefix)
                + (reason != nullptr ? reason : "unknown error");
        }

        // Holds stbi-owned pixels until they are copied into the result.
        struct LoadedPixels {
            std::uint8_t* data = nullptr;

            ~LoadedPixels() {
                if (data != nullptr) {
                    stbi_image_free(data);
                }
            }
        };

    } // namespace

    DecodedImage decode_image_bytes(const std::uint8_t* data, std::size_t size) {
        if (data == nullptr || size == 0) {
            throw std::invalid_argument("Image data is empty");
        }

        if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument("Image data is too large to decode");
        }

        int width = 0;
        int height = 0;
        int components = 0;

        // Inspect limits before any pixel decoding begins.
        if (!stbi_info_from_memory(
                    data,
                    static_cast<int>(size),
                    &width,
                    &height,
                    &components)) {
            throw std::invalid_argument(failure("Unsupported or corrupt image: "));
        }

        if (!within_limits(width, height)) {
            throw std::invalid_argument(
                    "Image dimensions exceed the decode limits");
        }

        LoadedPixels pixels;
        pixels.data = stbi_load_from_memory(
                data,
                static_cast<int>(size),
                &width,
                &height,
                &components,
                4);

        if (pixels.data == nullptr) {
            throw std::runtime_error(failure("Image decoding failed: "));
        }

        DecodedImage result;
        result.width = width;
        result.height = height;
        result.rgb.resize(pixel_bytes(width, height));

        const std::size_t pixel_count =
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height);

        for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
            const std::uint8_t* source = pixels.data + pixel * 4;
            std::uint8_t* target = result.rgb.data() + pixel * 3;
            const unsigned alpha = source[3];

            for (int channel = 0; channel < 3; ++channel) {
                // Composite over white; rounding matches common practice.
                target[channel] = static_cast<std::uint8_t>(
                        (source[channel] * alpha
                         + 0xFF * (0xFF - alpha)
                         + 127)
                        / 0xFF);
            }
        }

        return result;
    }

    DecodedImage decode_image(const std::filesystem::path& path) {
        std::error_code inspected;
        const auto size = std::filesystem::file_size(path, inspected);

        if (!inspected && size > max_file_bytes) {
            throw std::invalid_argument(
                    "Image file is larger than the allowed "
                    + std::to_string(max_file_bytes) + " bytes");
        }

        std::ifstream input(path, std::ios::binary);

        if (!input) {
            throw std::runtime_error(
                    "Cannot open image file: " + path.string());
        }

        std::vector<std::uint8_t> bytes(
                (std::istreambuf_iterator<char>(input)),
                std::istreambuf_iterator<char>());

        return decode_image_bytes(bytes.data(), bytes.size());
    }

    DecodedImage fit_to_canvas(const DecodedImage& image) {
        if (image.width < 1 || image.height < 1
                || image.rgb.size() != pixel_bytes(image.width, image.height)) {
            throw std::invalid_argument(
                    "Image size does not match its pixels");
        }

        const int canvas_w =
            static_cast<int>(framebuffer::landscape_width);
        const int canvas_h =
            static_cast<int>(framebuffer::landscape_height);

        const double width_ratio =
            static_cast<double>(canvas_w) / image.width;
        const double height_ratio =
            static_cast<double>(canvas_h) / image.height;
        const double scale =
            std::min({width_ratio, height_ratio, 1.0});

        const int sized_w = std::max(
                1,
                static_cast<int>(std::lround(image.width * scale))
                );
        const int sized_h = std::max(
                1,
                static_cast<int>(std::lround(image.height * scale))
                );
        const int offset_x = (canvas_w - sized_w) / 2;
        const int offset_y = (canvas_h - sized_h) / 2;

        DecodedImage result;
        result.width = canvas_w;
        result.height = canvas_h;
        result.rgb.assign(
                static_cast<std::size_t>(canvas_w) * canvas_h * 3,
                0xFF);

        for (int oy = 0; oy < sized_h; ++oy) {
            const int sy = static_cast<int>(
                    static_cast<std::int64_t>(oy) * image.height / sized_h);

            for (int ox = 0; ox < sized_w; ++ox) {
                const int sx = static_cast<int>(
                        static_cast<std::int64_t>(ox) * image.width / sized_w);

                const std::size_t source =
                    (static_cast<std::size_t>(sy) * image.width + sx) * 3;
                const std::size_t target =
                    (static_cast<std::size_t>(oy + offset_y) * canvas_w
                     + ox + offset_x)
                    * 3;

                for (int channel = 0; channel < 3; ++channel) {
                    result.rgb[target + channel] =
                        image.rgb[source + channel];
                }
            }
        }

        return result;
    }

    framebuffer::Color nearest_color(
            std::uint8_t red,
            std::uint8_t green,
            std::uint8_t blue) {
        return closest_palette_entry(red, green, blue).color;
    }

    //framebuffer::Color nearest_color(
    //    std::uint8_t red,
    //    std::uint8_t green,
    //    std::uint8_t blue)
    //{
    //    int best = 0;
    //    long long best_distance = 0;
    //    bool first = true;
    //
    //    for (int i = 0; i < static_cast<int>(palette.size()); ++i) {
    //        const long long dr = red - palette[i].rgb[0];
    //        const long long dg = green - palette[i].rgb[1];
    //        const long long db = blue - palette[i].rgb[2];
    //        const long long distance = dr * dr + dg * dg + db * db;
    //
    //        if (first || distance < best_distance) {
    //            first = false;
    //            best_distance = distance;
    //            best = i;
    //        }
    //    }
    //
    //    return palette[best].color;
    //}

    framebuffer::Buffer quantize_to_framebuffer(
            const DecodedImage& image,
            Dither method) {
        constexpr std::size_t width =
            framebuffer::landscape_width;
        constexpr std::size_t height =
            framebuffer::landscape_height;

        if (image.width != static_cast<int>(width)
                || image.height != static_cast<int>(height)
                || image.rgb.size() != width * height * 3) {
            throw std::invalid_argument(
                    "Quantization requires a fitted 600x400 RGB image");
        }

        if (method != Dither::nearest
                && method != Dither::floyd_steinberg) {
            throw std::invalid_argument(
                    "Unsupported dithering method");
        }

        const bool diffuse = method == Dither::floyd_steinberg;

        auto buffer = framebuffer::make_framebuffer();

        using Error = std::array<float, 3>;

        // Two rows of signed RGB errors, with one padding slot on each side.
        // Approximately 14 KB, independent of the input file's original size.
        std::vector<Error> current(width + 2);
        std::vector<Error> next(width + 2);

        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const std::size_t offset = (y * width + x) * 3;

                std::array<float, 3> corrected{};

                for (std::size_t channel = 0; channel < 3; ++channel) {
                    float value = image.rgb[offset + channel];

                    if (diffuse) {
                        value += current[x + 1][channel];
                    }

                    corrected[channel] =
                        std::clamp(value, 0.0f, 255.0f);
                }

                const auto& chosen = closest_palette_entry(
                        corrected[0],
                        corrected[1],
                        corrected[2]);

                framebuffer::set_landscape_pixel(
                        buffer,
                        static_cast<int>(x),
                        static_cast<int>(y),
                        chosen.color);

                if (!diffuse) {
                    continue;
                }

                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const float error =
                        corrected[channel] - chosen.rgb[channel];

                    // Right: 7/16.
                    if (x + 1 < width) {
                        current[x + 2][channel] +=
                            error * (7.0f / 16.0f);
                    }

                    if (y + 1 < height) {
                        // Below-left: 3/16.
                        if (x > 0) {
                            next[x][channel] +=
                                error * (3.0f / 16.0f);
                        }

                        // Below: 5/16.
                        next[x + 1][channel] +=
                            error * (5.0f / 16.0f);

                        // Below-right: 1/16.
                        if (x + 1 < width) {
                            next[x + 2][channel] +=
                                error * (1.0f / 16.0f);
                        }
                    }
                }
            }

            if (diffuse) {
                current.swap(next);
                std::fill(next.begin(), next.end(), Error{});
            }
        }

        return buffer;
    }

} // namespace inkhibit::image
