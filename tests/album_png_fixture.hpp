#ifndef INKHIBIT_TESTS_ALBUM_PNG_FIXTURE_HPP
#define INKHIBIT_TESTS_ALBUM_PNG_FIXTURE_HPP

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace album_fixture {
using Bytes = std::vector<std::uint8_t>;

inline void be32(Bytes& out, std::uint32_t value)
{
    for (int shift : {24, 16, 8, 0}) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

inline void chunk(Bytes& out, const std::string& type, const Bytes& data)
{
    be32(out, static_cast<std::uint32_t>(data.size()));
    const auto start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    std::uint32_t crc = 0xffffffffu;
    for (auto i = start; i < out.size(); ++i) {
        crc ^= out[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1u) ? 0xedb88320u : 0u);
        }
    }
    be32(out, crc ^ 0xffffffffu);
}

// Real RGB PNGs using stored DEFLATE blocks and CRC32/Adler32 checksums.
// No extra dependency or production encoder is needed.
inline Bytes png(std::uint32_t width = 600, std::uint32_t height = 400,
    std::uint8_t value = 255)
{
    Bytes raw;
    for (std::uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), width * 3, value);
    }
    Bytes compressed{0x78, 0x01};
    for (std::size_t offset = 0; offset < raw.size();) {
        const auto length = std::min<std::size_t>(65535, raw.size() - offset);
        compressed.push_back(offset + length == raw.size() ? 1 : 0);
        const auto n = static_cast<std::uint16_t>(length);
        const auto complement = static_cast<std::uint16_t>(~n);
        for (auto v : {n, complement}) {
            compressed.push_back(static_cast<std::uint8_t>(v));
            compressed.push_back(static_cast<std::uint8_t>(v >> 8));
        }
        compressed.insert(compressed.end(), raw.begin() + offset,
            raw.begin() + offset + length);
        offset += length;
    }
    std::uint32_t a = 1, b = 0;
    for (auto byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    be32(compressed, (b << 16) | a);
    Bytes out{137, 80, 78, 71, 13, 10, 26, 10};
    Bytes header;
    be32(header, width);
    be32(header, height);
    header.insert(header.end(), {8, 2, 0, 0, 0});
    chunk(out, "IHDR", header);
    chunk(out, "IDAT", compressed);
    chunk(out, "IEND", {});
    return out;
}
} // namespace album_fixture

#endif
