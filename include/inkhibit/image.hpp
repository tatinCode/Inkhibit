#ifndef INKHIBIT_IMAGE_HPP
#define INKHIBIT_IMAGE_HPP

#include "inkhibit/framebuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace inkhibit::image {

    // Decoded pixels as opaque RGB triples, width * height * 3 bytes.
    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgb;
    };

    // Decode JPEG or PNG bytes, compositing alpha over white.
    // Throws std::invalid_argument for empty, unsupported, oversized,
    // or corrupt input (checked before full decoding).
    // Throws std::runtime_error when decoding itself fails.
    DecodedImage decode_image_bytes(const std::uint8_t* data, std::size_t size);

    // Read and decode a JPEG or PNG file.
    // Throws std::runtime_error when the file cannot be opened.
    DecodedImage decode_image(const std::filesystem::path& path);

    // Return the image placed inside the 600x400 landscape canvas:
    // larger images are proportionally downscaled with nearest-neighbor
    // sampling, smaller images are never upscaled, and either is
    // centered on a white canvas. Output is always exactly canvas-sized.
    // Throws std::invalid_argument when the input is inconsistent.
    DecodedImage fit_to_canvas(const DecodedImage& image);

    // Map one RGB pixel to the closest supported framebuffer palette
    // color. Ties keep the earlier palette entry.
    framebuffer::Color nearest_color(
            std::uint8_t red,
            std::uint8_t green,
            std::uint8_t blue);

    enum class Dither {
        nearest,
        floyd_steinberg
    };

    /*
    * Convert an already-fitted 600x400 RGB image into the native framebuffer.
    * Floyd-Steinberg scans left-to-right, top-to-bottom.
    * The input image is never modified.
    *
    * Throws std::invalid_argument for invalid dimensions, pixel data,
    * or an unsupported dithering method.
    */
    framebuffer::Buffer quantize_to_framebuffer(
            const DecodedImage& image,
            Dither method = Dither::floyd_steinberg);

    /**
    * Expand a native packed framebuffer into a 600x400 landscape RGB image.
    * Uses the same RGB palette references as quantization.
    * Throws std::invalid_argument for an invalid buffer size or color code.
    */
    DecodedImage framebuffer_to_image(const framebuffer::Buffer& buffer);

    /**
    * Save the expanded framebuffer as a binary P6 PPM image.
    * Validates the framebuffer before opening the output file.
    * Throws std::runtime_error on file-open or write failure.
    */
    void write_preview_ppm(
            const framebuffer::Buffer& buffer,
            const std::filesystem::path& path);

} // namespace inkhibit::image

#endif
