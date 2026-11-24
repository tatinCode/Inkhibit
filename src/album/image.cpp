#include "inkhibit/album/image.hpp"

#include "inkhibit/framebuffer.hpp"
#include "inkhibit/image.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace inkhibit::album {
    namespace {

        // PNG signature, then the length and type of the first chunk (IHDR).
        constexpr std::array<std::uint8_t, 16> png_header{
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
                0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
        };

        std::uint32_t read_be32(const std::uint8_t* bytes)
        {
            return (static_cast<std::uint32_t>(bytes[0]) << 24)
                | (static_cast<std::uint32_t>(bytes[1]) << 16)
                | (static_cast<std::uint32_t>(bytes[2]) << 8)
                | static_cast<std::uint32_t>(bytes[3]);
        }

    } // namespace

    void validate_png(const std::vector<std::uint8_t>& bytes) {
        if (bytes.size() > max_png_bytes) {
            throw std::invalid_argument("Album image exceeds 2 MiB");
        }

        // A complete IHDR takes 33 bytes including the signature and chunk CRC.
        if (bytes.size() < 33
                || !std::equal(png_header.begin(), png_header.end(), bytes.begin())) {
            throw std::invalid_argument("Album image must be a PNG");
        }

        // Check dimensions before decoding so a small upload can't cause a
        // large pixel allocation just by declaring a huge canvas.
        const auto width = read_be32(bytes.data() + 16);
        const auto height = read_be32(bytes.data() + 20);

        if (width != framebuffer::landscape_width
                || height != framebuffer::landscape_height) {
            throw std::invalid_argument("Album image must be 600 x 400");
        }

        // A valid-looking header isn't enough: the pixel data must decode too.
        (void)image::decode_image_bytes(bytes.data(), bytes.size());
    }

} // namespace inkhibit::album
