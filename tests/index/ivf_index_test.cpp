#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/simd/distance_l2.h"

using namespace vdb;
using namespace vdb::index;

namespace fs = std::filesystem;

// ============================================================================
// Test fixture — builds a small IVF index, then opens it with IvfIndex
// ============================================================================

class IvfIndexTest : public ::testing::Test {
 protected:
    static constexpr uint32_t kN = 256;
    static constexpr Dim kDim = 64;
    static constexpr uint32_t kNlist = 4;
    static constexpr uint64_t kSeed = 42;

    void SetUp() override {
        test_dir_ = (fs::temp_directory_path() / "vdb_ivf_index_test").string();
        fs::create_directories(test_dir_);

        // Generate random vectors
        std::mt19937 rng(kSeed);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        vectors_.resize(static_cast<size_t>(kN) * kDim);
        for (auto& v : vectors_) v = dist(rng);

        // Build the index
        IvfBuilderConfig cfg;
        cfg.nlist = kNlist;
        cfg.max_iterations = 10;
        cfg.tolerance = 1e-4f;
        cfg.seed = kSeed;
        cfg.rabitq = {1, 64, 5.75f};
        cfg.calibration_samples = 10;
        cfg.calibration_topk = 5;
        cfg.calibration_percentile = 0.95f;
        cfg.page_size = 1;

        IvfBuilder builder(cfg);
        auto s = builder.Build(vectors_.data(), kN, kDim, test_dir_);
        ASSERT_TRUE(s.ok()) << s.message();
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string test_dir_;
    std::vector<float> vectors_;
};

class IvfIndexCosineTailTest : public ::testing::Test {
 protected:
    static constexpr uint32_t kN = 97;
    static constexpr Dim kDim = 64;
    static constexpr uint32_t kNlist = 5;
    static constexpr uint64_t kSeed = 7;

    void SetUp() override {
        test_dir_ = (fs::temp_directory_path() / "vdb_ivf_index_cosine_tail_test").string();
        fs::create_directories(test_dir_);

        std::mt19937 rng(kSeed);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        vectors_.resize(static_cast<size_t>(kN) * kDim);
        for (auto& v : vectors_) v = dist(rng);

        IvfBuilderConfig cfg;
        cfg.nlist = kNlist;
        cfg.max_iterations = 10;
        cfg.tolerance = 1e-4f;
        cfg.seed = kSeed;
        cfg.metric = "cosine";
        cfg.rabitq = {1, 64, 5.75f};
        cfg.calibration_samples = 10;
        cfg.calibration_topk = 5;
        cfg.calibration_percentile = 0.95f;
        cfg.page_size = 1;

        IvfBuilder builder(cfg);
        auto s = builder.Build(vectors_.data(), kN, kDim, test_dir_);
        ASSERT_TRUE(s.ok()) << s.message();
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    static float CosineScore(const float* a, const float* b, Dim dim) {
        float dot = 0.0f;
        float na = 0.0f;
        float nb = 0.0f;
        for (Dim i = 0; i < dim; ++i) {
            dot += a[i] * b[i];
            na += a[i] * a[i];
            nb += b[i] * b[i];
        }
        if (na <= 0.0f || nb <= 0.0f) return 0.0f;
        return dot / std::sqrt(na * nb);
    }

    std::string test_dir_;
    std::vector<float> vectors_;
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(IvfIndexTest, OpenSucceeds) {
    IvfIndex idx;
    auto s = idx.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(idx.nlist(), kNlist);
    EXPECT_EQ(idx.dim(), kDim);
    EXPECT_EQ(idx.cluster_ids().size(), kNlist);
}

TEST_F(IvfIndexTest, FindNearestClusters_Basic) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    // Pick first vector as query
    const float* query = vectors_.data();

    // nprobe = 1: should return exactly 1 cluster
    auto result = idx.FindNearestClusters(query, 1);
    EXPECT_EQ(result.size(), 1u);

    // nprobe = nlist: should return all clusters
    auto result_all = idx.FindNearestClusters(query, kNlist);
    EXPECT_EQ(result_all.size(), kNlist);

    // The nearest cluster from nprobe=1 should also be the first in nprobe=nlist
    EXPECT_EQ(result[0], result_all[0]);
}

TEST_F(IvfIndexTest, FindNearestClusters_SortedByDistance) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    const float* query = vectors_.data();
    auto result = idx.FindNearestClusters(query, kNlist);

    // Verify results are sorted by distance (ascending)
    for (uint32_t i = 0; i < kNlist; ++i) {
        // Find centroid index for this cluster ID
        uint32_t cidx_i = 0;
        for (uint32_t c = 0; c < kNlist; ++c) {
            if (idx.cluster_ids()[c] == result[i]) {
                cidx_i = c;
                break;
            }
        }
        if (i > 0) {
            uint32_t cidx_prev = 0;
            for (uint32_t c = 0; c < kNlist; ++c) {
                if (idx.cluster_ids()[c] == result[i - 1]) {
                    cidx_prev = c;
                    break;
                }
            }
            float d_prev = simd::L2Sqr(query, idx.centroid(cidx_prev), kDim);
            float d_curr = simd::L2Sqr(query, idx.centroid(cidx_i), kDim);
            EXPECT_LE(d_prev, d_curr + 1e-6f)
                << "Clusters not sorted by distance at position " << i;
        }
    }
}

TEST_F(IvfIndexTest, ConANN_LoadedCorrectly) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    // Global epsilon stores calibrated ε_ip (inner-product error bound).
    // d_k should still be positive after calibration.
    EXPECT_GT(idx.conann().epsilon(), 0.0f);
    EXPECT_GT(idx.conann().d_k(), 0.0f);
    EXPECT_FLOAT_EQ(idx.conann().safein_d_k(), idx.conann().legacy_d_k());
    EXPECT_FALSE(idx.conann().has_safein_d_k());
}


TEST_F(IvfIndexTest, SegmentAccessible) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    // Should be able to access clusters via the segment
    for (auto cid : idx.cluster_ids()) {
        EXPECT_EQ(idx.segment().GetNumRecords(cid), idx.segment().GetNumRecords(cid));
        EXPECT_GT(idx.segment().GetNumRecords(cid), 0u);
        EXPECT_NE(idx.segment().GetCentroid(cid), nullptr);
    }
}

TEST_F(IvfIndexTest, NprobeClamped) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    // nprobe > nlist should be clamped
    auto result = idx.FindNearestClusters(vectors_.data(), kNlist + 100);
    EXPECT_EQ(result.size(), kNlist);
}

TEST_F(IvfIndexTest, NprobeZero_ReturnsEmpty) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    auto result = idx.FindNearestClusters(vectors_.data(), 0);
    EXPECT_TRUE(result.empty());
}

TEST(IvfIndexTwoLevelCoarseTest, DefaultParameterDerivation) {
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(0), 0u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(1), 1u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(4096), 32u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(8192), 64u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(16384), 128u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(16384, 512), 512u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(16, 512), 16u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(16384, 0, 256, 2), 512u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(16384, 1024, 256, 2), 1024u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelSuperCount(300, 0, 256, 2), 300u);

    EXPECT_EQ(IvfIndex::DefaultTwoLevelCandidateBudget(0, 256, 8), 0u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelCandidateBudget(16384, 0, 8), 0u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelCandidateBudget(16384, 256, 8), 2048u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelCandidateBudget(1024, 256, 8), 1024u);
    EXPECT_EQ(IvfIndex::DefaultTwoLevelCandidateBudget(16384, 256, 0), 256u);
}

TEST_F(IvfIndexTest, TwoLevelRoutingDisabledMatchesExact) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    const float* query = vectors_.data();
    auto exact = idx.FindNearestClusters(query, kNlist);
    idx.SetTwoLevelCoarseRouting(false, 0, 0, 0, 8);
    auto disabled = idx.FindNearestClusters(query, kNlist);
    EXPECT_EQ(disabled, exact);
    EXPECT_EQ(idx.last_coarse_routing_mode(), 0u);
}

TEST_F(IvfIndexTest, TwoLevelRoutingFallsBackForL2Path) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    idx.SetTwoLevelCoarseRouting(true, 0, 0, 0, 8);
    auto result = idx.FindNearestClusters(vectors_.data(), kNlist);
    EXPECT_EQ(result.size(), kNlist);
    EXPECT_EQ(idx.last_coarse_routing_mode(), 0u);
    EXPECT_EQ(idx.last_coarse_exact_fallback(), 0u);
}

TEST_F(IvfIndexTest, OpenInvalidDir_Fails) {
    IvfIndex idx;
    auto s = idx.Open("/tmp/nonexistent_ivf_dir_12345");
    EXPECT_FALSE(s.ok());
}

TEST_F(IvfIndexCosineTailTest, FindNearestClusters_CosineSortedWithTailCentroids) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());
    EXPECT_EQ(idx.requested_metric(), "cosine");
    EXPECT_EQ(idx.effective_metric(), "ip");

    const float* query = vectors_.data() + static_cast<size_t>(3) * kDim;
    auto result = idx.FindNearestClusters(query, kNlist);
    ASSERT_EQ(result.size(), kNlist);

    for (uint32_t i = 1; i < kNlist; ++i) {
        uint32_t prev_idx = 0;
        uint32_t curr_idx = 0;
        for (uint32_t c = 0; c < kNlist; ++c) {
            if (idx.cluster_ids()[c] == result[i - 1]) prev_idx = c;
            if (idx.cluster_ids()[c] == result[i]) curr_idx = c;
        }
        const float prev_score = CosineScore(query, idx.centroid(prev_idx), kDim);
        const float curr_score = CosineScore(query, idx.centroid(curr_idx), kDim);
        EXPECT_GE(prev_score + 1e-5f, curr_score)
            << "Clusters not sorted by cosine score at position " << i;
    }
}

TEST_F(IvfIndexCosineTailTest, TwoLevelRoutingBuildsHierarchyAndMatchesExactWhenAllChildrenCovered) {
    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());

    const float* query = vectors_.data() + static_cast<size_t>(3) * kDim;
    auto exact = idx.FindNearestClusters(query, kNlist);

    idx.SetTwoLevelCoarseRouting(true, 0, 1, 0, 8, true);
    EXPECT_TRUE(idx.PrepareTwoLevelCoarseRouting(kNlist));
    EXPECT_TRUE(idx.PrepareTwoLevelCoarseRouting(kNlist));
    auto routed = idx.FindNearestClusters(query, kNlist);

    EXPECT_EQ(routed, exact);
    EXPECT_EQ(idx.last_coarse_routing_mode(), 1u);
    EXPECT_EQ(idx.last_coarse_super_count(), 1u);
    EXPECT_EQ(idx.last_coarse_super_probes(), 1u);
    EXPECT_EQ(idx.last_coarse_child_candidates_scored(), kNlist);
    EXPECT_EQ(idx.last_coarse_exact_fallback(), 0u);
    EXPECT_EQ(idx.last_coarse_exact_overlap(), kNlist);
}
