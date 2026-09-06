#include "pqoi/encoder.hpp"

#include "pqoi/core/qoi_encode.hpp"
#include "pqoi/metrics.hpp"
#include "pqoi/validation.hpp"

#include <mpi.h>

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr int colour_index_entries = 64;
constexpr int packed_pixel_bytes = 4;
constexpr int state_bytes = packed_pixel_bytes + colour_index_entries * packed_pixel_bytes;
constexpr int summary_bytes = state_bytes + colour_index_entries;
constexpr int encoded_stats_fields = 9;

struct MpiWorkspace {
    MPI_Datatype mpi_pixel{MPI_DATATYPE_NULL};
    bool initialized{false};

    // Rank 0 keeps one image entry.  The key is deliberately small and
    // deterministic so a changed file invalidates the cache immediately.
    pqoi::Image cached_image;
    std::string cached_path;
    std::uintmax_t cached_file_size{0U};
    std::filesystem::file_time_type cached_write_time{};
    bool cache_valid{false};

    std::vector<std::size_t> rank_block_begin;
    std::vector<std::size_t> rank_block_end;
    std::vector<int> pixel_counts;
    std::vector<int> pixel_displacements;
    std::vector<int> encoded_counts;
    std::vector<int> encoded_displacements;
    std::vector<pqoi::Pixel> local_pixels;
    std::vector<pqoi::BlockSummary> local_summaries;
    std::vector<unsigned char> local_summary_bytes;
    std::vector<unsigned char> gathered_summaries;
    std::vector<unsigned char> packed_states;
    std::vector<unsigned char> local_state_bytes;
    std::vector<pqoi::QoiState> local_entries;
    std::vector<std::uint8_t> local_bytes;
    std::vector<unsigned long long> gathered_encoded_stats;
    std::vector<std::uint8_t> encoded;

    ~MpiWorkspace() {
        int finalized = 0;
        MPI_Finalized(&finalized);
        if (!finalized && mpi_pixel != MPI_DATATYPE_NULL) MPI_Type_free(&mpi_pixel);
    }
};

struct ServerRequest {
    std::string request_id;
    std::string input;
    std::string output;
    std::string result;
    std::string preview;
    std::size_t blocks{0U};
    std::size_t segment_length{1024U};
    bool validate{false};
};

void write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot open output: " + path);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot write output: " + path);
}

void pack_pixel(const pqoi::Pixel pixel, unsigned char* destination) {
    destination[0] = pixel.r;
    destination[1] = pixel.g;
    destination[2] = pixel.b;
    destination[3] = pixel.a;
}

pqoi::Pixel unpack_pixel(const unsigned char* source) {
    return pqoi::Pixel{source[0], source[1], source[2], source[3]};
}

void pack_state(const pqoi::QoiState& state, unsigned char* destination) {
    pack_pixel(state.previous, destination);
    for (std::size_t index = 0; index < state.index.size(); ++index) {
        pack_pixel(state.index[index], destination + packed_pixel_bytes + index * packed_pixel_bytes);
    }
}

pqoi::QoiState unpack_state(const unsigned char* source) {
    pqoi::QoiState state;
    state.previous = unpack_pixel(source);
    for (std::size_t index = 0; index < state.index.size(); ++index) {
        state.index[index] = unpack_pixel(source + packed_pixel_bytes + index * packed_pixel_bytes);
    }
    return state;
}

void pack_summary(const pqoi::BlockSummary& summary, unsigned char* destination) {
    pack_pixel(summary.last_pixel, destination);
    for (std::size_t index = 0; index < summary.last_pixel_for_slot.size(); ++index) {
        pack_pixel(summary.last_pixel_for_slot[index],
                   destination + packed_pixel_bytes + index * packed_pixel_bytes);
        destination[state_bytes + index] = summary.touched[index] ? 1U : 0U;
    }
}

pqoi::BlockSummary unpack_summary(const unsigned char* source) {
    pqoi::BlockSummary summary;
    summary.last_pixel = unpack_pixel(source);
    for (std::size_t index = 0; index < summary.last_pixel_for_slot.size(); ++index) {
        summary.last_pixel_for_slot[index] = unpack_pixel(
            source + packed_pixel_bytes + index * packed_pixel_bytes);
        summary.touched[index] = source[state_bytes + index] != 0U;
    }
    return summary;
}

int checked_count(const std::size_t value, const char* what) {
    if (value > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error(std::string(what) + " is too large for MPI");
    }
    return static_cast<int>(value);
}

void ensure_workspace_datatype(MpiWorkspace& workspace) {
    if (workspace.mpi_pixel != MPI_DATATYPE_NULL) return;
    static_assert(sizeof(pqoi::Pixel) == packed_pixel_bytes);
    static_assert(std::is_trivially_copyable_v<pqoi::Pixel>);
    MPI_Type_contiguous(packed_pixel_bytes, MPI_UNSIGNED_CHAR, &workspace.mpi_pixel);
    MPI_Type_commit(&workspace.mpi_pixel);
}

std::string canonical_path(const std::string& path) {
    std::error_code error;
    std::filesystem::path candidate = std::filesystem::absolute(path, error);
    if (error) candidate = std::filesystem::path(path);
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(candidate, error);
    return (error ? candidate : canonical).string();
}

bool load_cached_image(const std::string& path, MpiWorkspace& workspace) {
    const std::string key = canonical_path(path);
    std::error_code error;
    const std::filesystem::path file_path(key);
    const std::uintmax_t size = std::filesystem::file_size(file_path, error);
    if (error) throw std::runtime_error("cannot stat input: " + path);
    const std::filesystem::file_time_type write_time = std::filesystem::last_write_time(file_path, error);
    if (error) throw std::runtime_error("cannot timestamp input: " + path);

    if (workspace.cache_valid && workspace.cached_path == key &&
        workspace.cached_file_size == size && workspace.cached_write_time == write_time) {
        return true;
    }

    pqoi::Image loaded = pqoi::load_image(path);
    workspace.cached_image = std::move(loaded);
    workspace.cached_path = key;
    workspace.cached_file_size = size;
    workspace.cached_write_time = write_time;
    workspace.cache_valid = true;
    return false;
}

std::size_t value_position(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    const std::size_t key_position = json.find(marker);
    if (key_position == std::string::npos) throw std::runtime_error("missing JSON field: " + key);
    const std::size_t colon = json.find(':', key_position + marker.size());
    if (colon == std::string::npos) throw std::runtime_error("invalid JSON field: " + key);
    std::size_t position = colon + 1U;
    while (position < json.size() && std::isspace(static_cast<unsigned char>(json[position])) != 0) ++position;
    return position;
}

std::string json_string(const std::string& json, const std::string& key) {
    std::size_t position = value_position(json, key);
    if (position >= json.size() || json[position] != '"') throw std::runtime_error("JSON field is not a string: " + key);
    ++position;
    std::string value;
    while (position < json.size()) {
        const char character = json[position++];
        if (character == '"') return value;
        if (character != '\\') {
            value += character;
            continue;
        }
        if (position >= json.size()) break;
        const char escaped = json[position++];
        switch (escaped) {
            case '"': value += '"'; break;
            case '\\': value += '\\'; break;
            case '/': value += '/'; break;
            case 'b': value += '\b'; break;
            case 'f': value += '\f'; break;
            case 'n': value += '\n'; break;
            case 'r': value += '\r'; break;
            case 't': value += '\t'; break;
            default: throw std::runtime_error("unsupported JSON escape in field: " + key);
        }
    }
    throw std::runtime_error("unterminated JSON string: " + key);
}

std::size_t json_size(const std::string& json, const std::string& key) {
    const std::size_t position = value_position(json, key);
    std::size_t end = position;
    while (end < json.size() && std::isdigit(static_cast<unsigned char>(json[end])) != 0) ++end;
    if (end == position) throw std::runtime_error("JSON field is not an unsigned integer: " + key);
    return std::stoull(json.substr(position, end - position));
}

bool json_bool(const std::string& json, const std::string& key) {
    const std::size_t position = value_position(json, key);
    if (json.compare(position, 4U, "true") == 0) return true;
    if (json.compare(position, 5U, "false") == 0) return false;
    throw std::runtime_error("JSON field is not a boolean: " + key);
}

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

ServerRequest parse_server_request(const std::string& line) {
    ServerRequest request;
    request.request_id = json_string(line, "request_id");
    request.input = json_string(line, "input");
    request.output = json_string(line, "output");
    request.result = json_string(line, "result");
    request.preview = json_string(line, "preview");
    request.blocks = json_size(line, "blocks");
    request.segment_length = json_size(line, "segment_length");
    request.validate = json_bool(line, "validate");
    if (request.input.empty() || request.output.empty() || request.result.empty()) {
        throw std::runtime_error("input, output and result must not be empty");
    }
    return request;
}

void broadcast_string(std::string& value, const int rank, const char* what) {
    unsigned long long length = rank == 0 ? static_cast<unsigned long long>(value.size()) : 0ULL;
    MPI_Bcast(&length, 1, MPI_UNSIGNED_LONG_LONG, 0, MPI_COMM_WORLD);
    value.resize(static_cast<std::size_t>(length));
    MPI_Bcast(value.empty() ? nullptr : value.data(), checked_count(value.size(), what), MPI_CHAR,
              0, MPI_COMM_WORLD);
}

pqoi::EncodeResult run_mpi_conversion_impl(const std::string& input_path,
                                           const std::string& output_path,
                                           const std::string& result_path,
                                           const std::string& preview_path,
                                           const pqoi::EncodeOptions& options,
                                           const bool validate,
                                           MpiWorkspace& workspace) {
    int rank = 0;
    int world = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world);

    pqoi::EncodeResult result;
    result.backend = "mpi";
    result.input_path = input_path;
    result.output_path = output_path;
    result.preview_path = preview_path;
    result.result_path = result_path;
    result.persistent_context_reused = workspace.initialized;
    const std::size_t requested_blocks = std::max<std::size_t>(
        static_cast<std::size_t>(world), options.blocks == 0U ? static_cast<std::size_t>(world) : options.blocks);
    result.blocks = requested_blocks;
    result.threads = static_cast<std::size_t>(world);
    result.segment_length = options.segment_length;
    result.cuda_threads_per_block = options.cuda_threads_per_block;
    const double total_start = MPI_Wtime();

    pqoi::Image one_shot_image;
    pqoi::Image* root_image = rank == 0 ? &one_shot_image : nullptr;
    int load_ok = 1;
    std::string load_error;
    if (rank == 0) {
        try {
            result.input_cache_reused = load_cached_image(input_path, workspace);
            root_image = &workspace.cached_image;
        } catch (const std::exception& error) {
            load_ok = 0;
            load_error = error.what();
        }
    }
    MPI_Bcast(&load_ok, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (!load_ok) {
        result.status = "error";
        result.error = rank == 0 ? load_error : "rank 0 could not load input image";
        if (rank == 0 && !result_path.empty()) pqoi::write_result_json(result_path, result);
        return result;
    }

    // Excludes file loading and includes metadata, communication, encoding,
    // the direct final gather, and QOI buffer assembly.
    const double pipeline_start = MPI_Wtime();
    const pqoi::Image& image_on_root = rank == 0 ? *root_image : one_shot_image;
    std::array<unsigned long long, 4> metadata{
        rank == 0 ? static_cast<unsigned long long>(image_on_root.pixels.size()) : 0ULL,
        rank == 0 ? static_cast<unsigned long long>(image_on_root.width) : 0ULL,
        rank == 0 ? static_cast<unsigned long long>(image_on_root.height) : 0ULL,
        rank == 0 ? static_cast<unsigned long long>(image_on_root.channels) : 0ULL};
    MPI_Bcast(metadata.data(), static_cast<int>(metadata.size()), MPI_UNSIGNED_LONG_LONG, 0, MPI_COMM_WORLD);
    const std::size_t pixel_count = static_cast<std::size_t>(metadata[0]);
    pqoi::Image image;
    if (rank == 0) {
        image.width = image_on_root.width;
        image.height = image_on_root.height;
        image.channels = image_on_root.channels;
    } else {
        image.width = static_cast<std::uint32_t>(metadata[1]);
        image.height = static_cast<std::uint32_t>(metadata[2]);
        image.channels = static_cast<std::uint8_t>(metadata[3]);
    }

    const std::vector<pqoi::Block> blocks = pqoi::partition_blocks(pixel_count, requested_blocks);
    const std::size_t actual_blocks = blocks.size();
    result.blocks = actual_blocks;

    workspace.rank_block_begin.resize(static_cast<std::size_t>(world));
    workspace.rank_block_end.resize(static_cast<std::size_t>(world));
    workspace.pixel_counts.resize(static_cast<std::size_t>(world));
    workspace.pixel_displacements.resize(static_cast<std::size_t>(world));
    for (int index = 0; index < world; ++index) {
        workspace.rank_block_begin[static_cast<std::size_t>(index)] =
            actual_blocks * static_cast<std::size_t>(index) / static_cast<std::size_t>(world);
        workspace.rank_block_end[static_cast<std::size_t>(index)] =
            actual_blocks * static_cast<std::size_t>(index + 1) / static_cast<std::size_t>(world);
        const std::size_t first_block = workspace.rank_block_begin[static_cast<std::size_t>(index)];
        const std::size_t final_block = workspace.rank_block_end[static_cast<std::size_t>(index)];
        const std::size_t pixel_begin = first_block < actual_blocks ? blocks[first_block].begin : pixel_count;
        const std::size_t pixel_end = final_block > first_block ? blocks[final_block - 1U].end : pixel_begin;
        workspace.pixel_counts[static_cast<std::size_t>(index)] = checked_count(pixel_end - pixel_begin, "MPI pixel block group");
        workspace.pixel_displacements[static_cast<std::size_t>(index)] = checked_count(pixel_begin, "MPI pixel displacement");
    }

    ensure_workspace_datatype(workspace);
    const std::size_t local_pixel_count = static_cast<std::size_t>(workspace.pixel_counts[static_cast<std::size_t>(rank)]);
    workspace.local_pixels.resize(local_pixel_count);
    const double pixel_scatter_start = MPI_Wtime();
    MPI_Scatterv(rank == 0 ? image_on_root.pixels.data() : nullptr,
                 workspace.pixel_counts.data(), workspace.pixel_displacements.data(), workspace.mpi_pixel,
                 workspace.local_pixels.empty() ? nullptr : workspace.local_pixels.data(),
                 workspace.pixel_counts[static_cast<std::size_t>(rank)], workspace.mpi_pixel,
                 0, MPI_COMM_WORLD);
    const double pixel_scatter_elapsed = (MPI_Wtime() - pixel_scatter_start) * 1000.0;

    const std::size_t local_block_begin = workspace.rank_block_begin[static_cast<std::size_t>(rank)];
    const std::size_t local_block_end = workspace.rank_block_end[static_cast<std::size_t>(rank)];
    const std::size_t global_pixel_begin = local_block_begin < actual_blocks ? blocks[local_block_begin].begin : pixel_count;

    const double local_summary_start = MPI_Wtime();
    workspace.local_summaries.clear();
    workspace.local_summaries.reserve(local_block_end - local_block_begin);
    for (std::size_t block_index = local_block_begin; block_index < local_block_end; ++block_index) {
        const pqoi::Block global_block = blocks[block_index];
        const pqoi::Block local_block{global_block.begin - global_pixel_begin, global_block.end - global_pixel_begin};
        workspace.local_summaries.push_back(pqoi::summarize_block(workspace.local_pixels, local_block));
    }
    const pqoi::BlockSummary rank_summary = pqoi::combine_block_summaries(workspace.local_summaries);
    const double local_summary_elapsed = (MPI_Wtime() - local_summary_start) * 1000.0;

    workspace.local_summary_bytes.resize(summary_bytes);
    pack_summary(rank_summary, workspace.local_summary_bytes.data());
    if (rank == 0) workspace.gathered_summaries.resize(static_cast<std::size_t>(world) * summary_bytes);
    else workspace.gathered_summaries.clear();
    const double summary_gather_start = MPI_Wtime();
    MPI_Gather(workspace.local_summary_bytes.data(), summary_bytes, MPI_UNSIGNED_CHAR,
               rank == 0 ? workspace.gathered_summaries.data() : nullptr, summary_bytes, MPI_UNSIGNED_CHAR,
               0, MPI_COMM_WORLD);
    const double summary_gather_elapsed = (MPI_Wtime() - summary_gather_start) * 1000.0;

    if (rank == 0) workspace.packed_states.resize(static_cast<std::size_t>(world) * state_bytes);
    else workspace.packed_states.clear();
    if (rank == 0) {
        std::vector<pqoi::BlockSummary> rank_summaries(static_cast<std::size_t>(world));
        for (int index = 0; index < world; ++index) {
            rank_summaries[static_cast<std::size_t>(index)] = unpack_summary(
                workspace.gathered_summaries.data() + static_cast<std::size_t>(index) * summary_bytes);
        }

        const double rank_propagation_start = MPI_Wtime();
        pqoi::QoiState state;
        for (int index = 0; index < world; ++index) {
            const std::size_t rank_index = static_cast<std::size_t>(index);
            pack_state(state, workspace.packed_states.data() + rank_index * state_bytes);
            if (workspace.rank_block_begin[rank_index] < workspace.rank_block_end[rank_index]) {
                state = pqoi::apply_block_summary(state, rank_summaries[rank_index]);
            }
        }
        result.propagation_ms = (MPI_Wtime() - rank_propagation_start) * 1000.0;
        result.width = image_on_root.width;
        result.height = image_on_root.height;
        result.channels = image_on_root.channels;
        result.load_ms = (pipeline_start - total_start) * 1000.0;
    }

    workspace.local_state_bytes.resize(state_bytes);
    const double state_scatter_start = MPI_Wtime();
    MPI_Scatter(rank == 0 ? workspace.packed_states.data() : nullptr, state_bytes, MPI_UNSIGNED_CHAR,
                workspace.local_state_bytes.data(), state_bytes, MPI_UNSIGNED_CHAR, 0, MPI_COMM_WORLD);
    const double state_scatter_elapsed = (MPI_Wtime() - state_scatter_start) * 1000.0;

    const double local_propagation_start = MPI_Wtime();
    workspace.local_entries = pqoi::propagate_states_from(
        unpack_state(workspace.local_state_bytes.data()), workspace.local_summaries);
    const double local_propagation_elapsed = (MPI_Wtime() - local_propagation_start) * 1000.0;

    const double local_encode_start = MPI_Wtime();
    workspace.local_bytes.clear();
    const std::size_t bytes_per_pixel = image.channels == 3U ? 4U : 5U;
    if (local_pixel_count <= std::numeric_limits<std::size_t>::max() / bytes_per_pixel) {
        workspace.local_bytes.reserve(local_pixel_count * bytes_per_pixel);
    }
    pqoi::BlockEncodingStats local_stats;
    for (std::size_t block_index = local_block_begin; block_index < local_block_end; ++block_index) {
        const pqoi::Block global_block = blocks[block_index];
        const pqoi::Block local_block{global_block.begin - global_pixel_begin, global_block.end - global_pixel_begin};
        pqoi::BlockEncodingStats block_stats;
        pqoi::encode_qoi_block(workspace.local_pixels, local_block,
                               workspace.local_entries[block_index - local_block_begin],
                               workspace.local_bytes, &block_stats);
        if (block_index != 0U) {
            local_stats.inherited_index_hits += block_stats.inherited_index_hits;
            local_stats.fallback_bytes_avoided += block_stats.fallback_bytes_avoided;
        }
        local_stats.run_chunks += block_stats.run_chunks;
        local_stats.index_chunks += block_stats.index_chunks;
        local_stats.diff_chunks += block_stats.diff_chunks;
        local_stats.luma_chunks += block_stats.luma_chunks;
        local_stats.rgb_chunks += block_stats.rgb_chunks;
        local_stats.rgba_chunks += block_stats.rgba_chunks;
    }
    const double local_encode_elapsed = (MPI_Wtime() - local_encode_start) * 1000.0;

    const std::array<unsigned long long, encoded_stats_fields> local_wire{
        static_cast<unsigned long long>(workspace.local_bytes.size()),
        static_cast<unsigned long long>(local_stats.run_chunks),
        static_cast<unsigned long long>(local_stats.index_chunks),
        static_cast<unsigned long long>(local_stats.diff_chunks),
        static_cast<unsigned long long>(local_stats.luma_chunks),
        static_cast<unsigned long long>(local_stats.rgb_chunks),
        static_cast<unsigned long long>(local_stats.rgba_chunks),
        static_cast<unsigned long long>(local_stats.inherited_index_hits),
        static_cast<unsigned long long>(local_stats.fallback_bytes_avoided)};
    if (rank == 0) workspace.gathered_encoded_stats.resize(static_cast<std::size_t>(world) * encoded_stats_fields);
    else workspace.gathered_encoded_stats.clear();
    const double encoded_stats_start = MPI_Wtime();
    MPI_Gather(local_wire.data(), encoded_stats_fields, MPI_UNSIGNED_LONG_LONG,
               rank == 0 ? workspace.gathered_encoded_stats.data() : nullptr,
               encoded_stats_fields, MPI_UNSIGNED_LONG_LONG, 0, MPI_COMM_WORLD);
    const double encoded_stats_elapsed = (MPI_Wtime() - encoded_stats_start) * 1000.0;

    int encoded_total = 0;
    if (rank == 0) {
        workspace.encoded_counts.resize(static_cast<std::size_t>(world));
        workspace.encoded_displacements.assign(static_cast<std::size_t>(world), 0);
        for (int index = 0; index < world; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) * encoded_stats_fields;
            workspace.encoded_counts[static_cast<std::size_t>(index)] = checked_count(
                static_cast<std::size_t>(workspace.gathered_encoded_stats[offset]), "MPI encoded block");
            workspace.encoded_displacements[static_cast<std::size_t>(index)] = encoded_total;
            encoded_total = checked_count(
                static_cast<std::size_t>(encoded_total) + static_cast<std::size_t>(workspace.encoded_counts[static_cast<std::size_t>(index)]),
                "MPI encoded output");
        }
        const double merge_start = MPI_Wtime();
        workspace.encoded = pqoi::prepare_qoi_buffer(image_on_root, static_cast<std::size_t>(encoded_total));
        result.merge_ms = (MPI_Wtime() - merge_start) * 1000.0;
    } else {
        workspace.encoded_counts.clear();
        workspace.encoded_displacements.clear();
        workspace.encoded.clear();
    }

    const double gather_start = MPI_Wtime();
    MPI_Gatherv(workspace.local_bytes.empty() ? nullptr : workspace.local_bytes.data(),
                checked_count(workspace.local_bytes.size(), "MPI encoded block"), MPI_UNSIGNED_CHAR,
                rank == 0 ? workspace.encoded.data() + pqoi::qoi_header_bytes : nullptr,
                rank == 0 ? workspace.encoded_counts.data() : nullptr,
                rank == 0 ? workspace.encoded_displacements.data() : nullptr,
                MPI_UNSIGNED_CHAR, 0, MPI_COMM_WORLD);
    const double gather_elapsed = (MPI_Wtime() - gather_start) * 1000.0;

    std::array<double, 9> local_timings{
        local_summary_elapsed, summary_gather_elapsed,
        rank == 0 ? result.propagation_ms : 0.0, local_propagation_elapsed,
        pixel_scatter_elapsed, state_scatter_elapsed, local_encode_elapsed,
        encoded_stats_elapsed, gather_elapsed};
    std::array<double, 9> max_timings{};
    MPI_Reduce(local_timings.data(), max_timings.data(), static_cast<int>(local_timings.size()),
               MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);

    if (rank == 0) {
        result.summary_ms = max_timings[0] + max_timings[1];
        result.propagation_ms = max_timings[2] + max_timings[3];
        result.transfer_in_ms = max_timings[4] + max_timings[5];
        result.encode_ms = max_timings[6];
        result.transfer_out_ms = max_timings[7] + max_timings[8];
        for (int index = 0; index < world; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) * encoded_stats_fields;
            result.run_chunks += static_cast<std::size_t>(workspace.gathered_encoded_stats[offset + 1U]);
            result.index_chunks += static_cast<std::size_t>(workspace.gathered_encoded_stats[offset + 2U]);
            result.diff_chunks += static_cast<std::size_t>(workspace.gathered_encoded_stats[offset + 3U]);
            result.luma_chunks += static_cast<std::size_t>(workspace.gathered_encoded_stats[offset + 4U]);
            result.rgb_chunks += static_cast<std::size_t>(workspace.gathered_encoded_stats[offset + 5U]);
            result.rgba_chunks += static_cast<std::size_t>(workspace.gathered_encoded_stats[offset + 6U]);
            result.inherited_index_hits += static_cast<std::size_t>(workspace.gathered_encoded_stats[offset + 7U]);
            result.fallback_bytes_avoided += static_cast<std::size_t>(workspace.gathered_encoded_stats[offset + 8U]);
        }
        result.core_pipeline_ms = (MPI_Wtime() - pipeline_start) * 1000.0;
        result.output_bytes = workspace.encoded.size();
        result.bmp_bytes = pqoi::equivalent_bmp_bytes(image_on_root);
        result.compression_ratio = workspace.encoded.empty() ? 0.0
            : static_cast<double>(result.bmp_bytes) / workspace.encoded.size();
        result.throughput_mpixels = result.encode_ms <= 0.0 ? 0.0
            : static_cast<double>(image_on_root.pixels.size()) / (result.encode_ms * 1000.0);
        result.core_pipeline_throughput_mpixels = result.core_pipeline_ms <= 0.0 ? 0.0
            : static_cast<double>(image_on_root.pixels.size()) / (result.core_pipeline_ms * 1000.0);
        // Chunk/cross-block metrics are gathered with the payload metadata;
        // there is no post-gather scan or re-encode on rank 0.
        result.metrics_analysis_ms = 0.0;
        const double write_start = MPI_Wtime();
        write_bytes(output_path, workspace.encoded);
        result.write_ms = (MPI_Wtime() - write_start) * 1000.0;
        if (validate) {
            const double validation_start = MPI_Wtime();
            const pqoi::ValidationDetails details = pqoi::validate_qoi_detailed(output_path, image_on_root);
            result.decoder_accepted = details.decoder_accepted;
            result.dimensions_match = details.dimensions_match;
            result.channels_match = details.channels_match;
            result.pixel_match = details.pixel_match;
            result.validation_passed = details.passed();
            if (result.validation_passed && !preview_path.empty()) pqoi::write_bmp(preview_path, pqoi::decode_qoi(output_path));
            result.validation_ms = (MPI_Wtime() - validation_start) * 1000.0;
        }
        result.status = result.validation_passed || !validate ? "success" : "validation_failed";
        result.total_ms = (MPI_Wtime() - total_start) * 1000.0;
        if (!result_path.empty()) pqoi::write_result_json(result_path, result);
    }

    int status_code = rank == 0 && result.status == "success" ? 0 : (rank == 0 ? 1 : 0);
    MPI_Bcast(&status_code, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (rank != 0) result.status = status_code == 0 ? "success" : "validation_failed";
    workspace.initialized = true;
    return result;
}

}  // namespace

namespace pqoi {

EncodeResult run_mpi_conversion(const std::string& input_path,
                                const std::string& output_path,
                                const std::string& result_path,
                                const std::string& preview_path,
                                const EncodeOptions& options,
                                const bool validate) {
    MpiWorkspace workspace;
    return run_mpi_conversion_impl(input_path, output_path, result_path, preview_path,
                                   options, validate, workspace);
}

int run_mpi_server() {
    int rank = 0;
    int world = 1;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world);
    MpiWorkspace workspace;

    while (true) {
        int command = 0;
        ServerRequest request;
        std::string parse_error;
        if (rank == 0) {
            std::string line;
            if (!std::getline(std::cin, line)) {
                command = 0;
            } else if (line.empty()) {
                command = 2;
                parse_error = "empty request";
            } else {
                try {
                    request = parse_server_request(line);
                    command = 1;
                } catch (const std::exception& error) {
                    command = 2;
                    parse_error = error.what();
                }
            }
        }
        MPI_Bcast(&command, 1, MPI_INT, 0, MPI_COMM_WORLD);
        if (command == 0) break;
        if (command == 2) {
            broadcast_string(request.request_id, rank, "MPI server request id");
            broadcast_string(parse_error, rank, "MPI server parse error");
            if (rank == 0) {
                std::cout << "{\"request_id\":\"" << escape_json(request.request_id)
                          << "\",\"status\":\"error\",\"error\":\""
                          << escape_json(parse_error) << "\"}" << std::endl;
            }
            continue;
        }

        std::array<unsigned long long, 3> wire{
            rank == 0 ? static_cast<unsigned long long>(request.blocks) : 0ULL,
            rank == 0 ? static_cast<unsigned long long>(request.segment_length) : 0ULL,
            rank == 0 && request.validate ? 1ULL : 0ULL};
        MPI_Bcast(wire.data(), static_cast<int>(wire.size()), MPI_UNSIGNED_LONG_LONG, 0, MPI_COMM_WORLD);
        broadcast_string(request.request_id, rank, "MPI server request id");
        broadcast_string(request.input, rank, "MPI server input path");
        broadcast_string(request.output, rank, "MPI server output path");
        broadcast_string(request.result, rank, "MPI server result path");
        broadcast_string(request.preview, rank, "MPI server preview path");
        request.blocks = static_cast<std::size_t>(wire[0]);
        request.segment_length = static_cast<std::size_t>(wire[1]);
        request.validate = wire[2] != 0ULL;

        EncodeOptions options{"mpi", request.blocks, static_cast<std::size_t>(world), request.segment_length};
        const EncodeResult result = run_mpi_conversion_impl(
            request.input, request.output, request.result, request.preview,
            options, request.validate, workspace);
        if (rank == 0) {
            std::cout << "{\"request_id\":\"" << escape_json(request.request_id)
                      << "\",\"status\":\"" << escape_json(result.status)
                      << "\",\"error\":\"" << escape_json(result.error) << "\"}" << std::endl;
        }
    }
    return 0;
}

}  // namespace pqoi
