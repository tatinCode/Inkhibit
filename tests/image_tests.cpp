#include "inkhibit/image.hpp"
#include "image_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace img = inkhibit::image;
namespace fb = inkhibit::framebuffer;
namespace fixtures = image_fixtures;
namespace fs = std::filesystem;
using Rgb = std::array<std::uint8_t, 3>;
constexpr Rgb white{255, 255, 255};
constexpr Rgb black{0, 0, 0};

// Checks remain enabled in Release builds.
#define REQUIRE(condition)                                                \
    do {                                                                  \
        if (!(condition)) {                                               \
            throw std::runtime_error(std::string(__FILE__) + ":"           \
                + std::to_string(__LINE__) + ": " + #condition);           \
        }                                                                 \
    } while (false)

template<class Exception, class Function>
void expect_throw(Function&& function, const std::string& message)
{
    try {
        function();
    } catch (const Exception& error) {
        if (std::string(error.what()).find(message) == std::string::npos) {
            throw std::runtime_error("Expected exception containing '" + message
                + "', got: " + error.what());
        }
        return;
    }
    throw std::runtime_error("Expected exception containing: " + message);
}

class TemporaryDirectory {
public:
    TemporaryDirectory()
    {
        // File tests stay in the build tree and are safe under parallel CTest.
        std::string pattern = (fs::current_path() / "image-tests-XXXXXX").string();
        char* result = ::mkdtemp(pattern.data());
        if (!result) {
            throw std::system_error(errno, std::generic_category(), "mkdtemp");
        }
        path = result;
    }

    ~TemporaryDirectory() noexcept
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    fs::path path;
};

Rgb pixel_at(const img::DecodedImage& image, int x, int y)
{
    REQUIRE(x >= 0 && x < image.width && y >= 0 && y < image.height);
    const auto offset = (static_cast<std::size_t>(y) * image.width + x) * 3;
    return {image.rgb.at(offset), image.rgb.at(offset + 1), image.rgb.at(offset + 2)};
}

void require_canvas(const img::DecodedImage& image)
{
    REQUIRE(image.width == 600);
    REQUIRE(image.height == 400);
    REQUIRE(image.rgb.size() == 600 * 400 * 3);
}

// Nonuniform pixels expose row-stride, sampling, rotation, and channel errors.
img::DecodedImage patterned_image(int width, int height)
{
    img::DecodedImage image{width, height, {}};
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.rgb.push_back(static_cast<std::uint8_t>(x % 251));
            image.rgb.push_back(static_cast<std::uint8_t>(y % 251));
            image.rgb.push_back(static_cast<std::uint8_t>((x + 3 * y) % 251));
        }
    }
    return image;
}

// Expected rectangle and integer sampling step are fixed by each test, not
// calculated using the implementation's aspect-ratio/rounding algorithm.
void require_integer_fit(const img::DecodedImage& source,
    int left, int top, int width, int height, int step)
{
    const auto original = source.rgb;
    const auto fitted = img::fit_to_canvas(source);
    REQUIRE(source.rgb == original);
    require_canvas(fitted);
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 600; ++x) {
            const bool inside = x >= left && x < left + width
                && y >= top && y < top + height;
            const auto expected = inside
                ? pixel_at(source, (x - left) * step, (y - top) * step)
                : white;
            REQUIRE(pixel_at(fitted, x, y) == expected);
        }
    }
}

// PNG signature, valid IHDR/CRC, and an IDAT chunk header, without pixel data.
// Metadata readers may scan as far as IDAT to look for transparency chunks.
// Dimension tests require the limit-specific error, proving header inspection
// rejects the size before attempting to decode the missing image body.
std::vector<std::uint8_t> png_header(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> bytes(fixtures::png_rgb, fixtures::png_rgb + 41);
    const auto put_be32 = [&](std::size_t offset, std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            bytes[offset + i] = static_cast<std::uint8_t>(value >> (24 - 8 * i));
        }
    };
    put_be32(16, width);
    put_be32(20, height);
    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 12; i < 29; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    put_be32(29, ~crc);
    return bytes;
}

void test_png_rgb()
{
    const auto image = img::decode_image_bytes(fixtures::png_rgb, sizeof fixtures::png_rgb);
    REQUIRE(image.width == 2 && image.height == 2);
    const std::vector<std::uint8_t> expected{
        255, 0, 0, 0, 255, 0, 0, 0, 255, 12, 34, 56};
    REQUIRE(image.rgb == expected);
}

void test_png_alpha()
{
    const auto image = img::decode_image_bytes(fixtures::png_alpha, sizeof fixtures::png_alpha);
    REQUIRE(image.width == 2 && image.height == 2);
    // Fixed expected compositing results, including half/quarter alpha rounding.
    const std::vector<std::uint8_t> expected{
        255, 255, 255, 132, 137, 142, 10, 20, 30, 216, 229, 241};
    REQUIRE(image.rgb == expected);
}

void test_png_gray()
{
    const auto image = img::decode_image_bytes(fixtures::png_gray, sizeof fixtures::png_gray);
    REQUIRE(image.width == 2 && image.height == 1);
    const std::vector<std::uint8_t> expected{0, 0, 0, 173, 173, 173};
    REQUIRE(image.rgb == expected);
}

void test_jpeg()
{
    const auto image = img::decode_image_bytes(fixtures::jpeg_gray, sizeof fixtures::jpeg_gray);
    REQUIRE(image.width == 8 && image.height == 8);
    REQUIRE(image.rgb.size() == 8 * 8 * 3);
    // JPEG is lossy; allow small decoder rounding differences, including SIMD.
    REQUIRE(std::all_of(image.rgb.begin(), image.rgb.end(),
        [](std::uint8_t value) { return value >= 126 && value <= 130; }));
}

void test_invalid_input()
{
    expect_throw<std::invalid_argument>([] {
        (void)img::decode_image_bytes(nullptr, 0);
    }, "empty");
    expect_throw<std::invalid_argument>([] {
        (void)img::decode_image_bytes(nullptr, 10);
    }, "empty");
    expect_throw<std::invalid_argument>([] {
        (void)img::decode_image_bytes(fixtures::png_rgb, 0);
    }, "empty");
    // Must reject the length before attempting to read beyond this small array.
    expect_throw<std::invalid_argument>([] {
        (void)img::decode_image_bytes(fixtures::png_rgb,
            static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1);
    }, "large");
    const std::uint8_t garbage[]{0, 1, 2, 3, 4, 5};
    expect_throw<std::invalid_argument>([&] {
        (void)img::decode_image_bytes(garbage, sizeof garbage);
    }, "Unsupported or corrupt");
}

void test_unsupported_format()
{
    expect_throw<std::invalid_argument>([] {
        (void)img::decode_image_bytes(fixtures::gif_rgb, sizeof fixtures::gif_rgb);
    }, "Unsupported or corrupt");
}

void test_side_limit()
{
    for (const auto& dimensions : {std::pair{8193u, 1u}, std::pair{1u, 8193u}}) {
        const auto bytes = png_header(dimensions.first, dimensions.second);
        expect_throw<std::invalid_argument>([&] {
            (void)img::decode_image_bytes(bytes.data(), bytes.size());
        }, "dimensions exceed");
    }
}

void test_pixel_limit()
{
    // Both dimensions are allowed individually, but 36 million pixels are not.
    const auto bytes = png_header(6000, 6000);
    expect_throw<std::invalid_argument>([&] {
        (void)img::decode_image_bytes(bytes.data(), bytes.size());
    }, "dimensions exceed");
}

void test_truncated_png()
{
    const auto bytes = png_header(2, 2);
    expect_throw<std::runtime_error>([&] {
        (void)img::decode_image_bytes(bytes.data(), bytes.size());
    }, "Image decoding failed");
}

void test_file_png()
{
    TemporaryDirectory directory;
    // Decode by contents, not extension; exercise paths containing spaces too.
    const auto path = directory.path / "picture with spaces.dat";
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(fixtures::png_rgb), sizeof fixtures::png_rgb);
    output.close();
    REQUIRE(!output.fail());
    const auto image = img::decode_image(path);
    const auto expected = img::decode_image_bytes(fixtures::png_rgb, sizeof fixtures::png_rgb);
    REQUIRE(image.width == expected.width && image.height == expected.height);
    REQUIRE(image.rgb == expected.rgb);
}

void test_file_errors()
{
    TemporaryDirectory directory;
    expect_throw<std::runtime_error>([&] {
        (void)img::decode_image(directory.path / "missing.png");
    }, "Cannot open");
    const auto empty = directory.path / "empty.png";
    std::ofstream output(empty, std::ios::binary);
    output.close();
    REQUIRE(!output.fail());
    expect_throw<std::invalid_argument>([&] {
        (void)img::decode_image(empty);
    }, "empty");
}

void test_file_limit()
{
    TemporaryDirectory directory;
    const auto path = directory.path / "oversized.png";
    // Make a sparse file: logical size exceeds the limit, no 64 MB payload.
    std::ofstream output(path, std::ios::binary);
    output.seekp(64'000'000);
    output.put('\0');
    output.close();
    REQUIRE(!output.fail());
    REQUIRE(fs::file_size(path) == 64'000'001);
    expect_throw<std::invalid_argument>([&] {
        (void)img::decode_image(path);
    }, "larger than");
}

void test_fit_exact()
{
    require_integer_fit(patterned_image(600, 400), 0, 0, 600, 400, 1);
}

void test_fit_small()
{
    require_integer_fit(patterned_image(100, 50), 250, 175, 100, 50, 1);
    // Odd margins: the extra white pixel belongs to the right/bottom edge.
    require_integer_fit(patterned_image(3, 3), 298, 198, 3, 3, 1);
    require_integer_fit(patterned_image(1, 1), 299, 199, 1, 1, 1);
}

void test_fit_horizontal()
{
    require_integer_fit(patterned_image(1200, 400), 0, 100, 600, 200, 2);
}

void test_fit_vertical()
{
    require_integer_fit(patterned_image(600, 800), 150, 0, 300, 400, 2);
}

void test_fit_both()
{
    require_integer_fit(patterned_image(1200, 800), 0, 0, 600, 400, 2);
}

void test_fit_mixed()
{
    require_integer_fit(patterned_image(1200, 200), 0, 150, 600, 100, 2);
    require_integer_fit(patterned_image(200, 800), 250, 0, 100, 400, 2);
}

void test_fit_fractional()
{
    const auto source = patterned_image(901, 400);
    const auto fitted = img::fit_to_canvas(source);
    require_canvas(fitted);
    // Fitted image is 600x266, with 67 white rows above and below.
    REQUIRE(pixel_at(fitted, 0, 67) == pixel_at(source, 0, 0));
    REQUIRE(pixel_at(fitted, 1, 68) == pixel_at(source, 1, 1));
    REQUIRE(pixel_at(fitted, 300, 200) == pixel_at(source, 450, 200));
    REQUIRE(pixel_at(fitted, 599, 332) == pixel_at(source, 899, 398));
    for (int y = 0; y < 400; ++y) {
        if (y >= 67 && y < 333) {
            continue;
        }
        for (int x = 0; x < 600; ++x) {
            REQUIRE(pixel_at(fitted, x, y) == white);
        }
    }
}

void require_black_rectangle(const img::DecodedImage& fitted,
    int left, int top, int width, int height)
{
    require_canvas(fitted);
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 600; ++x) {
            const bool inside = x >= left && x < left + width
                && y >= top && y < top + height;
            REQUIRE(pixel_at(fitted, x, y) == (inside ? black : white));
        }
    }
}

void test_fit_thin_vertical()
{
    const img::DecodedImage source{1, 8192, std::vector<std::uint8_t>(8192 * 3, 0)};
    // A nonempty image must retain at least one column after downscaling.
    require_black_rectangle(img::fit_to_canvas(source), 299, 0, 1, 400);
}

void test_fit_thin_horizontal()
{
    const img::DecodedImage source{8192, 1, std::vector<std::uint8_t>(8192 * 3, 0)};
    require_black_rectangle(img::fit_to_canvas(source), 0, 199, 600, 1);
}

void test_fit_invalid()
{
    const std::array<img::DecodedImage, 7> inputs{{
        {0, 1, {}}, {1, 0, {}}, {-1, 1, {}}, {1, -1, {}},
        {1, 1, {}}, {1, 1, {0, 0}}, {1, 1, {0, 0, 0, 0}}
    }};
    for (const auto& input : inputs) {
        const auto original = input.rgb;
        expect_throw<std::invalid_argument>([&] {
            (void)img::fit_to_canvas(input);
        }, "size does not match");
        REQUIRE(input.rgb == original);
    }
}

void test_color_palette()
{
    // Initial reference palette is part of this increment's contract.
    const std::array<std::pair<Rgb, fb::Color>, 6> cases{{
        {{20, 20, 20}, fb::Color::black}, {{240, 240, 240}, fb::Color::white},
        {{255, 214, 0}, fb::Color::yellow}, {{200, 34, 39}, fb::Color::red},
        {{35, 70, 147}, fb::Color::blue}, {{52, 137, 63}, fb::Color::green}
    }};
    for (const auto& entry : cases) {
        REQUIRE(img::nearest_color(entry.first[0], entry.first[1], entry.first[2])
            == entry.second);
    }
}

void test_color_hues()
{
    const std::array<std::pair<Rgb, fb::Color>, 8> cases{{
        {{0, 0, 0}, fb::Color::black}, {{255, 255, 255}, fb::Color::white},
        {{255, 255, 0}, fb::Color::yellow}, {{255, 0, 0}, fb::Color::red},
        {{0, 0, 255}, fb::Color::blue}, {{0, 255, 0}, fb::Color::green},
        {{230, 30, 40}, fb::Color::red}, {{60, 130, 70}, fb::Color::green}
    }};
    for (const auto& entry : cases) {
        REQUIRE(img::nearest_color(entry.first[0], entry.first[1], entry.first[2])
            == entry.second);
    }
}

void test_color_tie()
{
    // Black and blue both have squared distance 6201 from (1,72,76).
    // All other references are farther (green is next, at 6995).
    // Black is earlier in the palette, so it must win the tie.
    REQUIRE(img::nearest_color(1, 72, 76) == fb::Color::black);
}

void test_pipeline_orientation()
{
    const auto decoded = img::decode_image_bytes(fixtures::png_rgb, sizeof fixtures::png_rgb);
    const auto fitted = img::fit_to_canvas(decoded);
    require_canvas(fitted);
    auto buffer = fb::make_framebuffer();
    for (int y = 0; y < 400; ++y) {
        for (int x = 0; x < 600; ++x) {
            const auto rgb = pixel_at(fitted, x, y);
            fb::set_landscape_pixel(buffer, x, y,
                img::nearest_color(rgb[0], rgb[1], rgb[2]));
        }
    }
    // Centered 2x2 image: red/green at y=199, blue/black at y=200.
    // Fixed physical byte expectations independently check the whole chain.
    fb::Buffer expected(120'000, 0x11);
    expected[59'900] = 0x31;
    expected[60'100] = 0x61;
    expected[59'899] = 0x15;
    expected[60'099] = 0x10;
    REQUIRE(buffer == expected);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        REQUIRE(argc == 2);
        const std::string name = argv[1];
        const std::pair<const char*, void (*)()> tests[]{
            {"decode.png_rgb", test_png_rgb},
            {"decode.png_alpha", test_png_alpha},
            {"decode.png_gray", test_png_gray},
            {"decode.jpeg", test_jpeg},
            {"decode.invalid_input", test_invalid_input},
            {"decode.unsupported_format", test_unsupported_format},
            {"decode.side_limit", test_side_limit},
            {"decode.pixel_limit", test_pixel_limit},
            {"decode.truncated_png", test_truncated_png},
            {"decode.file_png", test_file_png},
            {"decode.file_errors", test_file_errors},
            {"decode.file_limit", test_file_limit},
            {"fit.exact", test_fit_exact},
            {"fit.small", test_fit_small},
            {"fit.horizontal", test_fit_horizontal},
            {"fit.vertical", test_fit_vertical},
            {"fit.both", test_fit_both},
            {"fit.mixed", test_fit_mixed},
            {"fit.fractional", test_fit_fractional},
            {"fit.thin_vertical", test_fit_thin_vertical},
            {"fit.thin_horizontal", test_fit_thin_horizontal},
            {"fit.invalid", test_fit_invalid},
            {"color.palette", test_color_palette},
            {"color.hues", test_color_hues},
            {"color.tie", test_color_tie},
            {"pipeline_orientation", test_pipeline_orientation}
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
