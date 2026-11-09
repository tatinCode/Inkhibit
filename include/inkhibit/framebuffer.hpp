#ifndef INKHIBIT_FRAMEBUFFER_HPP
#define INKHIBIT_FRAMEBUFFER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>


namespace inkhibit::framebuffer {

inline constexpr std::size_t landscape_width = 600;
inline constexpr std::size_t landscape_height = 400;

inline constexpr std::size_t native_width = 400;
inline constexpr std::size_t native_height = 600;

inline constexpr std::size_t framebuffer_size = 
    native_width * native_height / 2;

enum class Color : std::uint8_t {
    black  = 0x0,
    white  = 0x1,
    yellow = 0x2,
    red    = 0x3,
    blue   = 0x5,
    green  = 0x6
};

using Buffer = std::vector<std::uint8_t>;

// Create a native framebuffer filled with one supported color.
// Throws std::invalid_argument for an unsupported color.
Buffer make_framebuffer(Color fill = Color::white);

// Set one pixel using landscape coordinates.
// Throws std::invalid_argument for an incorrect buffer size or color.
// Throws std::out_of_range for coordinates outside the landscape canvas.
void set_landscape_pixel(
    Buffer& buffer,
    int x,
    int y,
    Color color
);

} // namespace inkhibit::framebuffer

#endif
