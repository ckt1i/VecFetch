#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
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
// Test fixture
// ============================================================================

class IvfBuilderTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = (fs::temp_directory_path() / "vdb_ivf_builder_test").string();
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    /// Generate N random Gaussian vectors (row-major, N × dim)
    static std::vector<float> GenerateVectors(uint32_t N, Dim dim,
                                               uint64_t seed = 42) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        std::vector<float> vecs(static_cast<size_t>(N) * dim);
        for (auto& v : vecs) v = dist(rng);
        return vecs;
    }

    std::string test_dir_;
};

static std::string ReadFileToString(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

// ============================================================================
// Basic build test
// ============================================================================

TEST_F(IvfBuilderTest, Build_Basic) {
    constexpr uint32_t N = 128;
    constexpr Dim dim = 64;
    constexpr uint32_t nlist = 4;

    auto vecs = GenerateVectors(N, dim);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 10;
    cfg.seed = 42;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 10;
    cfg.calibration_topk = 5;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    auto s = builder.Build(vecs.data(), N, dim, test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    // Verify output files exist
    EXPECT_TRUE(fs::exists(test_dir_ + "/segment.meta"));
    EXPECT_TRUE(fs::exists(test_dir_ + "/centroids.bin"));
    EXPECT_TRUE(fs::exists(test_dir_ + "/rotation.bin"));

    // Verify unified files
    EXPECT_TRUE(fs::exists(test_dir_ + "/cluster.clu"))
        << "Missing cluster.clu";
    EXPECT_TRUE(fs::exists(test_dir_ + "/data.dat"))
        << "Missing data.dat";

    // Verify assignments cover all vectors
    EXPECT_EQ(builder.assignments().size(), N);
    for (auto a : builder.assignments()) {
        EXPECT_LT(a, nlist);
    }

    // Verify centroids shape
    EXPECT_EQ(builder.centroids().size(),
              static_cast<size_t>(nlist) * dim);

    // Verify calibrated d_k is positive
    EXPECT_GT(builder.calibrated_dk(), 0.0f);
}

// ============================================================================
// Assignment completeness: every vector is assigned to exactly one cluster
// ============================================================================

TEST_F(IvfBuilderTest, AssignmentCompleteness) {
    constexpr uint32_t N = 200;
    constexpr Dim dim = 32;
    constexpr uint32_t nlist = 8;

    auto vecs = GenerateVectors(N, dim, 123);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 15;
    cfg.seed = 123;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 10;
    cfg.calibration_topk = 5;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    ASSERT_TRUE(builder.Build(vecs.data(), N, dim, test_dir_).ok());

    const auto& assignments = builder.assignments();
    ASSERT_EQ(assignments.size(), N);

    // Count assignments per cluster
    std::vector<uint32_t> counts(nlist, 0);
    for (auto a : assignments) {
        ASSERT_LT(a, nlist);
        counts[a]++;
    }

    // Every cluster should have at least 1 vector
    for (uint32_t k = 0; k < nlist; ++k) {
        EXPECT_GT(counts[k], 0u) << "Cluster " << k << " is empty";
    }

    // Total should equal N
    uint32_t total = 0;
    for (auto c : counts) total += c;
    EXPECT_EQ(total, N);
}

// ============================================================================
// Roundtrip: build → open → verify nearest cluster contains the vector
// ============================================================================

TEST_F(IvfBuilderTest, BuildAndOpen_Roundtrip) {
    constexpr uint32_t N = 128;
    constexpr Dim dim = 64;
    constexpr uint32_t nlist = 4;

    auto vecs = GenerateVectors(N, dim);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 10;
    cfg.seed = 42;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 10;
    cfg.calibration_topk = 5;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    ASSERT_TRUE(builder.Build(vecs.data(), N, dim, test_dir_).ok());

    // Open with IvfIndex
    IvfIndex idx;
    auto os = idx.Open(test_dir_);
    ASSERT_TRUE(os.ok()) << "Open failed: " << os.message();

    EXPECT_EQ(idx.nlist(), nlist);
    EXPECT_EQ(idx.dim(), dim);

    // For every vector, the nearest cluster from the builder should match
    // the nearest cluster from IvfIndex
    for (uint32_t i = 0; i < N; ++i) {
        const float* q = vecs.data() + static_cast<size_t>(i) * dim;
        auto nearest = idx.FindNearestClusters(q, 1);
        ASSERT_EQ(nearest.size(), 1u);
        // The builder's assignment should match (builder uses index k,
        // IvfIndex uses ClusterID which equals k)
        EXPECT_EQ(nearest[0], builder.assignments()[i])
            << "Mismatch for vector " << i;
    }
}

// ============================================================================
// Progress callback
// ============================================================================

TEST_F(IvfBuilderTest, ProgressCallback) {
    constexpr uint32_t N = 64;
    constexpr Dim dim = 32;
    constexpr uint32_t nlist = 4;

    auto vecs = GenerateVectors(N, dim);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 5;
    cfg.seed = 42;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 5;
    cfg.calibration_topk = 3;
    cfg.page_size = 1;

    uint32_t callback_count = 0;
    IvfBuilder builder(cfg);
    builder.SetProgressCallback([&](uint32_t idx, uint32_t total) {
        EXPECT_EQ(total, nlist);
        EXPECT_EQ(idx, callback_count);
        callback_count++;
    });

    ASSERT_TRUE(builder.Build(vecs.data(), N, dim, test_dir_).ok());
    EXPECT_EQ(callback_count, nlist);
}

// ============================================================================
// Invalid inputs
// ============================================================================

TEST_F(IvfBuilderTest, InvalidInputs) {
    IvfBuilderConfig cfg;
    cfg.nlist = 4;
    cfg.rabitq = {1, 64, 5.75f};

    IvfBuilder builder(cfg);

    // null vectors
    EXPECT_FALSE(builder.Build(nullptr, 100, 64, test_dir_).ok());

    // N = 0
    float dummy = 1.0f;
    EXPECT_FALSE(builder.Build(&dummy, 0, 64, test_dir_).ok());

    // dim = 0
    EXPECT_FALSE(builder.Build(&dummy, 100, 0, test_dir_).ok());

    // nlist > N
    IvfBuilderConfig cfg2;
    cfg2.nlist = 200;
    cfg2.rabitq = {1, 64, 5.75f};
    IvfBuilder builder2(cfg2);
    auto vecs = GenerateVectors(100, 64);
    EXPECT_FALSE(builder2.Build(vecs.data(), 100, 64, test_dir_).ok());
}

// ============================================================================
// Calibration queries (cross-modal d_k)
// ============================================================================

TEST_F(IvfBuilderTest, CalibrationQueries_ChangeDk) {
    constexpr uint32_t N = 128;
    constexpr Dim dim = 64;
    constexpr uint32_t nlist = 4;

    auto db_vecs = GenerateVectors(N, dim);

    // Generate query vectors with a different distribution (shifted mean)
    std::mt19937 rng(99);
    std::normal_distribution<float> dist(5.0f, 1.0f);  // shifted mean
    std::vector<float> qry_vecs(32 * dim);
    for (auto& v : qry_vecs) v = dist(rng);

    // Build without calibration queries
    IvfBuilderConfig cfg1;
    cfg1.nlist = nlist;
    cfg1.max_iterations = 5;
    cfg1.seed = 42;
    cfg1.rabitq = {1, 64, 5.75f};
    cfg1.calibration_samples = 10;
    cfg1.calibration_topk = 5;
    cfg1.calibration_percentile = 0.95f;
    cfg1.page_size = 1;

    std::string dir1 = test_dir_ + "/no_qry";
    fs::create_directories(dir1);
    IvfBuilder builder1(cfg1);
    ASSERT_TRUE(builder1.Build(db_vecs.data(), N, dim, dir1).ok());

    IvfIndex idx1;
    ASSERT_TRUE(idx1.Open(dir1).ok());
    float dk1 = idx1.conann().d_k();

    // Build with calibration queries (different distribution)
    IvfBuilderConfig cfg2 = cfg1;
    cfg2.calibration_queries = qry_vecs.data();
    cfg2.num_calibration_queries = 32;

    std::string dir2 = test_dir_ + "/with_qry";
    fs::create_directories(dir2);
    IvfBuilder builder2(cfg2);
    ASSERT_TRUE(builder2.Build(db_vecs.data(), N, dim, dir2).ok());

    IvfIndex idx2;
    ASSERT_TRUE(idx2.Open(dir2).ok());
    float dk2 = idx2.conann().d_k();

    // d_k should differ because query distribution is different
    EXPECT_NE(dk1, dk2);
    // Shifted queries are farther from database → d_k should be larger
    EXPECT_GT(dk2, dk1);
}

TEST_F(IvfBuilderTest, RedundantAssignmentBuild_PreservesPrimaryAndSecondaryState) {
    constexpr uint32_t N = 96;
    constexpr Dim dim = 32;
    constexpr uint32_t nlist = 8;

    auto vecs = GenerateVectors(N, dim, 777);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 10;
    cfg.seed = 777;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 10;
    cfg.calibration_topk = 5;
    cfg.page_size = 1;
    cfg.assignment_factor = 2;
    cfg.assignment_mode = AssignmentMode::RedundantTop2Naive;

    IvfBuilder builder(cfg);
    ASSERT_TRUE(builder.Build(vecs.data(), N, dim, test_dir_).ok());

    EXPECT_EQ(builder.assignment_mode(), AssignmentMode::RedundantTop2Naive);
    EXPECT_EQ(builder.clustering_source(), ClusteringSource::Auto);
    ASSERT_EQ(builder.assignments().size(), N);
    ASSERT_EQ(builder.secondary_assignments().size(), N);

    uint32_t distinct_secondary = 0;
    for (uint32_t i = 0; i < N; ++i) {
        EXPECT_LT(builder.assignments()[i], nlist);
        EXPECT_LT(builder.secondary_assignments()[i], nlist);
        if (builder.secondary_assignments()[i] != builder.assignments()[i]) {
            distinct_secondary++;
        }
    }
    EXPECT_GT(distinct_secondary, 0u);

    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());
    EXPECT_EQ(idx.assignment_mode(), AssignmentMode::RedundantTop2Naive);
    EXPECT_EQ(idx.assignment_factor(), 2u);
    EXPECT_FLOAT_EQ(idx.rair_lambda(), 0.75f);
    EXPECT_FALSE(idx.rair_strict_second_choice());
    EXPECT_EQ(idx.clustering_source(), ClusteringSource::Auto);
}

TEST_F(IvfBuilderTest, RairAssignmentBuild_DiffersFromNaiveAndPersistsMetadata) {
    constexpr uint32_t N = 96;
    constexpr Dim dim = 32;
    constexpr uint32_t nlist = 8;

    auto vecs = GenerateVectors(N, dim, 2026);

    IvfBuilderConfig naive_cfg;
    naive_cfg.nlist = nlist;
    naive_cfg.max_iterations = 10;
    naive_cfg.seed = 2026;
    naive_cfg.rabitq = {1, 64, 5.75f};
    naive_cfg.calibration_samples = 10;
    naive_cfg.calibration_topk = 5;
    naive_cfg.page_size = 1;
    naive_cfg.assignment_factor = 2;
    naive_cfg.assignment_mode = AssignmentMode::RedundantTop2Naive;

    auto naive_dir = (fs::path(test_dir_) / "naive").string();
    IvfBuilder naive_builder(naive_cfg);
    ASSERT_TRUE(naive_builder.Build(vecs.data(), N, dim, naive_dir).ok());

    IvfBuilderConfig rair_cfg = naive_cfg;
    rair_cfg.assignment_mode = AssignmentMode::RedundantTop2Rair;
    rair_cfg.rair_lambda = 0.75f;
    rair_cfg.rair_strict_second_choice = false;

    auto rair_dir = (fs::path(test_dir_) / "rair").string();
    IvfBuilder rair_builder(rair_cfg);
    ASSERT_TRUE(rair_builder.Build(vecs.data(), N, dim, rair_dir).ok());

    size_t differing_secondary = 0;
    for (uint32_t i = 0; i < N; ++i) {
        EXPECT_EQ(naive_builder.assignments()[i], rair_builder.assignments()[i]);
        if (naive_builder.secondary_assignments()[i] !=
            rair_builder.secondary_assignments()[i]) {
            differing_secondary++;
        }
    }
    EXPECT_GT(differing_secondary, 0u);

    IvfIndex idx;
    ASSERT_TRUE(idx.Open(rair_dir).ok());
    EXPECT_EQ(idx.assignment_mode(), AssignmentMode::RedundantTop2Rair);
    EXPECT_EQ(idx.assignment_factor(), 2u);
    EXPECT_FLOAT_EQ(idx.rair_lambda(), 0.75f);
    EXPECT_FALSE(idx.rair_strict_second_choice());
}

TEST_F(IvfBuilderTest, FaissKMeansBuild_AutoTrainingPersistsMetadata) {
    constexpr uint32_t N = 128;
    constexpr Dim dim = 32;
    constexpr uint32_t nlist = 8;

    auto vecs = GenerateVectors(N, dim, 2027);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.coarse_builder = CoarseBuilder::FaissKMeans;
    cfg.max_iterations = 6;
    cfg.faiss_train_size = 64;
    cfg.faiss_nredo = 1;
    cfg.metric = "cosine";
    cfg.seed = 2027;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 10;
    cfg.calibration_topk = 5;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    auto s = builder.Build(vecs.data(), N, dim, test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(builder.coarse_builder(), CoarseBuilder::FaissKMeans);
    EXPECT_EQ(builder.clustering_source(), ClusteringSource::Auto);
    ASSERT_EQ(builder.assignments().size(), N);
    ASSERT_EQ(builder.centroids().size(), static_cast<size_t>(nlist) * dim);

    const std::string sidecar = ReadFileToString(test_dir_ + "/build_metadata.json");
    EXPECT_NE(sidecar.find("\"coarse_builder\": \"faiss_kmeans\""), std::string::npos);
    EXPECT_NE(sidecar.find("\"clustering_source\": \"auto\""), std::string::npos);
    EXPECT_NE(sidecar.find("\"requested_metric\": \"cosine\""), std::string::npos);
    EXPECT_NE(sidecar.find("\"effective_metric\": \"ip\""), std::string::npos);
    EXPECT_NE(sidecar.find("\"faiss_train_size\": 64"), std::string::npos);
    EXPECT_NE(sidecar.find("\"faiss_niter\": 10"), std::string::npos);
    EXPECT_NE(sidecar.find("\"faiss_nredo\": 1"), std::string::npos);
    EXPECT_NE(sidecar.find("\"faiss_backend\": \"cpu\""), std::string::npos);

    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());
    EXPECT_EQ(idx.coarse_builder(), CoarseBuilder::FaissKMeans);
    EXPECT_EQ(idx.clustering_source(), ClusteringSource::Auto);
}

TEST_F(IvfBuilderTest, FaissKMeansBuild_PrecomputedImportPreservesBuilderIdentity) {
    constexpr uint32_t N = 96;
    constexpr Dim dim = 24;
    constexpr uint32_t nlist = 6;

    auto vecs = GenerateVectors(N, dim, 2028);

    const auto centroids_path = (fs::path(test_dir_) / "saved_centroids.fvecs").string();
    const auto assignments_path = (fs::path(test_dir_) / "saved_assignments.ivecs").string();

    IvfBuilderConfig export_cfg;
    export_cfg.nlist = nlist;
    export_cfg.coarse_builder = CoarseBuilder::FaissKMeans;
    export_cfg.max_iterations = 5;
    export_cfg.faiss_train_size = N;
    export_cfg.metric = "cosine";
    export_cfg.seed = 2028;
    export_cfg.rabitq = {1, 64, 5.75f};
    export_cfg.calibration_samples = 8;
    export_cfg.calibration_topk = 4;
    export_cfg.page_size = 1;
    export_cfg.save_centroids_path = centroids_path;
    export_cfg.save_assignments_path = assignments_path;

    auto export_dir = (fs::path(test_dir_) / "export").string();
    IvfBuilder export_builder(export_cfg);
    ASSERT_TRUE(export_builder.Build(vecs.data(), N, dim, export_dir).ok());
    EXPECT_TRUE(fs::exists(centroids_path));
    EXPECT_TRUE(fs::exists(assignments_path));

    IvfBuilderConfig import_cfg = export_cfg;
    import_cfg.centroids_path = centroids_path;
    import_cfg.assignments_path = assignments_path;
    import_cfg.save_centroids_path.clear();
    import_cfg.save_assignments_path.clear();

    auto import_dir = (fs::path(test_dir_) / "import").string();
    IvfBuilder import_builder(import_cfg);
    ASSERT_TRUE(import_builder.Build(vecs.data(), N, dim, import_dir).ok());

    EXPECT_EQ(import_builder.coarse_builder(), CoarseBuilder::FaissKMeans);
    EXPECT_EQ(import_builder.clustering_source(), ClusteringSource::Precomputed);

    const std::string sidecar = ReadFileToString(import_dir + "/build_metadata.json");
    EXPECT_NE(sidecar.find("\"coarse_builder\": \"faiss_kmeans\""), std::string::npos);
    EXPECT_NE(sidecar.find("\"clustering_source\": \"precomputed\""), std::string::npos);

    IvfIndex idx;
    ASSERT_TRUE(idx.Open(import_dir).ok());
    EXPECT_EQ(idx.coarse_builder(), CoarseBuilder::FaissKMeans);
    EXPECT_EQ(idx.clustering_source(), ClusteringSource::Precomputed);
}

TEST_F(IvfBuilderTest, FaissCosineIndex_UsesMetricAwareCentroidProbing) {
    constexpr Dim dim = 2;

    const std::vector<float> vecs = {
        1.0f, 0.0f,
        0.9f, 0.1f,
        0.0f, 1.0f,
        0.1f, 0.9f,
    };

    IvfBuilderConfig cfg;
    cfg.nlist = 2;
    cfg.coarse_builder = CoarseBuilder::FaissKMeans;
    cfg.faiss_train_size = 4;
    cfg.faiss_niter = 4;
    cfg.faiss_nredo = 1;
    cfg.metric = "cosine";
    cfg.seed = 7;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 2;
    cfg.calibration_topk = 2;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    ASSERT_TRUE(builder.Build(vecs.data(), 4, dim, test_dir_).ok());

    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());
    EXPECT_EQ(idx.requested_metric(), "cosine");
    EXPECT_EQ(idx.effective_metric(), "ip");

    const std::vector<float> query = {0.0f, 10.0f};
    auto nearest = idx.FindNearestClusters(query.data(), 1);
    ASSERT_EQ(nearest.size(), 1u);

    const auto& assignments = builder.assignments();
    int vertical_cluster = -1;
    for (size_t i = 2; i < assignments.size(); ++i) {
        if (vertical_cluster == -1) {
            vertical_cluster = static_cast<int>(assignments[i]);
        } else {
            EXPECT_EQ(assignments[i], static_cast<uint32_t>(vertical_cluster));
        }
    }
    ASSERT_GE(vertical_cluster, 0);
    EXPECT_EQ(nearest[0], static_cast<ClusterID>(vertical_cluster));
}

TEST_F(IvfBuilderTest, SuperKMeansCosineIndex_UsesMetricAwareCentroidProbing) {
    constexpr Dim dim = 2;
    std::vector<float> vecs;
    vecs.reserve(256);
    for (int i = 0; i < 64; ++i) {
        const float t = static_cast<float>(i % 8) * 0.01f;
        vecs.push_back(1.0f - t);
        vecs.push_back(0.0f + t);
    }
    for (int i = 0; i < 64; ++i) {
        const float t = static_cast<float>(i % 8) * 0.01f;
        vecs.push_back(0.0f + t);
        vecs.push_back(1.0f - t);
    }

    IvfBuilderConfig cfg;
    cfg.nlist = 2;
    cfg.coarse_builder = CoarseBuilder::SuperKMeans;
    cfg.max_iterations = 10;
    cfg.metric = "cosine";
    cfg.seed = 9;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 2;
    cfg.calibration_topk = 2;
    cfg.page_size = 1;

    IvfBuilder builder(cfg);
    ASSERT_TRUE(builder.Build(vecs.data(), 128, dim, test_dir_).ok());

    const std::string sidecar = ReadFileToString(test_dir_ + "/build_metadata.json");
    EXPECT_NE(sidecar.find("\"coarse_builder\": \"superkmeans\""), std::string::npos);
    EXPECT_NE(sidecar.find("\"requested_metric\": \"cosine\""), std::string::npos);
    EXPECT_NE(sidecar.find("\"effective_metric\": \"ip\""), std::string::npos);

    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());
    EXPECT_EQ(idx.requested_metric(), "cosine");
    EXPECT_EQ(idx.effective_metric(), "ip");

    const std::vector<float> query = {0.0f, 10.0f};
    auto nearest = idx.FindNearestClusters(query.data(), 1);
    ASSERT_EQ(nearest.size(), 1u);

    const auto& assignments = builder.assignments();
    int vertical_cluster = -1;
    for (size_t i = 64; i < assignments.size(); ++i) {
        if (vertical_cluster == -1) {
            vertical_cluster = static_cast<int>(assignments[i]);
        } else {
            EXPECT_EQ(assignments[i], static_cast<uint32_t>(vertical_cluster));
        }
    }
    ASSERT_GE(vertical_cluster, 0);
    EXPECT_EQ(nearest[0], static_cast<ClusterID>(vertical_cluster));
}

TEST_F(IvfBuilderTest, BlockedHadamardPermutedBuildAndOpen_768D_NoPadding) {
    constexpr uint32_t N = 96;
    constexpr Dim dim = 768;
    constexpr uint32_t nlist = 4;

    auto vecs = GenerateVectors(N, dim, 4040);

    IvfBuilderConfig cfg;
    cfg.nlist = nlist;
    cfg.max_iterations = 5;
    cfg.seed = 4040;
    cfg.rabitq = {1, 64, 5.75f};
    cfg.calibration_samples = 8;
    cfg.calibration_topk = 4;
    cfg.page_size = 1;
    cfg.pad_non_power_of_two_to_pow2 = false;
    cfg.use_blocked_hadamard_permuted = true;

    IvfBuilder builder(cfg);
    auto s = builder.Build(vecs.data(), N, dim, test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    EXPECT_EQ(builder.logical_dim(), dim);
    EXPECT_EQ(builder.effective_dim(), dim);
    EXPECT_EQ(builder.padding_mode(), "none");
    EXPECT_EQ(builder.rotation_mode(), "blocked_hadamard_permuted");
    EXPECT_TRUE(fs::exists(test_dir_ + "/rotated_centroids.bin"));

    const std::string sidecar = ReadFileToString(test_dir_ + "/build_metadata.json");
    EXPECT_NE(sidecar.find("\"rotation_mode\": \"blocked_hadamard_permuted\""),
              std::string::npos);
    EXPECT_NE(sidecar.find("\"padding_mode\": \"none\""), std::string::npos);
    EXPECT_NE(sidecar.find("\"logical_dim\": 768"), std::string::npos);
    EXPECT_NE(sidecar.find("\"effective_dim\": 768"), std::string::npos);
    EXPECT_NE(sidecar.find("\"rotation_seed\": 4040"), std::string::npos);
    EXPECT_NE(sidecar.find("\"rotation_block_sizes\": [512, 256]"), std::string::npos);

    IvfIndex idx;
    ASSERT_TRUE(idx.Open(test_dir_).ok());
    EXPECT_EQ(idx.logical_dim(), dim);
    EXPECT_EQ(idx.effective_dim(), dim);
    EXPECT_EQ(idx.padding_mode(), "none");
    EXPECT_EQ(idx.rotation_mode(), "blocked_hadamard_permuted");
    EXPECT_TRUE(idx.used_hadamard());
    EXPECT_FALSE(idx.uses_padded_hadamard());
}
