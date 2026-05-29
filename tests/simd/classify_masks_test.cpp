#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <limits>

#include "vdb/simd/fastscan.h"
#include "vdb/simd/stage2_classify.h"

namespace {

uint32_t ReferenceStage2SafeInMask(const float* ip_raw,
                                   const float* xipnorm,
                                   const float* norm_oc,
                                   const float* margin_s1,
                                   uint32_t active_mask,
                                   float norm_qc,
                                   float norm_qc_sq,
                                   float inv_margin_s2_divisor,
                                   float safein_dk,
                                   float safeout_frontier_upper) {
    uint32_t safein = 0;
    uint32_t safeout = 0;
    for (uint32_t lane = 0; lane < 8; ++lane) {
        const uint32_t bit = 1u << lane;
        if ((active_mask & bit) == 0) continue;
        const float ip_est = ip_raw[lane] * xipnorm[lane];
        float est_dist = norm_oc[lane] * norm_oc[lane] + norm_qc_sq -
                         2.0f * norm_oc[lane] * norm_qc * ip_est;
        est_dist = std::max(est_dist, 0.0f);
        const float margin = margin_s1[lane] * inv_margin_s2_divisor;
        if (est_dist > safeout_frontier_upper + margin) {
            safeout |= bit;
        } else if (est_dist < safein_dk - margin) {
            safein |= bit;
        }
    }
    return safein & ~safeout;
}

}  // namespace

TEST(FastScanStage1EvaluateTest, MatchesLegacyDequantizeAndMasks) {
    alignas(64) uint32_t raw_accu[32];
    alignas(64) float norms[32];
    alignas(64) float fused_dists[32];
    alignas(64) float legacy_dists[32];

    for (uint32_t dim : {512u, 768u, 1024u}) {
        const float inv_sqrt_dim = 1.0f / std::sqrt(static_cast<float>(dim));
        for (uint32_t count : {1u, 7u, 16u, 31u, 32u}) {
            for (uint32_t i = 0; i < 32; ++i) {
                raw_accu[i] = 1000u + 37u * i + (dim / 32u);
                norms[i] = 0.75f + 0.03125f * static_cast<float>((i * 7u) % 23u);
                fused_dists[i] = -1.0f;
                legacy_dists[i] = -2.0f;
            }

            const int32_t fs_shift = -321;
            const float fs_width = 1.0f / 8191.0f;
            const float sum_q = 3.25f;
            const float norm_qc = 1.35f;
            const float norm_qc_sq = norm_qc * norm_qc;
            const float safeout_frontier_upper = 1.8f;
            const float safeout_margin_factor = 0.11f;
            const float safein_dk = 1.2f;
            const float safein_margin_factor = 0.07f;

            vdb::simd::FastScanStage1EvalResult fused{};
            vdb::simd::FastScanStage1Evaluate(
                raw_accu, norms, count, fs_shift, fs_width, sum_q,
                inv_sqrt_dim, norm_qc, norm_qc_sq,
                safeout_frontier_upper, safeout_margin_factor,
                safein_dk, safein_margin_factor,
                /*enable_safein=*/true, fused_dists, &fused);

            vdb::simd::FastScanDequantize(
                raw_accu, norms, count, fs_shift, fs_width, sum_q,
                inv_sqrt_dim, norm_qc, norm_qc_sq, legacy_dists);
            const uint32_t legacy_so = vdb::simd::FastScanSafeOutMask(
                legacy_dists, norms, count, safeout_frontier_upper,
                safeout_margin_factor);
            const uint32_t legacy_si = vdb::simd::FastScanSafeInMask(
                legacy_dists, norms, count, safein_dk, safein_margin_factor);

            for (uint32_t i = 0; i < count; ++i) {
                EXPECT_NEAR(fused_dists[i], legacy_dists[i], 1e-5f)
                    << "dim=" << dim << " count=" << count << " lane=" << i;
            }
            EXPECT_EQ(fused.safeout_mask, legacy_so)
                << "dim=" << dim << " count=" << count;
            EXPECT_EQ(fused.safein_mask, legacy_si)
                << "dim=" << dim << " count=" << count;
        }
    }
}

TEST(FastScanStage1EvaluateTest, SafeInDisabledAndInfiniteFrontier) {
    alignas(64) uint32_t raw_accu[32];
    alignas(64) float norms[32];
    alignas(64) float dists[32];
    for (uint32_t i = 0; i < 32; ++i) {
        raw_accu[i] = 900u + i * 11u;
        norms[i] = 0.5f + 0.02f * static_cast<float>(i);
        dists[i] = 0.0f;
    }

    vdb::simd::FastScanStage1EvalResult result{};
    vdb::simd::FastScanStage1Evaluate(
        raw_accu, norms, 32,
        /*fs_shift=*/0, /*fs_width=*/1.0f / 8191.0f,
        /*sum_q=*/0.5f, /*inv_sqrt_dim=*/1.0f / std::sqrt(768.0f),
        /*norm_qc=*/1.0f, /*norm_qc_sq=*/1.0f,
        std::numeric_limits<float>::infinity(),
        /*safeout_margin_factor=*/0.1f,
        /*safein_threshold_base=*/100.0f,
        /*safein_margin_factor=*/0.1f,
        /*enable_safein=*/false,
        dists, &result);

    EXPECT_EQ(result.safeout_mask, 0u);
    EXPECT_EQ(result.safein_mask, 0u);
}

TEST(FastScanClassifyMaskTest, SafeOutUsesOneSidedCandidateMargin) {
    const float dists[] = {6.9f, 7.1f, 8.0f, 8.1f};
    const float norms[] = {1.0f, 1.0f, 2.0f, 2.0f};
    const float frontier = 6.0f;
    const float margin_factor = 1.0f;

    const uint32_t mask = vdb::simd::FastScanSafeOutMask(
        dists, norms, 4, frontier, margin_factor);

    EXPECT_EQ(mask & (1u << 0), 0u);        // 6.9 <= 6 + 1
    EXPECT_NE(mask & (1u << 1), 0u);        // 7.1 > 6 + 1
    EXPECT_EQ(mask & (1u << 2), 0u);        // 8.0 <= 6 + 2
    EXPECT_NE(mask & (1u << 3), 0u);        // 8.1 > 6 + 2
}

TEST(FastScanClassifyMaskTest, SafeInUsesOneSidedCandidateMargin) {
    const float dists[] = {3.9f, 4.0f, 2.9f, 3.0f};
    const float norms[] = {1.0f, 1.0f, 2.0f, 2.0f};
    const float safein_dk = 5.0f;
    const float margin_factor = 1.0f;

    const uint32_t mask = vdb::simd::FastScanSafeInMask(
        dists, norms, 4, safein_dk, margin_factor);

    EXPECT_NE(mask & (1u << 0), 0u);        // 3.9 < 5 - 1
    EXPECT_EQ(mask & (1u << 1), 0u);        // boundary
    EXPECT_NE(mask & (1u << 2), 0u);        // 2.9 < 5 - 2
    EXPECT_EQ(mask & (1u << 3), 0u);        // boundary
}

TEST(Stage2ClassifyMaskTest, MatchesReferenceOneSidedMargins) {
    alignas(32) float ip_raw[8] = {
        -0.5f, -1.0f, -2.0f, -3.5f, -4.0f, -1.5f, -5.0f, -0.25f};
    alignas(32) float xipnorm[8] = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    alignas(32) float norm_oc[8] = {
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    alignas(32) float margin_s1[8] = {
        2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f};

    const uint32_t active_mask = 0xFFu;
    const float inv_margin_s2_divisor = 0.5f;
    const float safein_dk = 5.0f;
    const float safeout_frontier_upper = 8.0f;

    const vdb::simd::Stage2ClassifyMasks masks = vdb::simd::Stage2ClassifyBatch(
        ip_raw, xipnorm, norm_oc, margin_s1, active_mask,
        /*norm_qc=*/1.0f, /*norm_qc_sq=*/1.0f, inv_margin_s2_divisor,
        safein_dk, safeout_frontier_upper);

    const uint32_t expected_safein = ReferenceStage2SafeInMask(
        ip_raw, xipnorm, norm_oc, margin_s1, active_mask,
        1.0f, 1.0f, inv_margin_s2_divisor,
        safein_dk, safeout_frontier_upper);

    EXPECT_EQ(masks.safein, expected_safein);
    EXPECT_EQ(masks.safeout, (1u << 4) | (1u << 6));
    EXPECT_EQ(masks.uncertain, active_mask & ~masks.safein & ~masks.safeout);
}
