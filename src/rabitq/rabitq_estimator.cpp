#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/simd/fastscan.h"
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/simd/popcount.h"
#include "vdb/simd/prepare_query.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if defined(VDB_USE_AVX512)
#include <immintrin.h>
#endif

namespace vdb {
namespace rabitq {

namespace {

constexpr bool kUseFusedFastScanPrepare = true;

void EnsurePrepareBuffers(Dim dim,
                          uint32_t words_per_plane,
                          PreparedQuery* pq,
                          ClusterPreparedScratch* scratch) {
    const uint32_t dim_u32 = static_cast<uint32_t>(dim);
    const size_t lut_size = static_cast<size_t>(dim) * 8 + 63;

    if (pq->rotated_size != dim_u32) {
        pq->rotated.resize(dim);
        pq->rotated_size = dim_u32;
    }
    if (pq->sign_code_size != words_per_plane) {
        pq->sign_code.resize(words_per_plane);
        pq->sign_code_size = words_per_plane;
    }
    if (scratch->residual_size != dim_u32) {
        scratch->residual.resize(dim);
        scratch->residual_size = dim_u32;
    }
    if (scratch->fastscan_lut_size != lut_size) {
        scratch->fastscan_lut.resize(lut_size);
        scratch->fastscan_lut_size = static_cast<uint32_t>(lut_size);
    }
    if (scratch->quant_query_size != dim_u32) {
        scratch->quant_query.resize(dim);
        scratch->quant_query_size = dim_u32;
    }

    const uint8_t* raw_base = scratch->fastscan_lut.data();
    if (scratch->lut_raw_base != raw_base || scratch->lut_aligned == nullptr) {
        uintptr_t raw = reinterpret_cast<uintptr_t>(scratch->fastscan_lut.data());
        uintptr_t aligned = (raw + 63) & ~uintptr_t(63);
        scratch->lut_aligned = reinterpret_cast<uint8_t*>(aligned);
        scratch->lut_raw_base = raw_base;
    }
}

void PrepareFastScanReference(const float* rotated,
                              float max_abs,
                              Dim dim,
                              PreparedQuery* pq,
                              ClusterPreparedScratch* scratch,
                              PrepareTimingBreakdown* timing) {
    auto t0 = std::chrono::steady_clock::now();
    pq->fs_width = simd::QuantizeQuery14BitWithMax(
        rotated, max_abs, scratch->quant_query.data(), dim);
    if (timing != nullptr) {
        timing->quantize_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    auto t1 = std::chrono::steady_clock::now();
    pq->fs_shift = simd::BuildFastScanLUTReference(
        scratch->quant_query.data(), scratch->lut_aligned, dim);
    if (timing != nullptr) {
        timing->lut_build_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t1).count();
    }
}

void PrepareFastScanFused(const float* rotated,
                          float max_abs,
                          Dim dim,
                          PreparedQuery* pq,
                          ClusterPreparedScratch* scratch,
                          PrepareTimingBreakdown* timing) {
    simd::FastScanPrepareTimingBreakdown fastscan_timing;
    simd::FastScanPrepareTimingBreakdown* timing_ptr =
        (timing != nullptr) ? &fastscan_timing : nullptr;
    pq->fs_width = simd::QuantizeQuery14BitWithMaxToFastScanLUT(
        rotated, max_abs, scratch->lut_aligned, dim, &pq->fs_shift, timing_ptr,
#ifndef NDEBUG
        scratch->quant_query.data()
#else
        nullptr
#endif
    );
    if (timing != nullptr) {
        timing->quantize_ms += fastscan_timing.quantize_ms;
        timing->lut_build_ms += fastscan_timing.lut_build_ms;
    }
#ifndef NDEBUG
    std::vector<uint8_t> ref_lut_storage(static_cast<size_t>(dim) * 8 + 63);
    uintptr_t ref_raw = reinterpret_cast<uintptr_t>(ref_lut_storage.data());
    uintptr_t ref_aligned = (ref_raw + 63) & ~uintptr_t(63);
    uint8_t* ref_lut = reinterpret_cast<uint8_t*>(ref_aligned);
    const int32_t ref_shift = simd::BuildFastScanLUTReference(
        scratch->quant_query.data(), ref_lut, dim);
    const float ref_width = simd::QuantizeQuery14BitWithMax(
        rotated, max_abs, scratch->quant_query.data(), dim);
    if (std::abs(ref_width - pq->fs_width) > 1e-12f ||
        ref_shift != pq->fs_shift ||
        std::memcmp(ref_lut, scratch->lut_aligned, static_cast<size_t>(dim) * 8) != 0) {
        throw std::runtime_error("Fused FastScan prepare path diverged from reference output");
    }
#endif
}

void PrepareFastScanQuery(const float* rotated,
                          float max_abs,
                          Dim dim,
                          PreparedQuery* pq,
                          ClusterPreparedScratch* scratch,
                          PrepareTimingBreakdown* timing) {
    if constexpr (kUseFusedFastScanPrepare) {
        PrepareFastScanFused(rotated, max_abs, dim, pq, scratch, timing);
    } else {
        PrepareFastScanReference(rotated, max_abs, dim, pq, scratch, timing);
    }
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

RaBitQEstimator::RaBitQEstimator(Dim dim, uint8_t bits)
    : dim_(dim),
      bits_(bits),
      words_per_plane_((dim + 63) / 64),
      inv_sqrt_dim_(1.0f / std::sqrt(static_cast<float>(dim))) {
    quant_scratch_.resize(dim_);
}

// ============================================================================
// PrepareQuery
// ============================================================================

PreparedQuery RaBitQEstimator::PrepareQuery(
    const float* query,
    const float* centroid,
    const RotationMatrix& rotation) const {
    PreparedQuery pq;
    thread_local ClusterPreparedScratch scratch;
    PrepareQueryInto(query, centroid, rotation, &pq, &scratch);
    return pq;
}

// ============================================================================
// PrepareQueryInto — reuses existing PreparedQuery buffers
// ============================================================================

void RaBitQEstimator::PrepareQueryInto(
    const float* query,
    const float* centroid,
    const RotationMatrix& rotation,
    PreparedQuery* pq,
    ClusterPreparedScratch* scratch,
    PrepareTimingBreakdown* timing) const {
    const size_t L = dim_;

    pq->dim       = dim_;
    pq->num_words = words_per_plane_;
    pq->bits      = bits_;
    EnsurePrepareBuffers(dim_, words_per_plane_, pq, scratch);

    float* residual = scratch->residual.data();

    // Fused subtract + norm_sq in one memory pass
    auto t0 = std::chrono::steady_clock::now();
    pq->norm_qc_sq = simd::SimdSubtractAndNormSq(
        query, centroid, residual, static_cast<uint32_t>(L));
    pq->norm_qc = std::sqrt(pq->norm_qc_sq);
    if (timing != nullptr) {
        timing->subtract_norm_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    // Normalize residual
    auto t1 = std::chrono::steady_clock::now();
    if (pq->norm_qc > 1e-30f) {
        float inv_norm = 1.0f / pq->norm_qc;
        for (size_t i = 0; i < L; ++i) {
            residual[i] *= inv_norm;
        }
    }
    if (timing != nullptr) {
        timing->normalize_sign_sum_maxabs_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t1).count();
    }

    // Rotate — Apply reads from residual, writes to rotated
    auto t_rot = std::chrono::steady_clock::now();
    rotation.Apply(residual, pq->rotated.data());
    if (timing != nullptr) {
        timing->rotation_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_rot).count();
    }

    // Fused sign-quantize + sum_q in one memory pass
    // inv_norm=1.0f: rotated is already unit-length; the multiply is a no-op
    simd::NormalizeSignSumResult norm_result =
        simd::SimdNormalizeSignSumMaxAbs(
            pq->rotated.data(), 1.0f,
            pq->sign_code.data(), words_per_plane_,
            static_cast<uint32_t>(L));
    pq->sum_q = norm_result.sum;

    // FastScan prepare boundary:
    //   prepare_query.cpp owns subtract / normalize / sign / max_abs
    //   fastscan.cpp owns reference and fused quantize+LUT generation
    // Note: BuildFastScanLUT writes every byte (M = dim/4 groups × 16 entries
    // × 2 bytes lo/hi = dim*8 bytes), so the prior zero-init is dead work.
#ifndef NDEBUG
    const size_t lut_size = static_cast<size_t>(dim_) * 8;
    std::memset(scratch->lut_aligned, 0xCD, lut_size);  // sentinel to catch bugs
#endif

    PrepareFastScanQuery(
        pq->rotated.data(), norm_result.max_abs, dim_, pq, scratch, timing);
    pq->lut_aligned = scratch->lut_aligned;
}

// ============================================================================
// PrepareQueryRotatedInto — pre-rotated path, skips per-cluster FWHT
// ============================================================================
//
// rotated_q        = P^T × q                    (one FWHT per query)
// rotated_centroid = P^T × c_i                  (precomputed at build time)
//
// diff = rotated_q - rotated_centroid = P^T × (q - c_i)  (linearity)
// ‖diff‖ = ‖q - c_i‖                                      (norm preservation)
//
// All downstream operations (sign_code, sum_q, LUT) are identical to
// PrepareQueryInto because they operate on diff/‖diff‖ = P^T × (q̄).

void RaBitQEstimator::PrepareQueryRotatedInto(
    const float* rotated_q,
    const float* rotated_centroid,
    PreparedQuery* pq,
    ClusterPreparedScratch* scratch,
    PrepareTimingBreakdown* timing) const {
    const size_t L = dim_;

    pq->dim       = dim_;
    pq->num_words = words_per_plane_;
    pq->bits      = bits_;
    EnsurePrepareBuffers(dim_, words_per_plane_, pq, scratch);

    // Step 1: diff = rotated_q - rotated_centroid; norm_sq = ‖diff‖²
    // Reuse pq->rotated as the diff buffer (same length L).
    auto t0 = std::chrono::steady_clock::now();
    pq->norm_qc_sq = simd::SimdSubtractAndNormSq(
        rotated_q, rotated_centroid, pq->rotated.data(), static_cast<uint32_t>(L));
    pq->norm_qc = std::sqrt(pq->norm_qc_sq);
    if (timing != nullptr) {
        timing->subtract_norm_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    // Step 2: Fused normalize + sign-quantize + sum_q.
    // diff → normalized diff = diff / ‖diff‖, extracting sign bits and sum.
    float inv_norm = (pq->norm_qc > 1e-30f) ? (1.0f / pq->norm_qc) : 1.0f;
    auto t1 = std::chrono::steady_clock::now();
    simd::NormalizeSignSumResult norm_result =
        simd::SimdNormalizeSignSumMaxAbs(
            pq->rotated.data(), inv_norm,
            pq->sign_code.data(), words_per_plane_,
            static_cast<uint32_t>(L));
    pq->sum_q = norm_result.sum;
    // pq->rotated now holds the normalized diff = P^T × (q̄)
    if (timing != nullptr) {
        timing->normalize_sign_sum_maxabs_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t1).count();
    }

    // Step 3: FastScan quantization + LUT (reference and fused paths live in fastscan.cpp)
#ifndef NDEBUG
    const size_t lut_size = static_cast<size_t>(dim_) * 8;
    std::memset(scratch->lut_aligned, 0xCD, lut_size);
#endif

    PrepareFastScanQuery(
        pq->rotated.data(), norm_result.max_abs, dim_, pq, scratch, timing);
    pq->lut_aligned = scratch->lut_aligned;
}

// ============================================================================
// EstimateDistance — Stage 1: fast XOR+popcount path (MSB plane only)
// ============================================================================

float RaBitQEstimator::EstimateDistance(
    const PreparedQuery& pq,
    const RaBitQCode& code) const {
    // Use only MSB plane (first words_per_plane_ words)
    return EstimateDistanceRaw(pq, code.code.data(), words_per_plane_,
                               code.norm);
}

// ============================================================================
// EstimateDistanceRaw — Stage 1: zero-copy fast XOR+popcount path
// ============================================================================

float RaBitQEstimator::EstimateDistanceRaw(
    const PreparedQuery& pq,
    const uint64_t* code_words,
    uint32_t num_words,
    float norm_oc) const {
    // Hamming distance = popcount(q_sign XOR db_code)
    uint32_t hamming = simd::PopcountXor(
        pq.sign_code.data(), code_words, num_words);

    // Popcount-based inner product estimate:
    //   ⟨q̄, ô⟩ ≈ 1 - 2·hamming/L
    float ip_est = 1.0f - 2.0f * static_cast<float>(hamming) /
                                  static_cast<float>(dim_);

    // Full distance: ‖o-q‖² ≈ ‖o-c‖² + ‖q-c‖² - 2·‖o-c‖·‖q-c‖·⟨q̄,ô⟩
    float dist_sq = norm_oc * norm_oc + pq.norm_qc_sq
                    - 2.0f * norm_oc * pq.norm_qc * ip_est;

    return std::max(dist_sq, 0.0f);
}

// ============================================================================
// EstimateDistanceFastScan — batch-32 VPSHUFB path
// ============================================================================

void RaBitQEstimator::EstimateDistanceFastScan(
    const PreparedQuery& pq,
    const uint8_t* packed_codes,
    const float* block_norms,
    uint32_t count,
    float* out_dist) const {
    // 1. VPSHUFB accumulation: get raw LUT sums for 32 vectors
    alignas(64) uint32_t raw_accu[32] = {0};
    simd::AccumulateBlock(packed_codes, pq.lut_aligned, raw_accu, dim_);

    // 2. De-quantize and compute distances.
    //
    // raw_accu[v] = Σ_{d where x[v][d]=1} (quant_q[d] - v_min_per_group)
    //
    // After shift + width:
    //   ip_raw = (raw_accu[v] + fs_shift) * fs_width  ≈  Σ_{x[d]=1} q'[d]
    //
    // Inner product estimate (matching EstimateDistanceAccurate):
    //   ⟨q̄, ô⟩ = (1/√D) * (2*ip_raw - sum_q)
    //
    // Distance:
    //   dist = norm_oc² + norm_qc² - 2*norm_oc*norm_qc*⟨q̄,ô⟩

    simd::FastScanDequantize(raw_accu, block_norms, count,
                              pq.fs_shift, pq.fs_width, pq.sum_q,
                              inv_sqrt_dim_, pq.norm_qc, pq.norm_qc_sq,
                              out_dist);
}

void RaBitQEstimator::EstimateDistanceFastScan(
    const PreparedClusterQueryView& view,
    const uint8_t* packed_codes,
    const float* block_norms,
    uint32_t count,
    float* out_dist) const {
    EstimateDistanceFastScan(*view.prepared, packed_codes, block_norms, count, out_dist);
}

// ============================================================================
// EstimateDistanceAccurate — float dot product path (1-bit)
// ============================================================================

float RaBitQEstimator::EstimateDistanceAccurate(
    const PreparedQuery& pq,
    const RaBitQCode& code) const {
    // Compute ⟨q̄, ô⟩ directly:
    //   ô[i] = (2·x[i] - 1) / √L
    //   ⟨q̄, ô⟩ = (1/√L) Σ q'[i] × (2·x[i] - 1)

    float dot = 0.0f;
    for (size_t i = 0; i < dim_; ++i) {
        size_t word_idx = i / 64;
        size_t bit_idx  = i % 64;
        int bit = (code.code[word_idx] >> bit_idx) & 1;
        float sign = static_cast<float>(2 * bit - 1);  // +1 or -1
        dot += pq.rotated[i] * sign;
    }

    float ip_est = dot * inv_sqrt_dim_;

    // Full distance formula
    float norm_oc = code.norm;
    float dist_sq = norm_oc * norm_oc + pq.norm_qc_sq
                    - 2.0f * norm_oc * pq.norm_qc * ip_est;

    return dist_sq;
}

// ============================================================================
// EstimateDistanceMultiBit — Stage 2: M-bit LUT scan
// ============================================================================

float RaBitQEstimator::EstimateDistanceMultiBit(
    const PreparedQuery& pq,
    const RaBitQCode& code) const {
    // Use ex_code/ex_sign (fast_quantize) when available — SIMD accelerated
    if (!code.ex_code.empty()) {
        float ip_raw = simd::IPExRaBitQ(
            pq.rotated.data(), code.ex_code.data(),
            code.ex_sign_packed.data(), true, dim_);
        float ip_est = ip_raw * code.xipnorm;
        float dist_sq = code.norm * code.norm + pq.norm_qc_sq
                      - 2.0f * code.norm * pq.norm_qc * ip_est;
        return std::max(dist_sq, 0.0f);
    }
    // Fallback to bit-plane extraction
    return EstimateDistanceMultiBitRaw(pq, code.code.data(),
                                       words_per_plane_, code.bits,
                                       code.norm, code.xipnorm);
}

// ============================================================================
// EstimateDistanceMultiBitRaw — Stage 2: xipnorm-corrected M-bit estimate
// ============================================================================

float RaBitQEstimator::EstimateDistanceMultiBitRaw(
    const PreparedQuery& pq,
    const uint64_t* code_words,
    uint32_t wpp,
    uint8_t bits,
    float norm_oc,
    float xipnorm) const {
    const uint8_t M = bits;
    const int max_code = (1 << M) - 1;

    // Compute: Σ q'[i] * sign[i] * (code_abs[i] + 0.5)
    // where sign is from MSB plane, code_abs is recovered from sign + code_stored
    //
    // From the bit-plane layout:
    //   MSB plane (plane 0) = sign bit: 1=positive, 0=negative
    //   code_stored = full M-bit value extracted from all planes
    //   code_abs = (sign==1) ? code_stored : (2^M-1) - code_stored
    //
    // Then: sign[i] * (code_abs[i] + 0.5) is the signed reconstruction value
    // and   xipnorm = 1 / Σ (code_abs[i] + 0.5) * |o'[i]|

    float ip_raw = 0.0f;
    for (size_t i = 0; i < dim_; ++i) {
        size_t word_idx = i / 64;
        size_t bit_idx  = i % 64;

        // Extract full M-bit code_stored from all M planes
        int code_stored = 0;
        for (uint8_t p = 0; p < M; ++p) {
            uint32_t bit = (code_words[static_cast<size_t>(p) * wpp +
                                       word_idx] >> bit_idx) & 1;
            code_stored |= (static_cast<int>(bit) << (M - 1 - p));
        }

        // MSB of code_stored ≈ sign (due to sign-flip encoding)
        // sign_bit = MSB → 1=positive, 0=negative
        int sign_bit = (code_stored >> (M - 1)) & 1;
        float sign = sign_bit ? 1.0f : -1.0f;

        // Recover code_abs from code_stored:
        //   positive: code_stored = code_abs → code_abs = code_stored
        //   negative: code_stored = max_code - code_abs → code_abs = max_code - code_stored
        int code_abs = sign_bit ? code_stored
                                : (max_code - code_stored);

        // Signed reconstruction: sign * (code_abs + 0.5)
        float recon = sign * (static_cast<float>(code_abs) + 0.5f);
        ip_raw += pq.rotated[i] * recon;
    }

    // Apply xipnorm correction: ip_est = ip_raw * xipnorm
    float ip_est = ip_raw * xipnorm;

    // Full distance formula
    float dist_sq = norm_oc * norm_oc + pq.norm_qc_sq
                    - 2.0f * norm_oc * pq.norm_qc * ip_est;

    return std::max(dist_sq, 0.0f);
}

// ============================================================================
// EstimateDistanceBatch
// ============================================================================

void RaBitQEstimator::EstimateDistanceBatch(
    const PreparedQuery& pq,
    const RaBitQCode* codes,
    uint32_t n,
    float* out_dist) const {
    for (uint32_t i = 0; i < n; ++i) {
        out_dist[i] = EstimateDistance(pq, codes[i]);
    }
}

}  // namespace rabitq
}  // namespace vdb
