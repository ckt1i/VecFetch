#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <numeric>
#include <random>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "vdb/common/distance.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"
#include "vdb/query/rerank_consumer.h"
#include "vdb/storage/hot_record.h"

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

    float ReadInitialDynamicSafeInFrontierThreshold() {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.enable_dynamic_safeout = false;
        config.dynamic_safein_mode = DynamicSafeInMode::Frontier;
        config.dynamic_safein_min_probes = 0;

        OverlapScheduler scheduler(*index_, reader, config);
        return scheduler.SafeInThresholdForProbe();
    }

    float BuildDynamicSafeInFrontierThreshold(uint32_t* ready_transitions) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.enable_dynamic_safeout = false;
        config.dynamic_safein_mode = DynamicSafeInMode::Frontier;
        config.dynamic_safein_min_probes = 0;
        config.dynamic_safein_stable_probes = 1;

        OverlapScheduler scheduler(*index_, reader, config);
        scheduler.est_top_k_ = 3;
        scheduler.AddSafeOutFrontierEstimate({1.0f, 0.0f});
        scheduler.AddSafeOutFrontierEstimate({2.0f, 0.0f});
        scheduler.AddSafeOutFrontierEstimate({3.0f, 0.0f});
        scheduler.AddSafeInFrontierEstimate(1.0f);
        scheduler.AddSafeInFrontierEstimate(2.0f);
        scheduler.AddSafeInFrontierEstimate(3.0f);

        SearchContext ctx(vectors_.data(), config);
        scheduler.UpdateDynamicSafeInState(ctx, /*advance_probe=*/true);
        *ready_transitions = ctx.stats().dynamic_safein_ready_transitions;
        return scheduler.SafeInThresholdForProbe();
    }

    uint32_t FlushDynamicSafeInDeferredPlans(
        float threshold,
        bool force,
        uint32_t* vec_only_count,
        MaterializationMode materialization_mode =
            MaterializationMode::EagerSafeIn) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.enable_dynamic_safeout = false;
        config.dynamic_safein_mode = DynamicSafeInMode::Frontier;
        config.safein_threshold_bytes = 4096;
        config.materialization_mode = materialization_mode;

        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        AddressEntry small;
        small.offset = 128;
        small.size = 512;
        AddressEntry large;
        large.offset = 1024;
        large.size = 8192;

        OverlapScheduler::DeferredSafeInPlan safein;
        safein.addr = small;
        safein.safein_upper_bound = threshold - 0.1f;
        safein.has_truth = true;
        safein.is_true_topk = true;
        scheduler.deferred_safein_plans_.push_back(safein);

        OverlapScheduler::DeferredSafeInPlan vec_only_by_bound;
        vec_only_by_bound.addr = small;
        vec_only_by_bound.safein_upper_bound = threshold + 0.1f;
        scheduler.deferred_safein_plans_.push_back(vec_only_by_bound);

        OverlapScheduler::DeferredSafeInPlan vec_only_by_size;
        vec_only_by_size.addr = large;
        vec_only_by_size.safein_upper_bound = threshold - 0.1f;
        scheduler.deferred_safein_plans_.push_back(vec_only_by_size);

        scheduler.FlushDeferredSafeInPlans(ctx, threshold, force);
        *vec_only_count =
            static_cast<uint32_t>(scheduler.pending_vec_only_plans_.size() -
                                  scheduler.pending_vec_only_head_);
        return static_cast<uint32_t>(scheduler.pending_all_plans_.size());
    }

    uint32_t ReadSafeInLength(uint32_t threshold_bytes, AddressEntry addr) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.safein_threshold_bytes = threshold_bytes;
        OverlapScheduler scheduler(*index_, reader, config);
        return scheduler.SafeInReadLength(addr);
    }

    uint32_t ReadInlineSafeInLength(uint32_t threshold_bytes,
                                    AddressEntry addr) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.safein_threshold_bytes = threshold_bytes;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        config.inline_hot_record_store.inline_payload_threshold = 0;
        OverlapScheduler scheduler(*index_, reader, config);
        return scheduler.SafeInReadLength(addr);
    }

    struct DescriptorResolutionResult {
        uint32_t descriptor_reads = 0;
        uint32_t submit_calls = 0;
        uint32_t cached_locations = 0;
        uint32_t pool_outstanding = 0;
    };

    DescriptorResolutionResult ResolveInlineDescriptorsAsOneBatch() {
        const fs::path path = test_dir_ / "descriptor_batch.dat";
        const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
        EXPECT_GE(fd, 0);
        if (fd < 0) return {};

        constexpr uint64_t kSecondOffset = 4096;
        const uint32_t vec_bytes = kDim * sizeof(float);
        const uint32_t descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        EXPECT_EQ(::ftruncate(fd, kSecondOffset + vec_bytes + descriptor_bytes), 0);

        vdb::storage::HotPayloadDescriptor desc;
        desc.payload_storage_type = static_cast<uint8_t>(
            vdb::storage::HotPayloadStorageType::kInlinePayload);
        uint8_t encoded[sizeof(desc)] = {};
        vdb::storage::EncodeHotPayloadDescriptor(desc, encoded);
        EXPECT_EQ(::pwrite(fd, encoded, sizeof(encoded), vec_bytes),
                  static_cast<ssize_t>(sizeof(encoded)));
        EXPECT_EQ(::pwrite(fd, encoded, sizeof(encoded),
                           kSecondOffset + vec_bytes),
                  static_cast<ssize_t>(sizeof(encoded)));

        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 2;
        config.nprobe = kNprobe;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes = descriptor_bytes;
        config.inline_hot_record_store.buffered_hot_record_fd = fd;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);
        std::vector<CollectorEntry> results = {
            {0.0f, AddressEntry{0, vec_bytes + descriptor_bytes}},
            {1.0f, AddressEntry{kSecondOffset, vec_bytes + descriptor_bytes}},
        };

        scheduler.ResolvePayloadLocations(ctx, reranker, results);
        DescriptorResolutionResult result;
        result.descriptor_reads = ctx.stats().inline_descriptor_read_requests;
        result.submit_calls = ctx.stats().total_submit_calls;
        result.cached_locations =
            static_cast<uint32_t>(scheduler.payload_location_cache_.size());
        result.pool_outstanding = scheduler.buffer_pool_.OutstandingCount();
        ::close(fd);
        return result;
    }

    DescriptorResolutionResult ResolveInlineMetadataWithoutReads() {
        constexpr uint64_t kSecondOffset = 4096;
        const uint32_t vec_bytes = kDim * sizeof(float);
        const uint32_t descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        InlineHotRecordStoreConfig::PayloadMetadataMap metadata;
        InlineHotRecordStoreConfig::PayloadMetadata entry;
        entry.payload_storage_type = static_cast<uint8_t>(
            vdb::storage::HotPayloadStorageType::kInlinePayload);
        metadata.emplace(0, entry);
        metadata.emplace(kSecondOffset, entry);

        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 2;
        config.nprobe = kNprobe;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes = descriptor_bytes;
        config.inline_hot_record_store.payload_metadata = &metadata;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);
        std::vector<CollectorEntry> results = {
            {0.0f, AddressEntry{0, vec_bytes + descriptor_bytes}},
            {1.0f, AddressEntry{kSecondOffset,
                                vec_bytes + descriptor_bytes}},
        };

        scheduler.ResolvePayloadLocations(ctx, reranker, results);
        DescriptorResolutionResult result;
        result.descriptor_reads = ctx.stats().inline_descriptor_read_requests;
        result.submit_calls = ctx.stats().total_submit_calls;
        result.cached_locations =
            static_cast<uint32_t>(scheduler.payload_location_cache_.size());
        result.pool_outstanding = scheduler.buffer_pool_.OutstandingCount();
        return result;
    }

    int ResolveInlinePayloadFd(int buffered_fd) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 1;
        config.nprobe = 1;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        config.inline_hot_record_store.buffered_hot_record_fd = buffered_fd;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        vdb::storage::HotPayloadDescriptor desc;
        desc.payload_storage_type = static_cast<uint8_t>(
            vdb::storage::HotPayloadStorageType::kInlinePayload);
        desc.inline_bytes = 64;
        desc.payload_bytes = 64;
        const auto location = scheduler.PayloadLocationFromDescriptorOrAbort(
            ctx, AddressEntry{4096, kDim * sizeof(float) + sizeof(desc) + 64},
            desc);
        return location.fd;
    }

    void DispatchShortCompletion(bool descriptor) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 1;
        config.nprobe = 1;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);
        const uint32_t expected = descriptor
            ? sizeof(vdb::storage::HotPayloadDescriptor)
            : 128u;
        uint8_t* buffer = scheduler.buffer_pool_.Acquire(expected);
        OverlapScheduler::PendingIO io;
        io.type = descriptor
            ? OverlapScheduler::PendingIO::Type::PAYLOAD_DESCRIPTOR
            : OverlapScheduler::PendingIO::Type::PAYLOAD;
        io.addr = AddressEntry{4096, expected};
        io.read_offset = 4096;
        io.read_length = expected;
        io.payload_total_length = expected;
        const uint32_t slot = scheduler.AllocatePendingSlot(
            io, buffer, OverlapScheduler::PendingBufferCleanup::Pool);
        scheduler.DispatchCompletion(slot, static_cast<int32_t>(expected - 1),
                                     ctx, reranker);
    }

    std::pair<uint32_t, uint32_t> DispatchVectorSpanCompletion() {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 2;
        config.nprobe = 1;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);

        const uint32_t vec_bytes = kDim * sizeof(float);
        const uint32_t second_offset = vec_bytes + 24;
        const uint32_t span_bytes = second_offset + vec_bytes;
        uint8_t* buffer = scheduler.buffer_pool_.Acquire(span_bytes);
        std::memcpy(buffer, vectors_.data(), vec_bytes);
        std::memcpy(buffer + second_offset, vectors_.data() + kDim,
                    vec_bytes);

        OverlapScheduler::PendingIO io;
        io.type = OverlapScheduler::PendingIO::Type::VEC_SPAN;
        io.addr = AddressEntry{4096, vec_bytes};
        io.read_offset = 4096;
        io.read_length = span_bytes;
        io.span_members.push_back(
            OverlapScheduler::VecSpanMember{AddressEntry{4096, vec_bytes}, 0});
        io.span_members.push_back(OverlapScheduler::VecSpanMember{
            AddressEntry{4096 + second_offset, vec_bytes}, second_offset});
        const uint32_t slot = scheduler.AllocatePendingSlot(
            std::move(io), buffer,
            OverlapScheduler::PendingBufferCleanup::Pool);
        scheduler.DispatchCompletion(slot, static_cast<int32_t>(span_bytes),
                                     ctx, reranker);
        return {reranker.BufferedCount(),
                scheduler.buffer_pool_.OutstandingCount()};
    }

    bool ShouldHoldDeferredPool(uint32_t probes_seen, size_t pool_size,
                                bool* defers_next_probe = nullptr) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.enable_dynamic_safeout = false;
        config.dynamic_safein_mode = DynamicSafeInMode::Frontier;
        config.dynamic_safein_defer_initial_clusters = 4;
        config.dynamic_safein_defer_until_ready = true;
        config.submit_batch_size = 32;

        OverlapScheduler scheduler(*index_, reader, config);
        scheduler.dynamic_safein_probes_seen_ = probes_seen;
        scheduler.dynamic_safein_ready_ = true;
        AddressEntry addr;
        addr.offset = 128;
        addr.size = 512;
        for (size_t i = 0; i < pool_size; ++i) {
            OverlapScheduler::DeferredSafeInPlan plan;
            plan.addr = addr;
            scheduler.deferred_safein_plans_.push_back(plan);
        }
        const bool hold = scheduler.ShouldHoldDeferredSafeInPlans();
        if (defers_next_probe != nullptr) {
            scheduler.dynamic_safein_probes_seen_ = probes_seen + 1;
            *defers_next_probe = scheduler.ShouldDeferSafeInPlans();
        }
        return hold;
    }

    struct BextraBudgetResult {
        size_t all_count = 0;
        size_t vec_only_count = 0;
        uint32_t admitted_extra_bytes = 0;
        uint32_t scheduled_candidates = 0;
        uint64_t scheduled_extra_bytes = 0;
        std::vector<BextraWindowTraceEntry> trace;
    };

    BextraBudgetResult RunBextraBudget(double rho,
                                       uint32_t first_extra_bytes,
                                       uint32_t second_extra_bytes,
                                       bool enable_trace = false) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = kTopK;
        config.nprobe = kNprobe;
        config.enable_safein_bextra_probe_budget = true;
        config.safein_bextra_rho = rho;
        config.safein_bextra_bytes_per_ms = 10000.0;
        std::vector<BextraWindowTraceEntry> trace;
        if (enable_trace) {
            config.bextra_window_trace = &trace;
        }

        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        const uint32_t vector_bytes = kDim * sizeof(float);
        for (const uint32_t extra_bytes :
             {first_extra_bytes, second_extra_bytes}) {
            OverlapScheduler::ReadPlanEntry plan;
            plan.addr.offset = scheduler.pending_all_plans_.size() * 4096;
            plan.addr.size = vector_bytes + extra_bytes;
            plan.read_length = plan.addr.size;
            scheduler.pending_all_plans_.push_back(plan);
        }

        scheduler.bextra_probe_cluster_ema_ms_ = 1.0;
        scheduler.ApplyBextraWindowBudget(ctx, 1, 1);

        BextraBudgetResult result;
        result.all_count = scheduler.pending_all_plans_.size();
        result.vec_only_count = scheduler.pending_vec_only_plans_.size();
        if (!scheduler.pending_all_plans_.empty()) {
            result.admitted_extra_bytes =
                scheduler.pending_all_plans_.front().bextra_extra_bytes;
        }
        result.scheduled_candidates = ctx.stats().bextra_scheduled_candidates;
        result.scheduled_extra_bytes = ctx.stats().bextra_scheduled_extra_bytes;
        result.trace = std::move(trace);
        return result;
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

TEST_F(OverlapSchedulerTest, DynamicSafeInFrontierDisablesUntilFrontierExists) {
    const float initial = ReadInitialDynamicSafeInFrontierThreshold();
    EXPECT_TRUE(std::isinf(initial));
    EXPECT_LT(initial, 0.0f);

    uint32_t ready_transitions = 0;
    const float threshold =
        BuildDynamicSafeInFrontierThreshold(&ready_transitions);
    EXPECT_TRUE(std::isfinite(threshold));
    EXPECT_FLOAT_EQ(threshold, 3.0f);
    EXPECT_EQ(ready_transitions, 1u);
}

TEST_F(OverlapSchedulerTest, DynamicSafeInDeferredFlushReclassifiesByFrontier) {
    uint32_t vec_only_count = 0;
    const uint32_t all_count =
        FlushDynamicSafeInDeferredPlans(4.0f, /*force=*/false,
                                        &vec_only_count);

    EXPECT_EQ(all_count, 2u);
    EXPECT_EQ(vec_only_count, 1u);
}

TEST_F(OverlapSchedulerTest, SafeInReadLengthUsesPrefixThresholdAndVectorMinimum) {
    AddressEntry large;
    large.offset = 1024;
    large.size = kDim * sizeof(float) + 8192;
    EXPECT_EQ(ReadSafeInLength(0, large), kDim * sizeof(float));
    EXPECT_EQ(ReadSafeInLength(4096, large), 4096u);

    AddressEntry small;
    small.offset = 2048;
    small.size = 512;
    EXPECT_EQ(ReadSafeInLength(4096, small), 512u);
}

TEST_F(OverlapSchedulerTest, InlineSafeInReadLengthUsesWholeHotRecord) {
    AddressEntry cold_hot_record;
    cold_hot_record.offset = 4096;
    cold_hot_record.size = kDim * sizeof(float) +
        sizeof(vdb::storage::HotPayloadDescriptor);

    EXPECT_EQ(ReadInlineSafeInLength(0, cold_hot_record),
              cold_hot_record.size);
    EXPECT_EQ(ReadInlineSafeInLength(16, cold_hot_record),
              cold_hot_record.size);
}

TEST_F(OverlapSchedulerTest, ResolvesUntouchedInlineDescriptorsInOneBatch) {
    const DescriptorResolutionResult result = ResolveInlineDescriptorsAsOneBatch();

    EXPECT_EQ(result.descriptor_reads, 2u);
    EXPECT_EQ(result.submit_calls, 1u);
    EXPECT_EQ(result.cached_locations, 2u);
    EXPECT_EQ(result.pool_outstanding, 0u);
}

TEST_F(OverlapSchedulerTest, ResidentInlineMetadataAvoidsDescriptorIo) {
    const DescriptorResolutionResult result =
        ResolveInlineMetadataWithoutReads();

    EXPECT_EQ(result.descriptor_reads, 0u);
    EXPECT_EQ(result.submit_calls, 0u);
    EXPECT_EQ(result.cached_locations, 2u);
    EXPECT_EQ(result.pool_outstanding, 0u);
}

TEST_F(OverlapSchedulerTest, InlinePayloadUsesBufferedHotRecordFd) {
    EXPECT_EQ(ResolveInlinePayloadFd(123), 123);
}

TEST_F(OverlapSchedulerTest, RejectsShortDescriptorCompletion) {
    EXPECT_DEATH(DispatchShortCompletion(/*descriptor=*/true),
                 "short or failed async read");
}

TEST_F(OverlapSchedulerTest, RejectsShortFinalPayloadCompletion) {
    EXPECT_DEATH(DispatchShortCompletion(/*descriptor=*/false),
                 "short or failed async read");
}

TEST_F(OverlapSchedulerTest, VectorSpanCompletionConsumesAllMembersAndReleasesBuffer) {
    const auto [buffered, outstanding] = DispatchVectorSpanCompletion();
    EXPECT_EQ(buffered, 2u);
    EXPECT_EQ(outstanding, 0u);
}

TEST_F(OverlapSchedulerTest, LateMaterializationDeferredSafeInUsesVecOnly) {
    uint32_t vec_only_count = 0;
    const uint32_t all_count =
        FlushDynamicSafeInDeferredPlans(
            4.0f, /*force=*/false, &vec_only_count,
            MaterializationMode::Late);

    EXPECT_EQ(all_count, 0u);
    EXPECT_EQ(vec_only_count, 3u);
}

TEST_F(OverlapSchedulerTest, DynamicSafeInDeferredSmallPoolHoldsOneExtraCluster) {
    bool defers_next_probe = false;
    EXPECT_TRUE(ShouldHoldDeferredPool(/*probes_seen=*/4, /*pool_size=*/64,
                                      &defers_next_probe));
    EXPECT_TRUE(defers_next_probe);
    EXPECT_FALSE(ShouldHoldDeferredPool(/*probes_seen=*/4, /*pool_size=*/128,
                                       &defers_next_probe));
    EXPECT_FALSE(defers_next_probe);
    EXPECT_FALSE(ShouldHoldDeferredPool(/*probes_seen=*/5, /*pool_size=*/64));
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
// Resident full-preload query path tests
// ============================================================================

TEST_F(OverlapSchedulerTest, ResidentPathMultipleRandomQueries) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.probe_batch_size = 64;

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

TEST_F(OverlapSchedulerTest, ResidentPathAutoPreloads) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;

    EXPECT_FALSE(index_->segment().resident_preload_enabled());
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
    EXPECT_TRUE(index_->segment().resident_preload_enabled());
    EXPECT_GT(index_->segment().resident_preload_bytes(), 0u);
}

TEST_F(OverlapSchedulerTest, MultipleQueries_StateReset) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;

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

TEST_F(OverlapSchedulerTest, SerialNoOverlapMatchesOverlapAndUsesSynchronousReads) {
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.io_queue_depth = 32;
    config.cluster_submit_reserve = 4;
    config.submit_batch_size = 8;

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    PreadFallbackReader overlap_reader;
    OverlapScheduler overlap_scheduler(*index_, overlap_reader, config);
    auto overlap_results = overlap_scheduler.Search(query.data());

    SearchConfig serial_config = config;
    serial_config.execution_mode = QueryExecutionMode::SerialNoOverlap;
    serial_config.budgeted_prefetch_limit = 4;
    PreadFallbackReader serial_reader;
    OverlapScheduler serial_scheduler(*index_, serial_reader, serial_config);
    auto serial_results = serial_scheduler.Search(query.data());

    ASSERT_EQ(overlap_results.size(), serial_results.size());
    for (uint32_t i = 0; i < overlap_results.size(); ++i) {
        EXPECT_FLOAT_EQ(overlap_results[i].distance, serial_results[i].distance);
    }

    const auto& overlap_stats = overlap_results.stats();
    const auto& serial_stats = serial_results.stats();
    EXPECT_EQ(overlap_stats.total_probed, serial_stats.total_probed);
    EXPECT_EQ(overlap_stats.total_safe_in, serial_stats.total_safe_in);
    EXPECT_EQ(overlap_stats.total_safe_out, serial_stats.total_safe_out);
    EXPECT_EQ(overlap_stats.total_uncertain, serial_stats.total_uncertain);
    EXPECT_EQ(overlap_stats.reranked_candidates, serial_stats.reranked_candidates);
    EXPECT_EQ(overlap_stats.vec_only_read_requests, serial_stats.vec_only_read_requests);
    EXPECT_EQ(overlap_stats.all_read_requests, serial_stats.all_read_requests);
    EXPECT_EQ(0u, serial_stats.total_submit_calls);
    EXPECT_EQ(0u, serial_stats.total_io_submitted);
    EXPECT_EQ(0.0, serial_stats.io_wait_time_ms);
    EXPECT_GT(serial_stats.serial_vector_read_requests +
              serial_stats.serial_full_record_read_requests, 0u);
    EXPECT_EQ(0u, serial_stats.budgeted_prefetch_scheduled);
}

TEST_F(OverlapSchedulerTest, ResidentPathUsesPreloadedClusters) {
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.io_queue_depth = 32;
    config.cluster_submit_reserve = 4;
    config.submit_batch_size = 8;

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    PreadFallbackReader preload_reader;
    OverlapScheduler preload_scheduler(*index_, preload_reader, config);
    auto preload_results = preload_scheduler.Search(query.data());

    ASSERT_GT(preload_results.size(), 0u);
    for (uint32_t i = 1; i < preload_results.size(); ++i) {
        EXPECT_LE(preload_results[i - 1].distance, preload_results[i].distance);
    }
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

TEST_F(OverlapSchedulerTest, BextraZeroBudgetDowngradesAllSafeInPlans) {
    const auto result = RunBextraBudget(0.0, 1024, 1024);

    EXPECT_EQ(0u, result.all_count);
    EXPECT_EQ(2u, result.vec_only_count);
    EXPECT_EQ(0u, result.scheduled_candidates);
}

TEST_F(OverlapSchedulerTest, BextraBudgetAdmitsOnlyPlansThatFit) {
    const auto result = RunBextraBudget(1.0, 1000, 20000,
                                        /*enable_trace=*/true);

    EXPECT_EQ(1u, result.all_count);
    EXPECT_EQ(1u, result.vec_only_count);
    EXPECT_EQ(1000u, result.admitted_extra_bytes);
    EXPECT_EQ(1u, result.scheduled_candidates);
    EXPECT_EQ(1000u, result.scheduled_extra_bytes);
    ASSERT_EQ(1u, result.trace.size());
    EXPECT_EQ(2u, result.trace[0].eligible_candidates);
    EXPECT_EQ(1u, result.trace[0].scheduled_candidates);
}
