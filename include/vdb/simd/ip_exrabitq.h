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

/// Pack/unpack ExRaBitQ Stage2 payload codes for packed `.clu` layouts.
/// Legacy v12 uses 2/4-bit magnitudes; official v13 also uses 3-bit ExData.
uint32_t ExRaBitQPackedMagnitudeBytes(uint32_t dim_block, uint8_t bits);

bool ExRaBitQPackMagnitudes(const uint8_t* VDB_RESTRICT decoded, uint32_t count, uint8_t bits,
                            uint8_t* VDB_RESTRICT packed, uint32_t packed_bytes);

bool ExRaBitQUnpackMagnitudes(const uint8_t* VDB_RESTRICT packed, uint32_t count, uint8_t bits,
                              uint8_t* VDB_RESTRICT decoded);

bool ExRaBitQDecodePackedBatchBlockMagnitudes(const uint8_t* VDB_RESTRICT packed_abs_blocks,
                                              uint32_t num_dim_blocks, uint32_t batch_size,
                                              uint32_t dim_block,
                                              uint32_t abs_bytes_per_lane_dim_block, uint8_t bits,
                                              uint8_t* VDB_RESTRICT decoded_abs_blocks);

bool ExRaBitQPackOfficialDirect3(const uint8_t* VDB_RESTRICT decoded, uint32_t count,
                                 RaBitQExDataLayout layout,
                                 uint8_t* VDB_RESTRICT packed, uint32_t packed_bytes);

bool ExRaBitQUnpackOfficialDirect3(const uint8_t* VDB_RESTRICT packed, uint32_t count,
                                   RaBitQExDataLayout layout,
                                   uint8_t* VDB_RESTRICT decoded);

bool ExRaBitQPackOfficialDirectBitplanes(const uint8_t* VDB_RESTRICT decoded,
                                         uint32_t count, uint8_t bits,
                                         uint8_t* VDB_RESTRICT packed,
                                         uint32_t packed_bytes);

bool ExRaBitQUnpackOfficialDirectBitplanes(const uint8_t* VDB_RESTRICT packed,
                                           uint32_t count, uint8_t bits,
                                           uint8_t* VDB_RESTRICT decoded);

bool ExRaBitQPackOfficialNibble4(const uint8_t* VDB_RESTRICT decoded,
                                 uint32_t count, uint8_t* VDB_RESTRICT packed,
                                 uint32_t packed_bytes);

bool ExRaBitQUnpackOfficialNibble4(const uint8_t* VDB_RESTRICT packed,
                                   uint32_t count, uint8_t* VDB_RESTRICT decoded);

bool ExRaBitQPackOfficial2Bit(const uint8_t* VDB_RESTRICT decoded,
                              uint32_t count, uint8_t* VDB_RESTRICT packed,
                              uint32_t packed_bytes);

bool ExRaBitQUnpackOfficial2Bit(const uint8_t* VDB_RESTRICT packed,
                                uint32_t count, uint8_t* VDB_RESTRICT decoded);

/// Cacheline-aware bit-major vector tile layout. Each lane record is split into
/// dimension tiles whose bitplanes target 64B, then 32B, then smaller powers of
/// two when the remaining dimensionality cannot fill a 64B bitplane.
uint32_t ExRaBitQBitMajorTileDims(uint32_t remaining_dims);
uint32_t ExRaBitQBitMajorTileVectorBytes(uint32_t dim, uint8_t bits);

bool ExRaBitQPackOfficialBitMajorTiles(const uint8_t* VDB_RESTRICT decoded,
                                       uint32_t count, uint8_t bits,
                                       uint8_t* VDB_RESTRICT packed,
                                       uint32_t packed_bytes);

bool ExRaBitQUnpackOfficialBitMajorTiles(const uint8_t* VDB_RESTRICT packed,
                                         uint32_t count, uint8_t bits,
                                         uint8_t* VDB_RESTRICT decoded);

/// Compute the signed inner product for ExRaBitQ Stage 2:
///
///   result = Σ query[i] * sign[i] * (code_abs[i] + 0.5)
///
/// @param query      Rotated query vector (float, length = dim)
/// @param code_abs   Per-dimension quantized absolute values (uint8_t, length =
/// dim)
/// @param sign       Sign payload, either packed bits or per-dimension flags
/// @param sign_packed Whether `sign` uses packed-bit layout
/// @param dim        Vector dimensionality
/// @return           Raw inner product (to be multiplied by xipnorm)
float IPExRaBitQ(const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT code_abs,
                 const uint8_t* VDB_RESTRICT sign, bool sign_packed, Dim dim);

/// Backward-compatible overload for legacy un-packed sign payloads.
VDB_FORCE_INLINE float IPExRaBitQ(const float* VDB_RESTRICT query,
                                  const uint8_t* VDB_RESTRICT code_abs,
                                  const uint8_t* VDB_RESTRICT sign, Dim dim) {
    return IPExRaBitQ(query, code_abs, sign, false, dim);
}

/// Packed-sign specialized Stage2 kernel for v10 serving path.
float IPExRaBitQPackedSign(const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT code_abs,
                           const uint8_t* VDB_RESTRICT packed_sign, Dim dim);

/// Batch packed-sign Stage2 kernel.
/// `code_abs_ptrs`, `packed_sign_ptrs`, and `out_ip_raw` must all have length
/// >= count.
void IPExRaBitQBatchPackedSign(const float* VDB_RESTRICT query,
                               const uint8_t* const* VDB_RESTRICT code_abs_ptrs,
                               const uint8_t* const* VDB_RESTRICT packed_sign_ptrs, uint32_t count,
                               Dim dim, float* VDB_RESTRICT out_ip_raw);

/// Compact-block packed-sign Stage2 kernel for v11 serving path.
/// `abs_blocks` layout: [num_dim_blocks][8][64]
/// `sign_blocks` layout: [num_dim_blocks][8][8B]
void IPExRaBitQBatchPackedSignCompact(const float* VDB_RESTRICT query,
                                      const uint8_t* VDB_RESTRICT abs_blocks,
                                      const uint8_t* VDB_RESTRICT sign_blocks, uint32_t valid_count,
                                      Dim dim, uint32_t dim_block, float* VDB_RESTRICT out_ip_raw,
                                      IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Parallel-friendly resident Stage2 kernel for v11 preload-time transcode
/// path. `abs_slices` layout: [num_dim_blocks][dim_block/16][8][16]
/// `sign_words` layout: [num_dim_blocks][dim_block/16][8]
void IPExRaBitQBatchPackedSignParallelCompact(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT abs_slices,
    const uint16_t* VDB_RESTRICT sign_words, uint32_t valid_count, Dim dim, uint32_t dim_block,
    uint32_t slices_per_dim_block, float* VDB_RESTRICT out_ip_raw,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware variant of the parallel-friendly Stage2 kernel.
/// Computes and writes only lanes selected by `lane_mask`; lane indexes in
/// `out_ip_raw` remain the original block-local lane ids.
void IPExRaBitQBatchPackedSignParallelCompactMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT abs_slices,
    const uint16_t* VDB_RESTRICT sign_words, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, uint32_t slices_per_dim_block, float* VDB_RESTRICT out_ip_raw,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Official RaBitQ 1+n ExData dot over decoded sign-folded ExData codes:
///
///   result = sum_i query[i] * ex_code[i]
///
/// The sign-bit contribution and `cb * sum_q` bias are combined separately via
/// OfficialRaBitQCombineNormalizedIP because v13 stores no per-vector sign
/// payload in the Stage2 ExData region.
float IPOfficialRaBitQExData(const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT ex_code,
                             Dim dim);

/// Compact-block official ExData kernel. `ex_code_blocks` layout is
/// [num_dim_blocks][8][dim_block] with decoded sign-folded ExData codes.
void IPOfficialRaBitQBatchCompact(const float* VDB_RESTRICT query,
                                  const uint8_t* VDB_RESTRICT ex_code_blocks, uint32_t valid_count,
                                  Dim dim, uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
                                  IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData compact-block kernel. Computes and writes only
/// lanes selected by `lane_mask`; lane indexes in `out_ip_ex` remain original
/// block-local lane ids.
void IPOfficialRaBitQBatchCompactMasked(const float* VDB_RESTRICT query,
                                        const uint8_t* VDB_RESTRICT ex_code_blocks,
                                        uint32_t lane_mask, uint32_t valid_count, Dim dim,
                                        uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
                                        IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData kernel over direct-compact 1/2/3-bit bitplane
/// layouts. `compact_blocks` layout is
/// [num_dim_blocks][8][ceil(dim_block * bits / 8)].
void IPOfficialRaBitQBatchCompactDirectBitplanesMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint8_t bits, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

void IPOfficialRaBitQBatchCompactDirectBitplanesStridedMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint8_t bits, uint32_t stored_lanes, uint32_t lane_mask, uint32_t valid_count,
    Dim dim, uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData kernel over lane-major vector bitplane layout.
/// `compact_blocks` layout is [valid_count][num_dim_blocks][ceil(dim_block * bits / 8)].
void IPOfficialRaBitQBatchCompactVectorBitplanesMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint8_t bits, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Same storage contract as IPOfficialRaBitQBatchCompactVectorBitplanesMasked,
/// but prefetches the next requested lane record before computing the current
/// one.
void IPOfficialRaBitQBatchCompactVectorBitplanesPrefetchMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint8_t bits, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Same storage contract as IPOfficialRaBitQBatchCompactVectorBitplanesMasked,
/// but processes requested lanes in small survivor chunks so each query block is
/// loaded once per chunk instead of once per lane.
void IPOfficialRaBitQBatchCompactVectorBitplanesMicroBatchMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint8_t bits, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData kernel over lane-major bit-major tile layout.
/// `compact_blocks` layout is [valid_count][tile][bit][tile_dims / 8], where
/// tile_dims is selected from 512/256/128/64 according to the remaining dims.
void IPOfficialRaBitQBatchCompactVectorBitMajorTilesMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint8_t bits, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData kernel over 4-lane subgroup bitplane layout.
/// `compact_blocks` layout is [subgroup4][dim_block][local_lane][bitplanes].
void IPOfficialRaBitQBatchCompactSmallLane4BitplanesMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint8_t bits, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData kernel over 2-lane subgroup bitplane layout.
/// `compact_blocks` layout is [subgroup2][dim_block][local_lane][bitplanes].
void IPOfficialRaBitQBatchCompactSmallLane2BitplanesMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint8_t bits, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData kernel over lane-major vector 4-bit nibble
/// layout. `compact_blocks` layout is [valid_count][num_dim_blocks][32B],
/// where each 64-dim block is four official RaBitQ 16-dim nibble groups.
void IPOfficialRaBitQBatchCompactVectorNibble4Masked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint32_t lane_mask, uint32_t valid_count, Dim dim, uint32_t dim_block,
    float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData kernel over lane-major vector 2-bit compact
/// layout. `compact_blocks` layout is [valid_count][num_dim_blocks][16B],
/// where each 64-dim block stores dimensions j, j+16, j+32, j+48 in byte j.
void IPOfficialRaBitQBatchCompactVector2BitMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint32_t lane_mask, uint32_t valid_count, Dim dim, uint32_t dim_block,
    float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

void IPOfficialRaBitQBatchCompactDirect3ZeroPlaneElideMasked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    uint32_t lane_mask, uint32_t valid_count, Dim dim, uint32_t dim_block,
    float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

/// Mask-aware official ExData kernel over legacy direct-compact 3-bit layouts.
/// `compact_blocks` layout is [num_dim_blocks][8][24B] for dim_block=64.
void IPOfficialRaBitQBatchCompactDirect3Masked(
    const float* VDB_RESTRICT query, const uint8_t* VDB_RESTRICT compact_blocks,
    RaBitQExDataLayout layout, uint32_t lane_mask, uint32_t valid_count, Dim dim,
    uint32_t dim_block, float* VDB_RESTRICT out_ip_ex,
    IPExRaBitQBatchPackedSignCompactTiming* timing = nullptr);

VDB_FORCE_INLINE float OfficialRaBitQExBias(uint8_t ex_bits) {
    return -(static_cast<float>(1u << ex_bits) - 0.5f);
}

/// Combine normalized-query official terms:
///
///   sum_i q[i] * ((sign_bit_i << ex_bits) + ex_code_i + cb)
///
/// where `ip_x0_qr` is sum_i(q[i] for sign_bit_i == 1).
VDB_FORCE_INLINE float OfficialRaBitQCombineNormalizedIP(float ip_x0_qr, float ip_ex_code,
                                                         float sum_q, uint8_t ex_bits) {
    const float sign_scale = static_cast<float>(1u << ex_bits);
    return sign_scale * ip_x0_qr + ip_ex_code + OfficialRaBitQExBias(ex_bits) * sum_q;
}

VDB_FORCE_INLINE float OfficialRaBitQEstimateDistance(float query_norm_sq, float factor_add,
                                                      float factor_rescale, float ex_ip) {
    const float dist = query_norm_sq + factor_add + factor_rescale * ex_ip;
    return dist > 0.0f ? dist : 0.0f;
}

} // namespace simd
} // namespace vdb
