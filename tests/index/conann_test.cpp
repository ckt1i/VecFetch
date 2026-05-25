#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/conann.h"

using namespace vdb;
using namespace vdb::index;

// ============================================================================
// ConANN::FromConfig — epsilon computation
// ============================================================================

TEST(ConANNTest, FromConfig_ComputesCorrectEpsilon) {
    // c_factor=5.75, bits=1, dim=64
    // epsilon = 5.75 * 2^(-0.5) / sqrt(64)
    //         = 5.75 * 0.70711 / 8.0
    //         ≈ 0.50824
    RaBitQConfig cfg;
    cfg.c_factor = 5.75f;
    cfg.bits = 1;
    cfg.block_size = 64;

    float d_k = 10.0f;
    ConANN conann = ConANN::FromConfig(cfg, 64, d_k);

    float expected_eps = 5.75f * std::pow(2.0f, -0.5f) / std::sqrt(64.0f);
    EXPECT_NEAR(conann.epsilon(), expected_eps, 1e-4f);
    EXPECT_FLOAT_EQ(conann.d_k(), 10.0f);
    EXPECT_NEAR(conann.tau_in(), d_k - 2.0f * expected_eps, 1e-4f);
    EXPECT_NEAR(conann.tau_out(), d_k + 2.0f * expected_eps, 1e-4f);
}

TEST(ConANNTest, FromConfig_DifferentBits) {
    // bits=2 → 2^(-1) = 0.5
    // epsilon = 5.75 * 0.5 / sqrt(128) ≈ 0.254
    RaBitQConfig cfg;
    cfg.c_factor = 5.75f;
    cfg.bits = 2;

    float d_k = 20.0f;
    ConANN conann = ConANN::FromConfig(cfg, 128, d_k);

    float expected_eps = 5.75f * std::pow(2.0f, -1.0f) / std::sqrt(128.0f);
    EXPECT_NEAR(conann.epsilon(), expected_eps, 1e-4f);
}

// ============================================================================
// ConANN::Classify — three-way classification
// ============================================================================

TEST(ConANNTest, Classify_SafeIn) {
    // epsilon=0.2, d_k=1.0 → tau_in=0.6, tau_out=1.4
    ConANN conann(0.2f, 1.0f);
    EXPECT_EQ(conann.tau_in(), 0.6f);

    // approx_dist=0.5 < 0.6 → SafeIn
    EXPECT_EQ(conann.Classify(0.5f), ResultClass::SafeIn);
    // approx_dist=0.0 → SafeIn
    EXPECT_EQ(conann.Classify(0.0f), ResultClass::SafeIn);
    // approx_dist=0.3 → SafeIn
    EXPECT_EQ(conann.Classify(0.3f), ResultClass::SafeIn);
}

TEST(ConANNTest, Classify_SafeOut) {
    // epsilon=0.2, d_k=1.0 → tau_in=0.6, tau_out=1.4
    ConANN conann(0.2f, 1.0f);
    EXPECT_EQ(conann.tau_out(), 1.4f);

    // approx_dist=1.5 > 1.4 → SafeOut
    EXPECT_EQ(conann.Classify(1.5f), ResultClass::SafeOut);
    // approx_dist=2.0 → SafeOut
    EXPECT_EQ(conann.Classify(2.0f), ResultClass::SafeOut);
    // approx_dist=100.0 → SafeOut
    EXPECT_EQ(conann.Classify(100.0f), ResultClass::SafeOut);
}

TEST(ConANNTest, Classify_Uncertain) {
    // epsilon=0.2, d_k=1.0 → tau_in=0.6, tau_out=1.4
    ConANN conann(0.2f, 1.0f);

    // approx_dist=1.0 ∈ [0.6, 1.4] → Uncertain
    EXPECT_EQ(conann.Classify(1.0f), ResultClass::Uncertain);
    // approx_dist=0.8 → Uncertain
    EXPECT_EQ(conann.Classify(0.8f), ResultClass::Uncertain);
    // approx_dist=1.2 → Uncertain
    EXPECT_EQ(conann.Classify(1.2f), ResultClass::Uncertain);
}

TEST(ConANNTest, Classify_BoundaryExact) {
    // Test exact boundary values: they should be Uncertain
    // epsilon=0.2, d_k=1.0 → tau_in=0.6, tau_out=1.4
    ConANN conann(0.2f, 1.0f);

    // approx_dist exactly equals tau_in → NOT < tau_in → Uncertain
    EXPECT_EQ(conann.Classify(0.6f), ResultClass::Uncertain);
    // approx_dist exactly equals tau_out → NOT > tau_out → Uncertain
    EXPECT_EQ(conann.Classify(1.4f), ResultClass::Uncertain);
}

TEST(ConANNTest, Classify_ZeroEpsilon) {
    // epsilon=0 → tau_in = d_k, tau_out = d_k
    // Everything except approx_dist < d_k is Uncertain or SafeOut
    ConANN conann(0.0f, 5.0f);

    EXPECT_EQ(conann.tau_in(), 5.0f);
    EXPECT_EQ(conann.tau_out(), 5.0f);

    // dist < d_k → SafeIn
    EXPECT_EQ(conann.Classify(4.9f), ResultClass::SafeIn);
    // dist == d_k → NOT < AND NOT > → Uncertain
    EXPECT_EQ(conann.Classify(5.0f), ResultClass::Uncertain);
    // dist > d_k → SafeOut
    EXPECT_EQ(conann.Classify(5.1f), ResultClass::SafeOut);
}

TEST(ConANNTest, Classify_LargeEpsilon) {
    // Very large epsilon → tau_in goes negative, tau_out goes very high
    // Almost everything classified as Uncertain
    ConANN conann(100.0f, 1.0f);

    EXPECT_LT(conann.tau_in(), 0.0f);  // tau_in = 1 − 200 = −199
    EXPECT_GT(conann.tau_out(), 100.0f);

    // Only negative distances would be SafeIn (not physically meaningful)
    EXPECT_EQ(conann.Classify(0.0f), ResultClass::Uncertain);
    EXPECT_EQ(conann.Classify(50.0f), ResultClass::Uncertain);
    EXPECT_EQ(conann.Classify(200.0f), ResultClass::Uncertain);
    // tau_out = 1 + 200 = 201, so 201.0 is exactly on boundary → Uncertain
    EXPECT_EQ(conann.Classify(201.0f), ResultClass::Uncertain);
    // Strictly greater than tau_out → SafeOut
    EXPECT_EQ(conann.Classify(201.1f), ResultClass::SafeOut);
}

TEST(ConANNTest, ClassifyAdaptive_UsesSafeInDkAndDynamicSafeOut) {
    ConANN conann(/*epsilon=*/0.1f, /*legacy_d_k=*/1.0f,
                  /*safein_d_k=*/1.3f, /*has_safein_d_k=*/true);

    EXPECT_TRUE(std::isinf(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/1.15f,
                                      /*margin=*/0.05f,
                                      std::numeric_limits<float>::infinity()),
              ResultClass::SafeIn);
    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/1.15f,
                                      /*margin=*/0.05f,
                                      /*dynamic_d_k=*/1.0f),
              ResultClass::SafeOut);
}

TEST(ConANNTest, SafeInFallback_UsesLegacyDkWhenExplicitValueMissing) {
    ConANN conann(/*epsilon=*/0.1f, /*legacy_d_k=*/1.0f,
                  /*safein_d_k=*/0.0f, /*has_safein_d_k=*/false);
    EXPECT_FALSE(conann.has_safein_d_k());
    EXPECT_FLOAT_EQ(conann.safein_d_k(), conann.legacy_d_k());
    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/0.75f,
                                      /*margin=*/0.1f,
                                      std::numeric_limits<float>::infinity()),
              ResultClass::SafeIn);
}

// ============================================================================
// ConANN::CalibrateDistanceThreshold
// ============================================================================

TEST(ConANNTest, CalibrateDistanceThreshold_Uniform) {
    // Generate 200 random vectors, dim=16
    const uint32_t N = 200;
    const Dim dim = 16;
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> vectors(N * dim);
    for (auto& v : vectors) v = dist(rng);

    float d_k = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim,
        /*num_samples=*/50,
        /*top_k=*/5,
        /*percentile=*/0.99f,
        /*seed=*/42);

    // d_k should be a positive finite number
    EXPECT_GT(d_k, 0.0f);
    EXPECT_TRUE(std::isfinite(d_k));
}

TEST(ConANNTest, CalibrateDistanceThreshold_Deterministic) {
    const uint32_t N = 100;
    const Dim dim = 8;
    std::mt19937 rng(999);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> vectors(N * dim);
    for (auto& v : vectors) v = dist(rng);

    float d_k_1 = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 30, 3, 0.95f, 42);
    float d_k_2 = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 30, 3, 0.95f, 42);

    // Same seed → same result
    EXPECT_FLOAT_EQ(d_k_1, d_k_2);
}

TEST(ConANNTest, CalibrateDistanceThreshold_DifferentSeeds) {
    const uint32_t N = 200;
    const Dim dim = 16;
    std::mt19937 rng(111);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> vectors(N * dim);
    for (auto& v : vectors) v = dist(rng);

    float d_k_1 = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 50, 5, 0.99f, 42);
    float d_k_2 = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 50, 5, 0.99f, 123);

    // Different seeds → likely different results (not guaranteed, but very likely)
    // Both should be positive
    EXPECT_GT(d_k_1, 0.0f);
    EXPECT_GT(d_k_2, 0.0f);
}

TEST(ConANNTest, CalibrateDistanceThreshold_HigherPercentileGivesLargerDk) {
    const uint32_t N = 500;
    const Dim dim = 16;
    std::mt19937 rng(777);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> vectors(N * dim);
    for (auto& v : vectors) v = dist(rng);

    float d_k_50 = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 100, 10, 0.50f, 42);
    float d_k_99 = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 100, 10, 0.99f, 42);

    // ConANN percentile takes a lower sample quantile:
    // larger percentile parameter => smaller calibrated d_k.
    EXPECT_LE(d_k_99, d_k_50);
}

TEST(ConANNTest, CalibrateDistanceThreshold_EdgeCase_SmallN) {
    // N < num_samples: should clamp samples to N
    const uint32_t N = 5;
    const Dim dim = 4;
    std::vector<float> vectors(N * dim);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& v : vectors) v = dist(rng);

    float d_k = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 100, 3, 0.99f, 42);

    EXPECT_GT(d_k, 0.0f);
    EXPECT_TRUE(std::isfinite(d_k));
}

TEST(ConANNTest, CalibrateDistanceThreshold_EdgeCase_TopKLargerThanN) {
    // top_k > N: should clamp to N
    const uint32_t N = 10;
    const Dim dim = 4;
    std::vector<float> vectors(N * dim);
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& v : vectors) v = dist(rng);

    float d_k = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 5, 100, 0.99f, 42);

    EXPECT_GE(d_k, 0.0f);
    EXPECT_TRUE(std::isfinite(d_k));
}

TEST(ConANNTest, CalibrateDistanceThreshold_EdgeCase_Empty) {
    // N=0 → should return 0
    float d_k = ConANN::CalibrateDistanceThreshold(
        nullptr, 0, 4, 50, 5, 0.99f, 42);
    EXPECT_FLOAT_EQ(d_k, 0.0f);
}

// ============================================================================
// ConANN::ClassifyAdaptive — dynamic SafeOut threshold
// ============================================================================

TEST(ConANNTest, ClassifyAdaptive_DynamicSafeOut) {
    // epsilon=0.2, d_k=1.0
    // Classify(margin=0): SafeOut > 1.0 + 0 = 1.0, SafeIn < 1.0 - 0 = 1.0
    ConANN conann(0.0f, 1.0f);
    float margin = 0.0f;

    // With dynamic_d_k = 0.5 (tighter than static d_k=1.0):
    // SafeOut: dist > 0.5 → more SafeOut
    EXPECT_EQ(conann.ClassifyAdaptive(0.6f, margin, 0.5f), ResultClass::SafeOut);
    // Same dist with static Classify would NOT be SafeOut (0.6 < 1.0)
    EXPECT_NE(conann.Classify(0.6f, margin), ResultClass::SafeOut);
}

TEST(ConANNTest, ClassifyAdaptive_SafeInUsesStaticDk) {
    // d_k=1.0, margin=0
    ConANN conann(0.0f, 1.0f);
    float margin = 0.0f;

    // SafeIn threshold is ALWAYS static d_k_ - 2*margin = 1.0
    // dynamic_d_k = 0.5: SafeOut if dist > 0.5, so pick dist below both thresholds
    // dist=0.4 < 0.5 → not SafeOut; dist=0.4 < 1.0 → SafeIn
    EXPECT_EQ(conann.ClassifyAdaptive(0.4f, margin, 0.5f), ResultClass::SafeIn);
    // Same with static Classify
    EXPECT_EQ(conann.Classify(0.4f, margin), ResultClass::SafeIn);

    // dist=0.9 > dynamic_d_k=0.5 → SafeOut (dynamic threshold wins)
    // But with static Classify: 0.9 < d_k=1.0 → SafeIn
    EXPECT_EQ(conann.ClassifyAdaptive(0.9f, margin, 0.5f), ResultClass::SafeOut);
    EXPECT_EQ(conann.Classify(0.9f, margin), ResultClass::SafeIn);
}

TEST(ConANNTest, ClassifyAdaptive_DynamicDkLargerThanStatic) {
    // dynamic_d_k > static d_k → SafeOut threshold is HIGHER → fewer SafeOut
    ConANN conann(0.0f, 1.0f);
    float margin = 0.0f;

    // dist=1.5, dynamic_d_k=2.0 → 1.5 < 2.0 → NOT SafeOut
    EXPECT_NE(conann.ClassifyAdaptive(1.5f, margin, 2.0f), ResultClass::SafeOut);
    // But with static d_k=1.0 → 1.5 > 1.0 → SafeOut
    EXPECT_EQ(conann.Classify(1.5f, margin), ResultClass::SafeOut);
}

TEST(ConANNTest, ClassifyAdaptive_WithMargin) {
    ConANN conann(0.5f, 10.0f);
    float margin = 1.0f;  // 2*margin = 2.0
    float dynamic_d_k = 5.0f;

    // SafeOut threshold: 5.0 + 2.0 = 7.0
    // SafeIn threshold:  10.0 - 2.0 = 8.0 (static)
    // Note: SafeOut checked first, so overlap zone [7.0, 8.0] → SafeOut wins

    // dist=7.1 > 7.0 → SafeOut
    EXPECT_EQ(conann.ClassifyAdaptive(7.1f, margin, dynamic_d_k), ResultClass::SafeOut);
    // dist=7.0 → NOT > 7.0 → check SafeIn: 7.0 < 8.0 → SafeIn
    EXPECT_EQ(conann.ClassifyAdaptive(7.0f, margin, dynamic_d_k), ResultClass::SafeIn);

    // dist=4.0 < 7.0 and < 8.0 → SafeIn
    EXPECT_EQ(conann.ClassifyAdaptive(4.0f, margin, dynamic_d_k), ResultClass::SafeIn);

    // Now use dynamic_d_k=9.0 (close to static d_k=10.0, no overlap)
    // SafeOut threshold: 9.0 + 2.0 = 11.0
    // SafeIn threshold:  10.0 - 2.0 = 8.0
    // Uncertain zone: [8.0, 11.0]
    EXPECT_EQ(conann.ClassifyAdaptive(9.0f, 1.0f, 9.0f), ResultClass::Uncertain);
    EXPECT_EQ(conann.ClassifyAdaptive(7.9f, 1.0f, 9.0f), ResultClass::SafeIn);
    EXPECT_EQ(conann.ClassifyAdaptive(11.1f, 1.0f, 9.0f), ResultClass::SafeOut);
}

TEST(ConANNTest, ClassifyAdaptive_MoreSafeOutWithTighterDk) {
    // Verify that lowering dynamic_d_k increases SafeOut count
    ConANN conann(0.0f, 10.0f);
    float margin = 0.5f;
    // SafeOut threshold = ddk + 1.0, SafeIn threshold = 10.0 - 1.0 = 9.0

    std::vector<float> test_dists = {2, 4, 6, 8, 10, 12, 14, 16};
    auto count_safeout = [&](float ddk) {
        int count = 0;
        for (float d : test_dists) {
            if (conann.ClassifyAdaptive(d, margin, ddk) == ResultClass::SafeOut)
                count++;
        }
        return count;
    };

    // ddk=10.0 → SafeOut > 11.0 → {12,14,16} = 3
    // ddk=5.0  → SafeOut > 6.0  → {8,10,12,14,16} = 5 (and some SafeIn become SafeOut)
    int so_high = count_safeout(10.0f);
    int so_low = count_safeout(5.0f);
    EXPECT_GT(so_low, so_high);

    // With no overlap (ddk close to d_k), SafeIn count is stable
    // ddk=10.0: SafeIn < 9.0 → {2,4,6,8} = 4
    // ddk=9.0:  SafeOut > 10.0 → {12,14,16}; SafeIn < 9.0 → {2,4,6,8} = 4
    auto count_safein = [&](float ddk) {
        int count = 0;
        for (float d : test_dists) {
            if (conann.ClassifyAdaptive(d, margin, ddk) == ResultClass::SafeIn)
                count++;
        }
        return count;
    };
    EXPECT_EQ(count_safein(10.0f), count_safein(9.0f));
}

// ============================================================================
// Integration: FromConfig + CalibrateDistanceThreshold → Classify
// ============================================================================

TEST(ConANNTest, EndToEnd_CalibrateAndClassify) {
    // Generate a small dataset, calibrate d_k, build ConANN, classify
    const uint32_t N = 300;
    const Dim dim = 32;
    std::mt19937 rng(42);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> vectors(N * dim);
    for (auto& v : vectors) v = dist(rng);

    // Calibrate
    float d_k = ConANN::CalibrateDistanceThreshold(
        vectors.data(), N, dim, 50, 5, 0.95f, 42);

    // Build ConANN
    RaBitQConfig cfg;
    cfg.c_factor = 5.75f;
    cfg.bits = 1;
    ConANN conann = ConANN::FromConfig(cfg, dim, d_k);

    // Verify thresholds are reasonable
    EXPECT_GT(conann.tau_in(), 0.0f);  // d_k - 2ε should be positive for reasonable data
    EXPECT_GT(conann.tau_out(), conann.tau_in());
    EXPECT_GT(conann.epsilon(), 0.0f);

    // Classify with various distances
    // Very small distance → SafeIn
    if (conann.tau_in() > 0.0f) {
        EXPECT_EQ(conann.Classify(0.0f), ResultClass::SafeIn);
    }
    // Very large distance → SafeOut
    EXPECT_EQ(conann.Classify(conann.tau_out() + 100.0f), ResultClass::SafeOut);
    // d_k itself → Uncertain (since d_k is between tau_in and tau_out)
    EXPECT_EQ(conann.Classify(d_k), ResultClass::Uncertain);
}
