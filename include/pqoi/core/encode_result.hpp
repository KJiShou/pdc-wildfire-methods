#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace pqoi {

struct EncodeResult {
    std::string status{"error"};
    std::string backend;
    std::string error;
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint8_t channels{0};
    std::size_t output_bytes{0};
    std::size_t bmp_bytes{0};
    double load_ms{0.0};
    // CUDA-only setup phases. They remain zero for CPU and MPI backends.
    double cuda_init_ms{0.0};
    // OpenMP-only runtime setup. It remains zero for other backends.
    double openmp_init_ms{0.0};
    double allocation_ms{0.0};
    double summary_ms{0.0};
    double propagation_ms{0.0};
    double transfer_in_ms{0.0};
    double encode_ms{0.0};
    double transfer_out_ms{0.0};
    double merge_ms{0.0};
    double prefix_scan_ms{0.0};
    double compaction_ms{0.0};
    double core_pipeline_ms{0.0};
    double write_ms{0.0};
    double metrics_analysis_ms{0.0};
    double validation_ms{0.0};
    double total_ms{0.0};
    double compression_ratio{0.0};
    double throughput_mpixels{0.0};
    double core_pipeline_throughput_mpixels{0.0};
    std::size_t run_chunks{0};
    std::size_t index_chunks{0};
    std::size_t diff_chunks{0};
    std::size_t luma_chunks{0};
    std::size_t rgb_chunks{0};
    std::size_t rgba_chunks{0};
    std::size_t inherited_index_hits{0};
    std::size_t fallback_bytes_avoided{0};
    std::size_t blocks{1};
    std::size_t threads{1};
    std::size_t segment_length{1024};
    std::size_t cuda_threads_per_block{128};
    std::string cuda_device_architecture;
    bool persistent_context_reused{false};
    bool input_cache_reused{false};
    bool validation_passed{false};
    bool decoder_accepted{false};
    bool dimensions_match{false};
    bool channels_match{false};
    bool pixel_match{false};
    std::string input_path;
    std::string output_path;
    std::string preview_path;
    std::string result_path;
};

}  // namespace pqoi
