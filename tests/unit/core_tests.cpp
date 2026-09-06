#include "pqoi/encoder.hpp"
#include "pqoi/metrics.hpp"
#include "pqoi/validation.hpp"
#include "pqoi/core/qoi_encode.hpp"

#include <cassert>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

int main() {
    pqoi::Image image;
    image.width = 17U;
    image.height = 9U;
    image.channels = 4U;
    image.pixels.reserve(image.width * image.height);
    for (std::size_t index = 0U; index < image.width * image.height; ++index) {
        image.pixels.push_back(pqoi::Pixel{
            static_cast<std::uint8_t>(index * 3U),
            static_cast<std::uint8_t>(index * 5U),
            static_cast<std::uint8_t>(index * 7U),
            static_cast<std::uint8_t>(index % 3U == 0U ? 255U : 200U)});
    }
    assert(pqoi::equivalent_bmp_bytes(image) == 54U + 17U * 9U * 4U);
    pqoi::Image oversized;
    oversized.width = (std::numeric_limits<std::uint32_t>::max)();
    oversized.height = (std::numeric_limits<std::uint32_t>::max)();
    bool overflow_rejected = false;
    try {
        (void)pqoi::equivalent_bmp_bytes(oversized);
    } catch (const std::runtime_error&) {
        overflow_rejected = true;
    }
    assert(overflow_rejected);
    const pqoi::EncodeOptions options{"serial", 7U, 1U, 128U};
    const auto bytes = pqoi::encode_qoi(image, options);
    const std::string ratio_input = "pqoi_ratio_input.bmp";
    const std::string ratio_output = "pqoi_ratio_output.qoi";
    const std::string ratio_result = "pqoi_ratio_result.json";
    pqoi::write_bmp(ratio_input, image);
    const auto conversion = pqoi::run_conversion(ratio_input, ratio_output, ratio_result, "", options, true);
    assert(conversion.status == "success");
    assert(conversion.openmp_init_ms == 0.0);
    assert(conversion.bmp_bytes == pqoi::equivalent_bmp_bytes(image));
    assert(conversion.output_bytes > 0U);
    assert(conversion.compression_ratio == static_cast<double>(conversion.bmp_bytes) / conversion.output_bytes);
    std::ifstream result_file(ratio_result);
    const std::string result_text((std::istreambuf_iterator<char>(result_file)), std::istreambuf_iterator<char>());
    assert(result_text.find("\"openmp_init_ms\"") != std::string::npos);
    assert(result_text.find("sha256") == std::string::npos);
    std::remove(ratio_input.c_str());
    std::remove(ratio_output.c_str());
    std::remove(ratio_result.c_str());
    const std::string path = "pqoi_core_test.qoi";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    assert(pqoi::validate_qoi(path, image));
    pqoi::Image changed = image;
    changed.pixels.front().r ^= 1U;
    assert(!pqoi::validate_qoi(path, changed));
    std::remove(path.c_str());

#ifdef PQOI_HAS_OPENMP
    pqoi::EncodeResult openmp_metrics;
    const auto openmp_bytes = pqoi::encode_qoi(
        image, pqoi::EncodeOptions{"openmp", 4U, 4U, 128U}, &openmp_metrics);
    assert(openmp_metrics.openmp_init_ms >= 0.0);
    assert(openmp_metrics.summary_ms >= 0.0);
    assert(openmp_metrics.core_pipeline_ms >= openmp_metrics.openmp_init_ms);
    const std::string openmp_path = "pqoi_openmp_test.qoi";
    {
        std::ofstream output(openmp_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(openmp_bytes.data()), static_cast<std::streamsize>(openmp_bytes.size()));
    }
    assert(pqoi::validate_qoi(openmp_path, image));
    std::remove(openmp_path.c_str());
#endif

    const std::vector<pqoi::Block> summary_blocks = pqoi::partition_blocks(image.pixels.size(), 6U);
    std::vector<pqoi::BlockSummary> summaries;
    summaries.reserve(summary_blocks.size());
    for (const pqoi::Block block : summary_blocks) summaries.push_back(pqoi::summarize_block(image.pixels, block));
    const pqoi::BlockSummary combined = pqoi::combine_block_summaries(summaries);
    const pqoi::BlockSummary direct = pqoi::summarize_block(image.pixels, pqoi::Block{0U, image.pixels.size()});
    assert(combined.last_pixel == direct.last_pixel);
    assert(combined.last_pixel_for_slot == direct.last_pixel_for_slot);
    assert(combined.touched == direct.touched);

    pqoi::QoiState initial_state;
    initial_state.previous = pqoi::Pixel{91U, 82U, 73U, 255U};
    initial_state.index[3U] = pqoi::Pixel{7U, 8U, 9U, 255U};
    const auto whole_entries = pqoi::propagate_states_from(initial_state, summaries);
    const std::size_t split = summaries.size() / 2U;
    const std::vector<pqoi::BlockSummary> first_half(summaries.begin(), summaries.begin() + split);
    const std::vector<pqoi::BlockSummary> second_half(summaries.begin() + split, summaries.end());
    const auto first_entries = pqoi::propagate_states_from(initial_state, first_half);
    assert(first_entries.size() == split);
    pqoi::QoiState second_initial = initial_state;
    for (const pqoi::BlockSummary& summary : first_half) second_initial = pqoi::apply_block_summary(second_initial, summary);
    const auto second_entries = pqoi::propagate_states_from(second_initial, second_half);
    for (std::size_t index = 0U; index < first_entries.size(); ++index) assert(whole_entries[index].previous == first_entries[index].previous);
    for (std::size_t index = 0U; index < second_entries.size(); ++index) {
        assert(whole_entries[split + index].previous == second_entries[index].previous);
        assert(whole_entries[split + index].index == second_entries[index].index);
    }

    pqoi::Image assembly_image;
    assembly_image.width = 2U;
    assembly_image.height = 1U;
    assembly_image.channels = 4U;
    assembly_image.pixels.assign(2U, pqoi::Pixel{1U, 2U, 3U, 255U});
    const std::vector<std::uint8_t> payload{10U, 20U, 30U, 40U};
    const auto nested_assembly = pqoi::assemble_qoi(
        assembly_image, std::vector<std::vector<std::uint8_t>>{{10U, 20U}, {30U, 40U}});
    const auto contiguous_assembly = pqoi::assemble_qoi(assembly_image, payload);
    assert(nested_assembly == contiguous_assembly);
    auto direct_buffer = pqoi::prepare_qoi_buffer(assembly_image, payload.size());
    std::copy(payload.begin(), payload.end(), direct_buffer.begin() + pqoi::qoi_header_bytes);
    assert(direct_buffer == contiguous_assembly);

    pqoi::BlockEncodingStats online_stats;
    std::vector<std::uint8_t> online_payload;
    pqoi::encode_qoi_block(image.pixels, pqoi::Block{0U, image.pixels.size()}, pqoi::QoiState{}, online_payload, &online_stats);
    pqoi::EncodeResult scanned_stats;
    pqoi::populate_chunk_distribution(pqoi::assemble_qoi(image, online_payload), scanned_stats);
    assert(online_stats.run_chunks == scanned_stats.run_chunks);
    assert(online_stats.index_chunks == scanned_stats.index_chunks);
    assert(online_stats.diff_chunks == scanned_stats.diff_chunks);
    assert(online_stats.luma_chunks == scanned_stats.luma_chunks);
    assert(online_stats.rgb_chunks == scanned_stats.rgb_chunks);
    assert(online_stats.rgba_chunks == scanned_stats.rgba_chunks);

    const pqoi::QoiState empty_state = pqoi::apply_block_summary(initial_state, pqoi::BlockSummary{});
    assert(empty_state.previous == initial_state.previous);
    assert(empty_state.index == initial_state.index);

    pqoi::Image run_image;
    run_image.width = 128U;
    run_image.height = 1U;
    run_image.channels = 4U;
    run_image.pixels.assign(128U, pqoi::Pixel{12U, 34U, 56U, 255U});
    const auto run_bytes = pqoi::encode_qoi(run_image, pqoi::EncodeOptions{"serial", 5U, 1U, 64U});
    {
        std::ofstream output("pqoi_run_test.qoi", std::ios::binary);
        output.write(reinterpret_cast<const char*>(run_bytes.data()), static_cast<std::streamsize>(run_bytes.size()));
    }
    assert(pqoi::validate_qoi("pqoi_run_test.qoi", run_image));
    std::remove("pqoi_run_test.qoi");

    pqoi::EncodeResult control_metrics;
    const auto control_bytes = pqoi::encode_qoi(
        image, pqoi::EncodeOptions{"one-pass", 7U, 1U, 128U}, &control_metrics);
    pqoi::populate_chunk_distribution(control_bytes, control_metrics);
    {
        std::ofstream output("pqoi_control_test.qoi", std::ios::binary);
        output.write(reinterpret_cast<const char*>(control_bytes.data()), static_cast<std::streamsize>(control_bytes.size()));
    }
    assert(pqoi::validate_qoi("pqoi_control_test.qoi", image));
    assert(control_metrics.rgba_chunks >= 7U);
    std::remove("pqoi_control_test.qoi");

    pqoi::Image rgb_image = image;
    rgb_image.channels = 3U;
    for (pqoi::Pixel& pixel : rgb_image.pixels) pixel.a = 255U;
    assert(pqoi::equivalent_bmp_bytes(rgb_image) == 54U + 17U * 9U * 4U);
    const auto rgb_bytes = pqoi::encode_qoi(rgb_image, pqoi::EncodeOptions{"serial", 4U, 1U, 64U});
    {
        std::ofstream output("pqoi_rgb_test.qoi", std::ios::binary);
        output.write(reinterpret_cast<const char*>(rgb_bytes.data()), static_cast<std::streamsize>(rgb_bytes.size()));
    }
    assert(pqoi::validate_qoi("pqoi_rgb_test.qoi", rgb_image));
    std::remove("pqoi_rgb_test.qoi");

    pqoi::Image black_image;
    black_image.width = 3U;
    black_image.height = 1U;
    black_image.channels = 4U;
    black_image.pixels = {
        pqoi::Pixel{0U, 0U, 0U, 255U},
        pqoi::Pixel{0U, 0U, 0U, 0U},
        pqoi::Pixel{0U, 0U, 0U, 255U},
    };
    const auto black_bytes = pqoi::encode_qoi(black_image, pqoi::EncodeOptions{"serial", 1U, 1U, 64U});
    {
        std::ofstream output("pqoi_black_alpha_test.qoi", std::ios::binary);
        output.write(reinterpret_cast<const char*>(black_bytes.data()), static_cast<std::streamsize>(black_bytes.size()));
    }
    assert(pqoi::validate_qoi("pqoi_black_alpha_test.qoi", black_image));
    std::remove("pqoi_black_alpha_test.qoi");

    return 0;
}
