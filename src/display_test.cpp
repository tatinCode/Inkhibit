#include "inkhibit/display.hpp"
#include "inkhibit/framebuffer.hpp"

#include <array>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

namespace fb = inkhibit::framebuffer;

constexpr int canvas_width = 
    static_cast<int>(fb::landscape_width);
constexpr int canvas_height = 
    static_cast<int>(fb::landscape_height);

volatile std::sig_atomic_t requested_signal = 0;

void handle_signal(int signal) {
    // Never call driver functions or perform I/O from a signal handler.
    requested_signal = signal;
}


fb::Buffer make_pattern() {
    auto framebuffer = fb::make_framebuffer();

    // Left to right: black, white, yellow, red, blue, green.
    constexpr std::array<fb::Color, 6> colors{
        fb::Color::black,
        fb::Color::white,
        fb::Color::yellow,
        fb::Color::red,
        fb::Color::blue,
        fb::Color::green
    };

    for (int y = 0; y < canvas_height; ++y) {
        for (int x = 0; x < canvas_width; ++x) {
            auto color =
                colors[static_cast<std::size_t>(x / 100)];

            // Black outer border and white inner border.
            if (x < 4 || y < 4
                || x >= canvas_width - 4
                || y >= canvas_height - 4) {
                color = fb::Color::black;
            } else if (x < 6 || y < 6
                || x >= canvas_width - 6
                || y >= canvas_height - 6) {
                color = fb::Color::white;
            }

            // White square in the upper-left corner.
            if (x >= 14 && x < 54 && y >= 14 && y < 54) {
                color = fb::Color::white;
            }

            // An upright black L inside the square reveals rotation/mirroring.
            const bool vertical =
                x >= 20 && x < 26 && y >= 20 && y < 46;

            const bool horizontal =
                x >= 20 && x < 46 && y >= 40 && y < 46;

            if (vertical || horizontal) {
                color = fb::Color::black;
            }

            fb::set_landscape_pixel(framebuffer, x, y, color);
        }
    }

    return framebuffer;
}

void print_usage() {
    std::cout
        << "Usage: inkhibit-display-test\n\n"
        << "Displays one six-color test pattern, then sleeps and exits.\n"
        << "Expected landscape order: black, white, yellow, red, blue, green.\n"
        << "The upper-left corner contains a white square with a black L.\n"
        << "State directory: /var/lib/inkhibit\n"
        << "Minimum refresh cooldown: 180 seconds after shutdown.\n";
}

} // namespace

int main(int argc, char** argv) {
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
