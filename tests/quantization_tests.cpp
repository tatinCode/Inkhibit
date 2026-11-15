#include "inkhibit/image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace img = inkhibit::image;
namespace fb = inkhibit::framebuffer;
using Rgb = std::array<std::uint8_t, 3>;

constexpr std::array<img::Dither, 2> methods{
    img::Dither::nearest, img::Dither::floyd_steinberg};

struct PaletteReference {
    Rgb rgb;
    std::uint8_t code;
};

// Initial palette contract, independent of the implementation's private table.
constexpr std::array<PaletteReference, 6> palette{{
    {{20, 20, 20}, 0x0}, {{240, 240, 240}, 0x1},
    {{255, 214, 0}, 0x2}, {{200, 34, 39}, 0x3},
    {{35, 70, 147}, 0x5}, {{52, 137, 63}, 0x6}
}};

// Checks remain enabled in Release builds.
#define REQUIRE(condition)                                                \
    do {                                                                  \
        if (!(condition)) {                                               \
            throw std::runtime_error(std::string(__FILE__) + ":"           \
                + std::to_string(__LINE__) + ": " + #condition);           \
        }                                                                 \
    } while (false)

template<class Function>
void expect_invalid(Function&& function, const std::string& message)
{
    try {
        function();
    } catch (const std::invalid_argument& error) {
        if (std::string(error.what()).find(message) == std::string::npos) {
            throw std::runtime_error("Expected exception containing '" + message
                + "', got: " + error.what());
        }
        return;
    }
    throw std::runtime_error("Expected invalid_argument containing: " + message);
}

img::DecodedImage solid_canvas(const Rgb& rgb)
{
    img::DecodedImage image{600, 400, std::vector<std::uint8_t>(600 * 400 * 3)};
    for (std::size_t i = 0; i < image.rgb.size(); i += 3) {
        std::copy(rgb.begin(), rgb.end(), image.rgb.begin() + i);
    }
    return image;
}

void set_pixel(img::DecodedImage& image, int x, int y, const Rgb& rgb)
{
    REQUIRE(x >= 0 && x < 600 && y >= 0 && y < 400);
    const auto offset = (static_cast<std::size_t>(y) * 600 + x) * 3;
    std::copy(rgb.begin(), rgb.end(), image.rgb.begin() + offset);
}

std::uint8_t pixel_code(const fb::Buffer& buffer, int x, int y)
{
    REQUIRE(buffer.size() == 120'000);
    REQUIRE(x >= 0 && x < 600 && y >= 0 && y < 400);
    // Fixed panel contract: landscape columns are 200-byte native rows;
    // even landscape y occupies the low nibble, odd y the high nibble.
    const auto offset = static_cast<std::size_t>(x) * 200 + (399 - y) / 2;
    return static_cast<std::uint8_t>((buffer.at(offset) >> ((y % 2) ? 4 : 0)) & 0xf);
}

void require_code(const fb::Buffer& buffer, int x, int y, std::uint8_t expected)
{
    const auto actual = pixel_code(buffer, x, y);
    if (actual != expected) {
        throw std::runtime_error("Pixel (" + std::to_string(x) + ","
            + std::to_string(y) + "): expected code " + std::to_string(expected)
            + ", got " + std::to_string(actual));
    }
}

void require_valid_buffer(const fb::Buffer& buffer)
{
    REQUIRE(buffer.size() == 120'000);
    for (const auto byte : buffer) {
        for (const unsigned code : {byte >> 4, byte & 0xf}) {
            REQUIRE(code <= 3 || code == 5 || code == 6);
        }
    }
}

img::DecodedImage gradient_canvas()
{
    auto image = solid_canvas({0, 0, 0});
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 600; ++x) {
            set_pixel(image, x, y, {
                static_cast<std::uint8_t>(x * 255 / 599),
                static_cast<std::uint8_t>(y * 255 / 399),
                static_cast<std::uint8_t>((x + 2 * y) * 37 % 256)});
        }
    }
    return image;
}

// Integer-distance oracle preserves the pre-dithering converter's behavior.
// It does not call the newly shared floating-point palette selector.
std::uint8_t legacy_nearest_code(const Rgb& rgb)
{
    int best_distance = std::numeric_limits<int>::max();
    std::uint8_t best = 0;
    for (const auto& entry : palette) {
        int distance = 0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int delta = static_cast<int>(rgb[channel]) - entry.rgb[channel];
            distance += delta * delta;
        }
        if (distance < best_distance) {
            best_distance = distance;
            best = entry.code;
        }
    }
    return best;
}

void test_nearest_compatibility()
{
    auto image = gradient_canvas();
    // Exact black/blue tie: both squared distances are 6201; black wins.
    set_pixel(image, 0, 0, {1, 72, 76});
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::nearest);
    require_valid_buffer(buffer);
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 600; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * 600 + x) * 3;
            require_code(buffer, x, y, legacy_nearest_code({
                image.rgb[offset], image.rgb[offset + 1], image.rgb[offset + 2]}));
        }
    }
    require_code(buffer, 0, 0, 0x0);
}

void test_palette_fills()
{
    for (const auto& entry : palette) {
        const auto image = solid_canvas(entry.rgb);
        const auto byte = static_cast<std::uint8_t>((entry.code << 4) | entry.code);
        for (const auto method : methods) {
            const auto buffer = img::quantize_to_framebuffer(image, method);
            REQUIRE(buffer.size() == 120'000);
            REQUIRE(std::all_of(buffer.begin(), buffer.end(),
                [=](std::uint8_t value) { return value == byte; }));
        }
    }
}

void test_orientation()
{
    auto image = solid_canvas(palette[1].rgb);
    set_pixel(image, 0, 0, palette[3].rgb);
    set_pixel(image, 599, 0, palette[4].rgb);
    set_pixel(image, 0, 399, palette[5].rgb);
    set_pixel(image, 599, 399, palette[2].rgb);
    // Interior upright L checks row strides and nibble ordering as well.
    for (const auto& xy : {std::pair{20, 20}, std::pair{20, 21},
            std::pair{21, 21}, std::pair{22, 21}}) {
        set_pixel(image, xy.first, xy.second, palette[0].rgb);
    }
    fb::Buffer expected(120'000, 0x11);
    expected[199] = 0x13;
    expected[119'999] = 0x15;
    expected[0] = 0x61;
    expected[119'800] = 0x21;
    expected[4'189] = 0x00;
    expected[4'389] = 0x01;
    expected[4'589] = 0x01;
    for (const auto method : methods) {
        REQUIRE(img::quantize_to_framebuffer(image, method) == expected);
    }
}

// (20,68,68) quantizes to black (20,20,20), giving error (0,48,48).
// Each isolated probe below is black without diffusion, blue with the correct
// weight, and black if that weight is reduced by 1/16. All arithmetic in these
// examples is exact, with no ambiguous floating-point palette boundaries.
void test_diffusion_right()
{
    auto image = solid_canvas(palette[0].rgb);
    set_pixel(image, 0, 399, {20, 68, 68});
    set_pixel(image, 1, 399, {20, 20, 65});
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    require_code(buffer, 0, 399, 0x0);
    // Corrected probe (20,41,86), from 7/16 of the source error.
    require_code(buffer, 1, 399, 0x5);
}

void test_diffusion_below_left()
{
    auto image = solid_canvas(palette[0].rgb);
    set_pixel(image, 1, 398, {20, 68, 68});
    // Absorb the rightward error at exact white, yielding zero further error.
    set_pixel(image, 2, 398, {240, 219, 219});
    set_pixel(image, 0, 399, {20, 20, 82});
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    require_code(buffer, 1, 398, 0x0);
    require_code(buffer, 2, 398, 0x1);
    // Corrected probe (20,29,91), from 3/16.
    require_code(buffer, 0, 399, 0x5);
}

void test_diffusion_below()
{
    auto image = solid_canvas(palette[0].rgb);
    set_pixel(image, 0, 398, {20, 68, 68});
    set_pixel(image, 1, 398, {240, 219, 219});
    set_pixel(image, 0, 399, {20, 20, 74});
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    require_code(buffer, 1, 398, 0x1);
    // Corrected probe (20,35,89), from 5/16.
    require_code(buffer, 0, 399, 0x5);
}

void test_diffusion_below_right()
{
    auto image = solid_canvas(palette[0].rgb);
    set_pixel(image, 0, 398, {20, 68, 68});
    set_pixel(image, 1, 398, {240, 219, 219});
    // Absorb the downward error so it cannot affect the right-hand probe.
    set_pixel(image, 0, 399, {240, 225, 225});
    set_pixel(image, 1, 399, {20, 20, 91});
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    require_code(buffer, 1, 398, 0x1);
    require_code(buffer, 0, 399, 0x1);
    // Corrected probe (20,23,94), from 1/16.
    require_code(buffer, 1, 399, 0x5);
}

void test_signed_error()
{
    auto image = solid_canvas(palette[0].rgb);
    set_pixel(image, 0, 399, {35, 38, 115});
    set_pixel(image, 1, 399, {35, 20, 93});
    const auto nearest = img::quantize_to_framebuffer(image, img::Dither::nearest);
    require_code(nearest, 1, 399, 0x5);
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    // Blue source error is (0,-32,-32); right receives (0,-14,-14).
    require_code(buffer, 0, 399, 0x5);
    require_code(buffer, 1, 399, 0x0);
}

void test_clipping()
{
    // Exact-rational bottom-row examples. Without clipping corrected RGB to
    // [0,255] before selecting a color AND computing error, these diverge.
    constexpr std::array<std::array<Rgb, 6>, 2> rows{{
        {{{255, 255, 255}, {255, 0, 0}, {124, 47, 114},
          {117, 170, 248}, {26, 210, 60}, {197, 191, 111}}},
        {{{0, 0, 0}, {0, 0, 0}, {120, 141, 124},
          {61, 214, 140}, {125, 255, 55}, {46, 220, 162}}}
    }};
    constexpr std::array<std::array<std::uint8_t, 6>, 2> expected{{
        {{1, 3, 3, 5, 6, 1}}, {{0, 0, 6, 6, 6, 6}}
    }};
    for (std::size_t row = 0; row < rows.size(); ++row) {
        auto image = solid_canvas(palette[0].rgb);
        for (int x = 0; x < 6; ++x) {
            set_pixel(image, x, 399, rows[row][x]);
        }
        const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
        for (int x = 0; x < 6; ++x) {
            require_code(buffer, x, 399, expected[row][x]);
        }
    }
}

void test_known_patch()
{
    // Golden output computed independently with exact rational arithmetic and
    // a full pixel/error plane (600 columns), rather than two rolling rows.
    // This checks RGB-channel diffusion, scan order, and clearing reused rows.
    constexpr std::array<Rgb, 24> patch{{
        {125, 217, 47}, {251, 143, 118}, {181, 231, 33},
        {17, 139, 42}, {131, 91, 213}, {253, 127, 44},
        {178, 53, 139}, {21, 86, 209}, {55, 214, 135},
        {200, 46, 150}, {14, 138, 76}, {108, 101, 20},
        {253, 32, 195}, {200, 93, 36}, {13, 194, 27},
        {115, 94, 182}, {208, 32, 126}, {110, 83, 208},
        {253, 112, 14}, {182, 89, 5}, {151, 194, 11},
        {100, 8, 215}, {100, 88, 189}, {255, 235, 239}
    }};
    constexpr std::array<std::uint8_t, 24> expected{
        6, 2, 2, 6, 5, 2,
        3, 5, 5, 1, 5, 6,
        1, 3, 6, 5, 3, 5,
        3, 3, 2, 5, 5, 1
    };
    auto image = solid_canvas(palette[0].rgb);
    for (int i = 0; i < 24; ++i) {
        set_pixel(image, i % 6, i / 6, patch[i]);
    }
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    require_valid_buffer(buffer);
    for (int i = 0; i < 24; ++i) {
        require_code(buffer, i % 6, i / 6, expected[i]);
    }
}

void test_edge_left()
{
    auto image = solid_canvas(palette[0].rgb);
    set_pixel(image, 0, 398, {20, 68, 68});
    set_pixel(image, 1, 398, {240, 219, 219});
    set_pixel(image, 0, 399, {20, 20, 74});
    set_pixel(image, 599, 399, {20, 20, 82});
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    require_code(buffer, 0, 399, 0x5);
    // Off-left 3/16 error must not wrap to the opposite side of the canvas.
    require_code(buffer, 599, 399, 0x0);
}

void test_edge_right()
{
    auto image = solid_canvas(palette[0].rgb);
    set_pixel(image, 599, 398, {20, 68, 68});
    set_pixel(image, 598, 399, {20, 20, 82});
    set_pixel(image, 0, 399, {20, 20, 65});
    const auto buffer = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    // Valid below-left diffusion still occurs at the rightmost column.
    require_code(buffer, 598, 399, 0x5);
    // Off-right 7/16 error must not become next row's first-pixel error.
    require_code(buffer, 0, 399, 0x0);
}

void test_bottom_right()
{
    auto image = solid_canvas(palette[0].rgb);
    set_pixel(image, 599, 399, {35, 38, 115});
    fb::Buffer expected(120'000, 0x00);
    expected[119'800] = 0x50;
    REQUIRE(img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg) == expected);
}

void test_default_method()
{
    const auto image = gradient_canvas();
    const auto defaults = img::quantize_to_framebuffer(image);
    require_valid_buffer(defaults);
    REQUIRE(defaults == img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg));
    REQUIRE(defaults != img::quantize_to_framebuffer(image, img::Dither::nearest));
}

void test_repeatability()
{
    const auto image = gradient_canvas();
    const auto original = image.rgb;
    const auto first = img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg);
    // Interleaved images/modes must not leak accumulated error between calls.
    (void)img::quantize_to_framebuffer(solid_canvas({100, 110, 120}));
    (void)img::quantize_to_framebuffer(image, img::Dither::nearest);
    REQUIRE(img::quantize_to_framebuffer(image, img::Dither::floyd_steinberg) == first);
    REQUIRE(image.width == 600 && image.height == 400);
    REQUIRE(image.rgb == original);
}

void test_invalid_dimensions()
{
    const std::array<std::pair<int, int>, 10> dimensions{{
        {0, 0}, {-1, 400}, {600, -1}, {400, 600}, {599, 400},
        {601, 400}, {600, 399}, {600, 401},
        {std::numeric_limits<int>::max(), 400},
        {600, std::numeric_limits<int>::min()}
    }};
    auto image = solid_canvas(palette[0].rgb);
    const auto original = image.rgb;
    for (const auto& wh : dimensions) {
        image.width = wh.first;
        image.height = wh.second;
        for (const auto method : methods) {
            expect_invalid([&] {
                (void)img::quantize_to_framebuffer(image, method);
            }, "fitted 600x400 RGB image");
            REQUIRE(image.width == wh.first && image.height == wh.second);
            REQUIRE(image.rgb == original);
        }
    }
}

void test_invalid_buffer_sizes()
{
    for (const std::size_t size : {0, 1, 719'999, 720'001}) {
        const img::DecodedImage image{600, 400, std::vector<std::uint8_t>(size, 20)};
        const auto original = image.rgb;
        for (const auto method : methods) {
            expect_invalid([&] {
                (void)img::quantize_to_framebuffer(image, method);
            }, "fitted 600x400 RGB image");
            REQUIRE(image.rgb == original);
        }
    }
}

void test_invalid_method()
{
    const auto image = solid_canvas(palette[0].rgb);
    const auto original = image.rgb;
    for (const int value : {-1, 2, 255}) {
        expect_invalid([&] {
            (void)img::quantize_to_framebuffer(image, static_cast<img::Dither>(value));
        }, "Unsupported dithering method");
        REQUIRE(image.rgb == original);
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        REQUIRE(argc == 2);
        const std::string name = argv[1];
        const std::pair<const char*, void (*)()> tests[]{
            {"nearest_compatibility", test_nearest_compatibility},
            {"palette_fills", test_palette_fills},
            {"orientation", test_orientation},
            {"diffusion_right", test_diffusion_right},
            {"diffusion_below_left", test_diffusion_below_left},
            {"diffusion_below", test_diffusion_below},
            {"diffusion_below_right", test_diffusion_below_right},
            {"signed_error", test_signed_error},
            {"clipping", test_clipping},
            {"known_patch", test_known_patch},
            {"edge_left", test_edge_left},
            {"edge_right", test_edge_right},
            {"bottom_right", test_bottom_right},
            {"default_method", test_default_method},
            {"repeatability", test_repeatability},
            {"invalid_dimensions", test_invalid_dimensions},
            {"invalid_buffer_sizes", test_invalid_buffer_sizes},
            {"invalid_method", test_invalid_method}
        };
        for (const auto& test : tests) {
            if (name == test.first) {
                test.second();
                std::cout << "PASS: " << name << '\n';
                return EXIT_SUCCESS;
            }
        }
        throw std::runtime_error("Unknown test: " + name);
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << (argc > 1 ? argv[1] : "missing test name")
                  << ": " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
