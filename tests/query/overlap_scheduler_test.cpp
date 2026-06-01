#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <random>
#include <vector>

#include "vdb/common/distance.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"

using namespace vdb;
using namespace vdb::query;
using namespace vdb::index;
namespace fs = std::filesystem;

class OverlapSchedulerTest : public ::testing::Test {
 protected:
    static constexpr uint32_t N = 256;
    static constexpr Dim kDim = 64;
    static constexpr uint32_t kNlist = 4;
    static constexpr uint32_t kTopK = 10;
    static constexpr uint32_t kNprobe = 4;  // Probe all clusters

    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_scheduler_test";
        fs::create_directories(test_dir_);

        // Generate random vectors
        std::mt19937 rng(42);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        vectors_.resize(static_cast<size_t>(N) * kDim);
        for (auto& v : vectors_) v = dist(rng);

        // Build index
        IvfBuilderConfig cfg;
        cfg.nlist = kNlist;
        cfg.max_iterations = 20;
        cfg.seed = 42;
        cfg.rabitq.c_factor = 5.75f;
        cfg.calibration_samples = 50;
        cfg.calibration_topk = kTopK;
        cfg.epsilon_samples = 20;
        cfg.page_size = 1;  // No padding for simple test

        IvfBuilder builder(cfg);
        auto s = builder.Build(vectors_.data(), N, kDim, test_dir_.string());
        ASSERT_TRUE(s.ok()) << s.message();

        // Open index
        index_ = std::make_unique<IvfIndex>();
        s = index_->Open(test_dir_.string());
        ASSERT_TRUE(s.ok()) << s.message();
    }

    void TearDown() override {
        index_.reset();
        fs::remove_all(test_dir_);
    }

    // Brute-force L2 TopK for ground truth
    std::vector<std::pair<float, uint32_t>> BruteForceTopK(
        const float* query, uint32_t top_k) {
        std::vector<std::pair<float, uint32_t>> all;
        all.reserve(N);
        for (uint32_t i = 0; i < N; ++i) {
            float d = L2Sqr(query, vectors_.data() + static_cast<size_t>(i) * kDim, kDim);
            all.push_back({d, i});
        }
        std::partial_sort(all.begin(), all.begin() + top_k, all.end());
        all.resize(top_k);
        return all;
    }

    float BuildUpperBoundFrontier() {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.enable_dynamic_safeout = true;
        OverlapScheduler scheduler(*index_, reader, config);

        scheduler.est_top_k_ = 3;
        scheduler.AddSafeOutFrontierEstimate({1.0f, 100.0f});  // U=101, low d_hat
        scheduler.AddSafeOutFrontierEstimate({2.0f, 0.0f});    // U=2
        scheduler.AddSafeOutFrontierEstimate({3.0f, 0.0f});    // U=3
        scheduler.AddSafeOutFrontierEstimate({4.0f, 0.0f});    // U=4 replaces U=101
        return scheduler.SafeOutFrontierUpper();
    }

    float ReadUnfilledUpperBoundFrontier() {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.enable_dynamic_safeout = true;
        OverlapScheduler scheduler(*index_, reader, config);

        scheduler.est_top_k_ = 3;
        scheduler.AddSafeOutFrontierEstimate({2.0f, 0.0f});
        scheduler.AddSafeOutFrontierEstimate({3.0f, 0.0f});
        return scheduler.SafeOutFrontierUpper();
    }

    fs::path test_dir_;
    std::vector<float> vectors_;
    std::unique_ptr<IvfIndex> index_;
};

TEST_F(OverlapSchedulerTest, SafeOutFrontierUsesTopKUpperBound) {
    const float frontier = BuildUpperBoundFrontier();

    EXPECT_FLOAT_EQ(frontier, 4.0f);
}

TEST_F(OverlapSchedulerTest, SafeOutFrontierIsInfinityUntilHeapFull) {
    const float frontier = ReadUnfilledUpperBoundFrontier();

    EXPECT_TRUE(std::isinf(frontier));
}

TEST_F(OverlapSchedulerTest, EndToEnd_PreadFallback) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.probe_batch_size = 64;

    OverlapScheduler scheduler(*index_, reader, config);

    // Run multiple queries
    std::mt19937 rng(123);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 5; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        auto ground_truth = BruteForceTopK(query.data(), kTopK);

        // With per-cluster epsilon, some true top-K vectors may be SafeOut'd.
        // Verify we got results and they are sorted.
        ASSERT_GT(results.size(), 0u)
            << "Query " << q << " returned 0 results";

        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance)
                << "Query " << q << " results not sorted at index " << i;
        }
    }
}

TEST_F(OverlapSchedulerTest, StatsArePopulated) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;

    // We need access to stats, but Search returns SearchResults.
    // For now just verify it doesn't crash and returns results.
    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());
    EXPECT_GT(results.size(), 0u);
    EXPECT_EQ(results.stats().fixed_vec_buffer_hits, 0u);
    EXPECT_EQ(results.stats().fixed_vec_buffer_misses,
              results.stats().vec_only_read_requests);
    EXPECT_EQ(results.stats().payload_read_requests, 0u);
    EXPECT_EQ(results.stats().vec_only_read_requests +
              results.stats().all_read_requests,
              results.stats().total_submit_window_requests);
    EXPECT_EQ(results.stats().probe_submit_pending_slot_alloc_ms, 0.0);
    EXPECT_EQ(results.stats().probe_submit_prep_read_ms, 0.0);
    EXPECT_EQ(results.stats().rerank_vec_alloc_ms, 0.0);
    EXPECT_EQ(results.stats().rerank_vec_copy_ms, 0.0);
}

TEST_F(OverlapSchedulerTest, StageUncertainStatsRemainConsistent) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;

    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());
    const auto& stats = results.stats();

    EXPECT_EQ(stats.total_uncertain,
              stats.s1_uncertain_raw - stats.s2_safe_in - stats.s2_safe_out);
    if (stats.s2_safe_in + stats.s2_safe_out + stats.s2_uncertain > 0) {
        EXPECT_EQ(stats.s1_uncertain_raw,
                  stats.s2_safe_in + stats.s2_safe_out + stats.s2_uncertain);
        EXPECT_EQ(stats.total_uncertain, stats.s2_uncertain);
    } else {
        EXPECT_EQ(stats.total_uncertain, stats.s1_uncertain_raw);
    }
}

TEST_F(OverlapSchedulerTest, DynamicSafeOutWorksWithoutExternalStopParams) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = 1;
    config.nprobe = kNprobe;
    config.enable_dynamic_safeout = true;
    config.safeout_epsilon_override = 0.0f;

    OverlapScheduler scheduler(*index_, reader, config);
    const float* query = vectors_.data();
    auto results = scheduler.Search(query);
    const auto& stats = results.stats();

    ASSERT_GT(results.size(), 0u);
    EXPECT_GT(stats.total_safeout_frontier_estimates_buffered, 0u);
    EXPECT_GT(stats.total_safeout_frontier_updates, 0u);
    EXPECT_GT(stats.total_safe_out + stats.s2_safe_out, 0u);
}

TEST_F(OverlapSchedulerTest, DynamicSafeOutDoesNotBufferFrontierWhenDisabled) {
    PreadFallbackReader reader;

    SearchConfig config;
    config.top_k = 1;
    config.nprobe = kNprobe;
    config.enable_dynamic_safeout = false;
    config.safeout_epsilon_override = 0.0f;

    OverlapScheduler scheduler(*index_, reader, config);
    const float* query = vectors_.data();
    auto results = scheduler.Search(query);
    const auto& stats = results.stats();

    ASSERT_GT(results.size(), 0u);
    EXPECT_EQ(stats.total_safeout_frontier_estimates_buffered, 0u);
    EXPECT_EQ(stats.total_safeout_frontier_updates, 0u);
    EXPECT_EQ(stats.total_safe_out + stats.s2_safe_out, 0u);
}

TEST_F(OverlapSchedulerTest, DetailedHotpathTiming_PopulatesStats) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.enable_hotpath_detailed_timing = true;

    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());
    EXPECT_GT(results.size(), 0u);
    EXPECT_GE(results.stats().probe_submit_pending_slot_alloc_ms, 0.0);
    EXPECT_GE(results.stats().probe_submit_prep_read_ms, 0.0);
    EXPECT_GE(results.stats().rerank_vec_alloc_ms, 0.0);
    EXPECT_GE(results.stats().rerank_vec_copy_ms, 0.0);
}

TEST_F(OverlapSchedulerTest, FixedVecBufferCountFallbackPreservesPreadBehavior) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.fixed_vec_buffer_count = 128;

    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());
    EXPECT_GT(results.size(), 0u);
    EXPECT_EQ(results.stats().fixed_vec_buffer_hits, 0u);
    EXPECT_EQ(results.stats().fixed_vec_buffer_misses,
              results.stats().vec_only_read_requests);
}

TEST_F(OverlapSchedulerTest, IoUringFixedVecBufferCountCanEnableFixedHits) {
    IoUringReader reader;
    auto init_status = reader.Init(16, 64);
    if (!init_status.ok()) {
        GTEST_SKIP() << "io_uring not available: " << init_status.message();
    }

    const int data_fd = index_->segment().data_reader().fd();
    ASSERT_GE(data_fd, 0);
    auto reg_status = reader.RegisterFiles(&data_fd, 1);
    if (!reg_status.ok()) {
        GTEST_SKIP() << "file registration unavailable: " << reg_status.message();
    }

    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.clu_read_mode = CluReadMode::FullPreload;
    config.use_resident_clusters = true;
    config.io_queue_depth = 16;
    config.fixed_vec_buffer_count = 2;

    auto preload_status = index_->segment().PreloadAllClusters();
    ASSERT_TRUE(preload_status.ok()) << preload_status.message();

    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());

    EXPECT_GT(results.size(), 0u);
    EXPECT_GT(results.stats().vec_only_read_requests, 0u);
    EXPECT_GT(results.stats().fixed_vec_buffer_hits, 0u);
}

// ============================================================================
// Phase 8: Async cluster prefetch tests
// ============================================================================

TEST_F(OverlapSchedulerTest, PrefetchConfig_SmallDepth) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.probe_batch_size = 64;
    config.prefetch_depth = 4;
    config.refill_threshold = 1;
    config.refill_count = 1;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(456);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 5; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        ASSERT_GT(results.size(), 0u) << "Query " << q;

        // Sorted check
        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance);
        }
    }
}

TEST_F(OverlapSchedulerTest, PrefetchDepth_ExceedsNprobe) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 100;  // >> nprobe=4
    config.refill_threshold = 2;
    config.refill_count = 2;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(789);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    for (int q = 0; q < 3; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        ASSERT_GT(results.size(), 0u) << "Query " << q;

        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance);
        }
    }
}

TEST_F(OverlapSchedulerTest, MultipleQueries_StateReset) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;

    OverlapScheduler scheduler(*index_, reader, config);

    std::mt19937 rng(999);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    // Run 10 sequential queries on the same scheduler — verify state resets
    for (int q = 0; q < 10; ++q) {
        std::vector<float> query(kDim);
        for (auto& v : query) v = dist(rng);

        auto results = scheduler.Search(query.data());
        ASSERT_GT(results.size(), 0u) << "Query " << q;

        for (uint32_t i = 1; i < results.size(); ++i) {
            EXPECT_LE(results[i - 1].distance, results[i].distance);
        }
    }
}

TEST_F(OverlapSchedulerTest, MultipleQueriesUseFixedProbePath) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;
    config.refill_threshold = 2;
    config.refill_count = 2;

    OverlapScheduler scheduler(*index_, reader, config);

    for (uint32_t q = 0; q < 6; ++q) {
        const float* query = vectors_.data() + static_cast<size_t>(q) * kDim;
        auto results = scheduler.Search(query);
        ASSERT_GT(results.size(), 0u) << "Query " << q;
        EXPECT_GT(results.stats().total_submit_calls, 0u);
    }
}

TEST_F(OverlapSchedulerTest, SubmitBatchingAndReservePopulateStats) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;
    config.refill_threshold = 2;
    config.refill_count = 2;
    config.io_queue_depth = 32;
    config.cluster_submit_reserve = 4;
    config.submit_batch_size = 8;

    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());
    EXPECT_GT(results.size(), 0u);
    EXPECT_GT(results.stats().total_submit_calls, 0u);
    EXPECT_GE(results.stats().uring_submit_ms, 0.0);

    for (uint32_t i = 1; i < results.size(); ++i) {
        EXPECT_LE(results[i - 1].distance, results[i].distance);
    }
}

TEST_F(OverlapSchedulerTest, SharedAndIsolatedModesProduceSameResults) {
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;
    config.refill_threshold = 2;
    config.refill_count = 2;
    config.io_queue_depth = 32;
    config.cluster_submit_reserve = 4;
    config.submit_batch_size = 8;

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    PreadFallbackReader shared_reader;
    OverlapScheduler shared_scheduler(*index_, shared_reader, config);
    auto shared_results = shared_scheduler.Search(query.data());

    PreadFallbackReader cluster_reader;
    PreadFallbackReader data_reader;
    config.submission_mode = SubmissionMode::Isolated;
    OverlapScheduler isolated_scheduler(*index_, cluster_reader, data_reader, config);
    auto isolated_results = isolated_scheduler.Search(query.data());

    ASSERT_EQ(shared_results.size(), isolated_results.size());
    for (uint32_t i = 0; i < shared_results.size(); ++i) {
        EXPECT_FLOAT_EQ(shared_results[i].distance, isolated_results[i].distance);
    }
}

TEST_F(OverlapSchedulerTest, WindowAndFullPreloadModesProduceSameResults) {
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.prefetch_depth = 4;
    config.refill_threshold = 2;
    config.refill_count = 2;
    config.io_queue_depth = 32;
    config.cluster_submit_reserve = 4;
    config.submit_batch_size = 8;

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    PreadFallbackReader window_reader;
    OverlapScheduler window_scheduler(*index_, window_reader, config);
    auto window_results = window_scheduler.Search(query.data());

    PreadFallbackReader preload_reader;
    config.clu_read_mode = CluReadMode::FullPreload;
    OverlapScheduler preload_scheduler(*index_, preload_reader, config);
    auto preload_results = preload_scheduler.Search(query.data());

    ASSERT_EQ(window_results.size(), preload_results.size());
    for (uint32_t i = 0; i < window_results.size(); ++i) {
        EXPECT_FLOAT_EQ(window_results[i].distance, preload_results[i].distance);
    }
    EXPECT_EQ(preload_results.stats().parse_cluster_ms, 0.0);
    EXPECT_TRUE(index_->segment().resident_preload_enabled());
    EXPECT_GT(index_->segment().resident_preload_bytes(), 0u);
    if (preload_results.stats().stage2_masked_kernel_calls > 0) {
        EXPECT_GT(preload_results.stats().stage2_lanes_total_valid, 0u);
        EXPECT_GT(preload_results.stats().stage2_lanes_requested, 0u);
        EXPECT_EQ(preload_results.stats().stage2_lanes_requested +
                  preload_results.stats().stage2_lanes_skipped,
                  preload_results.stats().stage2_lanes_total_valid);
    }
}

TEST_F(OverlapSchedulerTest, RedundantAssignmentDeduplicatesBeforeRerank) {
    auto ra_dir = fs::temp_directory_path() / "vdb_scheduler_test_ra";
    fs::remove_all(ra_dir);
    fs::create_directories(ra_dir);

    IvfBuilderConfig cfg;
    cfg.nlist = kNlist;
    cfg.max_iterations = 20;
    cfg.seed = 42;
    cfg.rabitq.c_factor = 5.75f;
    cfg.calibration_samples = 50;
    cfg.calibration_topk = kTopK;
    cfg.epsilon_samples = 20;
    cfg.page_size = 1;
    cfg.assignment_factor = 2;
    cfg.assignment_mode = AssignmentMode::RedundantTop2Naive;

    IvfBuilder builder(cfg);
    auto s = builder.Build(vectors_.data(), N, kDim, ra_dir.string());
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex ra_index;
    s = ra_index.Open(ra_dir.string());
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(ra_index.assignment_factor(), 2u);

    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;

    OverlapScheduler scheduler(ra_index, reader, config);
    const float* query = vectors_.data();
    auto results = scheduler.Search(query);

    ASSERT_GT(results.size(), 0u);
    EXPECT_GT(results.stats().duplicate_candidates, 0u);
    EXPECT_GT(results.stats().deduplicated_candidates, 0u);
    EXPECT_EQ(results.stats().total_reranked,
              results.stats().unique_fetch_candidates);

    fs::remove_all(ra_dir);
}

TEST_F(OverlapSchedulerTest, RairAssignmentDeduplicatesBeforeRerank) {
    auto ra_dir = fs::temp_directory_path() / "vdb_scheduler_test_rair";
    fs::remove_all(ra_dir);
    fs::create_directories(ra_dir);

    IvfBuilderConfig cfg;
    cfg.nlist = kNlist;
    cfg.max_iterations = 20;
    cfg.seed = 42;
    cfg.rabitq.c_factor = 5.75f;
    cfg.calibration_samples = 50;
    cfg.calibration_topk = kTopK;
    cfg.epsilon_samples = 20;
    cfg.page_size = 1;
    cfg.assignment_factor = 2;
    cfg.assignment_mode = AssignmentMode::RedundantTop2Rair;
    cfg.rair_lambda = 0.75f;

    IvfBuilder builder(cfg);
    auto s = builder.Build(vectors_.data(), N, kDim, ra_dir.string());
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex ra_index;
    s = ra_index.Open(ra_dir.string());
    ASSERT_TRUE(s.ok()) << s.message();
    EXPECT_EQ(ra_index.assignment_mode(), AssignmentMode::RedundantTop2Rair);
    EXPECT_EQ(ra_index.assignment_factor(), 2u);
    EXPECT_FLOAT_EQ(ra_index.rair_lambda(), 0.75f);

    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;

    OverlapScheduler scheduler(ra_index, reader, config);
    const float* query = vectors_.data();
    auto results = scheduler.Search(query);

    ASSERT_GT(results.size(), 0u);
    EXPECT_GT(results.stats().duplicate_candidates, 0u);
    EXPECT_GT(results.stats().deduplicated_candidates, 0u);
    EXPECT_EQ(results.stats().total_reranked,
              results.stats().unique_fetch_candidates);

    fs::remove_all(ra_dir);
}
