#include "rabitq_bench_calibration.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/storage/pack_codes.h"

namespace vdb {
namespace bench {
namespace {

struct ToyCalibrationData {
    Dim dim = 64;
    std::vector<float> vectors;
    std::vector<float> centroids;
    std::vector<std::vector<uint32_t>> cluster_members;
    std::vector<std::vector<rabitq::RaBitQCode>> all_codes;
    rabitq::RotationMatrix rotation;

    ToyCalibrationData()
        : rotation(dim, Identity(dim)) {
        const uint32_t nlist = 3;
        const uint32_t cluster_size = 6;
        vectors.resize(static_cast<size_t>(nlist) * cluster_size * dim);
        centroids.resize(static_cast<size_t>(nlist) * dim, 0.0f);
        cluster_members.resize(nlist);
        for (uint32_t cid = 0; cid < nlist; ++cid) {
            centroids[static_cast<size_t>(cid) * dim] =
                static_cast<float>(cid) * 4.0f;
            for (uint32_t local = 0; local < cluster_size; ++local) {
                const uint32_t row = cid * cluster_size + local;
                cluster_members[cid].push_back(row);
                for (Dim d = 0; d < dim; ++d) {
                    const float base = centroids[static_cast<size_t>(cid) * dim + d];
                    vectors[static_cast<size_t>(row) * dim + d] =
                        base + 0.02f * static_cast<float>(local + 1) +
                        0.003f * static_cast<float>(d + 1);
                }
            }
        }
        EncodeAllCodes(vectors, nlist * cluster_size, dim, cluster_members,
                       centroids, rotation, 1, nullptr, &all_codes);
    }

    static std::vector<float> Identity(Dim dim) {
        std::vector<float> out(static_cast<size_t>(dim) * dim, 0.0f);
        for (Dim d = 0; d < dim; ++d) {
            out[static_cast<size_t>(d) * dim + d] = 1.0f;
        }
        return out;
    }
};

float UpperPercentileForTest(std::vector<float> values, float percentile) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    const float findex = percentile * static_cast<float>(values.size() - 1);
    const auto idx = static_cast<size_t>(
        std::min(findex, static_cast<float>(values.size() - 1)));
    return values[idx];
}

float ManualFastScanPerClusterEpsilon(
    const std::vector<std::vector<rabitq::RaBitQCode>>& all_codes,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const std::vector<float>& vectors,
    const std::vector<float>& centroids,
    const rabitq::RotationMatrix& rotation,
    Dim dim,
    uint32_t max_samples_per_cluster,
    float percentile,
    uint64_t seed,
    float d_k,
    const RaBitQConfig& config) {
    const float lo = 0.1f * d_k;
    const float hi = 10.0f * d_k;
    const uint32_t packed_size = storage::FastScanPackedSize(dim);
    rabitq::RaBitQEstimator estimator(dim, config.active_code_bits());
    std::vector<uint8_t> packed_codes(packed_size, 0);
    std::vector<float> errors;
    for (uint32_t cid = 0; cid < cluster_members.size(); ++cid) {
        const auto& members = cluster_members[cid];
        const auto& codes = all_codes[cid];
        const uint32_t n_members = static_cast<uint32_t>(members.size());
        if (n_members < 2) continue;
        uint32_t n_queries = max_samples_per_cluster;
        if (n_members < 2 * max_samples_per_cluster) {
            n_queries = std::max(n_members / 2, 1u);
        }
        n_queries = std::min(n_queries, n_members);

        std::vector<uint32_t> sample_ids(n_members);
        std::iota(sample_ids.begin(), sample_ids.end(), 0u);
        std::mt19937 rng(static_cast<uint32_t>(seed + cid));
        std::shuffle(sample_ids.begin(), sample_ids.end(), rng);

        const float* centroid = centroids.data() + static_cast<size_t>(cid) * dim;
        rabitq::ClusterPreparedScratch scratch;
        rabitq::PreparedQuery pq;
        float block_norms[32];
        alignas(64) float fs_dists[32];
        for (uint32_t qi = 0; qi < n_queries; ++qi) {
            const uint32_t local_q = sample_ids[qi];
            const float* query =
                vectors.data() + static_cast<size_t>(members[local_q]) * dim;
            estimator.PrepareQueryInto(query, centroid, rotation, &pq, &scratch);
            for (uint32_t t_base = 0; t_base < n_members; t_base += 32) {
                const uint32_t block_count = std::min(32u, n_members - t_base);
                storage::PackSignBitsForFastScan(&codes[t_base], block_count,
                                                 dim, packed_codes.data());
                for (uint32_t j = 0; j < block_count; ++j) {
                    block_norms[j] = codes[t_base + j].norm;
                }
                estimator.EstimateDistanceFastScan(
                    pq, packed_codes.data(), block_norms, block_count, fs_dists);
                for (uint32_t j = 0; j < block_count; ++j) {
                    const uint32_t target_local = t_base + j;
                    if (target_local == local_q) continue;
                    const float* target =
                        vectors.data() +
                        static_cast<size_t>(members[target_local]) * dim;
                    const float true_dist = simd::L2Sqr(query, target, dim);
                    if (true_dist < lo || true_dist > hi) continue;
                    const float denom = 2.0f * block_norms[j] * pq.norm_qc;
                    if (denom > 1e-10f) {
                        errors.push_back(std::abs(fs_dists[j] - true_dist) /
                                         denom);
                    }
                }
            }
        }
    }
    return UpperPercentileForTest(std::move(errors), percentile);
}

TEST(EpsilonCalibrationSamplingTest, GlobalPairSampleCountIsBounded) {
    ToyCalibrationData data;
    EpsilonCalibrationStats stats;
    const float eps = CalibrateSplitEpsilon(
        data.all_codes, data.cluster_members, data.vectors.data(),
        data.centroids, data.rotation, data.dim, 7, 0.95f, 42, 0.1f, 1,
        nullptr, EpsilonSamplingMode::GlobalPair, &stats);

    EXPECT_TRUE(std::isfinite(eps));
    EXPECT_EQ(stats.sampling_mode, EpsilonSamplingMode::GlobalPair);
    EXPECT_EQ(stats.requested_samples, 7u);
    EXPECT_LE(stats.valid_error_count, 7u);
    EXPECT_GT(stats.valid_error_count, 0u);
    EXPECT_GE(stats.attempted_pairs, stats.valid_error_count);
}

TEST(EpsilonCalibrationSamplingTest, GlobalPairIsDeterministicForSeed) {
    ToyCalibrationData data;
    EpsilonCalibrationStats stats1;
    EpsilonCalibrationStats stats2;
    const float eps1 = CalibrateSplitEpsilon(
        data.all_codes, data.cluster_members, data.vectors.data(),
        data.centroids, data.rotation, data.dim, 20, 0.95f, 123, 0.1f, 1,
        nullptr, EpsilonSamplingMode::GlobalPair, &stats1);
    const float eps2 = CalibrateSplitEpsilon(
        data.all_codes, data.cluster_members, data.vectors.data(),
        data.centroids, data.rotation, data.dim, 20, 0.95f, 123, 0.1f, 1,
        nullptr, EpsilonSamplingMode::GlobalPair, &stats2);

    EXPECT_FLOAT_EQ(eps1, eps2);
    EXPECT_EQ(stats1.valid_error_count, stats2.valid_error_count);
    EXPECT_EQ(stats1.attempted_pairs, stats2.attempted_pairs);
}

TEST(EpsilonCalibrationSamplingTest, DefaultModeMatchesLegacyExplicitMode) {
    ToyCalibrationData data;
    EpsilonCalibrationStats default_stats;
    EpsilonCalibrationStats explicit_stats;
    const float eps_default = CalibrateSplitEpsilon(
        data.all_codes, data.cluster_members, data.vectors.data(),
        data.centroids, data.rotation, data.dim, 1, 0.95f, 99, 0.1f, 1,
        nullptr, EpsilonSamplingMode::LegacyPerCluster, &default_stats);
    const float eps_explicit = CalibrateSplitEpsilon(
        data.all_codes, data.cluster_members, data.vectors.data(),
        data.centroids, data.rotation, data.dim, 1, 0.95f, 99, 0.1f, 1,
        nullptr, EpsilonSamplingMode::LegacyPerCluster, &explicit_stats);

    EXPECT_FLOAT_EQ(eps_default, eps_explicit);
    EXPECT_EQ(default_stats.sampling_mode,
              EpsilonSamplingMode::LegacyPerCluster);
    EXPECT_EQ(default_stats.valid_error_count, explicit_stats.valid_error_count);
    EXPECT_GT(default_stats.valid_error_count, 1u);
}

TEST(EpsilonCalibrationSamplingTest, ParsesSamplingMode) {
    auto legacy = ParseEpsilonSamplingModeArg("legacy_per_cluster");
    ASSERT_TRUE(legacy.ok());
    EXPECT_EQ(legacy.value(), EpsilonSamplingMode::LegacyPerCluster);

    auto global = ParseEpsilonSamplingModeArg("global_pair");
    ASSERT_TRUE(global.ok());
    EXPECT_EQ(global.value(), EpsilonSamplingMode::GlobalPair);

    auto invalid = ParseEpsilonSamplingModeArg("bad_mode");
    EXPECT_FALSE(invalid.ok());
}

TEST(EpsilonCalibrationSamplingTest,
     LegacyPerClusterMatchesFastScanBlockReferenceOfficialBits) {
    ToyCalibrationData data;
    RaBitQConfig config;
    config.total_bits = 4;
    config.ex_bits = 3;
    config.estimator_mode = RaBitQEstimatorMode::kOfficial1PlusN;

    std::vector<std::vector<rabitq::RaBitQCode>> official_codes;
    EncodeAllCodes(data.vectors,
                   static_cast<uint32_t>(data.vectors.size() / data.dim),
                   data.dim, data.cluster_members, data.centroids,
                   data.rotation, config, nullptr, &official_codes);

    EpsilonCalibrationStats stats;
    const float actual = CalibrateSplitEpsilon(
        official_codes, data.cluster_members, data.vectors.data(),
        data.centroids, data.rotation, data.dim, 4, 0.95f, 42, 0.1f,
        config, nullptr, EpsilonSamplingMode::LegacyPerCluster, &stats);
    const float expected = ManualFastScanPerClusterEpsilon(
        official_codes, data.cluster_members, data.vectors, data.centroids,
        data.rotation, data.dim, 4, 0.95f, 42, 0.1f, config);

    EXPECT_FLOAT_EQ(actual, expected);
    EXPECT_GT(stats.valid_error_count, 0u);
    EXPECT_GE(stats.attempted_pairs, stats.valid_error_count);
}

TEST(SafeInDkCalibrationTest, GeneratesExactQueryToBaseKthSamples) {
    const Dim dim = 1;
    std::vector<float> queries = {1.0f, 6.0f};
    std::vector<float> base = {0.0f, 10.0f, 20.0f};

    auto samples = GenerateExactSafeInDkSamples(
        queries.data(), 2, base.data(), 3, dim, 2, 2, 42,
        SafeInDkSamplingMode::Unique);
    std::sort(samples.begin(), samples.end());

    ASSERT_EQ(samples.size(), 2u);
    EXPECT_FLOAT_EQ(samples[0], 36.0f);
    EXPECT_FLOAT_EQ(samples[1], 81.0f);
}

TEST(SafeInDkCalibrationTest, CalibratesExactDkWithConannPercentileRule) {
    const Dim dim = 1;
    std::vector<float> queries = {1.0f, 6.0f};
    std::vector<float> base = {0.0f, 10.0f, 20.0f};

    const float threshold = CalibrateExactSafeInDk(
        queries.data(), 2, base.data(), 3, dim, 2, 2, 0.90f, 42);

    EXPECT_FLOAT_EQ(threshold, 36.0f);
}

TEST(SafeInDkCalibrationTest, OfficialRabitqSamplesAndSplitEpsilonAreFinite) {
    ToyCalibrationData data;
    RaBitQConfig config;
    config.total_bits = 4;
    config.ex_bits = 3;
    config.estimator_mode = RaBitQEstimatorMode::kOfficial1PlusN;

    std::vector<std::vector<rabitq::RaBitQCode>> official_codes;
    EncodeAllCodes(data.vectors,
                   static_cast<uint32_t>(data.vectors.size() / data.dim),
                   data.dim, data.cluster_members, data.centroids,
                   data.rotation, config, nullptr, &official_codes);

    auto samples = GenerateRabitqSafeInDkSamples(
        data.vectors.data(),
        static_cast<uint32_t>(data.vectors.size() / data.dim),
        data.dim, 2, 4, data.cluster_members, official_codes,
        data.centroids, data.rotation, config, 42,
        SafeInDkSamplingMode::Unique);
    ASSERT_FALSE(samples.empty());
    for (float sample : samples) {
        EXPECT_TRUE(std::isfinite(sample));
    }

    EpsilonCalibrationStats stats;
    const float eps = CalibrateSplitEpsilon(
        official_codes, data.cluster_members, data.vectors.data(),
        data.centroids, data.rotation, data.dim, 4, 0.95f, 42, 0.1f,
        config, nullptr, EpsilonSamplingMode::GlobalPair, &stats);
    EXPECT_TRUE(std::isfinite(eps));
    EXPECT_GT(stats.valid_error_count, 0u);
}

TEST(SafeInDkCalibrationTest, NamesThresholdSources) {
    EXPECT_STREQ(SafeInThresholdSourceName(SafeInThresholdSource::ExactL2),
                 "exact_l2");
    EXPECT_STREQ(SafeInThresholdSourceName(SafeInThresholdSource::RabitqS2Kth),
                 "rabitq_s2_kth");
}

}  // namespace
}  // namespace bench
}  // namespace vdb
