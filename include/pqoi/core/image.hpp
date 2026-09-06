#pragma once

#include "pqoi/core/pixel.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace pqoi {

struct Image {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint8_t channels{4};
    std::vector<Pixel> pixels;

    [[nodiscard]] std::size_t pixel_count() const noexcept { return pixels.size(); }
};

Image load_image(const std::string& path);
// Size of the canonical uncompressed 32-bit BMP representation written by
// write_bmp, including its 54-byte file/info header.
[[nodiscard]] std::size_t equivalent_bmp_bytes(const Image& image);
void write_bmp(const std::string& path, const Image& image);

}  // namespace pqoi
