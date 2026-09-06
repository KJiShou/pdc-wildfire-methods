#include "pqoi/core/image.hpp"

#include "qoi/qoi.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <vector>

#include "stb/stb_image.h"

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#endif

namespace {

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes.at(offset)) |
           (static_cast<std::uint16_t>(bytes.at(offset + 1U)) << 8U);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes.at(offset)) |
           (static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 8U) |
           (static_cast<std::uint32_t>(bytes.at(offset + 2U)) << 16U) |
           (static_cast<std::uint32_t>(bytes.at(offset + 3U)) << 24U);
}

std::int32_t read_i32(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return static_cast<std::int32_t>(read_u32(bytes, offset));
}

pqoi::Image load_bmp(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 54U || bytes[0] != 'B' || bytes[1] != 'M') throw std::runtime_error("invalid BMP header");
    const std::uint32_t data_offset = read_u32(bytes, 10U);
    const std::int32_t width = read_i32(bytes, 18U);
    const std::int32_t signed_height = read_i32(bytes, 22U);
    const std::uint16_t bit_count = read_u16(bytes, 28U);
    const std::uint32_t compression = read_u32(bytes, 30U);
    if (width <= 0 || signed_height == 0 || (bit_count != 24U && bit_count != 32U) || compression != 0U) {
        throw std::runtime_error("only uncompressed 24/32-bit BMP files are supported");
    }
    const std::uint32_t height = static_cast<std::uint32_t>(signed_height < 0 ? -signed_height : signed_height);
    const bool top_down = signed_height < 0;
    const std::size_t bytes_per_pixel = bit_count / 8U;
    const std::size_t row_stride = ((static_cast<std::size_t>(width) * bytes_per_pixel + 3U) / 4U) * 4U;
    if (data_offset + row_stride * height > bytes.size()) throw std::runtime_error("BMP pixel data is truncated");
    pqoi::Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = height;
    image.channels = bit_count == 24U ? 3U : 4U;
    image.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0U; y < height; ++y) {
        const std::uint32_t source_y = top_down ? y : height - 1U - y;
        const std::size_t row = data_offset + static_cast<std::size_t>(source_y) * row_stride;
        for (std::uint32_t x = 0U; x < static_cast<std::uint32_t>(width); ++x) {
            const std::size_t offset = row + static_cast<std::size_t>(x) * bytes_per_pixel;
            image.pixels[static_cast<std::size_t>(y) * width + x] = pqoi::Pixel{
                bytes[offset + 2U], bytes[offset + 1U], bytes[offset],
                bytes_per_pixel == 4U ? bytes[offset + 3U] : static_cast<std::uint8_t>(255U)};
        }
    }
    return image;
}

std::uint8_t png_channel_count(const std::vector<std::uint8_t>& bytes) {
    constexpr std::array<std::uint8_t, 8> signature{137U, 80U, 78U, 71U, 13U, 10U, 26U, 10U};
    if (bytes.size() < 26U || !std::equal(signature.begin(), signature.end(), bytes.begin())) return 4U;
    const std::uint8_t color_type = bytes[25U];
    if (color_type == 4U || color_type == 6U) return 4U;
    if (color_type == 3U) {
        const std::array<std::uint8_t, 4> transparency{'t', 'R', 'N', 'S'};
        return std::search(bytes.begin(), bytes.end(), transparency.begin(), transparency.end()) == bytes.end() ? 3U : 4U;
    }
    return 3U;
}

#ifdef _WIN32

template <typename T>
void release_com(T* pointer) { if (pointer != nullptr) pointer->Release(); }

std::wstring to_wide(const std::string& value) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (length <= 0) throw std::runtime_error("cannot convert input path to UTF-16");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), length);
    result.resize(result.size() - 1U);
    return result;
}

pqoi::Image load_with_wic(const std::string& path) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool should_uninitialize = SUCCEEDED(initialized);
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    const auto cleanup = [&] {
        release_com(converter); release_com(frame); release_com(decoder); release_com(factory);
        if (should_uninitialize) CoUninitialize();
    };
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
    if (FAILED(result)) { cleanup(); throw std::runtime_error("cannot create Windows image decoder"); }
    result = factory->CreateDecoderFromFilename(to_wide(path).c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(result)) { cleanup(); throw std::runtime_error("image decoder cannot open input"); }
    result = decoder->GetFrame(0U, &frame);
    if (FAILED(result)) { cleanup(); throw std::runtime_error("image has no decodable frame"); }
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result)) { cleanup(); throw std::runtime_error("cannot create image converter"); }
    result = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                   nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(result)) { cleanup(); throw std::runtime_error("cannot convert image to RGBA"); }
    UINT width = 0U; UINT height = 0U; converter->GetSize(&width, &height);
    pqoi::Image image; image.width = width; image.height = height; image.channels = 4U;
    image.pixels.resize(static_cast<std::size_t>(width) * height);
    std::vector<std::uint8_t> rgba(image.pixels.size() * 4U);
    result = converter->CopyPixels(nullptr, width * 4U, static_cast<UINT>(rgba.size()), rgba.data());
    if (FAILED(result)) { cleanup(); throw std::runtime_error("cannot read image pixels"); }
    for (std::size_t index = 0U; index < image.pixels.size(); ++index) {
        image.pixels[index] = pqoi::Pixel{rgba[index * 4U], rgba[index * 4U + 1U],
                                          rgba[index * 4U + 2U], rgba[index * 4U + 3U]};
    }
    cleanup(); return image;
}

#endif

}  // namespace

namespace pqoi {

std::size_t equivalent_bmp_bytes(const Image& image) {
    constexpr std::size_t header_bytes = 54U;
    constexpr std::size_t bytes_per_pixel = 4U;
    if (static_cast<std::size_t>(image.width) > (std::numeric_limits<std::size_t>::max)() / bytes_per_pixel) {
        throw std::runtime_error("equivalent BMP row size overflows size_t");
    }
    const std::size_t row_stride = static_cast<std::size_t>(image.width) * bytes_per_pixel;
    if (row_stride == 0U) return header_bytes;
    if (static_cast<std::size_t>(image.height) >
        ((std::numeric_limits<std::size_t>::max)() - header_bytes) / row_stride) {
        throw std::runtime_error("equivalent BMP size overflows size_t");
    }
    return header_bytes + row_stride * static_cast<std::size_t>(image.height);
}

Image load_image(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open input: " + path);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() >= 4U && bytes[0] == 'B' && bytes[1] == 'M') return load_bmp(bytes);
    if (bytes.size() >= 4U && std::memcmp(bytes.data(), "qoif", 4U) == 0) {
        qoi_desc desc{};
        auto* decoded = static_cast<std::uint8_t*>(qoi_decode(bytes.data(), static_cast<int>(bytes.size()), &desc, 4));
        if (decoded == nullptr) throw std::runtime_error("invalid QOI input");
        Image image; image.width = desc.width; image.height = desc.height; image.channels = 4U;
        image.pixels.resize(static_cast<std::size_t>(desc.width) * desc.height);
        for (std::size_t index = 0U; index < image.pixels.size(); ++index) {
            image.pixels[index] = Pixel{decoded[index * 4U], decoded[index * 4U + 1U],
                                        decoded[index * 4U + 2U], decoded[index * 4U + 3U]};
        }
        std::free(decoded); return image;
    }
#ifdef _WIN32
    Image image = load_with_wic(path);
    image.channels = png_channel_count(bytes);
    return image;
#else
    int width = 0;
    int height = 0;
    int source_channels = 0;
    unsigned char* rgba = stbi_load(path.c_str(), &width, &height, &source_channels, 4);
    if (rgba == nullptr || width <= 0 || height <= 0) {
        throw std::runtime_error(std::string("stb_image cannot decode input: ") + stbi_failure_reason());
    }
    Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.channels = source_channels == 2 || source_channels == 4 ? 4U : 3U;
    image.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::size_t index = 0U; index < image.pixels.size(); ++index) {
        image.pixels[index] = Pixel{rgba[index * 4U], rgba[index * 4U + 1U],
                                    rgba[index * 4U + 2U], rgba[index * 4U + 3U]};
    }
    stbi_image_free(rgba);
    return image;
#endif
}

void write_bmp(const std::string& path, const Image& image) {
    constexpr std::size_t header_bytes = 54U;
    const std::size_t total_bytes = equivalent_bmp_bytes(image);
    if (total_bytes > (std::numeric_limits<std::uint32_t>::max)()) {
        throw std::runtime_error("BMP output exceeds the 32-bit file-size limit");
    }
    const std::size_t pixel_bytes = total_bytes - header_bytes;
    const std::uint32_t file_size = static_cast<std::uint32_t>(total_bytes);
    std::vector<std::uint8_t> header(54U, 0U);
    header[0] = 'B'; header[1] = 'M';
    const auto write_u32_le = [&header](const std::size_t offset, const std::uint32_t value) {
        header[offset] = static_cast<std::uint8_t>(value & 0xffU);
        header[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        header[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
        header[offset + 3U] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    };
    write_u32_le(2U, file_size); write_u32_le(10U, 54U); write_u32_le(14U, 40U);
    write_u32_le(18U, image.width); write_u32_le(22U, image.height);
    header[26] = 1U; header[28] = 32U; write_u32_le(34U, static_cast<std::uint32_t>(pixel_bytes));
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open preview: " + path);
    output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    for (std::size_t y = image.height; y-- > 0U;) {
        for (std::size_t x = 0U; x < image.width; ++x) {
            const Pixel& pixel = image.pixels[y * image.width + x];
            const std::array<std::uint8_t, 4> bgra{pixel.b, pixel.g, pixel.r, pixel.a};
            output.write(reinterpret_cast<const char*>(bgra.data()), 4);
        }
    }
}

}  // namespace pqoi
