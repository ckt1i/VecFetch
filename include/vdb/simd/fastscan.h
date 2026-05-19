#pragma once

#include <cstdint>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace simd {

struct FastScanPrepareTimingBreakdown {
    double quantize_ms = 0.0;
    double lut_build_ms = 0.0;
};

/// VPSHUFB batch-32 accumulation of packed 4-bit LUT lookups.
///
/// Processes 32 vectors simultaneously using VPSHUFB. The packed_codes must
/// be in block-32 nibble-interleaved layout (as produced by PackSignBitsForFastScan).
/// The LUT must be in matching AVX-512/AVX2 lane layout (from BuildFastScanLUT).
///
/// Uses two-plane accumulation (lo/hi bytes) to avoid int16 overflow.
/// After combining: result[v] = lo_plane[v] + (hi_plane[v] << 8)
///
/// @param packed_codes  Packed sign bits, block-32 layout (dim*4 bytes)
/// @param lut           Packed LUT, lo+hi byte planes (dim*8 bytes, 64-byte aligned)
/// @param result        Output: 32 raw accumulated values (caller owns, >= 32 entries)
/// @param dim           Vector dimensionality (must be multiple of 4)
void AccumulateBlock(const uint8_t* VDB_RESTRICT packed_codes,
                     const uint8_t* VDB_RESTRICT lut,
                     uint32_t* VDB_RESTRICT result,
                     Dim dim);

/// Build the packed VPSHUFB LUT from a 14-bit quantized query.
///
/// For each group of 4 dimensions, builds a 16-entry lookup table mapping
/// every 4-bit nibble pattern to the sum of corresponding quantized query
/// values. The LUT entries are shifted to unsigned, split into lo/hi byte
/// planes, and arranged for AVX-512 128-bit lane structure.
///
/// @param quant_query   14-bit signed quantized query (int16_t, length = dim)
/// @param lut_out       Output LUT buffer (dim*8 bytes, must be 64-byte aligned)
/// @param dim           Vector dimensionality (must be multiple of 4)
/// @return              Accumulated shift (sum of v_min per group, for de-quantization)
int32_t BuildFastScanLUT(const int16_t* VDB_RESTRICT quant_query,
                          uint8_t* VDB_RESTRICT lut_out,
                          Dim dim);

/// Reference path used for equivalence checks and debug fallback.
///
/// This preserves the original two-stage flow:
///   QuantizeQuery14BitWithMax -> BuildFastScanLUTReference
///
/// The layout contract matches BuildFastScanLUT.
int32_t BuildFastScanLUTReference(const int16_t* VDB_RESTRICT quant_query,
                                  uint8_t* VDB_RESTRICT lut_out,
                                  Dim dim);

/// Quantize a unit query vector to 14-bit signed integers.
///
/// Symmetric quantization: width = max(|q|) / 8191.
/// Each q[i] is rounded to the nearest int16 in [-8191, 8191].
/// The sum of any 4 consecutive values fits in int16 without overflow.
///
/// @param query         Unit query vector (float, length = dim)
/// @param quant_out     Output: quantized values (int16_t, length = dim)
/// @param dim           Vector dimensionality
/// @return              Quantization step width (for de-quantization: value ≈ quant * width)
float QuantizeQuery14Bit(const float* VDB_RESTRICT query,
                        int16_t* VDB_RESTRICT quant_out,
                        Dim dim);

float QuantizeQuery14BitWithMax(const float* VDB_RESTRICT query,
                                float vmax,
                                int16_t* VDB_RESTRICT quant_out,
                                Dim dim);

/// Fused prepare path: quantize a normalized query and write the final
/// FastScan LUT layout directly, without requiring a fully materialized
/// quantized-query buffer on the main path.
///
/// The generated width, shift and packed LUT bytes are required to be
/// bit-equivalent to:
///   QuantizeQuery14BitWithMax(query, vmax, quant_out, dim)
///   BuildFastScanLUTReference(quant_out, lut_out, dim)
///
/// @param query         Normalized / rotated query vector
/// @param vmax          Max absolute value used by 14-bit quantization
/// @param lut_out       Output LUT buffer in final FastScan layout
/// @param dim           Vector dimensionality (must be multiple of 4)
/// @param fs_shift_out  Output accumulated v_min shift
/// @param timing        Optional split timing for quantize and LUT write phases
/// @param quant_out_opt Optional debug / regression buffer; when non-null the
///                      fused path also writes the per-dimension quant values
/// @return              Quantization step width
float QuantizeQuery14BitWithMaxToFastScanLUT(
    const float* VDB_RESTRICT query,
    float vmax,
    uint8_t* VDB_RESTRICT lut_out,
    Dim dim,
    int32_t* VDB_RESTRICT fs_shift_out,
    FastScanPrepareTimingBreakdown* timing = nullptr,
    int16_t* VDB_RESTRICT quant_out_opt = nullptr);

/// Compute a 16-bit (or 32-bit) SafeOut bitmask for a batch of FastScan dists.
///
/// For each lane v in [0, count):
///   margin_v   = margin_factor * block_norms[v]
///   so_thresh  = est_kth + 2 * margin_v
///   bit_v      = 1 if dists[v] > so_thresh else 0
///
/// Lanes beyond `count` (up to ceiling of 32) are forced to 0 in the mask.
///
/// AVX-512 path uses `_mm512_cmp_ps_mask` (16 lanes per call); the function
/// processes 32 lanes total in two iterations and returns a uint32_t mask
/// where bit v corresponds to lane v.
///
/// @param dists         Input: estimated L2² distances, length >= count
/// @param block_norms   Input: per-vector ||o-c||, length >= count
/// @param count         Number of valid lanes (1..32)
/// @param est_kth       Current k-th distance threshold
/// @param margin_factor Per-cluster constant: 2 * ||q-c|| * eps_ip
/// @return              SafeOut bitmask: bit v = 1 if lane v is SafeOut
uint32_t FastScanSafeOutMask(const float* VDB_RESTRICT dists,
                              const float* VDB_RESTRICT block_norms,
                              uint32_t count,
                              float est_kth,
                              float margin_factor);

/// Compute a SafeIn bitmask for a batch of FastScan dists.
///
/// A lane is SafeIn when its estimated distance is below the static threshold
/// minus the dynamic margin. Lanes beyond `count` are forced to 0.
uint32_t FastScanSafeInMask(const float* VDB_RESTRICT dists,
                             const float* VDB_RESTRICT block_norms,
                             uint32_t count,
                             float safein_threshold_base,
                             float margin_factor);

/// De-quantize FastScan raw_accu into final L2² distances for `count` vectors.
///
/// For each lane v in [0, count):
///   ip_raw  = (raw_accu[v] + fs_shift) * fs_width
///   ip_est  = (2 * ip_raw - sum_q) * inv_sqrt_dim
///   dist_sq = block_norms[v]² + norm_qc_sq
///             - 2 * block_norms[v] * norm_qc * ip_est
///   out_dist[v] = max(dist_sq, 0)
///
/// AVX-512 path processes 16 lanes per iteration; scalar tail handles the rest.
/// Typical use: count = 32 (one FastScan block).
///
/// @param raw_accu      Input: raw VPSHUFB accumulator output (uint32, length >= count)
/// @param block_norms   Input: per-vector ||o-c|| (float, length >= count)
/// @param count         Number of valid lanes (1..32)
/// @param fs_shift      Accumulated v_min shift from BuildFastScanLUT
/// @param fs_width      Quantization step width
/// @param sum_q         Σ rotated query components
/// @param inv_sqrt_dim  1 / √dim (precomputed)
/// @param norm_qc       ||q-c||
/// @param norm_qc_sq    ||q-c||²
/// @param out_dist      Output: estimated L2² distances (caller owns, >= count entries)
void FastScanDequantize(const uint32_t* VDB_RESTRICT raw_accu,
                        const float* VDB_RESTRICT block_norms,
                        uint32_t count,
                        int32_t fs_shift,
                        float fs_width,
                        float sum_q,
                        float inv_sqrt_dim,
                        float norm_qc,
                        float norm_qc_sq,
                        float* VDB_RESTRICT out_dist);

}  // namespace simd
}  // namespace vdb
