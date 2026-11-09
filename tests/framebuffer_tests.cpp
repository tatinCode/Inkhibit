#include "inkhibit/framebuffer.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

namespace fb = inkhibit::framebuffer;

// These are the panel contract, independent of the implementation's constants.
constexpr std::size_t expected_size = 120'000;
static_assert(fb::landscape_width == 600 && fb::landscape_height == 400);
static_assert(fb::native_width == 400 && fb::native_height == 600);
static_assert(fb::framebuffer_size == expected_size);

struct ColorExpectation {
    fb::Color color;
    std::uint8_t code;
    std::uint8_t filled_byte;
};

constexpr std::array<ColorExpectation, 6> palette{{
    {fb::Color::black,  0x0, 0x00},
    {fb::Color::white,  0x1, 0x11},
    {fb::Color::yellow, 0x2, 0x22},
    {fb::Color::red,    0x3, 0x33},
    {fb::Color::blue,   0x5, 0x55},
    {fb::Color::green,  0x6, 0x66}
}};

// Like the other C++ tests, checks stay enabled in Release builds.
#define REQUIRE(condition)                                                \
    do {                                                                  \
        if (!(condition)) {                                               \
            throw std::runtime_error(std::string(__FILE__) + ":"           \
                + std::to_string(__LINE__) + ": " + #condition);           \
        }                                                                 \
    } while (false)

template<class Exception, class Function>
void expect_throw(Function&& function, const std::string& context)
{
    try {
        function();
    } catch (const Exception&) {
        return;
    } catch (const std::exception& error) {
        throw std::runtime_error(context + ": wrong exception type: " + error.what());
    }
    throw std::runtime_error(context + ": expected an exception");
}

bool supported_code(unsigned code)
{
    return std::any_of(palette.begin(), palette.end(),
        [=](const auto& entry) { return entry.code == code; });
}

void require_filled(const fb::Buffer& buffer, std::uint8_t byte)
{
    REQUIRE(buffer.size() == expected_size);
    REQUIRE(std::all_of(buffer.begin(), buffer.end(),
        [=](std::uint8_t value) { return value == byte; }));
}

void test_default_fill()
{
    require_filled(fb::make_framebuffer(), 0x11);
}

void test_color_fills()
{
    for (const auto& entry : palette) {
        require_filled(fb::make_framebuffer(entry.color), entry.filled_byte);
    }
}

void test_invalid_fill_colors()
{
    for (unsigned code = 0; code < 256; ++code) {
        if (!supported_code(code)) {
            expect_throw<std::invalid_argument>([&] {
                (void)fb::make_framebuffer(static_cast<fb::Color>(code));
            }, "fill color " + std::to_string(code));
        }
    }
}

void test_corners()
{
    auto buffer = fb::make_framebuffer();
    fb::Buffer expected(expected_size, 0x11);

    // Fixed native byte addresses and values, not a copy of the mapping code.
    fb::set_landscape_pixel(buffer, 0, 0, fb::Color::red);
    expected[199] = 0x13;
    fb::set_landscape_pixel(buffer, 599, 0, fb::Color::blue);
    expected[119'999] = 0x15;
    fb::set_landscape_pixel(buffer, 0, 399, fb::Color::green);
    expected[0] = 0x61;
    fb::set_landscape_pixel(buffer, 599, 399, fb::Color::yellow);
    expected[119'800] = 0x21;

    REQUIRE(buffer == expected);
}

void test_orientation_marker()
{
    auto buffer = fb::make_framebuffer();
    fb::Buffer expected(expected_size, 0x11);

    // A small upright L in landscape space: two pixels tall, three wide.
    // It exercises interior row strides and both halves of a packed byte.
    fb::set_landscape_pixel(buffer, 20, 20, fb::Color::black);
    fb::set_landscape_pixel(buffer, 20, 21, fb::Color::black);
    fb::set_landscape_pixel(buffer, 21, 21, fb::Color::black);
    fb::set_landscape_pixel(buffer, 22, 21, fb::Color::black);
    expected[4'189] = 0x00;
    expected[4'389] = 0x01;
    expected[4'589] = 0x01;

    REQUIRE(buffer == expected);
}

void test_neighbor_preservation()
{
    fb::Buffer buffer(expected_size, 0x56);
    auto expected = buffer;

    // These landscape pixels share native byte zero: high, then low nibble.
    fb::set_landscape_pixel(buffer, 0, 399, fb::Color::red);
    expected[0] = 0x36;
    REQUIRE(buffer == expected);

    fb::set_landscape_pixel(buffer, 0, 398, fb::Color::yellow);
    expected[0] = 0x32;
    REQUIRE(buffer == expected);

    // Overwrite both pixels, proving old bits are cleared as well as set.
    fb::set_landscape_pixel(buffer, 0, 399, fb::Color::black);
    expected[0] = 0x02;
    REQUIRE(buffer == expected);

    fb::set_landscape_pixel(buffer, 0, 398, fb::Color::white);
    expected[0] = 0x01;
    REQUIRE(buffer == expected);
}

void test_color_pairs()
{
    auto buffer = fb::make_framebuffer();
    fb::Buffer expected(expected_size, 0x11);
    for (const auto& high : palette) {
        for (const auto& low : palette) {
            // Low first here; neighbor_preservation also tests high first.
            fb::set_landscape_pixel(buffer, 0, 398, low.color);
            fb::set_landscape_pixel(buffer, 0, 399, high.color);
            expected[0] = static_cast<std::uint8_t>((high.code << 4) | low.code);
            REQUIRE(buffer == expected);
        }
    }
}

void test_full_canvas()
{
    fb::Buffer buffer(expected_size, 0x00);
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 600; ++x) {
            fb::set_landscape_pixel(buffer, x, y, fb::Color::white);
        }
    }
    // Any untouched pixel, including one skipped by duplicate mappings,
    // would remain black. All 240,000 native pixels must be reached.
    require_filled(buffer, 0x11);
}

void test_invalid_coordinates()
{
    const std::array<std::pair<int, int>, 10> coordinates{{
        {-1, 0}, {0, -1}, {-1, -1},
        {600, 0}, {0, 400}, {600, 400},
        {std::numeric_limits<int>::min(), 0},
        {0, std::numeric_limits<int>::min()},
        {std::numeric_limits<int>::max(), 0},
        {0, std::numeric_limits<int>::max()}
    }};
    auto buffer = fb::make_framebuffer(fb::Color::green);
    const auto original = buffer;

    for (const auto& coordinate : coordinates) {
        expect_throw<std::out_of_range>([&] {
            fb::set_landscape_pixel(buffer, coordinate.first, coordinate.second,
                fb::Color::red);
        }, "coordinates " + std::to_string(coordinate.first) + ","
            + std::to_string(coordinate.second));
        REQUIRE(buffer == original);
    }
}

void test_invalid_buffer_sizes()
{
    constexpr std::array<std::size_t, 4> sizes{0, 1, 119'999, 120'001};
    for (const auto size : sizes) {
        fb::Buffer buffer(size, 0x35);
        const auto original = buffer;
        // Test coordinates addressing the start and end of a valid buffer.
        for (const auto& coordinate : {std::pair{0, 399}, std::pair{599, 0}}) {
            expect_throw<std::invalid_argument>([&] {
                fb::set_landscape_pixel(buffer, coordinate.first, coordinate.second,
                    fb::Color::white);
            }, "buffer size " + std::to_string(size));
            REQUIRE(buffer == original);
        }
    }
}

void test_invalid_pixel_colors()
{
    auto buffer = fb::make_framebuffer(fb::Color::blue);
    const auto original = buffer;
    for (unsigned code = 0; code < 256; ++code) {
        if (supported_code(code)) {
            continue;
        }
        for (const int y : {398, 399}) {
            expect_throw<std::invalid_argument>([&] {
                fb::set_landscape_pixel(buffer, 0, y, static_cast<fb::Color>(code));
            }, "pixel color " + std::to_string(code));
            REQUIRE(buffer == original);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        REQUIRE(argc == 2);
        const std::string name = argv[1];
        const std::pair<const char*, void (*)()> tests[]{
            {"default_fill", test_default_fill},
            {"color_fills", test_color_fills},
            {"invalid_fill_colors", test_invalid_fill_colors},
            {"corners", test_corners},
            {"orientation_marker", test_orientation_marker},
            {"neighbor_preservation", test_neighbor_preservation},
            {"color_pairs", test_color_pairs},
            {"full_canvas", test_full_canvas},
            {"invalid_coordinates", test_invalid_coordinates},
            {"invalid_buffer_sizes", test_invalid_buffer_sizes},
            {"invalid_pixel_colors", test_invalid_pixel_colors}
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
