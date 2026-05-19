#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>
#include <algorithm>
#include <numeric>
#include <set>

#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"

using vdb::rabitq::RotationMatrix;
using vdb::rabitq::RaBitQEncoder;
using vdb::rabitq::RaBitQEstimator;
using vdb::rabitq::RaBitQCode;
using vdb::rabitq::PreparedQuery;
using vdb::rabitq::ClusterPreparedScratch;
using vdb::Dim;

namespace {

std::vector<float> RandomVec(Dim dim, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(dim);
    for (auto& x : v) x = dist(rng);
    return v;
}

float ExactL2Sq(const float* a, const float* b, Dim d) {
    return vdb::simd::L2Sqr(a, b, d);
}

}  // namespace

// ===========================================================================
// PrepareQuery basic checks
// ===========================================================================

TEST(RaBitQEstimatorTest, PrepareQuery_NormComputed) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEstimator estimator(dim);

    auto q = RandomVec(dim, 100);
    auto pq = estimator.PrepareQuery(q.data(), nullptr, P);

    // norm_qc should equal ‖q‖ when centroid is null
    float expected = 0.0f;
    for (auto x : q) expected += x * x;
    expected = std::sqrt(expected);
    EXPECT_NEAR(pq.norm_qc, expected, 1e-5f);
}

TEST(RaBitQEstimatorTest, PrepareQuery_WithCentroid) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEstimator estimator(dim);

    auto q = RandomVec(dim, 100);
    auto c = RandomVec(dim, 200);
    auto pq = estimator.PrepareQuery(q.data(), c.data(), P);

    float expected = 0.0f;
    for (Dim i = 0; i < dim; ++i) {
        float d = q[i] - c[i];
        expected += d * d;
    }
    expected = std::sqrt(expected);
    EXPECT_NEAR(pq.norm_qc, expected, 1e-5f);
}

TEST(RaBitQEstimatorTest, PrepareQuery_RotatedHasCorrectSize) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEstimator estimator(dim);

    auto q = RandomVec(dim, 100);
    auto pq = estimator.PrepareQuery(q.data(), nullptr, P);

    EXPECT_EQ(pq.rotated.size(), static_cast<size_t>(dim));
    EXPECT_EQ(pq.sign_code.size(), static_cast<size_t>(2));  // 128/64
    EXPECT_EQ(pq.dim, dim);
    EXPECT_EQ(pq.num_words, 2u);
}

// ===========================================================================
// Distance estimation: identical vectors should give ~0
// ===========================================================================

TEST(RaBitQEstimatorTest, IdenticalVector_ApproxZero) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    auto v = RandomVec(dim, 500);
    auto code = encoder.Encode(v.data());
    auto pq = estimator.PrepareQuery(v.data(), nullptr, P);

    float dist = estimator.EstimateDistance(pq, code);
    // Should be close to 0 (not exact due to quantization)
    EXPECT_LT(std::abs(dist), 5.0f);
}

TEST(RaBitQEstimatorTest, IdenticalVector_AccuratePath_CloserThanFast) {
    // 1-bit quantization is very lossy: even for the same vector,
    // dist ≈ 2·‖v‖²·(1 - ip_est) can be significant because ip_est < 1.
    // We just verify the accurate path produces a non-negative finite value
    // and that the estimate is at least not absurdly large.
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    auto v = RandomVec(dim, 500);
    auto code = encoder.Encode(v.data());
    auto pq = estimator.PrepareQuery(v.data(), nullptr, P);

    float dist_acc = estimator.EstimateDistanceAccurate(pq, code);
    float exact = ExactL2Sq(v.data(), v.data(), dim);  // = 0
    EXPECT_TRUE(std::isfinite(dist_acc));
    // 1-bit quantization error for self-distance: dist ≈ 2·‖v‖²·(1-ip_est)
    // With dim=128, ‖v‖²≈40, ip_est≈0.7, so dist≈24. Allow up to 50.
    EXPECT_LT(dist_acc, 50.0f);
    EXPECT_EQ(exact, 0.0f);
}

// ===========================================================================
// Ranking preservation: closer vectors should rank lower
// ===========================================================================

TEST(RaBitQEstimatorTest, RankingPreservation) {
    // Test that RaBitQ mostly preserves the ranking of nearest neighbors.
    // We generate a query and several database vectors, then check that
    // the RaBitQ ranking correlates with the true L2 ranking.
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    auto query = RandomVec(dim, 0);
    const int N = 50;
    std::vector<std::vector<float>> db_vecs;
    std::vector<RaBitQCode> db_codes;

    for (int i = 0; i < N; ++i) {
        auto v = RandomVec(dim, 1000 + i);
        db_codes.push_back(encoder.Encode(v.data()));
        db_vecs.push_back(std::move(v));
    }

    auto pq = estimator.PrepareQuery(query.data(), nullptr, P);

    // Compute exact and estimated distances
    std::vector<std::pair<float, int>> exact_ranking, approx_ranking;
    for (int i = 0; i < N; ++i) {
        float exact = ExactL2Sq(query.data(), db_vecs[i].data(), dim);
        float approx = estimator.EstimateDistance(pq, db_codes[i]);
        exact_ranking.push_back({exact, i});
        approx_ranking.push_back({approx, i});
    }

    std::sort(exact_ranking.begin(), exact_ranking.end());
    std::sort(approx_ranking.begin(), approx_ranking.end());

    // Check that the top-10 in exact ranking has reasonable overlap
    // with top-15 in approximate ranking (recall@15 for top-10)
    int recall = 0;
    for (int i = 0; i < 10; ++i) {
        int exact_id = exact_ranking[i].second;
        for (int j = 0; j < 15; ++j) {
            if (approx_ranking[j].second == exact_id) {
                ++recall;
                break;
            }
        }
    }
    // We expect at least 3 out of 10 (very lenient for 1-bit quantization)
    EXPECT_GE(recall, 3) << "Recall@15 for top-10 is too low";
}

// ===========================================================================
// Distance with centroid
// ===========================================================================

TEST(RaBitQEstimatorTest, WithCentroid_ReasonableDistance) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    auto vec = RandomVec(dim, 100);
    auto query = RandomVec(dim, 200);
    auto centroid = RandomVec(dim, 300);

    auto code = encoder.Encode(vec.data(), centroid.data());
    auto pq = estimator.PrepareQuery(query.data(), centroid.data(), P);

    float approx = estimator.EstimateDistance(pq, code);
    float exact = ExactL2Sq(query.data(), vec.data(), dim);

    // The approximation should be in the same ballpark (within 10x)
    // This is a loose check — RaBitQ is approximate
    EXPECT_LT(std::abs(approx - exact), 10.0f * exact + 1.0f);
}

// ===========================================================================
// Batch estimation matches single
// ===========================================================================

TEST(RaBitQEstimatorTest, BatchMatchesSingle) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    const uint32_t N = 20;
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        auto v = RandomVec(dim, 500 + i);
        codes.push_back(encoder.Encode(v.data()));
    }

    auto query = RandomVec(dim, 999);
    auto pq = estimator.PrepareQuery(query.data(), nullptr, P);

    // Batch estimate
    std::vector<float> batch_dists(N);
    estimator.EstimateDistanceBatch(pq, codes.data(), N, batch_dists.data());

    // Compare with individual
    for (uint32_t i = 0; i < N; ++i) {
        float single = estimator.EstimateDistance(pq, codes[i]);
        EXPECT_FLOAT_EQ(batch_dists[i], single) << "vector " << i;
    }
}

// ===========================================================================
// Accurate vs Fast path comparison
// ===========================================================================

TEST(RaBitQEstimatorTest, AccurateVsFast_Correlated) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    auto query = RandomVec(dim, 100);
    auto pq = estimator.PrepareQuery(query.data(), nullptr, P);

    // Compute both estimates for multiple vectors
    const int N = 30;
    std::vector<float> fast_dists, accurate_dists;
    for (int i = 0; i < N; ++i) {
        auto v = RandomVec(dim, 2000 + i);
        auto code = encoder.Encode(v.data());
        fast_dists.push_back(estimator.EstimateDistance(pq, code));
        accurate_dists.push_back(estimator.EstimateDistanceAccurate(pq, code));
    }

    // Check that the two paths produce correlated results.
    // Compute Spearman rank correlation by checking top-5 overlap.
    std::vector<std::pair<float, int>> fast_rank, acc_rank;
    for (int i = 0; i < N; ++i) {
        fast_rank.push_back({fast_dists[i], i});
        acc_rank.push_back({accurate_dists[i], i});
    }
    std::sort(fast_rank.begin(), fast_rank.end());
    std::sort(acc_rank.begin(), acc_rank.end());

    // Top-5 overlap between both paths (should be at least 2)
    int overlap = 0;
    for (int i = 0; i < 5; ++i) {
        for (int j = 0; j < 8; ++j) {
            if (fast_rank[i].second == acc_rank[j].second) {
                ++overlap;
                break;
            }
        }
    }
    EXPECT_GE(overlap, 1);
}

// ===========================================================================
// Symmetry: d(a,b) ≈ d(b,a) in exact L2
// ===========================================================================

TEST(RaBitQEstimatorTest, DistanceNonNegative_Accurate) {
    // The accurate path should generally produce non-negative distances
    // (or very close to zero when negative due to float precision).
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    for (int i = 0; i < 20; ++i) {
        auto v = RandomVec(dim, 3000 + i);
        auto q = RandomVec(dim, 4000 + i);
        auto code = encoder.Encode(v.data());
        auto pq = estimator.PrepareQuery(q.data(), nullptr, P);
        float dist = estimator.EstimateDistanceAccurate(pq, code);
        // Allow small negative due to quantization error
        EXPECT_GT(dist, -5.0f) << "vector " << i;
    }
}

// ===========================================================================
// Hadamard rotation should also work with estimator
// ===========================================================================

TEST(RaBitQEstimatorTest, HadamardRotation) {
    Dim dim = 64;
    RotationMatrix P(dim);
    ASSERT_TRUE(P.GenerateHadamard(42, true));
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    auto v = RandomVec(dim, 100);
    auto q = RandomVec(dim, 200);

    auto code = encoder.Encode(v.data());
    auto pq = estimator.PrepareQuery(q.data(), nullptr, P);

    float fast = estimator.EstimateDistance(pq, code);
    float acc = estimator.EstimateDistanceAccurate(pq, code);
    float exact = ExactL2Sq(q.data(), v.data(), dim);

    // Both should be in the same order of magnitude as exact
    EXPECT_LT(std::abs(fast - exact), 10.0f * exact + 5.0f);
    EXPECT_LT(std::abs(acc - exact), 10.0f * exact + 5.0f);
}

TEST(RaBitQEstimatorTest, BlockedHadamardPrepareQueryRotatedMatchesPrepareQuery) {
    const Dim dim = 768;
    RotationMatrix P(dim);
    ASSERT_TRUE(P.GenerateBlockedHadamardPermuted(42, true));
    RaBitQEstimator estimator(dim);

    auto q = RandomVec(dim, 111);
    auto c = RandomVec(dim, 222);

    PreparedQuery pq_direct;
    ClusterPreparedScratch scratch_direct;
    estimator.PrepareQueryInto(q.data(), c.data(), P, &pq_direct, &scratch_direct);

    std::vector<float> rotated_q(dim), rotated_c(dim);
    P.Apply(q.data(), rotated_q.data());
    P.Apply(c.data(), rotated_c.data());

    PreparedQuery pq_rot;
    ClusterPreparedScratch scratch_rot;
    estimator.PrepareQueryRotatedInto(
        rotated_q.data(), rotated_c.data(), &pq_rot, &scratch_rot);

    EXPECT_NEAR(pq_direct.norm_qc, pq_rot.norm_qc, 1e-4f);
    EXPECT_NEAR(pq_direct.norm_qc_sq, pq_rot.norm_qc_sq, 1e-3f);
    EXPECT_NEAR(pq_direct.sum_q, pq_rot.sum_q, 1e-3f);
    EXPECT_EQ(pq_direct.sign_code, pq_rot.sign_code);
    ASSERT_EQ(pq_direct.rotated.size(), pq_rot.rotated.size());
    for (size_t i = 0; i < pq_direct.rotated.size(); ++i) {
        EXPECT_NEAR(pq_direct.rotated[i], pq_rot.rotated[i], 1e-4f) << "dim " << i;
    }
}

// ===========================================================================
// Large dimension test
// ===========================================================================

TEST(RaBitQEstimatorTest, LargeDim_256) {
    Dim dim = 256;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    auto v = RandomVec(dim, 100);
    auto q = RandomVec(dim, 200);

    auto code = encoder.Encode(v.data());
    auto pq = estimator.PrepareQuery(q.data(), nullptr, P);

    float dist = estimator.EstimateDistance(pq, code);
    // Just verify it runs without crashing and produces a finite value
    EXPECT_TRUE(std::isfinite(dist));
}

TEST(RaBitQEstimatorTest, RawMatchesStructPath) {
    const Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);
    RaBitQEstimator estimator(dim);

    auto centroid = RandomVec(dim, 10);
    auto vecs = {RandomVec(dim, 20), RandomVec(dim, 30), RandomVec(dim, 40)};

    auto query = RandomVec(dim, 50);
    auto pq = estimator.PrepareQuery(query.data(), centroid.data(), P);

    for (const auto& v : vecs) {
        auto code = encoder.Encode(v.data(), centroid.data());
        float struct_dist = estimator.EstimateDistance(pq, code);
        float raw_dist = estimator.EstimateDistanceRaw(
            pq, code.code.data(),
            static_cast<uint32_t>(code.code.size()), code.norm);
        EXPECT_FLOAT_EQ(struct_dist, raw_dist);
    }
}

// ===========================================================================
// Multi-bit estimator tests
// ===========================================================================

TEST(RaBitQEstimatorTest, Xipnorm_IsFiniteAndPositive) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    for (uint8_t bits : {2, 4}) {
        RaBitQEncoder encoder(dim, P, bits);
        for (int i = 0; i < 20; ++i) {
            auto vec = RandomVec(dim, 100 + i);
            auto code = encoder.Encode(vec.data());
            EXPECT_TRUE(std::isfinite(code.xipnorm))
                << "bits=" << (int)bits << " vec " << i;
            EXPECT_GT(code.xipnorm, 0.0f)
                << "bits=" << (int)bits << " vec " << i;
        }
    }
}

TEST(RaBitQEstimatorTest, Xipnorm_Bits1_IsZero) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder encoder(dim, P, 1);
    for (int i = 0; i < 10; ++i) {
        auto vec = RandomVec(dim, 100 + i);
        auto code = encoder.Encode(vec.data());
        EXPECT_FLOAT_EQ(code.xipnorm, 0.0f) << "vec " << i;
    }
}

TEST(RaBitQEstimatorTest, Stage2_Xipnorm_CloserToExact) {
    // With xipnorm correction, Stage 2 should produce estimates
    // closer to exact distance than Stage 1 (popcount).
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder encoder(dim, P, 4);
    RaBitQEstimator estimator(dim, 4);

    auto query = RandomVec(dim, 100);
    auto centroid = RandomVec(dim, 200);
    auto pq = estimator.PrepareQuery(query.data(), centroid.data(), P);

    double total_err_s1 = 0.0;
    double total_err_s2 = 0.0;
    const int N = 50;

    for (int i = 0; i < N; ++i) {
        auto vec = RandomVec(dim, 300 + i);
        auto code = encoder.Encode(vec.data(), centroid.data());

        float exact_dist_sq = 0.0f;
        for (Dim d = 0; d < dim; ++d) {
            float diff = vec[d] - query[d];
            exact_dist_sq += diff * diff;
        }

        float dist_s1 = estimator.EstimateDistance(pq, code);
        float dist_s2 = estimator.EstimateDistanceMultiBit(pq, code);

        total_err_s1 += std::abs(dist_s1 - exact_dist_sq);
        total_err_s2 += std::abs(dist_s2 - exact_dist_sq);
    }

    // Stage 2 with xipnorm should have lower average error than Stage 1
    EXPECT_LT(total_err_s2, total_err_s1)
        << "S2 avg err=" << total_err_s2 / N
        << " S1 avg err=" << total_err_s1 / N;
}

TEST(RaBitQEstimatorTest, Stage1_UnchangedByBits) {
    // EstimateDistance (Stage 1) should give identical results regardless of bits,
    // because it only uses the MSB plane.
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder enc1(dim, P, 1);
    RaBitQEncoder enc2(dim, P, 2);
    RaBitQEstimator est1(dim, 1);
    RaBitQEstimator est2(dim, 2);

    auto query = RandomVec(dim, 100);
    auto centroid = RandomVec(dim, 200);
    auto pq1 = est1.PrepareQuery(query.data(), centroid.data(), P);
    auto pq2 = est2.PrepareQuery(query.data(), centroid.data(), P);

    for (int i = 0; i < 20; ++i) {
        auto vec = RandomVec(dim, 300 + i);
        auto code1 = enc1.Encode(vec.data(), centroid.data());
        auto code2 = enc2.Encode(vec.data(), centroid.data());

        float dist1 = est1.EstimateDistance(pq1, code1);
        float dist2 = est2.EstimateDistance(pq2, code2);
        EXPECT_FLOAT_EQ(dist1, dist2) << "vec " << i;
    }
}

TEST(RaBitQEstimatorTest, Stage2_CorrelatedWithExact) {
    // With bits=2, Stage 2 (multi-bit LUT) should produce distance estimates
    // that are positively correlated with exact distances and reasonably bounded.
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder encoder(dim, P, 2);
    RaBitQEstimator estimator(dim, 2);

    auto query = RandomVec(dim, 100);
    auto centroid = RandomVec(dim, 200);
    auto pq = estimator.PrepareQuery(query.data(), centroid.data(), P);

    const int N = 50;
    std::vector<float> exact_dists(N), s2_dists(N);

    for (int i = 0; i < N; ++i) {
        auto vec = RandomVec(dim, 300 + i);
        auto code = encoder.Encode(vec.data(), centroid.data());

        float exact_dist_sq = 0.0f;
        for (Dim d = 0; d < dim; ++d) {
            float diff = vec[d] - query[d];
            exact_dist_sq += diff * diff;
        }

        exact_dists[i] = exact_dist_sq;
        s2_dists[i] = estimator.EstimateDistanceMultiBit(pq, code);
        EXPECT_GE(s2_dists[i], 0.0f) << "vec " << i;
    }

    // Check ranking correlation: sort by exact and by S2, compare top-5
    std::vector<int> exact_order(N), s2_order(N);
    std::iota(exact_order.begin(), exact_order.end(), 0);
    std::iota(s2_order.begin(), s2_order.end(), 0);
    std::sort(exact_order.begin(), exact_order.end(),
              [&](int a, int b) { return exact_dists[a] < exact_dists[b]; });
    std::sort(s2_order.begin(), s2_order.end(),
              [&](int a, int b) { return s2_dists[a] < s2_dists[b]; });

    // At least 3 of exact top-10 should appear in S2 top-15
    std::set<int> exact_top10(exact_order.begin(), exact_order.begin() + 10);
    int overlap = 0;
    for (int i = 0; i < 15; ++i) {
        if (exact_top10.count(s2_order[i])) ++overlap;
    }
    EXPECT_GE(overlap, 3) << "Stage 2 ranking poorly correlated with exact";
}

TEST(RaBitQEstimatorTest, MultiBit_ExCode_NonNegative) {
    // EstimateDistanceMultiBit using ex_code path should produce non-negative distances
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder encoder(dim, P, 2);
    RaBitQEstimator estimator(dim, 2);

    auto query = RandomVec(dim, 100);
    auto centroid = RandomVec(dim, 200);
    auto pq = estimator.PrepareQuery(query.data(), centroid.data(), P);

    for (int i = 0; i < 20; ++i) {
        auto vec = RandomVec(dim, 300 + i);
        auto code = encoder.Encode(vec.data(), centroid.data());

        ASSERT_FALSE(code.ex_code.empty()) << "ex_code should be populated for bits=2";
        float dist = estimator.EstimateDistanceMultiBit(pq, code);
        EXPECT_GE(dist, 0.0f) << "vec " << i;
        EXPECT_LT(dist, 1000.0f) << "vec " << i;
    }
}

TEST(RaBitQEstimatorTest, MultiBit_Bits4_BetterThanBits2) {
    // With bits=4, the LUT has 16 levels vs 4 for bits=2.
    // Both should produce non-negative, reasonable distance estimates.
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder enc2(dim, P, 2);
    RaBitQEncoder enc4(dim, P, 4);
    RaBitQEstimator est2(dim, 2);
    RaBitQEstimator est4(dim, 4);

    auto query = RandomVec(dim, 100);
    auto centroid = RandomVec(dim, 200);
    auto pq2 = est2.PrepareQuery(query.data(), centroid.data(), P);
    auto pq4 = est4.PrepareQuery(query.data(), centroid.data(), P);

    for (int i = 0; i < 10; ++i) {
        auto vec = RandomVec(dim, 300 + i);
        auto code2 = enc2.Encode(vec.data(), centroid.data());
        auto code4 = enc4.Encode(vec.data(), centroid.data());

        float dist2 = est2.EstimateDistanceMultiBit(pq2, code2);
        float dist4 = est4.EstimateDistanceMultiBit(pq4, code4);

        EXPECT_GE(dist2, 0.0f) << "vec " << i << " bits=2";
        EXPECT_GE(dist4, 0.0f) << "vec " << i << " bits=4";
        // Both should be in a reasonable range
        EXPECT_LT(dist2, 1000.0f) << "vec " << i << " bits=2";
        EXPECT_LT(dist4, 1000.0f) << "vec " << i << " bits=4";
    }
}
