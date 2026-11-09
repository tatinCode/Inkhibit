#include "inkhibit/framebuffer.hpp"

#include <stdexcept>

namespace inkhibit::framebuffer {

static_assert(
    native_width == landscape_height
        && native_height == landscape_width,
    "Native and landscape dimensions must be rotated counterparts"
);

static_assert(
    native_width % 2 == 0,
    "Native width must be even for two pixels per byte"
);

namespace {

std::uint8_t checked_color_code(Color color)
{
    const auto code = static_cast<std::uint8_t>(color);

    if (!(code <= 3 || code == 5 || code == 6)) {
        throw std::invalid_argument(
            "Unsupported framebuffer color"
        );
    }

    return code;
}

} // namespace

Buffer make_framebuffer(Color fill)
{
    const auto code = checked_color_code(fill);

    // Both pixels in each byte receive the same color.
    const auto packed = static_cast<std::uint8_t>(
        (code << 4) | code
    );

    return Buffer(framebuffer_size, packed);
}

void set_landscape_pixel(
    Buffer& buffer,
    int x,
    int y,
    Color color)
{
    if (buffer.size() != framebuffer_size) {
        throw std::invalid_argument(
            "Framebuffer must contain exactly 120000 bytes"
        );
    }

    if (x < 0 || y < 0
        || static_cast<std::size_t>(x) >= landscape_width
        || static_cast<std::size_t>(y) >= landscape_height) {
        throw std::out_of_range(
            "Landscape pixel coordinates are outside the canvas"
        );
    }

    const auto code = checked_color_code(color);

    // Convert landscape coordinates into the driver's native layout.
    const std::size_t native_x =
        native_width - 1 - static_cast<std::size_t>(y);

    const std::size_t native_y =
        static_cast<std::size_t>(x);

    const std::size_t offset =
        native_y * (native_width / 2) + native_x / 2;

    auto& byte = buffer[offset];

    if (native_x % 2 == 0) {
        // Replace the high nibble; preserve the low nibble.
        byte = static_cast<std::uint8_t>(
            (byte & 0x0F) | (code << 4)
        );
    } else {
        // Replace the low nibble; preserve the high nibble.
        byte = static_cast<std::uint8_t>(
            (byte & 0xF0) | code
        );
    }
}

} // namespace inkhibit::framebuffer
