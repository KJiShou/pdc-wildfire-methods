#include "pqoi/validation.hpp"

#include "qoi/qoi.h"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace pqoi {

Image decode_qoi(const std::string& qoi_path) {
    std::ifstream input(qoi_path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open QOI: " + qoi_path);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    qoi_desc description{};
    auto* decoded = static_cast<std::uint8_t*>(qoi_decode(bytes.data(), static_cast<int>(bytes.size()), &description, 4));
    if (decoded == nullptr) throw std::runtime_error("official QOI decoder rejected output");
    Image image;
    image.width = description.width;
    image.height = description.height;
    image.channels = description.channels;
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height);
    for (std::size_t index = 0U; index < image.pixels.size(); ++index) {
        image.pixels[index] = Pixel{decoded[index * 4U], decoded[index * 4U + 1U],
                                    decoded[index * 4U + 2U], decoded[index * 4U + 3U]};
    }
    std::free(decoded);
    return image;
}

ValidationDetails validate_qoi_detailed(const std::string& qoi_path, const Image& expected) {
    ValidationDetails result;
    try {
        const Image actual = decode_qoi(qoi_path);
        result.decoder_accepted = true;
        result.dimensions_match = actual.width == expected.width && actual.height == expected.height;
        result.channels_match = actual.channels == expected.channels;
        result.pixel_match = actual.pixels == expected.pixels;
    } catch (...) {
        return result;
    }
    return result;
}

bool validate_qoi(const std::string& qoi_path, const Image& expected) {
    return validate_qoi_detailed(qoi_path, expected).passed();
}

}  // namespace pqoi
