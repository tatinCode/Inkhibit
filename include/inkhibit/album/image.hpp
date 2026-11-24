#ifndef INKHIBIT_ALBUM_IMAGE_HPP
#define INKHIBIT_ALBUM_IMAGE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace inkhibit::album {

    inline constexpr std::size_t max_png_bytes = 2 * 1024 * 1024;

    // Throws if the bytes are not a decodable 600 × 400 PNG within the size limit.
    void validate_png(const std::vector<std::uint8_t>& bytes);

} // namespace inkhibit::album

#endif
