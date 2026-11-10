#include "inkhibit/display.hpp"
#include "inkhibit/image.hpp"

#include <csignal>
#include <exception>
#include <iostream>
#include <string>

namespace {

namespace fb = inkhibit::framebuffer;

volatile std::sig_atomic_t requested_signal = 0;

void handle_signal(int signal)
{
    // Never call driver functions or perform I/O from a signal handler.
    requested_signal = signal;
}

fb::Buffer make_frame(const inkhibit::image::DecodedImage& image)
{
    const auto fitted = inkhibit::image::fit_to_canvas(image);

    auto framebuffer = fb::make_framebuffer(fb::Color::white);

    const int canvas_w =
        static_cast<int>(fb::landscape_width);
    const int canvas_h =
        static_cast<int>(fb::landscape_height);

    for (int y = 0; y < canvas_h; ++y) {
        for (int x = 0; x < canvas_w; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * canvas_w + x) * 3;

            const auto color = inkhibit::image::nearest_color(
                fitted.rgb[offset],
                fitted.rgb[offset + 1],
                fitted.rgb[offset + 2]);

            fb::set_landscape_pixel(framebuffer, x, y, color);
        }
    }

    return framebuffer;
}

void print_usage()
{
    std::cout
        << "Usage: inkhibit-display <image>\n\n"
        << "Displays one JPG or PNG image on the panel, then sleeps.\n"
        << "Larger images are proportionally downscaled; smaller and\n"
        << "transparent images are composited and centered on white.\n"
        << "Colors map to the panel's six-color palette.\n"
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

    if (argc != 2) {
        print_usage();
        return 2;
    }

    if (std::signal(SIGINT, handle_signal) == SIG_ERR
        || std::signal(SIGTERM, handle_signal) == SIG_ERR) {
        std::cerr << "Could not install termination handlers.\n";
        return 1;
    }

    try {
        const auto image = inkhibit::image::decode_image(argv[1]);
        const auto framebuffer = make_frame(image);

        if (requested_signal != 0) {
            return 128 + requested_signal;
        }

        inkhibit::Display display;

        std::cout
            << "Updating the display from " << argv[1] << ". "
            << "A termination request will allow the panel operation "
            << "and cleanup to finish.\n"
            << std::flush;

        display.show(framebuffer);

        std::cout
            << "Update complete. Panel slept and resources were released.\n";

        return requested_signal == 0 ? 0 : 128 + requested_signal;
    } catch (const std::exception& error) {
        std::cerr << "inkhibit-display: " << error.what() << '\n';
        return 1;
    }
}
