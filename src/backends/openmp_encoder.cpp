#include "pqoi/encoder.hpp"

#include "pqoi/core/qoi_encode.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

double elapsed_ms(const clock_type::time_point start, const clock_type::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

}  // namespace

namespace pqoi {

std::vector<std::uint8_t> encode_openmp_qoi(const Image& image,
                                            const EncodeOptions& options,
                                            EncodeResult* metrics) {
    const std::size_t requested_threads = std::max<std::size_t>(1U, options.threads);
    if (requested_threads > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("OpenMP thread count exceeds the supported range");
    }
    const std::size_t requested_blocks = options.blocks == 0U
        ? requested_threads * 2U
        : options.blocks;
    const int thread_count = static_cast<int>(requested_threads);

    // The first OpenMP parallel region in a process pays for creating and
    // waking the worker team. Keep that runtime setup cost separate from the
    // pixel summary pass so phase comparisons describe the actual work.
    const auto openmp_init_start = clock_type::now();
#ifdef PQOI_HAS_OPENMP
    int initialized_threads = 0;
#pragma omp parallel num_threads(thread_count) reduction(+: initialized_threads)
    {
        initialized_threads += 1;
    }
    (void)initialized_threads;
#endif
    if (metrics) metrics->openmp_init_ms = elapsed_ms(openmp_init_start, clock_type::now());

    const auto summary_start = clock_type::now();
    const std::vector<Block> blocks = partition_blocks(image.pixels.size(), requested_blocks);
    if (blocks.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("OpenMP block count exceeds the supported loop range");
    }
    const int block_count = static_cast<int>(blocks.size());
    std::vector<BlockSummary> summaries(blocks.size());
#ifdef PQOI_HAS_OPENMP
#pragma omp parallel for schedule(static) num_threads(thread_count)
#endif
    for (int index_value = 0; index_value < block_count; ++index_value) {
        const std::size_t index = static_cast<std::size_t>(index_value);
        summaries[index] = summarize_block(image.pixels, blocks[index]);
    }
    if (metrics) {
        metrics->blocks = blocks.size();
        metrics->summary_ms = elapsed_ms(summary_start, clock_type::now());
    }

    const auto propagation_start = clock_type::now();
    const std::vector<QoiState> entries = propagate_states(summaries);
    if (metrics) metrics->propagation_ms = elapsed_ms(propagation_start, clock_type::now());

    const auto encode_start = clock_type::now();
    std::vector<std::vector<std::uint8_t>> block_bytes(blocks.size());
#ifdef PQOI_HAS_OPENMP
#pragma omp parallel for schedule(static) num_threads(thread_count)
#endif
    for (int index_value = 0; index_value < block_count; ++index_value) {
        const std::size_t index = static_cast<std::size_t>(index_value);
        encode_qoi_block(image.pixels, blocks[index], entries[index], block_bytes[index]);
    }
    if (metrics) metrics->encode_ms = elapsed_ms(encode_start, clock_type::now());
    const auto merge_start = clock_type::now();
    std::vector<std::uint8_t> encoded = assemble_qoi(image, block_bytes);
    if (metrics) metrics->merge_ms = elapsed_ms(merge_start, clock_type::now());
    return encoded;
}

}  // namespace pqoi
