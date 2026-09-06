#include "pqoi/encoder.hpp"

#include "pqoi/metrics.hpp"
#include "pqoi/validation.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>

namespace {

using clock_type = std::chrono::steady_clock;

double elapsed_ms(const clock_type::time_point start, const clock_type::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open output: " + path);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot write output: " + path);
}

}  // namespace

namespace pqoi {

EncodeResult run_conversion(const std::string& input_path,
                            const std::string& output_path,
                            const std::string& result_path,
                            const std::string& preview_path,
                            const EncodeOptions& options,
                            const bool validate) {
    EncodeResult result;
    result.backend = options.backend;
    result.input_path = input_path;
    result.output_path = output_path;
    result.preview_path = preview_path;
    result.result_path = result_path;
    result.blocks = options.blocks;
    result.threads = options.threads;
    result.segment_length = options.segment_length;
    result.cuda_threads_per_block = options.cuda_threads_per_block;
    const auto total_start = clock_type::now();
    try {
        const auto load_start = clock_type::now();
        const Image image = load_image(input_path);
        result.load_ms = elapsed_ms(load_start, clock_type::now());
        result.width = image.width;
        result.height = image.height;
        result.channels = image.channels;
        const auto encode_start = clock_type::now();
        const std::vector<std::uint8_t> encoded = encode_qoi(image, options, &result);
        const auto encode_end = clock_type::now();
        if (result.core_pipeline_ms <= 0.0) result.core_pipeline_ms = elapsed_ms(encode_start, encode_end);
        if (result.encode_ms <= 0.0) result.encode_ms = elapsed_ms(encode_start, encode_end);
        result.output_bytes = encoded.size();
        result.bmp_bytes = equivalent_bmp_bytes(image);
        result.compression_ratio = encoded.empty() ? 0.0 : static_cast<double>(result.bmp_bytes) / encoded.size();
        result.throughput_mpixels = result.encode_ms <= 0.0
            ? 0.0
            : static_cast<double>(image.pixels.size()) / (result.encode_ms * 1000.0);
        result.core_pipeline_throughput_mpixels = result.core_pipeline_ms <= 0.0
            ? 0.0
            : static_cast<double>(image.pixels.size()) / (result.core_pipeline_ms * 1000.0);
        const auto write_start = clock_type::now();
        write_bytes(output_path, encoded);
        result.write_ms = elapsed_ms(write_start, clock_type::now());
        if (validate) {
            const auto validation_start = clock_type::now();
            const ValidationDetails details = validate_qoi_detailed(output_path, image);
            result.decoder_accepted = details.decoder_accepted;
            result.dimensions_match = details.dimensions_match;
            result.channels_match = details.channels_match;
            result.pixel_match = details.pixel_match;
            result.validation_passed = details.passed();
            if (result.validation_passed && !preview_path.empty()) {
                write_bmp(preview_path, decode_qoi(output_path));
            }
            result.validation_ms = elapsed_ms(validation_start, clock_type::now());
        }
        result.status = result.validation_passed || !validate ? "success" : "validation_failed";
        result.total_ms = elapsed_ms(total_start, clock_type::now());
        const auto analysis_start = clock_type::now();
        populate_chunk_distribution(encoded, result);
        if (options.backend != "one-pass") analyze_cross_block_benefit(image, result.blocks, result);
        result.metrics_analysis_ms = elapsed_ms(analysis_start, clock_type::now());
    } catch (const std::exception& error) {
        result.status = "error";
        result.error = error.what();
    }
    if (result.total_ms <= 0.0) result.total_ms = elapsed_ms(total_start, clock_type::now());
    if (!result_path.empty()) write_result_json(result_path, result);
    return result;
}

}  // namespace pqoi
