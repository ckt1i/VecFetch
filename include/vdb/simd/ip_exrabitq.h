#pragma once

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

#include <cstdint>

namespace vdb {
namespace simd {

struct IPExRaBitQBatchPackedSignCompactTiming {
    double sign_flip_ms = 0;
    double abs_fma_ms = 0;
    double tail_ms = 0;
    double reduce_ms = 0;
};

/// Compute the signed inner product for ExRaBitQ Stage 2:
///
///   result = Σ query[i] * sign[i] * (code_abs[i] + 0.5)
///
/// @param query      Rotated query vector (float, length = dim)
/// @param code_abs   Per-dimension quantized absolute values (uint8_t, length = dim)
/// @param sign       Sign payload, either packed bits or per-dimension flags
/// @param sign_packed Whether `sign` uses packed-bit layout
/// @param dim        Vector dimensionality
/// @return           Raw inner product (to be multiplied by xipnorm)
float IPExRaBitQ(const float* VDB_RESTRICT query,
                 const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign,
                 bool sign_packed,
                 Dim dim);

/// Backward-compatible overload for legacy un-packed sign payloads.
VDB_FORCE_INLINE float IPExRaBitQ(const float* VDB_RESTRICT query,
                                  const uint8_t* VDB_RESTRICT code_abs,
                                  const uint8_t* VDB_RESTRICT sign,
                                  Dim dim) {
    return IPExRaBitQ(query, code_abs, sign, false, dim);
}

/// Packed-sign specialized Stage2 kernel for v10 serving path.
float IPExRaBitQPackedSign(const float* VDB_RESTRICT query,
                           const uint8_t* VDB_RESTRICT code_abs,
                           const uint8_t* VDB_RESTRICT packed_sign,
                           Dim dim);

/// Batch packed-sign Stage2 kernel.
/// `code_abs_ptrs`, `packed_sign_ptrs`, and `out_ip_raw` must all have length >= count.
void IPExRaBitQBatchPackedSign(const float* VDB_RESTRICT query,
                               const uint8_t* const* VDB_RESTRICT code_abs_ptrs,
                               const uint8_t* const* VDB_RESTRICT packed_sign_ptrs,
                               uint32_t count,
                               Dim dim,
                               float* VDB_RESTRICT out_ip_raw);

/// Compact-block packed-sign Stage2 kernel for v11 serving path.
/// `abs_blocks` layout: [num_dim_blocks][8][64]
/// `sign_blocks` layout: [num_dim_blocks][8][8B]
void IPExRaBitQBatchPackedSignCompact(const float* VDB_RESTRICT query,
                                      const uint8_t* VDB_RESTRICT abs_blocks,
                                      const uint8_t* VDB_RESTRICT sign_blocks,
                                      uint32_t valid_count,
                                      Dim dim,
                                      uint32_t dim_block,
                                      float* VDB_RESTRICT out_ip_raw,
                                      IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Parallel-friendly resident Stage2 kernel for v11 preload-time transcode path.
/// `abs_slices` layout: [num_dim_blocks][dim_block/16][8][16]
/// `sign_words` layout: [num_dim_blocks][dim_block/16][8]
void IPExRaBitQBatchPackedSignParallelCompact(
    const float* VDB_RESTRICT query,
    const uint8_t* VDB_RESTRICT abs_slices,
    const uint16_t* VDB_RESTRICT sign_words,
    uint32_t valid_count,
    Dim dim,
    uint32_t dim_block,
    uint32_t slices_per_dim_block,
    float* VDB_RESTRICT out_ip_raw,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

}  // namespace simd
}  // namespace vdb
