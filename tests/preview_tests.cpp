#include "inkhibit/image.hpp"
#include "image_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace img = inkhibit::image;
namespace fb = inkhibit::framebuffer;
namespace fixtures = image_fixtures;
namespace fs = std::filesystem;
using Rgb = std::array<std::uint8_t, 3>;

struct PaletteReference {
    std::uint8_t code;
    Rgb rgb;
};

// Independent expectations for the current palette, not read from production.
constexpr std::array<PaletteReference, 6> palette{{
    {0x0, {20, 20, 20}}, {0x1, {240, 240, 240}},
    {0x2, {255, 214, 0}}, {0x3, {200, 34, 39}},
    {0x5, {35, 70, 147}}, {0x6, {52, 137, 63}}
}};
constexpr char ppm_header[] = "P6\n600 400\n255\n";
constexpr std::size_t pixel_bytes = 600 * 400 * 3;
static_assert(sizeof ppm_header - 1 == 15);

fs::path preview_executable;

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
        // Like the existing tests, use isolated directories in the build tree.
        std::string pattern = (fs::current_path() / "preview-tests-XXXXXX").string();
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

std::string read_file(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.is_open());
    std::string contents((std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    REQUIRE(!input.bad());
    return contents;
}

void write_file(const fs::path& path, const std::string& contents)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.is_open());
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    REQUIRE(!output.fail());
}

template<std::size_t N>
void write_fixture(const fs::path& path, const std::uint8_t (&bytes)[N])
{
    write_file(path, std::string(reinterpret_cast<const char*>(bytes), N));
}

void require_canvas(const img::DecodedImage& image)
{
    REQUIRE(image.width == 600);
    REQUIRE(image.height == 400);
    REQUIRE(image.rgb.size() == pixel_bytes);
}

img::DecodedImage white_canvas()
{
    return {600, 400, std::vector<std::uint8_t>(pixel_bytes, 240)};
}

void set_expected_pixel(img::DecodedImage& image, int x, int y, const Rgb& rgb)
{
    REQUIRE(x >= 0 && x < 600 && y >= 0 && y < 400);
    const auto offset = (static_cast<std::size_t>(y) * 600 + x) * 3;
    std::copy(rgb.begin(), rgb.end(), image.rgb.begin() + offset);
}

std::string expected_ppm(const img::DecodedImage& image)
{
    require_canvas(image);
    std::string result = ppm_header;
    result.append(reinterpret_cast<const char*>(image.rgb.data()), image.rgb.size());
    return result;
}

struct CliResult {
    int exit_code;
    std::string output;
};

CliResult run_cli(const TemporaryDirectory& directory,
    const std::vector<std::string>& arguments)
{
    std::vector<std::string> command{preview_executable.string()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : command) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const auto log_path = directory.path / "cli.log";
    const int log_fd = ::open(log_path.c_str(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (log_fd < 0) {
        throw std::system_error(errno, std::generic_category(), "Opening CLI log");
    }

    const pid_t child = ::fork();
    if (child < 0) {
        const int saved = errno;
        ::close(log_fd);
        throw std::system_error(saved, std::generic_category(), "fork");
    }
    if (child == 0) {
        if (::dup2(log_fd, STDOUT_FILENO) < 0 || ::dup2(log_fd, STDERR_FILENO) < 0) {
            ::_exit(126);
        }
        ::close(log_fd);
        // No shell: spaces and metacharacters remain literal path characters.
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(log_fd);

    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        throw std::system_error(errno, std::generic_category(), "waitpid");
    }
    const auto output = read_file(log_path);
    if (!WIFEXITED(status)) {
        throw std::runtime_error("Preview CLI did not exit normally:\n" + output);
    }
    return {WEXITSTATUS(status), output};
}

void require_exit(const CliResult& result, int expected)
{
    if (result.exit_code != expected) {
        throw std::runtime_error("Expected CLI exit " + std::to_string(expected)
            + ", got " + std::to_string(result.exit_code) + ":\n" + result.output);
    }
}

void test_palette_fills()
{
    for (const auto& entry : palette) {
        const auto packed = static_cast<std::uint8_t>((entry.code << 4) | entry.code);
        const fb::Buffer buffer(120'000, packed);
        const auto original = buffer;
        const auto image = img::framebuffer_to_image(buffer);
        require_canvas(image);
        for (std::size_t i = 0; i < image.rgb.size(); i += 3) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                REQUIRE(image.rgb[i + channel] == entry.rgb[channel]);
            }
        }
        REQUIRE(buffer == original);
    }
}

void test_nibble_pairs()
{
    for (const auto& high : palette) {
        for (const auto& low : palette) {
            fb::Buffer buffer(120'000, 0x11);
            buffer[0] = static_cast<std::uint8_t>((high.code << 4) | low.code);
            auto expected = white_canvas();
            // Fixed native byte zero: high is (0,399), low is (0,398).
            set_expected_pixel(expected, 0, 399, high.rgb);
            set_expected_pixel(expected, 0, 398, low.rgb);
            const auto image = img::framebuffer_to_image(buffer);
            require_canvas(image);
            REQUIRE(image.rgb == expected.rgb);
        }
    }
}

void test_orientation()
{
    fb::Buffer buffer(120'000, 0x11);
    // Golden byte addresses, independent of the framebuffer pixel setter.
    buffer[199] = 0x13;
    buffer[119'999] = 0x15;
    buffer[0] = 0x61;
    buffer[119'800] = 0x21;
    buffer[4'189] = 0x00;
    buffer[4'389] = 0x01;
    buffer[4'589] = 0x01;
    auto expected = white_canvas();
    set_expected_pixel(expected, 0, 0, palette[3].rgb);
    set_expected_pixel(expected, 599, 0, palette[4].rgb);
    set_expected_pixel(expected, 0, 399, palette[5].rgb);
    set_expected_pixel(expected, 599, 399, palette[2].rgb);
    // An asymmetric upright L inside the canvas.
    for (const auto& xy : {std::pair{20, 20}, std::pair{20, 21},
            std::pair{21, 21}, std::pair{22, 21}}) {
        set_expected_pixel(expected, xy.first, xy.second, palette[0].rgb);
    }
    const auto original = buffer;
    const auto image = img::framebuffer_to_image(buffer);
    require_canvas(image);
    REQUIRE(image.rgb == expected.rgb);
    REQUIRE(buffer == original);
}

void test_invalid_sizes()
{
    for (const std::size_t size : {0, 1, 119'999, 120'001}) {
        const fb::Buffer buffer(size, 0x11);
        expect_throw<std::invalid_argument>([&] {
            (void)img::framebuffer_to_image(buffer);
        }, "120000-byte framebuffer");
    }
}

void test_invalid_codes()
{
    for (unsigned code = 0; code < 16; ++code) {
        const bool supported = std::any_of(palette.begin(), palette.end(),
            [=](const auto& entry) { return entry.code == code; });
        if (supported) {
            continue;
        }
        for (const std::size_t position : {0, 60'000, 119'999}) {
            for (const unsigned shift : {0, 4}) {
                fb::Buffer buffer(120'000, 0x11);
                buffer[position] = static_cast<std::uint8_t>(
                    shift == 0 ? 0x10 | code : (code << 4) | 0x1);
                const auto original = buffer;
                expect_throw<std::invalid_argument>([&] {
                    (void)img::framebuffer_to_image(buffer);
                }, "color code");
                REQUIRE(buffer == original);
            }
        }
    }
}

void test_round_trip()
{
    fb::Buffer buffer(120'000);
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<std::uint8_t>(
            (palette[(i / 6) % 6].code << 4) | palette[i % 6].code);
    }
    const auto image = img::framebuffer_to_image(buffer);
    // Exact reference RGB values have zero quantization error in either mode.
    for (const auto method : {img::Dither::nearest, img::Dither::floyd_steinberg}) {
        REQUIRE(img::quantize_to_framebuffer(image, method) == buffer);
    }
}

void test_ppm_contents()
{
    TemporaryDirectory directory;
    const auto path = directory.path / "preview with spaces.ppm";
    // Alternating landscape rows of blue and yellow, including 0/255 bytes.
    const fb::Buffer buffer(120'000, 0x25);
    const auto original = buffer;
    img::write_preview_ppm(buffer, path);
    std::string expected = ppm_header;
    for (int y = 0; y < 400; ++y) {
        const auto& rgb = (y % 2 == 0) ? palette[4].rgb : palette[2].rgb;
        for (int x = 0; x < 600; ++x) {
            expected.append(reinterpret_cast<const char*>(rgb.data()), rgb.size());
        }
    }
    REQUIRE(fs::file_size(path) == 720'015);
    REQUIRE(read_file(path) == expected);
    REQUIRE(buffer == original);
}

void test_ppm_overwrite()
{
    TemporaryDirectory directory;
    const auto path = directory.path / "existing.ppm";
    write_file(path, std::string(800'000, 'x'));
    img::write_preview_ppm(fb::Buffer(120'000, 0x00), path);
    REQUIRE(read_file(path) == std::string(ppm_header) + std::string(pixel_bytes, '\x14'));
}

void test_ppm_invalid_input()
{
    TemporaryDirectory directory;
    const auto existing = directory.path / "existing.ppm";
    const auto absent = directory.path / "absent.ppm";
    const std::string sentinel = "keep this output intact";
    write_file(existing, sentinel);
    std::array<fb::Buffer, 4> invalid{{
        {}, fb::Buffer(119'999, 0x11),
        fb::Buffer(120'000, 0x11), fb::Buffer(120'000, 0x11)
    }};
    invalid[2][0] = 0x41;
    invalid[3][119'999] = 0x1f;
    for (const auto& buffer : invalid) {
        for (const auto& path : {existing, absent}) {
            expect_throw<std::invalid_argument>([&] {
                img::write_preview_ppm(buffer, path);
            }, "");
            REQUIRE(read_file(existing) == sentinel);
            REQUIRE(!fs::exists(absent));
        }
    }
}

void test_ppm_open_error()
{
    TemporaryDirectory directory;
    const fb::Buffer buffer(120'000, 0x11);
    for (const auto& path : {directory.path / "missing" / "preview.ppm", directory.path}) {
        expect_throw<std::runtime_error>([&] {
            img::write_preview_ppm(buffer, path);
        }, "Cannot open preview file");
    }
}

void test_ppm_write_error()
{
    // Linux/Pi's standard always-full device exercises buffered write/close
    // errors without filling a filesystem or relying on permission bits.
    REQUIRE(fs::exists("/dev/full"));
    expect_throw<std::runtime_error>([] {
        img::write_preview_ppm(fb::Buffer(120'000, 0x11), "/dev/full");
    }, "Writing preview file failed");
}

void test_cli_help()
{
    TemporaryDirectory directory;
    const auto result = run_cli(directory, {"--help"});
    require_exit(result, 0);
    REQUIRE(result.output.find("Usage: inkhibit-preview") != std::string::npos);
    REQUIRE(result.output.find("Default dithering: floyd-steinberg") != std::string::npos);
}

void test_cli_usage()
{
    TemporaryDirectory directory;
    const std::vector<std::vector<std::string>> cases{
        {}, {"input.png"}, {"--dither"}, {"--dither", "nearest"},
        {"--dither", "nearest", "input.png"},
        {"input.png", "output.ppm", "extra"}, {"--unknown", "output.ppm"}
    };
    for (const auto& arguments : cases) {
        const auto result = run_cli(directory, arguments);
        require_exit(result, 2);
        REQUIRE(result.output.find("Usage: inkhibit-preview") != std::string::npos);
    }
}

void test_cli_invalid_method()
{
    TemporaryDirectory directory;
    const auto output = directory.path / "existing.ppm";
    write_file(output, "unchanged");
    for (const std::string method : {"invalid", "Nearest", ""}) {
        const auto result = run_cli(directory,
            {"--dither", method, "missing.png", output.string()});
        require_exit(result, 2);
        REQUIRE(result.output.find("Unknown dithering method") != std::string::npos);
        REQUIRE(read_file(output) == "unchanged");
    }
}

void test_cli_nearest()
{
    TemporaryDirectory directory;
    const auto input = directory.path / "input with spaces.png";
    const auto output = directory.path / "output with spaces.ppm";
    write_fixture(input, fixtures::png_rgb);
    const auto original = read_file(input);
    const auto result = run_cli(directory,
        {"--dither", "nearest", input.string(), output.string()});
    require_exit(result, 0);
    REQUIRE(result.output.find("Saved 600x400 preview") != std::string::npos);
    auto expected = white_canvas();
    // Independent expected layout for the centered 2x2 PNG fixture.
    set_expected_pixel(expected, 299, 199, palette[3].rgb);
    set_expected_pixel(expected, 300, 199, palette[5].rgb);
    set_expected_pixel(expected, 299, 200, palette[4].rgb);
    set_expected_pixel(expected, 300, 200, palette[0].rgb);
    REQUIRE(read_file(output) == expected_ppm(expected));
    REQUIRE(read_file(input) == original);
}

void test_cli_floyd_and_default()
{
    TemporaryDirectory directory;
    const auto input = directory.path / "gray.jpg";
    const auto explicit_path = directory.path / "explicit.ppm";
    const auto default_path = directory.path / "default.ppm";
    write_fixture(input, fixtures::jpeg_gray);
    const auto original = read_file(input);
    const auto fitted = img::fit_to_canvas(img::decode_image_bytes(
        fixtures::jpeg_gray, sizeof fixtures::jpeg_gray));
    const auto nearest = img::quantize_to_framebuffer(fitted, img::Dither::nearest);
    const auto floyd = img::quantize_to_framebuffer(fitted, img::Dither::floyd_steinberg);
    // This fixture must distinguish the methods, otherwise ignoring the CLI
    // option could accidentally pass the integration test.
    REQUIRE(nearest != floyd);
    const auto expected = expected_ppm(img::framebuffer_to_image(floyd));
    require_exit(run_cli(directory,
        {"--dither", "floyd-steinberg", input.string(), explicit_path.string()}), 0);
    require_exit(run_cli(directory, {input.string(), default_path.string()}), 0);
    REQUIRE(read_file(explicit_path) == expected);
    REQUIRE(read_file(default_path) == expected);
    REQUIRE(read_file(input) == original);
}

void test_cli_input_errors()
{
    TemporaryDirectory directory;
    const auto input = directory.path / "input.png";
    const auto output = directory.path / "existing.ppm";
    write_file(output, "unchanged");
    const auto missing = run_cli(directory, {input.string(), output.string()});
    require_exit(missing, 1);
    REQUIRE(missing.output.find("inkhibit-preview: Cannot open image file")
        != std::string::npos);
    REQUIRE(read_file(output) == "unchanged");
    for (const std::string& contents : {std::string(), std::string("not an image"),
            std::string(reinterpret_cast<const char*>(fixtures::gif_rgb), sizeof fixtures::gif_rgb)}) {
        write_file(input, contents);
        const auto result = run_cli(directory, {input.string(), output.string()});
        require_exit(result, 1);
        REQUIRE(result.output.find("inkhibit-preview:") != std::string::npos);
        REQUIRE(read_file(output) == "unchanged");
    }
}

void test_cli_output_errors()
{
    TemporaryDirectory directory;
    const auto input = directory.path / "input.png";
    write_fixture(input, fixtures::png_rgb);
    for (const auto& output : {directory.path / "missing" / "output.ppm", directory.path}) {
        const auto result = run_cli(directory, {input.string(), output.string()});
        require_exit(result, 1);
        REQUIRE(result.output.find("inkhibit-preview: Cannot open preview file")
            != std::string::npos);
    }
    const auto result = run_cli(directory, {input.string(), "/dev/full"});
    require_exit(result, 1);
    REQUIRE(result.output.find("inkhibit-preview: Writing preview file failed")
        != std::string::npos);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        REQUIRE(argc == 3);
        const std::string name = argv[1];
        preview_executable = fs::absolute(argv[2]);
        const std::pair<const char*, void (*)()> tests[]{
            {"palette_fills", test_palette_fills},
            {"nibble_pairs", test_nibble_pairs},
            {"orientation", test_orientation},
            {"invalid_sizes", test_invalid_sizes},
            {"invalid_codes", test_invalid_codes},
            {"round_trip", test_round_trip},
            {"ppm_contents", test_ppm_contents},
            {"ppm_overwrite", test_ppm_overwrite},
            {"ppm_invalid_input", test_ppm_invalid_input},
            {"ppm_open_error", test_ppm_open_error},
            {"ppm_write_error", test_ppm_write_error},
            {"cli_help", test_cli_help},
            {"cli_usage", test_cli_usage},
            {"cli_invalid_method", test_cli_invalid_method},
            {"cli_nearest", test_cli_nearest},
            {"cli_floyd_and_default", test_cli_floyd_and_default},
            {"cli_input_errors", test_cli_input_errors},
            {"cli_output_errors", test_cli_output_errors}
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
