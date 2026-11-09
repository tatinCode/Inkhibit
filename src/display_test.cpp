#include "inkhibit/display.hpp"

#include <array>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int canvas_width = 600;
constexpr int canvas_height = 400;

volatile std::sig_atomic_t requested_signal = 0;

void handle_signal(int signal)
{
    // Never call driver functions or perform I/O from a signal handler.
    requested_signal = signal;
}

void set_landscape_pixel(
    std::vector<std::uint8_t>& framebuffer,
    int x,
    int y,
    std::uint8_t color)
{
    const std::size_t native_x =
        static_cast<std::size_t>(canvas_height - 1 - y);
    const std::size_t native_y =
        static_cast<std::size_t>(x);

    const std::size_t offset =
        native_y * (inkhibit::Display::native_width / 2)
        + native_x / 2;

    auto& byte = framebuffer[offset];

    if (native_x % 2 == 0) {
        byte = static_cast<std::uint8_t>(
            (byte & 0x0F) | (color << 4)
        );
    } else {
        byte = static_cast<std::uint8_t>(
            (byte & 0xF0) | color
        );
    }
}

std::vector<std::uint8_t> make_pattern()
{
    std::vector<std::uint8_t> framebuffer(
        inkhibit::Display::framebuffer_size,
        0x11
    );

    // Left to right: black, white, yellow, red, blue, green.
    constexpr std::array<std::uint8_t, 6> colors{
        0x0, 0x1, 0x2, 0x3, 0x5, 0x6
    };

    for (int y = 0; y < canvas_height; ++y) {
        for (int x = 0; x < canvas_width; ++x) {
            std::uint8_t color =
                colors[static_cast<std::size_t>(x / 100)];

            // Black outer border and white inner border.
            if (x < 4 || y < 4
                || x >= canvas_width - 4
                || y >= canvas_height - 4) {
                color = 0x0;
            } else if (x < 6 || y < 6
                || x >= canvas_width - 6
                || y >= canvas_height - 6) {
                color = 0x1;
            }

            // White square in the upper-left corner.
            if (x >= 14 && x < 54 && y >= 14 && y < 54) {
                color = 0x1;
            }

            // An upright black L inside the square reveals rotation/mirroring.
            const bool vertical =
                x >= 20 && x < 26 && y >= 20 && y < 46;
            const bool horizontal =
                x >= 20 && x < 46 && y >= 40 && y < 46;

            if (vertical || horizontal) {
                color = 0x0;
            }

            set_landscape_pixel(framebuffer, x, y, color);
        }
    }

    return framebuffer;
}

void print_usage()
{
    std::cout
        << "Usage: inkhibit-display-test\n\n"
        << "Displays one six-color test pattern, then sleeps and exits.\n"
        << "Expected landscape order: black, white, yellow, red, blue, green.\n"
        << "The upper-left corner contains a white square with a black L.\n"
        << "State directory: /var/lib/inkhibit\n"
        << "Minimum refresh cooldown: 180 seconds after shutdown.\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage();
        return 0;
    }

    if (argc != 1) {
        print_usage();
        return 2;
    }

    if (std::signal(SIGINT, handle_signal) == SIG_ERR
        || std::signal(SIGTERM, handle_signal) == SIG_ERR) {
        std::cerr << "Could not install termination handlers.\n";
        return 1;
    }

    try {
        const auto framebuffer = make_pattern();

        if (requested_signal != 0) {
            return 128 + requested_signal;
        }

        inkhibit::Display display;

        std::cout
            << "Updating the display once. "
            << "A termination request will allow the panel operation "
            << "and cleanup to finish.\n"
            << std::flush;

        display.show(framebuffer);

        std::cout
            << "Update complete. Panel slept and resources were released.\n"
            << "Expected color order: black, white, yellow, red, blue, green.\n"
            << "Check that the black L is upright in the upper-left corner.\n";

        return requested_signal == 0 ? 0 : 128 + requested_signal;
    } catch (const std::exception& error) {
        std::cerr << "inkhibit-display-test: " << error.what() << '\n';
        return 1;
    }
}
