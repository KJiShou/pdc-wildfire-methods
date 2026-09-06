#include "pqoi/metrics.hpp"

#include "pqoi/core/qoi_encode.hpp"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

std::string escape_json(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += character; break;
        }
    }
    return escaped;
}

}  // namespace

namespace pqoi {

void populate_chunk_distribution(const std::vector<std::uint8_t>& encoded, EncodeResult& result) {
    constexpr std::size_t header_bytes = 14U;
    constexpr std::size_t end_marker_bytes = 8U;
    if (encoded.size() < header_bytes + end_marker_bytes) return;

    std::size_t cursor = header_bytes;
    const std::size_t payload_end = encoded.size() - end_marker_bytes;
    while (cursor < payload_end) {
        const std::uint8_t opcode = encoded[cursor];
        if (opcode == 0xfeU) {
            ++result.rgb_chunks;
            cursor += 4U;
        } else if (opcode == 0xffU) {
            ++result.rgba_chunks;
            cursor += 5U;
        } else {
            switch (opcode & 0xc0U) {
                case 0x00U: ++result.index_chunks; ++cursor; break;
                case 0x40U: ++result.diff_chunks; ++cursor; break;
                case 0x80U: ++result.luma_chunks; cursor += 2U; break;
                default: ++result.run_chunks; ++cursor; break;
            }
        }
        if (cursor > payload_end) throw std::runtime_error("invalid QOI chunk stream while collecting metrics");
    }
}

void analyze_cross_block_benefit(const Image& image, const std::size_t block_count, EncodeResult& result) {
    const std::vector<Block> blocks = partition_blocks(image.pixels.size(), block_count);
    std::vector<BlockSummary> summaries;
    summaries.reserve(blocks.size());
    for (const Block block : blocks) summaries.push_back(summarize_block(image.pixels, block));
    const std::vector<QoiState> states = propagate_states(summaries);
    for (std::size_t index = 1U; index < blocks.size(); ++index) {
        BlockEncodingStats stats;
        std::vector<std::uint8_t> discarded;
        encode_qoi_block(image.pixels, blocks[index], states[index], discarded, &stats);
        result.inherited_index_hits += stats.inherited_index_hits;
        result.fallback_bytes_avoided += stats.fallback_bytes_avoided;
    }
}

std::string result_json(const EncodeResult& result) {
    std::ostringstream json;
    json << std::fixed << std::setprecision(4);
    json << "{\n"
         << "  \"status\": \"" << escape_json(result.status) << "\",\n"
         << "  \"backend\": \"" << escape_json(result.backend) << "\",\n"
         << "  \"error\": \"" << escape_json(result.error) << "\",\n"
         << "  \"input\": {\"path\": \"" << escape_json(result.input_path) << "\", \"width\": "
         << result.width << ", \"height\": " << result.height << ", \"channels\": "
         << static_cast<int>(result.channels) << "},\n"
         << "  \"configuration\": {\"blocks\": " << result.blocks << ", \"threads\": "
         << result.threads << ", \"segment_length\": " << result.segment_length
         << ", \"cuda_threads_per_block\": " << result.cuda_threads_per_block
         << ", \"cuda_device_architecture\": \"" << escape_json(result.cuda_device_architecture)
         << "\", \"persistent_context_reused\": " << (result.persistent_context_reused ? "true" : "false")
         << ", \"input_cache_reused\": " << (result.input_cache_reused ? "true" : "false") << "},\n"
         << "  \"timing\": {\"load_ms\": " << result.load_ms
         << ", \"cuda_init_ms\": " << result.cuda_init_ms
         << ", \"openmp_init_ms\": " << result.openmp_init_ms
         << ", \"allocation_ms\": " << result.allocation_ms
         << ", \"summary_ms\": " << result.summary_ms
         << ", \"propagation_ms\": " << result.propagation_ms
         << ", \"transfer_in_ms\": " << result.transfer_in_ms
         << ", \"encode_ms\": " << result.encode_ms
         << ", \"transfer_out_ms\": " << result.transfer_out_ms
         << ", \"merge_ms\": " << result.merge_ms
         << ", \"prefix_scan_ms\": " << result.prefix_scan_ms
         << ", \"compaction_ms\": " << result.compaction_ms
         << ", \"core_pipeline_ms\": " << result.core_pipeline_ms
         << ", \"write_ms\": " << result.write_ms
         << ", \"metrics_analysis_ms\": " << result.metrics_analysis_ms
         << ", \"validation_ms\": " << result.validation_ms
         << ", \"total_ms\": " << result.total_ms << "},\n"
         << "  \"output\": {\"path\": \"" << escape_json(result.output_path) << "\", \"bytes\": "
         << result.output_bytes << ", \"bmp_bytes\": " << result.bmp_bytes
         << ", \"compression_ratio\": " << result.compression_ratio
         << ", \"throughput_mpixels\": " << result.throughput_mpixels
         << ", \"core_pipeline_throughput_mpixels\": " << result.core_pipeline_throughput_mpixels << "},\n"
         << "  \"chunks\": {\"run\": " << result.run_chunks
         << ", \"index\": " << result.index_chunks
         << ", \"diff\": " << result.diff_chunks
         << ", \"luma\": " << result.luma_chunks
         << ", \"rgb\": " << result.rgb_chunks
         << ", \"rgba\": " << result.rgba_chunks << "},\n"
         << "  \"cross_block\": {\"inherited_index_hits\": " << result.inherited_index_hits
         << ", \"fallback_bytes_avoided\": " << result.fallback_bytes_avoided << "},\n"
         << "  \"preview_path\": \"" << escape_json(result.preview_path) << "\",\n"
         << "  \"validation\": {\"passed\": " << (result.validation_passed ? "true" : "false")
         << ", \"decoder_accepted\": " << (result.decoder_accepted ? "true" : "false")
         << ", \"dimensions_match\": " << (result.dimensions_match ? "true" : "false")
         << ", \"channels_match\": " << (result.channels_match ? "true" : "false")
         << ", \"pixel_match\": " << (result.pixel_match ? "true" : "false") << "}\n"
         << "}\n";
    return json.str();
}

void write_result_json(const std::string& path, const EncodeResult& result) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open result JSON: " + path);
    output << result_json(result);
}

}  // namespace pqoi
