#include "inkhibit/display.hpp"
#include "inkhibit/image.hpp"

#include <csignal>
#include <exception>
#include <iostream>
#include <string>

namespace {

    namespace fb = inkhibit::framebuffer;
    namespace img = inkhibit::image;

    volatile std::sig_atomic_t requested_signal = 0;

    void handle_signal(int signal) {
        // Never call driver functions or perform I/O from a signal handler.
        requested_signal = signal;
    }

    fb::Buffer make_frame(const img::DecodedImage& image, img::Dither method) {
        const auto fitted = img::fit_to_canvas(image);
        auto image_quantized = img::quantize_to_framebuffer(fitted, method);

        return image_quantized;
    }

    void print_usage() {
        std::cout
            << "Usage: inkhibit-display "
            << "[--dither nearest|floyd-steinberg] <image>\n\n"
            << "Displays one JPG or PNG image on the panel, then sleeps.\n"
            << "Default dithering: floyd-steinberg.\n"
            << "Larger images are proportionally downscaled; smaller images\n"
            << "are centered on white. Transparency is composited over white.\n"
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

    img::Dither method = img::Dither::floyd_steinberg;
    const char* image_path = nullptr;

    if (argc == 2 && argv[1][0] != '-') {
        image_path = argv[1];
    } else if (argc == 4 && std::string(argv[1]) == "--dither") {
        const std::string selected = argv[2];

        if (selected == "nearest") {
            method = img::Dither::nearest;
        } else if (selected == "floyd-steinberg") {
            method = img::Dither::floyd_steinberg;
        } else {
            std::cerr
                << "Unknown dithering method: " << selected << '\n';
            print_usage();
            return 2;
        }

        image_path = argv[3];
    } else {
        print_usage();
        return 2;
    }
    if (std::signal(SIGINT, handle_signal) == SIG_ERR
            || std::signal(SIGTERM, handle_signal) == SIG_ERR) {
        std::cerr << "Could not install termination handlers.\n";
        return 1;
    }

    try {
        const auto image = img::decode_image(image_path);
        const auto framebuffer = make_frame(image, method);

        if (requested_signal != 0) {
            return 128 + requested_signal;
        }

        inkhibit::Display display;

        std::cout
            << "Updating the display from " << image_path << ". "
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
