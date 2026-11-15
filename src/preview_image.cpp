#include "inkhibit/image.hpp"

#include <exception>
#include <iostream>
#include <string>

namespace {

    namespace img = inkhibit::image;

    void print_usage() {
        std::cout
            << "Usage: inkhibit-preview "
            << "[--dither nearest|floyd-steinberg] "
            << "<image> <output.ppm>\n\n"
            << "Creates a 600x400 preview of the packed display framebuffer.\n"
            << "Input formats: JPG and PNG.\n"
            << "Default dithering: floyd-steinberg.\n"
            << "Preview colors use the quantizer's RGB palette references.\n";
    }

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        print_usage();
        return 0;
    }

    img::Dither method = img::Dither::floyd_steinberg;
    const char* image_path = nullptr;
    const char* output_path = nullptr;

    if (argc == 3 && argv[1][0] != '-') {
        image_path = argv[1];
        output_path = argv[2];
    } else if (argc == 5 && std::string(argv[1]) == "--dither") {
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
        output_path = argv[4];
    } else {
        print_usage();
        return 2;
    }

    try {
        const auto image = img::decode_image(image_path);
        const auto fitted = img::fit_to_canvas(image);

        const auto framebuffer =
            img::quantize_to_framebuffer(fitted, method);

        img::write_preview_ppm(framebuffer, output_path);

        std::cout
            << "Saved 600x400 preview to " << output_path << '\n';

        return 0;
    } catch (const std::exception& error) {
        std::cerr
            << "inkhibit-preview: " << error.what() << '\n';
        return 1;
    }
}
