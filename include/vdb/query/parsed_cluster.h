#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"
#include "vdb/simd/address_decode.h"
#include "vdb/storage/address_column.h"

namespace vdb {
namespace query {

/// Custom deleter for buffers allocated with aligned_alloc / std::aligned_alloc.
struct FreeDeleter {
    void operator()(uint8_t* p) const noexcept { std::free(p); }
};
using AlignedBufPtr = std::unique_ptr<uint8_t[], FreeDeleter>;

/// Location of a cluster block within the .clu file.
struct ClusterBlockLocation {
    uint64_t offset;       // Absolute byte offset in .clu file
    uint64_t size;         // Block size in bytes
    uint32_t num_records;  // Record count in this cluster
};

/// Parsed cluster data from a raw block buffer (async-read path).
///
/// Phase-1 / dual-region layout:
///   Region 1: FastScan blocks (packed sign bits + norm_oc factors)
///   Region 2: ExRaBitQ entries (ex_code + ex_sign + xipnorm, only when bits > 1)
///   Region 3: Address packed data + mini-trailer
///
/// Owns the raw block buffer; pointers are zero-copy into it.
/// decoded_addresses is separately materialized via SIMD decode.
/// Move-only.
struct ParsedCluster {
    struct ExRaBitQView {
        const uint8_t* code_abs = nullptr;
        const uint8_t* sign = nullptr;
        uint32_t sign_bytes = 0;
        bool sign_packed = false;
        float xipnorm = 0.0f;
    };

    struct ExRaBitQBatchBlockView {
        const uint8_t* abs_blocks = nullptr;   // [dim_block_count][8][64]
        const uint8_t* sign_blocks = nullptr;  // [dim_block_count][8][8B]
        const float* xipnorms = nullptr;       // [8]
        uint32_t valid_count = 0;
        uint32_t batch_block_id = 0;
    };

    struct ExRaBitQBatchParallelBlockView {
        const uint8_t* abs_slices = nullptr;      // [dim_block_count][slice][8][16]
        const uint16_t* sign_words = nullptr;     // [dim_block_count][slice][8]
        const float* xipnorms = nullptr;          // [8]
        uint32_t valid_count = 0;
        uint32_t batch_block_id = 0;
        uint32_t slices_per_dim_block = 0;
    };

    AlignedBufPtr block_buf;  // Owns entire raw block (aligned_alloc, freed via free()).

    // --- Region 1: FastScan ---
    const uint8_t* fastscan_blocks = nullptr;  // Pointer to first block
    uint32_t fastscan_block_size = 0;          // Bytes per block (packed_codes + norms)
    uint32_t num_fastscan_blocks = 0;          // ceil(num_records / 32)

    // --- Region 2: ExRaBitQ ---
    const uint8_t* exrabitq_entries = nullptr;  // Pointer to first entry (nullptr if bits=1)
    uint32_t exrabitq_entry_size = 0;           // dim + sign_bytes + 4 (format-dependent)
    uint32_t exrabitq_sign_bytes = 0;           // dim for legacy, ceil(dim/8) for packed-sign
    bool exrabitq_sign_packed = false;
    uint32_t exrabitq_storage_version = 0;
    const uint8_t* exrabitq_batch_blocks = nullptr;
    uint32_t exrabitq_batch_block_size = 0;
    uint32_t exrabitq_batch_size = 0;
    uint32_t exrabitq_dim_block = 0;
    uint32_t exrabitq_num_dim_blocks = 0;
    uint32_t exrabitq_num_batch_blocks = 0;
    const uint8_t* exrabitq_parallel_abs_blocks = nullptr;
    const uint16_t* exrabitq_parallel_sign_words = nullptr;
    uint32_t exrabitq_parallel_abs_block_size = 0;
    uint32_t exrabitq_parallel_sign_words_per_block = 0;
    uint32_t exrabitq_parallel_slices_per_dim_block = 0;

    // --- Existing ---
    uint32_t num_records = 0;
    float epsilon = 0.0f;  // r_max = max(||o-c||) within cluster
    const storage::RawAddressEntryV2* raw_addresses = nullptr;
    uint32_t address_page_size = 0;
    bool addresses_are_raw_v2 = false;
    std::vector<AddressEntry> decoded_addresses;

    // --- Legacy (kept for compatibility during transition) ---
    const uint8_t* codes_start = nullptr;  // Zero-copy into block_buf
    uint32_t code_entry_size = 0;          // Bytes per RaBitQ code entry

    ParsedCluster() = default;
    ParsedCluster(ParsedCluster&&) = default;
    ParsedCluster& operator=(ParsedCluster&&) = default;
    VDB_DISALLOW_COPY(ParsedCluster);

    float norm_oc(uint32_t block_idx, uint32_t vec_in_block) const {
        const uint8_t* block =
            fastscan_blocks + static_cast<size_t>(block_idx) * fastscan_block_size;
        const uint32_t packed_codes_size = fastscan_block_size - 32 * sizeof(float);
        const float* norms = reinterpret_cast<const float*>(block + packed_codes_size);
        return norms[vec_in_block];
    }

    ExRaBitQView exrabitq_view(uint32_t vec_idx, Dim dim) const {
        ExRaBitQView view;
        if (exrabitq_entries == nullptr || exrabitq_storage_version >= 11) {
            return view;
        }
        const uint8_t* entry =
            exrabitq_entries + static_cast<size_t>(vec_idx) * exrabitq_entry_size;
        view.code_abs = entry;
        view.sign = entry + dim;
        view.sign_bytes = exrabitq_sign_bytes;
        view.sign_packed = exrabitq_sign_packed;
        std::memcpy(&view.xipnorm, entry + dim + exrabitq_sign_bytes, sizeof(float));
        return view;
    }

    ExRaBitQBatchBlockView exrabitq_batch_block_view(uint32_t batch_block_id) const {
        ExRaBitQBatchBlockView view;
        if (exrabitq_batch_blocks == nullptr ||
            batch_block_id >= exrabitq_num_batch_blocks ||
            exrabitq_batch_block_size == 0 ||
            exrabitq_batch_size == 0 ||
            exrabitq_dim_block == 0) {
            return view;
        }
        const uint8_t* block =
            exrabitq_batch_blocks +
            static_cast<size_t>(batch_block_id) * exrabitq_batch_block_size;
        const uint32_t abs_total_bytes =
            exrabitq_num_dim_blocks * exrabitq_batch_size * exrabitq_dim_block;
        view.batch_block_id = batch_block_id;
        std::memcpy(&view.valid_count, block, sizeof(uint32_t));
        const uint8_t* payload = block + sizeof(uint32_t);
        view.abs_blocks = payload;
        view.sign_blocks = payload + abs_total_bytes;
        view.xipnorms = reinterpret_cast<const float*>(
            block + exrabitq_batch_block_size - exrabitq_batch_size * sizeof(float));
        return view;
    }

    ExRaBitQBatchParallelBlockView exrabitq_batch_parallel_block_view(
        uint32_t batch_block_id) const {
        ExRaBitQBatchParallelBlockView view;
        if (exrabitq_parallel_abs_blocks == nullptr ||
            exrabitq_parallel_sign_words == nullptr ||
            batch_block_id >= exrabitq_num_batch_blocks ||
            exrabitq_parallel_abs_block_size == 0 ||
            exrabitq_parallel_sign_words_per_block == 0 ||
            exrabitq_batch_size == 0) {
            return view;
        }
        view.batch_block_id = batch_block_id;
        view.abs_slices =
            exrabitq_parallel_abs_blocks +
            static_cast<size_t>(batch_block_id) * exrabitq_parallel_abs_block_size;
        view.sign_words =
            exrabitq_parallel_sign_words +
            static_cast<size_t>(batch_block_id) * exrabitq_parallel_sign_words_per_block;
        view.slices_per_dim_block = exrabitq_parallel_slices_per_dim_block;
        const auto base_view = exrabitq_batch_block_view(batch_block_id);
        view.valid_count = base_view.valid_count;
        view.xipnorms = base_view.xipnorms;
        return view;
    }

    const uint8_t* ex_code(uint32_t vec_idx) const {
        if (exrabitq_storage_version >= 11) return nullptr;
        return exrabitq_entries + static_cast<size_t>(vec_idx) * exrabitq_entry_size;
    }

    const uint8_t* ex_sign(uint32_t vec_idx, Dim dim) const {
        if (exrabitq_storage_version >= 11) return nullptr;
        return exrabitq_entries + static_cast<size_t>(vec_idx) * exrabitq_entry_size + dim;
    }

    uint32_t ex_sign_bytes() const { return exrabitq_sign_bytes; }
    bool ex_sign_is_packed() const { return exrabitq_sign_packed; }

    float xipnorm(uint32_t vec_idx, Dim dim) const {
        if (exrabitq_storage_version >= 11) {
            const uint32_t block_id = vec_idx / exrabitq_batch_size;
            const uint32_t lane_id = vec_idx % exrabitq_batch_size;
            const auto view = exrabitq_batch_block_view(block_id);
            if (view.xipnorms == nullptr || lane_id >= view.valid_count) {
                return 0.0f;
            }
            return view.xipnorms[lane_id];
        }
        float val;
        std::memcpy(
            &val,
            exrabitq_entries + static_cast<size_t>(vec_idx) * exrabitq_entry_size +
                dim + exrabitq_sign_bytes,
            sizeof(float));
        return val;
    }

    AddressEntry AddressAt(uint32_t vec_idx) const {
        if (addresses_are_raw_v2 && raw_addresses != nullptr) {
            return storage::AddressColumn::DecodeRawEntryV2(
                raw_addresses[vec_idx], address_page_size);
        }
        return decoded_addresses[vec_idx];
    }

    void DecodeAddressBatch(const uint32_t* vec_idxs,
                            uint32_t count,
                            AddressEntry* out) const {
        if (addresses_are_raw_v2 && raw_addresses != nullptr) {
            simd::DecodeAddressBatch(raw_addresses, vec_idxs, count,
                                     address_page_size, out);
            return;
        }
        simd::DecodeAddressBatch(decoded_addresses.data(), vec_idxs, count, out);
    }
};

}  // namespace query
}  // namespace vdb
