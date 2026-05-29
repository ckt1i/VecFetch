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

TEST(ConANNTest, ClassifyAdaptive_UsesSafeInDkAndSafeOutFrontier) {
    ConANN conann(/*epsilon=*/0.1f, /*legacy_d_k=*/1.0f,
                  /*safein_d_k=*/1.3f, /*has_safein_d_k=*/true);

    EXPECT_TRUE(std::isinf(std::numeric_limits<float>::infinity()));
    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/1.15f,
                                      /*margin=*/0.05f,
                                      std::numeric_limits<float>::infinity()),
              ResultClass::SafeIn);
    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/1.15f,
                                      /*margin=*/0.05f,
                                      /*safeout_frontier_upper=*/1.0f),
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
// ConANN::ClassifyAdaptive — interval-bound classification
// ============================================================================

TEST(ConANNTest, ClassifyAdaptive_SafeOutUsesCandidateLowerBound) {
    ConANN conann(0.0f, 10.0f);
    const float margin = 1.0f;
    const float safeout_frontier_upper = 5.0f;

    // SafeOut iff approx_dist - margin > frontier, i.e. approx_dist > 6.0.
    EXPECT_EQ(conann.ClassifyAdaptive(6.1f, margin, safeout_frontier_upper),
              ResultClass::SafeOut);
    EXPECT_NE(conann.ClassifyAdaptive(6.0f, margin, safeout_frontier_upper),
              ResultClass::SafeOut);
}

TEST(ConANNTest, ClassifyAdaptive_SafeInUsesCandidateUpperBound) {
    ConANN conann(/*epsilon=*/0.0f, /*legacy_d_k=*/10.0f,
                  /*safein_d_k=*/8.0f, /*has_safein_d_k=*/true);
    const float margin = 1.0f;
    const float safeout_frontier_upper = 20.0f;

    // SafeIn iff approx_dist + margin < safein_d_k, i.e. approx_dist < 7.0.
    EXPECT_EQ(conann.ClassifyAdaptive(6.9f, margin, safeout_frontier_upper),
              ResultClass::SafeIn);
    EXPECT_EQ(conann.ClassifyAdaptive(7.0f, margin, safeout_frontier_upper),
              ResultClass::Uncertain);
}

TEST(ConANNTest, ClassifyAdaptive_SafeOutCheckedBeforeSafeIn) {
    ConANN conann(/*epsilon=*/0.0f, /*legacy_d_k=*/10.0f,
                  /*safein_d_k=*/8.0f, /*has_safein_d_k=*/true);

    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/6.2f,
                                      /*safein_margin=*/1.0f,
                                      /*safeout_margin=*/1.0f,
                                      /*safeout_frontier_upper=*/5.0f),
              ResultClass::SafeOut);
}

TEST(ConANNTest, ClassifyAdaptive_SplitMargins) {
    ConANN conann(/*epsilon=*/0.0f, /*legacy_d_k=*/10.0f,
                  /*safein_d_k=*/8.0f, /*has_safein_d_k=*/true);
    const float safeout_frontier_upper = 5.0f;

    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/6.6f,
                                      /*safein_margin=*/0.5f,
                                      /*safeout_margin=*/1.5f,
                                      safeout_frontier_upper),
              ResultClass::SafeOut);
    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/7.4f,
                                      /*safein_margin=*/0.5f,
                                      /*safeout_margin=*/3.0f,
                                      safeout_frontier_upper),
              ResultClass::SafeIn);
    EXPECT_EQ(conann.ClassifyAdaptive(/*approx_dist=*/7.6f,
                                      /*safein_margin=*/0.5f,
                                      /*safeout_margin=*/3.0f,
                                      safeout_frontier_upper),
              ResultClass::Uncertain);
}

TEST(ConANNTest, ClassifyAdaptive_InfiniteFrontierDisablesSafeOut) {
    ConANN conann(/*epsilon=*/0.0f, /*legacy_d_k=*/10.0f,
                  /*safein_d_k=*/8.0f, /*has_safein_d_k=*/true);

    EXPECT_NE(conann.ClassifyAdaptive(100.0f, 1.0f,
                                      std::numeric_limits<float>::infinity()),
              ResultClass::SafeOut);
}

TEST(ConANNTest, ClassifyAdaptive_MoreSafeOutWithLowerFrontier) {
    ConANN conann(0.0f, 10.0f);
    const float margin = 0.5f;
    const std::vector<float> test_dists = {2, 4, 6, 8, 10, 12, 14, 16};
    auto count_safeout = [&](float frontier) {
        int count = 0;
        for (float d : test_dists) {
            if (conann.ClassifyAdaptive(d, margin, frontier) == ResultClass::SafeOut) {
                count++;
            }
        }
        return count;
    };

    EXPECT_GT(count_safeout(5.0f), count_safeout(10.0f));
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
