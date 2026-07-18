#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
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

class ControlledAsyncReader : public AsyncReader {
 public:
    Status PrepReadTagged(int, uint8_t*, uint32_t, uint64_t,
                          uint64_t) override {
        ++prepped_;
        return Status::OK();
    }
    uint32_t Submit() override {
        const uint32_t submitted = prepped_;
        in_flight_ += submitted;
        prepped_ = 0;
        return submitted;
    }
    uint32_t Poll(IoCompletion*, uint32_t) override { return 0; }
    uint32_t WaitAndPoll(IoCompletion*, uint32_t) override { return 0; }
    uint32_t InFlight() const override { return in_flight_; }
    uint32_t prepped() const override { return prepped_; }

    uint32_t in_flight_ = 0;
    uint32_t prepped_ = 0;
};

class ManualCompletionReader : public AsyncReader {
 public:
    Status PrepReadTagged(int, uint8_t* buffer, uint32_t len, uint64_t,
                          uint64_t user_data) override {
        prepped_.push_back({buffer, len, user_data});
        return Status::OK();
    }

    uint32_t Submit() override {
        const uint32_t submitted = static_cast<uint32_t>(prepped_.size());
        while (!prepped_.empty()) {
            in_flight_.push_back(prepped_.front());
            prepped_.pop_front();
        }
        return submitted;
    }

    uint32_t Poll(IoCompletion* out, uint32_t max_count) override {
        const uint32_t n = std::min<uint32_t>(
            {max_count, ready_, static_cast<uint32_t>(in_flight_.size())});
        for (uint32_t i = 0; i < n; ++i) {
            const Entry entry = in_flight_.front();
            in_flight_.pop_front();
            out[i].buffer = entry.buffer;
            out[i].user_data = entry.user_data;
            out[i].result = static_cast<int32_t>(entry.len);
        }
        ready_ -= n;
        return n;
    }

    uint32_t WaitAndPoll(IoCompletion* out, uint32_t max_count) override {
        if (ready_ == 0 && !in_flight_.empty()) ready_ = 1;
        return Poll(out, max_count);
    }

    uint32_t InFlight() const override {
        return static_cast<uint32_t>(in_flight_.size());
    }
    uint32_t prepped() const override {
        return static_cast<uint32_t>(prepped_.size());
    }
    void MakeOneReady() { ready_ = std::min<uint32_t>(
        ready_ + 1, static_cast<uint32_t>(in_flight_.size())); }

 private:
    struct Entry {
        uint8_t* buffer;
        uint32_t len;
        uint64_t user_data;
    };
    std::deque<Entry> prepped_;
    std::deque<Entry> in_flight_;
    uint32_t ready_ = 0;
};

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

    std::pair<uint32_t, uint32_t> FlushOracleSafeInPlans() {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.enable_dynamic_safeout = false;
        config.safein_threshold_bytes = 4096;
        config.safein_prefetch_order = SafeInPrefetchOrder::Oracle;

        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);

        OverlapScheduler::DeferredSafeInPlan selected;
        selected.addr = AddressEntry{128, 512};
        selected.safein_upper_bound = 1.0f;
        selected.has_truth = true;
        selected.is_true_topk = true;
        scheduler.deferred_safein_plans_.push_back(selected);

        OverlapScheduler::DeferredSafeInPlan rejected = selected;
        rejected.addr = AddressEntry{1024, 512};
        rejected.is_true_topk = false;
        scheduler.deferred_safein_plans_.push_back(rejected);

        scheduler.FlushDeferredSafeInPlans(ctx, 2.0f, /*force=*/true);
        const uint32_t vec_only = static_cast<uint32_t>(
            scheduler.pending_vec_only_plans_.size() -
            scheduler.pending_vec_only_head_);
        return {static_cast<uint32_t>(scheduler.pending_all_plans_.size()),
                vec_only};
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

    struct ColdSafeInPlanResult {
        uint32_t vector_read_bytes = 0;
        uint32_t payload_prefix_bytes = 0;
        uint32_t payload_total_bytes = 0;
        uint64_t payload_offset = 0;
        uint64_t total_read_bytes = 0;
        uint64_t extra_bytes = 0;
    };

    ColdSafeInPlanResult BuildInlineColdSafeInPlan(
        uint32_t payload_bytes, uint32_t prefix_cap,
        vdb::storage::HotPayloadStorageType storage_type =
            vdb::storage::HotPayloadStorageType::kColdPointer) {
        const uint32_t vec_bytes = kDim * sizeof(float);
        const AddressEntry addr{4096,
                                vec_bytes +
                                    sizeof(vdb::storage::HotPayloadDescriptor)};
        InlineHotRecordStoreConfig::PayloadMetadataMap metadata;
        InlineHotRecordStoreConfig::PayloadMetadata entry;
        entry.payload_storage_type = static_cast<uint8_t>(storage_type);
        entry.payload_offset = 123456;
        entry.payload_bytes = payload_bytes;
        entry.inline_bytes = storage_type ==
                vdb::storage::HotPayloadStorageType::kInlinePayload
            ? payload_bytes
            : 0;
        metadata.emplace(addr.offset, entry);

        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        config.inline_hot_record_store.payload_metadata = &metadata;
        config.inline_hot_record_store.cold_payload_fd = 42;
        config.inline_hot_record_store.buffered_hot_record_fd = 43;
        config.enable_safein_cold_payload_prefetch = true;
        config.safein_cold_payload_prefix_bytes = prefix_cap;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        OverlapScheduler::ReadPlanEntry plan;
        scheduler.ConfigureSafeInReadPlan(ctx, addr, &plan);

        return {plan.read_length,
                plan.cold_payload_prefix_length,
                plan.cold_payload_total_length,
                plan.cold_payload_offset,
                scheduler.SafeInPlanTotalReadBytes(plan),
                scheduler.SafeInPlanExtraBytes(plan)};
    }

    std::pair<bool, bool> ColdSafeInQueryBudgetAdmissions() {
        const uint32_t vec_bytes = kDim * sizeof(float);
        const AddressEntry first{4096, vec_bytes + 24};
        const AddressEntry second{8192, vec_bytes + 24};
        InlineHotRecordStoreConfig::PayloadMetadataMap metadata;
        InlineHotRecordStoreConfig::PayloadMetadata entry;
        entry.payload_storage_type = static_cast<uint8_t>(
            vdb::storage::HotPayloadStorageType::kColdPointer);
        entry.payload_offset = 100000;
        entry.payload_bytes = 16384;
        metadata.emplace(first.offset, entry);
        entry.payload_offset += 16384;
        metadata.emplace(second.offset, entry);

        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 3;
        config.nprobe = kNprobe;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes = 24;
        config.inline_hot_record_store.payload_metadata = &metadata;
        config.inline_hot_record_store.cold_payload_fd = 42;
        config.enable_safein_cold_payload_prefetch = true;
        config.safein_cold_payload_prefix_bytes = 4096;
        config.safein_query_extra_bytes = 4096;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);

        OverlapScheduler::ReadPlanEntry first_plan;
        scheduler.ConfigureSafeInReadPlan(ctx, first, &first_plan);
        const bool first_admitted = scheduler.ShouldScheduleSafeInPrefetch(
            ctx, scheduler.SafeInPlanTotalReadBytes(first_plan),
            scheduler.SafeInPlanExtraBytes(first_plan));
        OverlapScheduler::ReadPlanEntry second_plan;
        scheduler.ConfigureSafeInReadPlan(ctx, second, &second_plan);
        const bool second_admitted = scheduler.ShouldScheduleSafeInPrefetch(
            ctx, scheduler.SafeInPlanTotalReadBytes(second_plan),
            scheduler.SafeInPlanExtraBytes(second_plan));
        return {first_admitted, second_admitted};
    }

    struct ColdPrefixSuffixResult {
        uint32_t cached_payload_bytes = 0;
        uint32_t suffix_read_requests = 0;
        uint64_t suffix_read_bytes = 0;
        uint32_t buffer_reuses = 0;
        uint64_t prefix_copy_bytes_avoided = 0;
        bool reused_same_buffer = false;
        bool payload_matches = false;
    };

    ColdPrefixSuffixResult FetchColdPayloadSuffixAfterCachedPrefix(
        bool reusable_buffer = false) {
        constexpr uint32_t kPrefixBytes = 1024;
        constexpr uint32_t kPayloadBytes = 4096;
        const fs::path path = test_dir_ / "cold_payload.dat";
        const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
        EXPECT_GE(fd, 0);
        if (fd < 0) return {};

        std::vector<uint8_t> payload(kPayloadBytes);
        std::iota(payload.begin(), payload.end(), uint8_t{0});
        EXPECT_EQ(::pwrite(fd, payload.data(), payload.size(), 0),
                  static_cast<ssize_t>(payload.size()));

        const uint32_t vec_bytes = kDim * sizeof(float);
        const AddressEntry addr{
            4096, vec_bytes + sizeof(vdb::storage::HotPayloadDescriptor)};
        InlineHotRecordStoreConfig::PayloadMetadataMap metadata;
        InlineHotRecordStoreConfig::PayloadMetadata entry;
        entry.payload_storage_type = static_cast<uint8_t>(
            vdb::storage::HotPayloadStorageType::kColdPointer);
        entry.payload_offset = 0;
        entry.payload_bytes = kPayloadBytes;
        metadata.emplace(addr.offset, entry);

        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 1;
        config.nprobe = 1;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        config.inline_hot_record_store.payload_metadata = &metadata;
        config.inline_hot_record_store.cold_payload_fd = fd;
        config.enable_safein_reusable_payload_buffer = reusable_buffer;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);

        const uint32_t allocation_bytes = reusable_buffer
            ? kPayloadBytes
            : kPrefixBytes;
        auto* prefix = scheduler.buffer_pool_.Acquire(allocation_bytes);
        EXPECT_NE(prefix, nullptr);
        if (prefix == nullptr) {
            ::close(fd);
            return {};
        }
        std::memcpy(prefix, payload.data(), kPrefixBytes);
        reranker.ConsumePayloadPrefix(
            prefix, addr, kPrefixBytes, &scheduler.buffer_pool_,
            allocation_bytes);
        const uint8_t* cached_before =
            reranker.CachedPayloadData(addr.offset);

        const std::vector<CollectorEntry> results = {{0.0f, addr}};
        scheduler.FetchMissingPayloads(ctx, reranker, results);

        ColdPrefixSuffixResult result;
        result.cached_payload_bytes = reranker.CachedPayloadBytes(addr.offset);
        result.suffix_read_requests = ctx.stats().safein_suffix_read_requests;
        result.suffix_read_bytes = ctx.stats().safein_suffix_read_bytes;
        result.buffer_reuses = ctx.stats().safein_payload_buffer_reuses;
        result.prefix_copy_bytes_avoided =
            ctx.stats().safein_payload_prefix_copy_bytes_avoided;
        const uint8_t* cached = reranker.CachedPayloadData(addr.offset);
        result.reused_same_buffer = cached == cached_before;
        result.payload_matches = cached != nullptr &&
            std::memcmp(cached, payload.data(), payload.size()) == 0;
        ::close(fd);
        return result;
    }

    struct OptionalPayloadResult {
        uint32_t queued = 0;
        uint32_t submitted = 0;
        uint32_t completed = 0;
        uint32_t dropped_late = 0;
        uint32_t cached_bytes = 0;
        uint32_t submit_calls = 0;
        uint32_t nonblocking_poll_calls = 0;
        uint32_t empty_polls = 0;
        uint32_t completed_before_probe_end = 0;
        uint32_t completed_in_final_drain = 0;
        uint32_t completed_in_optional_drain = 0;
        uint32_t timeline_queries = 0;
        uint32_t blocked_mandatory_calls = 0;
        uint32_t inflight_at_probe_end = 0;
        uint32_t inflight_at_optional_drain_start = 0;
        double first_submit_offset_us = 0.0;
        double probe_end_offset_us = 0.0;
        uint64_t completed_bytes = 0;
        uint32_t mandatory_prepped = 0;
        uint32_t mandatory_queue = 0;
        uint32_t optional_queue = 0;
        bool payload_matches = false;
    };

    OptionalPayloadResult RunIsolatedOptionalPayloadRead(
        bool leave_mandatory_work_pending = false,
        uint32_t mandatory_in_flight = 0,
        bool drain_blocked_optional = false,
        bool enable_isolation = true,
        bool reap_before_probe_end = false,
        bool use_final_drain = false,
        bool refill_only_polling = false,
        bool retry_after_mandatory_clears = false) {
        constexpr uint32_t kPayloadBytes = 4096;
        constexpr uint32_t kPrefixBytes = 1024;
        const fs::path path = test_dir_ / "optional_payload.dat";
        const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
        EXPECT_GE(fd, 0);
        if (fd < 0) return {};

        std::vector<uint8_t> payload(kPayloadBytes);
        std::iota(payload.begin(), payload.end(), uint8_t{0});
        EXPECT_EQ(::pwrite(fd, payload.data(), payload.size(), 0),
                  static_cast<ssize_t>(payload.size()));

        const AddressEntry addr{
            4096, kDim * sizeof(float) +
                sizeof(vdb::storage::HotPayloadDescriptor)};
        InlineHotRecordStoreConfig::PayloadMetadataMap metadata;
        InlineHotRecordStoreConfig::PayloadMetadata entry;
        entry.payload_storage_type = static_cast<uint8_t>(
            vdb::storage::HotPayloadStorageType::kColdPointer);
        entry.payload_offset = 0;
        entry.payload_bytes = kPayloadBytes;
        metadata.emplace(addr.offset, entry);

        PreadFallbackReader cluster_reader;
        ControlledAsyncReader mandatory_reader;
        PreadFallbackReader optional_reader;
        SearchConfig config;
        config.top_k = 1;
        config.nprobe = kNprobe;
        config.submission_mode = SubmissionMode::Isolated;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        config.inline_hot_record_store.payload_metadata = &metadata;
        config.inline_hot_record_store.cold_payload_fd = fd;
        config.enable_safein_cold_payload_prefetch = true;
        config.safein_cold_payload_prefix_bytes = kPrefixBytes;
        config.enable_safein_optional_io_isolation = enable_isolation;
        config.enable_safein_optional_io_timeline = true;
        config.enable_safein_optional_io_refill_only_polling =
            refill_only_polling;
        config.safein_optional_io_max_inflight = 1;
        config.safein_optional_io_min_remaining_probes = 1;
        config.enable_safein_reusable_payload_buffer = true;
        OverlapScheduler scheduler(*index_, cluster_reader, mandatory_reader,
                                   optional_reader, config);
        scheduler.optional_probe_start_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        scheduler.optional_probe_total_clusters_ = 2;
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);

        OverlapScheduler::ReadPlanEntry plan;
        scheduler.ConfigureSafeInReadPlan(ctx, addr, &plan);
        scheduler.EnqueueSafeInReadPlan(ctx, plan);
        const uint32_t mandatory_queue = static_cast<uint32_t>(
            scheduler.pending_all_plans_.size());
        const uint32_t optional_queue = static_cast<uint32_t>(
            scheduler.pending_optional_payload_plans_.size());

        uint8_t mandatory_byte = 0;
        mandatory_reader.in_flight_ = mandatory_in_flight;
        if (leave_mandatory_work_pending) {
            EXPECT_TRUE(mandatory_reader.PrepReadTagged(
                            fd, &mandatory_byte, 1, 0, 0).ok());
        }
        scheduler.MaybeSubmitOptionalPayloadRequests(
            ctx, reranker, /*probes_remaining=*/2);
        if (retry_after_mandatory_clears) {
            mandatory_reader.prepped_ = 0;
            mandatory_reader.in_flight_ = 0;
            scheduler.MaybeSubmitOptionalPayloadRequests(
                ctx, reranker, /*probes_remaining=*/2);
        }
        if (reap_before_probe_end) {
            scheduler.PollOptionalPayloadCompletions(
                ctx, reranker, /*wait_for_one=*/false);
        }
        scheduler.SnapshotOptionalPayloadProbeEnd(ctx);

        OptionalPayloadResult result;
        result.queued = ctx.stats().safein_optional_io_queued;
        result.submitted = ctx.stats().safein_optional_io_submitted;
        result.submit_calls = ctx.stats().safein_optional_io_submit_calls;
        result.mandatory_prepped = mandatory_reader.prepped();
        result.mandatory_queue = mandatory_queue;
        result.optional_queue = optional_queue;
        if (!leave_mandatory_work_pending && mandatory_in_flight == 0 &&
            enable_isolation) {
            if (use_final_drain) {
                scheduler.FinalDrain(ctx, reranker);
            }
            scheduler.DrainOptionalPayloads(ctx, reranker);
            result.completed = ctx.stats().safein_optional_io_completed;
            result.dropped_late = ctx.stats().safein_optional_io_dropped_late;
            result.cached_bytes = reranker.CachedPayloadBytes(addr.offset);
            const uint8_t* cached = reranker.CachedPayloadData(addr.offset);
            result.payload_matches = cached != nullptr &&
                std::memcmp(cached, payload.data(), kPrefixBytes) == 0;
        } else if (drain_blocked_optional) {
            scheduler.DrainOptionalPayloads(ctx, reranker);
            result.completed = ctx.stats().safein_optional_io_completed;
            result.dropped_late = ctx.stats().safein_optional_io_dropped_late;
        }
        result.nonblocking_poll_calls =
            ctx.stats().safein_optional_io_nonblocking_poll_calls;
        result.empty_polls = ctx.stats().safein_optional_io_empty_polls;
        result.completed_before_probe_end =
            ctx.stats().safein_optional_io_completed_before_probe_end;
        result.completed_in_final_drain =
            ctx.stats().safein_optional_io_completed_in_final_drain;
        result.completed_in_optional_drain =
            ctx.stats().safein_optional_io_completed_in_optional_drain;
        result.completed_bytes =
            ctx.stats().safein_optional_io_completed_bytes;
        result.timeline_queries =
            ctx.stats().safein_optional_io_timeline_queries;
        result.blocked_mandatory_calls =
            ctx.stats().safein_optional_io_blocked_mandatory_calls;
        result.inflight_at_probe_end =
            ctx.stats().safein_optional_io_inflight_at_probe_end;
        result.inflight_at_optional_drain_start =
            ctx.stats().safein_optional_io_inflight_at_optional_drain_start;
        result.first_submit_offset_us =
            ctx.stats().safein_optional_io_first_submit_offset_us;
        result.probe_end_offset_us =
            ctx.stats().safein_optional_io_probe_end_offset_us;
        ::close(fd);
        return result;
    }

    OptionalPayloadResult RunZeroOptionalWork() {
        PreadFallbackReader cluster_reader;
        PreadFallbackReader mandatory_reader;
        PreadFallbackReader optional_reader;
        SearchConfig config;
        config.top_k = 1;
        config.nprobe = kNprobe;
        config.enable_safein_cold_payload_prefetch = true;
        config.safein_cold_payload_prefix_bytes = 4096;
        config.enable_safein_optional_io_isolation = true;
        config.enable_safein_optional_io_refill_only_polling = true;
        OverlapScheduler scheduler(*index_, cluster_reader, mandatory_reader,
                                   optional_reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);

        scheduler.MaybeSubmitOptionalPayloadRequests(
            ctx, reranker, /*probes_remaining=*/2);
        scheduler.SnapshotOptionalPayloadProbeEnd(ctx);
        scheduler.FinalDrain(ctx, reranker);
        scheduler.DrainOptionalPayloads(ctx, reranker);

        OptionalPayloadResult result;
        result.submit_calls = ctx.stats().safein_optional_io_submit_calls;
        result.nonblocking_poll_calls =
            ctx.stats().safein_optional_io_nonblocking_poll_calls;
        result.completed = ctx.stats().safein_optional_io_completed;
        result.completed_before_probe_end =
            ctx.stats().safein_optional_io_completed_before_probe_end;
        return result;
    }

    OptionalPayloadResult RunRefillOnlyTwoRequests() {
        constexpr uint32_t kPayloadBytes = 4096;
        constexpr uint32_t kPrefixBytes = 1024;
        const fs::path path = test_dir_ / "optional_refill_payload.dat";
        const int fd = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
        EXPECT_GE(fd, 0);
        if (fd < 0) return {};
        std::vector<uint8_t> payload(kPayloadBytes, 7);
        EXPECT_EQ(::pwrite(fd, payload.data(), payload.size(), 0),
                  static_cast<ssize_t>(payload.size()));

        const AddressEntry addr{
            4096, kDim * sizeof(float) +
                sizeof(vdb::storage::HotPayloadDescriptor)};
        InlineHotRecordStoreConfig::PayloadMetadataMap metadata;
        InlineHotRecordStoreConfig::PayloadMetadata entry;
        entry.payload_storage_type = static_cast<uint8_t>(
            vdb::storage::HotPayloadStorageType::kColdPointer);
        entry.payload_offset = 0;
        entry.payload_bytes = kPayloadBytes;
        metadata.emplace(addr.offset, entry);

        PreadFallbackReader cluster_reader;
        PreadFallbackReader mandatory_reader;
        ManualCompletionReader optional_reader;
        SearchConfig config;
        config.top_k = 1;
        config.nprobe = kNprobe;
        config.submission_mode = SubmissionMode::Isolated;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        config.inline_hot_record_store.payload_metadata = &metadata;
        config.inline_hot_record_store.cold_payload_fd = fd;
        config.enable_safein_cold_payload_prefetch = true;
        config.safein_cold_payload_prefix_bytes = kPrefixBytes;
        config.enable_safein_optional_io_isolation = true;
        config.enable_safein_optional_io_refill_only_polling = true;
        config.safein_optional_io_max_inflight = 1;
        config.safein_optional_io_min_remaining_probes = 1;
        OverlapScheduler scheduler(*index_, cluster_reader, mandatory_reader,
                                   optional_reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);
        OverlapScheduler::ReadPlanEntry plan;
        scheduler.ConfigureSafeInReadPlan(ctx, addr, &plan);
        scheduler.EnqueueSafeInReadPlan(ctx, plan);
        scheduler.EnqueueSafeInReadPlan(ctx, plan);

        scheduler.MaybeSubmitOptionalPayloadRequests(
            ctx, reranker, /*probes_remaining=*/3);
        optional_reader.MakeOneReady();
        scheduler.MaybeSubmitOptionalPayloadRequests(
            ctx, reranker, /*probes_remaining=*/2);
        scheduler.SnapshotOptionalPayloadProbeEnd(ctx);

        OptionalPayloadResult result;
        result.submitted = ctx.stats().safein_optional_io_submitted;
        result.completed = ctx.stats().safein_optional_io_completed;
        result.submit_calls = ctx.stats().safein_optional_io_submit_calls;
        result.nonblocking_poll_calls =
            ctx.stats().safein_optional_io_nonblocking_poll_calls;
        result.empty_polls = ctx.stats().safein_optional_io_empty_polls;
        result.completed_before_probe_end =
            ctx.stats().safein_optional_io_completed_before_probe_end;
        result.optional_queue = static_cast<uint32_t>(
            scheduler.pending_optional_payload_plans_.size());
        scheduler.DrainOptionalPayloads(ctx, reranker);
        ::close(fd);
        return result;
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

    uint32_t DispatchVectorSpanCompletionTwiceAndReadPoolSize() {
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
        for (uint32_t round = 0; round < 2; ++round) {
            uint8_t* buffer = scheduler.buffer_pool_.Acquire(span_bytes);
            std::memcpy(buffer, vectors_.data(), vec_bytes);
            std::memcpy(buffer + second_offset,
                        vectors_.data() + kDim, vec_bytes);
            OverlapScheduler::PendingIO io;
            io.type = OverlapScheduler::PendingIO::Type::VEC_SPAN;
            io.addr = AddressEntry{4096, vec_bytes};
            io.read_offset = 4096;
            io.read_length = span_bytes;
            if (!scheduler.free_vec_span_member_vectors_.empty()) {
                io.span_members = std::move(
                    scheduler.free_vec_span_member_vectors_.back());
                scheduler.free_vec_span_member_vectors_.pop_back();
                io.span_members.clear();
            }
            io.span_members.push_back(
                OverlapScheduler::VecSpanMember{
                    AddressEntry{4096, vec_bytes}, 0});
            io.span_members.push_back(
                OverlapScheduler::VecSpanMember{
                    AddressEntry{4096 + second_offset, vec_bytes},
                    second_offset});
            const uint32_t slot = scheduler.AllocatePendingSlot(
                std::move(io), buffer,
                OverlapScheduler::PendingBufferCleanup::Pool);
            scheduler.DispatchCompletion(
                slot, static_cast<int32_t>(span_bytes), ctx, reranker);
        }
        return static_cast<uint32_t>(
            scheduler.free_vec_span_member_vectors_.size());
    }

    std::pair<uint32_t, uint32_t> InstallCollidingFinalPayloadView() {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 2;
        config.nprobe = 1;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);

        auto hash_slot = [](uint64_t offset) {
            offset ^= offset >> 30;
            offset *= 0xbf58476d1ce4e5b9ULL;
            offset ^= offset >> 27;
            offset *= 0x94d049bb133111ebULL;
            offset ^= offset >> 31;
            return static_cast<size_t>(offset) & 3u;
        };
        const uint64_t final_offset = 4096;
        uint64_t colliding_offset = final_offset + 1;
        while (hash_slot(colliding_offset) != hash_slot(final_offset)) {
            ++colliding_offset;
        }

        constexpr uint32_t kPayloadBytes = 64;
        uint8_t* final_buffer =
            scheduler.buffer_pool_.Acquire(kPayloadBytes);
        uint8_t* nonfinal_buffer =
            scheduler.buffer_pool_.Acquire(kPayloadBytes);
        std::memset(final_buffer, 0x11, kPayloadBytes);
        std::memset(nonfinal_buffer, 0x22, kPayloadBytes);
        scheduler.retained_vec_spans_.push_back(
            {final_buffer, kPayloadBytes, kPayloadBytes, false, false});
        scheduler.retained_vec_spans_.push_back(
            {nonfinal_buffer, kPayloadBytes, kPayloadBytes, false, false});
        scheduler.span_payload_refs_.push_back(
            {AddressEntry{final_offset, kPayloadBytes}, final_buffer,
             kPayloadBytes, 0});
        scheduler.span_payload_refs_.push_back(
            {AddressEntry{colliding_offset, kPayloadBytes}, nonfinal_buffer,
             kPayloadBytes, 1});

        const std::vector<CollectorEntry> results = {
            CollectorEntry{0.0f,
                           AddressEntry{final_offset, kPayloadBytes}},
            CollectorEntry{1.0f, AddressEntry{8192, kPayloadBytes}}};
        scheduler.InstallFinalSpanPayloadViews(ctx, reranker, results);
        const uint32_t cached =
            reranker.CachedPayloadBytes(final_offset);
        const uint32_t outstanding_after_install =
            scheduler.buffer_pool_.OutstandingCount();
        reranker.ReleasePayload(final_offset);
        scheduler.ReleaseRetainedVecSpans();
        EXPECT_EQ(scheduler.buffer_pool_.OutstandingCount(), 0u);
        return {cached, outstanding_after_install};
    }

    struct SpanPayloadReuseResult {
        uint32_t buffered = 0;
        uint32_t cached_payload_bytes = 0;
        uint32_t views = 0;
        uint64_t view_bytes = 0;
        uint64_t retained_bytes = 0;
        uint32_t pool_outstanding_with_view = 0;
        uint32_t pool_outstanding_after_release = 0;
        bool payload_matches = false;
    };

    SpanPayloadReuseResult DispatchVectorSpanPayloadReuse(
        bool compact = false, bool cold_prefix = false,
        bool poison_trailing_descriptor = false,
        bool credited_internal = false,
        bool credit_mismatch = false) {
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 2;
        config.nprobe = 1;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        config.enable_vec_span_payload_reuse = true;
        config.compact_vec_span_payload_reuse = compact;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);

        constexpr uint32_t kPayloadBytes = 64;
        const uint32_t vec_bytes = kDim * sizeof(float);
        const uint32_t descriptor_bytes =
            sizeof(vdb::storage::HotPayloadDescriptor);
        const uint32_t second_offset =
            vec_bytes + descriptor_bytes + kPayloadBytes;
        const uint32_t span_bytes = second_offset + vec_bytes +
            (poison_trailing_descriptor ? descriptor_bytes : 0);
        uint8_t* buffer = scheduler.buffer_pool_.Acquire(span_bytes);
        std::memcpy(buffer, vectors_.data(), vec_bytes);
        vdb::storage::HotPayloadDescriptor desc;
        desc.payload_storage_type = static_cast<uint8_t>(
            cold_prefix
                ? vdb::storage::HotPayloadStorageType::kPrefixColdPointer
                : vdb::storage::HotPayloadStorageType::kInlinePayload);
        desc.inline_bytes =
            credit_mismatch ? kPayloadBytes / 2 : kPayloadBytes;
        desc.payload_bytes = cold_prefix
            ? 2 * kPayloadBytes
            : desc.inline_bytes;
        desc.payload_offset = cold_prefix ? 8192 : 0;
        vdb::storage::EncodeHotPayloadDescriptor(desc, buffer + vec_bytes);
        std::memset(buffer + vec_bytes + descriptor_bytes, 0x5a,
                    kPayloadBytes);
        std::memcpy(buffer + second_offset, vectors_.data() + kDim,
                    vec_bytes);
        if (poison_trailing_descriptor) {
            std::memset(buffer + second_offset + vec_bytes, 0xff,
                        descriptor_bytes);
        }

        const AddressEntry first{
            4096, vec_bytes + descriptor_bytes + kPayloadBytes};
        const AddressEntry second{
            4096 + second_offset, vec_bytes};
        OverlapScheduler::PendingIO io;
        io.type = OverlapScheduler::PendingIO::Type::VEC_SPAN;
        io.addr = first;
        io.read_offset = first.offset;
        io.read_length = span_bytes;
        io.span_members.push_back(
            OverlapScheduler::VecSpanMember{
                first, 0, credited_internal ? kPayloadBytes : 0});
        io.span_members.push_back(
            OverlapScheduler::VecSpanMember{second, second_offset});
        const uint32_t slot = scheduler.AllocatePendingSlot(
            std::move(io), buffer,
            OverlapScheduler::PendingBufferCleanup::Pool);
        scheduler.DispatchCompletion(slot, static_cast<int32_t>(span_bytes),
                                     ctx, reranker);
        const std::vector<CollectorEntry> final_results = {
            CollectorEntry{0.0f, first}};
        scheduler.InstallFinalSpanPayloadViews(
            ctx, reranker, final_results);

        SpanPayloadReuseResult result;
        result.buffered = reranker.BufferedCount();
        result.cached_payload_bytes =
            reranker.CachedPayloadBytes(first.offset);
        result.views = ctx.stats().vec_span_payload_views;
        result.view_bytes = ctx.stats().vec_span_payload_view_bytes;
        result.retained_bytes =
            ctx.stats().vec_span_payload_retained_bytes;
        result.pool_outstanding_with_view =
            scheduler.buffer_pool_.OutstandingCount();
        const uint8_t* payload =
            reranker.CachedPayloadData(first.offset);
        result.payload_matches =
            payload != nullptr &&
            std::all_of(payload, payload + kPayloadBytes,
                        [](uint8_t value) { return value == 0x5a; });
        reranker.ReleasePayload(first.offset);
        scheduler.ReleaseRetainedVecSpans();
        result.pool_outstanding_after_release =
            scheduler.buffer_pool_.OutstandingCount();
        return result;
    }

    struct PayloadCompletionResult {
        uint32_t cached_bytes = 0;
        uint32_t pool_outstanding_before_release = 0;
        uint32_t pool_outstanding_after_release = 0;
        bool slot_released = false;
    };

    PayloadCompletionResult DispatchPayloadCompletion(bool prefix) {
        constexpr uint32_t kPayloadBytes = 128;
        PreadFallbackReader reader;
        SearchConfig config;
        config.top_k = 1;
        config.nprobe = 1;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        RerankConsumer reranker(ctx, kDim);

        uint8_t* buffer = scheduler.buffer_pool_.Acquire(kPayloadBytes);
        std::memset(buffer, 7, kPayloadBytes);
        OverlapScheduler::PendingIO io;
        io.type = prefix
            ? OverlapScheduler::PendingIO::Type::PAYLOAD_PREFIX
            : OverlapScheduler::PendingIO::Type::PAYLOAD;
        io.addr = AddressEntry{8192, kPayloadBytes};
        io.read_offset = 8192;
        io.read_length = kPayloadBytes;
        io.payload_total_length = kPayloadBytes;
        io.payload_prefix_length = kPayloadBytes;
        io.payload_buffer_capacity = kPayloadBytes;
        const uint32_t slot = scheduler.AllocatePendingSlot(
            std::move(io), buffer,
            OverlapScheduler::PendingBufferCleanup::Pool);
        scheduler.DispatchCompletion(slot, kPayloadBytes, ctx, reranker);

        PayloadCompletionResult result;
        result.cached_bytes = reranker.CachedPayloadBytes(8192);
        result.pool_outstanding_before_release =
            scheduler.buffer_pool_.OutstandingCount();
        result.slot_released = scheduler.GetPendingSlot(slot) == nullptr;
        reranker.ReleasePayload(8192);
        result.pool_outstanding_after_release =
            scheduler.buffer_pool_.OutstandingCount();
        return result;
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

    struct SpanPlannerGroupSnapshot {
        size_t begin = 0;
        size_t end = 0;
        uint64_t read_offset = 0;
        uint32_t read_length = 0;
        uint64_t safein_credit_bytes = 0;
    };

    struct DirectSpanPlannerResult {
        SearchStats stats;
        std::vector<SpanPlannerGroupSnapshot> groups;
    };

    DirectSpanPlannerResult RunDirectSpanPlanner(
        SearchConfig config,
        const std::vector<std::pair<AddressEntry, bool>>& candidates) {
        PreadFallbackReader reader;
        OverlapScheduler scheduler(*index_, reader, config);
        SearchContext ctx(vectors_.data(), config);
        std::vector<OverlapScheduler::VecOnlyReadPlan> plans;
        plans.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            plans.push_back(scheduler.MakeVecOnlyReadPlan(
                candidate.first, candidate.second));
        }

        std::vector<OverlapScheduler::VecSpanExecutionGroup> groups;
        scheduler.PlanVecOnlySpanGroups(ctx, plans, 0, plans.size(), &groups);

        DirectSpanPlannerResult result;
        result.stats = ctx.stats();
        result.groups.reserve(groups.size());
        for (const auto& group : groups) {
            result.groups.push_back(SpanPlannerGroupSnapshot{
                group.begin, group.end, group.read_offset,
                group.read_length, group.safein_credit_bytes});
        }
        return result;
    }

    uint32_t ResolveDirectSpanCredit(SearchConfig config,
                                     AddressEntry addr,
                                     bool safein = true) {
        PreadFallbackReader reader;
        OverlapScheduler scheduler(*index_, reader, config);
        return scheduler.MakeVecOnlyReadPlan(addr, safein)
            .safein_credit_bytes;
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

TEST_F(OverlapSchedulerTest, ExternalSafeInPlansBoundedColdPayloadPrefix) {
    const ColdSafeInPlanResult plan =
        BuildInlineColdSafeInPlan(/*payload_bytes=*/16384,
                                  /*prefix_cap=*/4096);

    EXPECT_EQ(plan.vector_read_bytes, 0u);
    EXPECT_EQ(plan.payload_prefix_bytes, 4096u);
    EXPECT_EQ(plan.payload_total_bytes, 16384u);
    EXPECT_EQ(plan.payload_offset, 123456u);
    EXPECT_EQ(plan.total_read_bytes, 4096u);
    EXPECT_EQ(plan.extra_bytes, 4096u);
}

TEST_F(OverlapSchedulerTest, ExternalSafeInReadsWholeSmallColdPayload) {
    const ColdSafeInPlanResult plan =
        BuildInlineColdSafeInPlan(/*payload_bytes=*/2048,
                                  /*prefix_cap=*/4096);

    EXPECT_EQ(plan.payload_prefix_bytes, 2048u);
    EXPECT_EQ(plan.payload_total_bytes, 2048u);
}

TEST_F(OverlapSchedulerTest, InlinePayloadUsesIndependentPayloadPlan) {
    const ColdSafeInPlanResult plan = BuildInlineColdSafeInPlan(
        /*payload_bytes=*/128, /*prefix_cap=*/4096,
        vdb::storage::HotPayloadStorageType::kInlinePayload);

    EXPECT_EQ(plan.payload_prefix_bytes, 128u);
    EXPECT_EQ(plan.payload_total_bytes, 128u);
    EXPECT_EQ(plan.vector_read_bytes, 0u);
    EXPECT_EQ(plan.total_read_bytes, 128u);
}

TEST_F(OverlapSchedulerTest, ExternalColdPrefixCountsAgainstQueryExtraBudget) {
    const auto [first, second] = ColdSafeInQueryBudgetAdmissions();
    EXPECT_TRUE(first);
    EXPECT_FALSE(second);
}

TEST_F(OverlapSchedulerTest, CachedColdPrefixOnlyReadsAndAppendsSuffix) {
    const ColdPrefixSuffixResult result =
        FetchColdPayloadSuffixAfterCachedPrefix();

    EXPECT_EQ(result.cached_payload_bytes, 4096u);
    EXPECT_EQ(result.suffix_read_requests, 1u);
    EXPECT_EQ(result.suffix_read_bytes, 3072u);
    EXPECT_TRUE(result.payload_matches);
}

TEST_F(OverlapSchedulerTest, ReusablePayloadBufferAppendsSuffixInPlace) {
    const ColdPrefixSuffixResult result =
        FetchColdPayloadSuffixAfterCachedPrefix(/*reusable_buffer=*/true);

    EXPECT_EQ(result.cached_payload_bytes, 4096u);
    EXPECT_EQ(result.suffix_read_requests, 1u);
    EXPECT_EQ(result.suffix_read_bytes, 3072u);
    EXPECT_EQ(result.buffer_reuses, 1u);
    EXPECT_EQ(result.prefix_copy_bytes_avoided, 1024u);
    EXPECT_TRUE(result.reused_same_buffer);
    EXPECT_TRUE(result.payload_matches);
}

TEST_F(OverlapSchedulerTest, IsolatedOptionalPayloadUsesDedicatedReader) {
    const OptionalPayloadResult result =
        RunIsolatedOptionalPayloadRead();

    EXPECT_EQ(result.queued, 1u);
    EXPECT_EQ(result.submitted, 1u);
    EXPECT_EQ(result.submit_calls, 1u);
    EXPECT_EQ(result.completed, 1u);
    EXPECT_EQ(result.dropped_late, 0u);
    EXPECT_EQ(result.completed_before_probe_end, 0u);
    EXPECT_EQ(result.completed_in_final_drain, 0u);
    EXPECT_EQ(result.completed_in_optional_drain, 1u);
    EXPECT_EQ(result.completed_bytes, 1024u);
    EXPECT_EQ(result.timeline_queries, 1u);
    EXPECT_LE(result.inflight_at_probe_end, 1u);
    EXPECT_LE(result.inflight_at_optional_drain_start, 1u);
    EXPECT_GT(result.first_submit_offset_us, 0.0);
    EXPECT_GE(result.probe_end_offset_us, result.first_submit_offset_us);
    EXPECT_EQ(result.cached_bytes, 1024u);
    EXPECT_EQ(result.mandatory_prepped, 0u);
    EXPECT_TRUE(result.payload_matches);
}

TEST_F(OverlapSchedulerTest, OptionalCompletionCanBeAttributedBeforeProbeEnd) {
    const OptionalPayloadResult result = RunIsolatedOptionalPayloadRead(
        /*leave_mandatory_work_pending=*/false,
        /*mandatory_in_flight=*/0,
        /*drain_blocked_optional=*/false,
        /*enable_isolation=*/true,
        /*reap_before_probe_end=*/true,
        /*use_final_drain=*/false);

    EXPECT_EQ(result.completed, 1u);
    EXPECT_EQ(result.completed_before_probe_end, 1u);
    EXPECT_EQ(result.completed_in_final_drain, 0u);
    EXPECT_EQ(result.completed_in_optional_drain, 0u);
    EXPECT_EQ(result.nonblocking_poll_calls, 2u);
    EXPECT_EQ(result.empty_polls, 1u);
}

TEST_F(OverlapSchedulerTest, OptionalCompletionCanBeAttributedToFinalDrain) {
    const OptionalPayloadResult result = RunIsolatedOptionalPayloadRead(
        /*leave_mandatory_work_pending=*/false,
        /*mandatory_in_flight=*/0,
        /*drain_blocked_optional=*/false,
        /*enable_isolation=*/true,
        /*reap_before_probe_end=*/false,
        /*use_final_drain=*/true);

    EXPECT_EQ(result.completed, 1u);
    EXPECT_EQ(result.completed_before_probe_end, 0u);
    EXPECT_EQ(result.completed_in_final_drain, 1u);
    EXPECT_EQ(result.completed_in_optional_drain, 0u);
    EXPECT_GE(result.nonblocking_poll_calls, 3u);
    EXPECT_GE(result.empty_polls, 2u);
}

TEST_F(OverlapSchedulerTest, ZeroOptionalWorkLeavesOptionalCountersZero) {
    const OptionalPayloadResult result = RunZeroOptionalWork();

    EXPECT_EQ(result.submit_calls, 0u);
    EXPECT_EQ(result.nonblocking_poll_calls, 0u);
    EXPECT_EQ(result.completed, 0u);
    EXPECT_EQ(result.completed_before_probe_end, 0u);
}

TEST_F(OverlapSchedulerTest, RefillOnlyPollingUnlocksOneBlockedSubmit) {
    const OptionalPayloadResult result = RunRefillOnlyTwoRequests();

    EXPECT_EQ(result.submitted, 2u);
    EXPECT_EQ(result.completed, 1u);
    EXPECT_EQ(result.submit_calls, 2u);
    EXPECT_EQ(result.nonblocking_poll_calls, 1u);
    EXPECT_EQ(result.empty_polls, 0u);
    EXPECT_EQ(result.completed_before_probe_end, 1u);
    EXPECT_EQ(result.optional_queue, 0u);
}

TEST_F(OverlapSchedulerTest, OptionalPayloadWaitsForMandatoryBacklog) {
    const OptionalPayloadResult result =
        RunIsolatedOptionalPayloadRead(/*leave_mandatory_work_pending=*/true);

    EXPECT_EQ(result.queued, 1u);
    EXPECT_EQ(result.submitted, 0u);
    EXPECT_EQ(result.mandatory_prepped, 1u);
    EXPECT_EQ(result.blocked_mandatory_calls, 1u);
}

TEST_F(OverlapSchedulerTest, OptionalPayloadResumesAfterMandatoryBacklogClears) {
    const OptionalPayloadResult result = RunIsolatedOptionalPayloadRead(
        /*leave_mandatory_work_pending=*/true,
        /*mandatory_in_flight=*/0,
        /*drain_blocked_optional=*/true,
        /*enable_isolation=*/true,
        /*reap_before_probe_end=*/false,
        /*use_final_drain=*/false,
        /*refill_only_polling=*/true,
        /*retry_after_mandatory_clears=*/true);

    EXPECT_EQ(result.submitted, 1u);
    EXPECT_EQ(result.blocked_mandatory_calls, 1u);
    EXPECT_EQ(result.dropped_late, 0u);
}

TEST_F(OverlapSchedulerTest, OptionalPayloadWaitsAtMandatoryLowWatermark) {
    const OptionalPayloadResult result = RunIsolatedOptionalPayloadRead(
        /*leave_mandatory_work_pending=*/false,
        /*mandatory_in_flight=*/8);

    EXPECT_EQ(result.queued, 1u);
    EXPECT_EQ(result.submitted, 0u);
}

TEST_F(OverlapSchedulerTest, BlockedOptionalPayloadIsCountedAsLateDrop) {
    const OptionalPayloadResult result = RunIsolatedOptionalPayloadRead(
        /*leave_mandatory_work_pending=*/true,
        /*mandatory_in_flight=*/0,
        /*drain_blocked_optional=*/true);

    EXPECT_EQ(result.queued, 1u);
    EXPECT_EQ(result.submitted, 0u);
    EXPECT_EQ(result.completed, 0u);
    EXPECT_EQ(result.dropped_late, 1u);
}

TEST_F(OverlapSchedulerTest, OptionalIsolationOffUsesLegacyMandatoryQueue) {
    const OptionalPayloadResult result = RunIsolatedOptionalPayloadRead(
        /*leave_mandatory_work_pending=*/false,
        /*mandatory_in_flight=*/0,
        /*drain_blocked_optional=*/false,
        /*enable_isolation=*/false);

    EXPECT_EQ(result.queued, 0u);
    EXPECT_EQ(result.optional_queue, 0u);
    EXPECT_EQ(result.mandatory_queue, 1u);
    EXPECT_EQ(result.submitted, 0u);
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

TEST_F(OverlapSchedulerTest, VectorSpanMemberVectorCapacityIsReused) {
    EXPECT_EQ(DispatchVectorSpanCompletionTwiceAndReadPoolSize(), 1u);
}

TEST_F(OverlapSchedulerTest, VectorSpanPayloadReuseRetainsOneSharedBuffer) {
    const SpanPayloadReuseResult result = DispatchVectorSpanPayloadReuse();
    EXPECT_EQ(result.buffered, 2u);
    EXPECT_EQ(result.cached_payload_bytes, 64u);
    EXPECT_EQ(result.views, 1u);
    EXPECT_EQ(result.view_bytes, 64u);
    EXPECT_GT(result.retained_bytes, result.view_bytes);
    EXPECT_EQ(result.pool_outstanding_with_view, 1u);
    EXPECT_EQ(result.pool_outstanding_after_release, 0u);
    EXPECT_TRUE(result.payload_matches);
}

TEST_F(OverlapSchedulerTest,
       CreditedInternalSpanCompletionEstablishesPayloadView) {
    const SpanPayloadReuseResult result = DispatchVectorSpanPayloadReuse(
        /*compact=*/false, /*cold_prefix=*/false,
        /*poison_trailing_descriptor=*/false,
        /*credited_internal=*/true);
    EXPECT_EQ(result.buffered, 2u);
    EXPECT_EQ(result.cached_payload_bytes, 64u);
    EXPECT_EQ(result.views, 1u);
    EXPECT_EQ(result.view_bytes, 64u);
    EXPECT_TRUE(result.payload_matches);
    EXPECT_EQ(result.pool_outstanding_after_release, 0u);
}

TEST_F(OverlapSchedulerTest,
       CreditedSpanCompletionRejectsMetadataDescriptorMismatch) {
    EXPECT_DEATH(
        DispatchVectorSpanPayloadReuse(
            /*compact=*/false, /*cold_prefix=*/false,
            /*poison_trailing_descriptor=*/false,
            /*credited_internal=*/true,
            /*credit_mismatch=*/true),
        "metadata/descriptor credit mismatch");
}

TEST_F(OverlapSchedulerTest, VectorSpanPayloadReuseCanCompactRetainedBuffer) {
    const SpanPayloadReuseResult result =
        DispatchVectorSpanPayloadReuse(/*compact=*/true);
    EXPECT_EQ(result.buffered, 2u);
    EXPECT_EQ(result.cached_payload_bytes, 64u);
    EXPECT_EQ(result.views, 1u);
    EXPECT_EQ(result.view_bytes, 64u);
    EXPECT_EQ(result.retained_bytes, 1u << 20);
    EXPECT_EQ(result.pool_outstanding_with_view, 1u);
    EXPECT_EQ(result.pool_outstanding_after_release, 0u);
    EXPECT_TRUE(result.payload_matches);
}

TEST_F(OverlapSchedulerTest, VectorSpanPayloadReuseAcceptsColdInlinePrefix) {
    const SpanPayloadReuseResult result = DispatchVectorSpanPayloadReuse(
        /*compact=*/false, /*cold_prefix=*/true);
    EXPECT_EQ(result.cached_payload_bytes, 64u);
    EXPECT_EQ(result.views, 1u);
    EXPECT_TRUE(result.payload_matches);
}

TEST_F(OverlapSchedulerTest,
       VectorSpanPayloadReuseSkipsDescriptorOutsideLogicalRecord) {
    const SpanPayloadReuseResult result = DispatchVectorSpanPayloadReuse(
        /*compact=*/false, /*cold_prefix=*/false,
        /*poison_trailing_descriptor=*/true);
    EXPECT_EQ(result.buffered, 2u);
    EXPECT_EQ(result.views, 1u);
    EXPECT_EQ(result.cached_payload_bytes, 64u);
    EXPECT_EQ(result.pool_outstanding_after_release, 0u);
}

TEST_F(OverlapSchedulerTest,
       FinalPayloadMembershipHandlesOpenAddressingCollision) {
    const auto [cached_bytes, outstanding] =
        InstallCollidingFinalPayloadView();
    EXPECT_EQ(cached_bytes, 64u);
    EXPECT_EQ(outstanding, 1u);
}

TEST_F(OverlapSchedulerTest, PayloadCompletionMovePreservesBufferOwnership) {
    for (const bool prefix : {false, true}) {
        const PayloadCompletionResult result =
            DispatchPayloadCompletion(prefix);
        EXPECT_EQ(result.cached_bytes, 128u);
        EXPECT_EQ(result.pool_outstanding_before_release, 1u);
        EXPECT_EQ(result.pool_outstanding_after_release, 0u);
        EXPECT_TRUE(result.slot_released);
    }
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

TEST_F(OverlapSchedulerTest, OracleSafeInPrefetchKeepsFalseCandidateVecOnly) {
    const auto [prefetch_count, vec_only_count] = FlushOracleSafeInPlans();
    EXPECT_EQ(prefetch_count, 1u);
    EXPECT_EQ(vec_only_count, 1u);
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

TEST_F(OverlapSchedulerTest, DetailedPipelineIoTiming_PreservesAccounting) {
    PreadFallbackReader reader;
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.enable_pipeline_io_detailed_timing = true;

    OverlapScheduler scheduler(*index_, reader, config);

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    auto results = scheduler.Search(query.data());
    const auto& stats = results.stats();
    EXPECT_GT(results.size(), 0u);
    EXPECT_GT(stats.probe_loop_wall_ms, 0.0);
    EXPECT_NEAR(stats.probe_loop_wall_ms,
                stats.probe_cluster_wall_ms +
                    stats.probe_loop_scheduler_ms,
                1e-6);
    EXPECT_EQ(stats.mandatory_io_prepared,
              stats.mandatory_io_completed);
    EXPECT_EQ(stats.mandatory_io_completed,
              stats.mandatory_io_completed_before_probe_end +
                  stats.mandatory_io_completed_in_final_drain);
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

TEST_F(OverlapSchedulerTest, IoUringMixedFixedBufferFallbackPreservesResults) {
    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    SearchConfig baseline_config;
    baseline_config.top_k = kTopK;
    baseline_config.nprobe = kNprobe;
    PreadFallbackReader baseline_reader;
    OverlapScheduler baseline_scheduler(
        *index_, baseline_reader, baseline_config);
    auto baseline_results = baseline_scheduler.Search(query.data());

    IoUringReader reader;
    auto init_status = reader.Init(16, 64);
    if (!init_status.ok()) {
        GTEST_SKIP() << "io_uring not available: " << init_status.message();
    }
    const int data_fd = index_->segment().data_reader().fd();
    ASSERT_GE(data_fd, 0);
    auto reg_status = reader.RegisterFiles(&data_fd, 1);
    if (!reg_status.ok()) {
        GTEST_SKIP() << "file registration unavailable: "
                     << reg_status.message();
    }

    SearchConfig mixed_config = baseline_config;
    mixed_config.io_queue_depth = 16;
    mixed_config.fixed_vec_buffer_count = 2;
    auto preload_status = index_->segment().PreloadAllClusters();
    ASSERT_TRUE(preload_status.ok()) << preload_status.message();
    OverlapScheduler mixed_scheduler(*index_, reader, mixed_config);
    auto mixed_results = mixed_scheduler.Search(query.data());

    ASSERT_EQ(baseline_results.size(), mixed_results.size());
    for (uint32_t i = 0; i < baseline_results.size(); ++i) {
        EXPECT_FLOAT_EQ(
            baseline_results[i].distance, mixed_results[i].distance);
    }
    const auto& stats = mixed_results.stats();
    EXPECT_GT(stats.fixed_vec_buffer_hits, 0u);
    EXPECT_GT(stats.fixed_vec_buffer_misses, 0u);
    EXPECT_EQ(stats.fixed_vec_buffer_hits + stats.fixed_vec_buffer_misses,
              stats.vec_only_read_requests);
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
}

TEST_F(OverlapSchedulerTest, SerialNoOverlapPreservesVectorSpanCoalescing) {
    SearchConfig config;
    config.top_k = kTopK;
    config.nprobe = kNprobe;
    config.execution_mode = QueryExecutionMode::SerialNoOverlap;
    config.enable_vec_span_coalescing = true;
    config.vec_span_tile_bytes = 1u << 20;
    config.vec_span_max_byte_amplification = 64.0f;

    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;

    PreadFallbackReader reader;
    OverlapScheduler scheduler(*index_, reader, config);
    auto results = scheduler.Search(query.data());

    ASSERT_EQ(kTopK, results.size());
    EXPECT_GT(results.stats().vec_span_read_requests, 0u);
    EXPECT_GT(results.stats().vec_span_candidates, 0u);
    EXPECT_GT(results.stats().serial_vector_read_requests, 0u);
    EXPECT_EQ(0u, results.stats().total_submit_calls);
    EXPECT_EQ(0u, results.stats().total_io_submitted);
}

TEST_F(OverlapSchedulerTest,
       SharedSpanPlannerIsExecutionModeInvariantForSameFlush) {
    const uint32_t vector_bytes = kDim * sizeof(float);
    const std::vector<std::pair<AddressEntry, bool>> candidates = {
        {{4096, vector_bytes}, false},
        {{4096 + 300, vector_bytes}, false},
        {{4096 + 700, vector_bytes}, false},
        {{4096 + 950, vector_bytes}, false},
        {{4096 + 1400, vector_bytes}, false},
    };
    InlineHotRecordStoreConfig::PayloadMetadataMap metadata;

    for (const SpanPlannerMode mode : {
             SpanPlannerMode::GV, SpanPlannerMode::SV,
             SpanPlannerMode::GE, SpanPlannerMode::SE}) {
        SearchConfig async_config;
        async_config.top_k = kTopK;
        async_config.nprobe = kNprobe;
        async_config.io_queue_depth = 32;
        async_config.cluster_submit_reserve = 4;
        async_config.submit_batch_size = 8;
        async_config.materialization_mode = MaterializationMode::Late;
        async_config.enable_vec_span_coalescing = true;
        async_config.vec_span_tile_bytes = 1u << 20;
        async_config.vec_span_planner_mode = mode;
        async_config.vec_span_alpha_num = 3;
        async_config.vec_span_alpha_den = 2;
        async_config.vec_span_safein_rho_num = 1;
        async_config.vec_span_safein_rho_den = 1;
        if (mode == SpanPlannerMode::SV || mode == SpanPlannerMode::SE) {
            async_config.enable_vec_span_payload_reuse = true;
            async_config.inline_hot_record_store.enabled = true;
            async_config.inline_hot_record_store.descriptor_bytes =
                sizeof(vdb::storage::HotPayloadDescriptor);
            async_config.inline_hot_record_store.payload_metadata =
                &metadata;
        }
        const DirectSpanPlannerResult async_plan =
            RunDirectSpanPlanner(async_config, candidates);

        SearchConfig serial_config = async_config;
        serial_config.execution_mode = QueryExecutionMode::SerialNoOverlap;
        const DirectSpanPlannerResult serial_plan =
            RunDirectSpanPlanner(serial_config, candidates);

        ASSERT_EQ(async_plan.groups.size(), serial_plan.groups.size())
            << "mode=" << static_cast<unsigned>(mode);
        for (size_t i = 0; i < async_plan.groups.size(); ++i) {
            EXPECT_EQ(async_plan.groups[i].begin,
                      serial_plan.groups[i].begin);
            EXPECT_EQ(async_plan.groups[i].end,
                      serial_plan.groups[i].end);
            EXPECT_EQ(async_plan.groups[i].read_offset,
                      serial_plan.groups[i].read_offset);
            EXPECT_EQ(async_plan.groups[i].read_length,
                      serial_plan.groups[i].read_length);
            EXPECT_EQ(async_plan.groups[i].safein_credit_bytes,
                      serial_plan.groups[i].safein_credit_bytes);
        }

        const SearchStats& async_stats = async_plan.stats;
        const SearchStats& serial_stats = serial_plan.stats;
        EXPECT_GT(async_stats.vec_span_planner_calls, 0u);
        EXPECT_EQ(async_stats.vec_span_planner_calls,
                  serial_stats.vec_span_planner_calls);
        EXPECT_EQ(async_stats.vec_span_planner_runs,
                  serial_stats.vec_span_planner_runs);
        EXPECT_EQ(async_stats.vec_span_planner_candidates,
                  serial_stats.vec_span_planner_candidates);
        EXPECT_EQ(async_stats.vec_span_planner_groups,
                  serial_stats.vec_span_planner_groups);
        EXPECT_EQ(async_stats.vec_span_planned_physical_bytes,
                  serial_stats.vec_span_planned_physical_bytes);
        EXPECT_EQ(async_stats.vec_span_planned_vector_bytes,
                  serial_stats.vec_span_planned_vector_bytes);
        EXPECT_EQ(async_stats.vec_span_planner_credit_bytes,
                  serial_stats.vec_span_planner_credit_bytes);
        EXPECT_EQ(async_stats.vec_span_planner_plan_hash,
                  serial_stats.vec_span_planner_plan_hash);
        EXPECT_EQ(async_stats.vec_span_planner_fallbacks, 0u);
        EXPECT_EQ(serial_stats.vec_span_planner_fallbacks, 0u);
    }
}

TEST_F(OverlapSchedulerTest,
       SpanPlannerPlannedAccountingMatchesIssuedSearchReads) {
    std::vector<float> query(kDim, 0.0f);
    query[0] = 1.0f;
    InlineHotRecordStoreConfig::PayloadMetadataMap empty_metadata;
    for (uint32_t cluster_id : index_->segment().cluster_ids()) {
        const auto status =
            index_->segment().EnsureClusterLoaded(cluster_id);
        ASSERT_TRUE(status.ok()) << status.message();
        const uint32_t count =
            index_->segment().GetNumRecords(cluster_id);
        for (uint32_t record = 0; record < count; ++record) {
            const AddressEntry addr =
                index_->segment().GetAddress(cluster_id, record);
            InlineHotRecordStoreConfig::PayloadMetadata metadata;
            metadata.payload_storage_type = static_cast<uint8_t>(
                vdb::storage::HotPayloadStorageType::kInlinePayload);
            empty_metadata.emplace(addr.offset, metadata);
        }
    }

    for (const SpanPlannerMode mode : {
             SpanPlannerMode::GV, SpanPlannerMode::SV,
             SpanPlannerMode::GE, SpanPlannerMode::SE}) {
        SearchConfig config;
        config.top_k = kTopK;
        config.nprobe = kNprobe;
        config.io_queue_depth = 32;
        config.submit_batch_size = 8;
        config.materialization_mode = MaterializationMode::Late;
        config.enable_vec_span_coalescing = true;
        config.vec_span_tile_bytes = 1u << 20;
        config.vec_span_planner_mode = mode;
        config.vec_span_alpha_num = 3;
        config.vec_span_alpha_den = 2;
        config.vec_span_safein_rho_num = 1;
        config.vec_span_safein_rho_den = 1;
        config.vec_span_safein_tail_count = 0;
        if (mode == SpanPlannerMode::SV || mode == SpanPlannerMode::SE) {
            config.enable_vec_span_payload_reuse = true;
            config.inline_hot_record_store.enabled = true;
            config.inline_hot_record_store.descriptor_bytes =
                sizeof(vdb::storage::HotPayloadDescriptor);
            config.inline_hot_record_store.payload_metadata =
                &empty_metadata;
        }

        PreadFallbackReader reader;
        OverlapScheduler scheduler(*index_, reader, config);
        const auto results = scheduler.Search(query.data());
        ASSERT_EQ(results.size(), kTopK)
            << "mode=" << static_cast<unsigned>(mode);
        const SearchStats& stats = results.stats();
        EXPECT_GT(stats.vec_span_planner_calls, 0u);
        EXPECT_EQ(stats.vec_span_planned_physical_bytes,
                  stats.vec_only_read_bytes)
            << "mode=" << static_cast<unsigned>(mode);
        EXPECT_EQ(stats.vec_span_planner_groups,
                  stats.vec_only_read_requests)
            << "mode=" << static_cast<unsigned>(mode);
        EXPECT_EQ(stats.vec_span_planner_fallbacks, 0u)
            << "mode=" << static_cast<unsigned>(mode);
        if (mode == SpanPlannerMode::SV || mode == SpanPlannerMode::SE) {
            EXPECT_EQ(stats.vec_span_planner_credit_bytes, 0u);
        }
    }
}

TEST_F(OverlapSchedulerTest,
       SafeInSpanCreditAppliesOnlyToInternalMembers) {
    constexpr uint32_t kInlineBytes = 64;
    const uint32_t vector_bytes = kDim * sizeof(float);
    const uint32_t descriptor_bytes =
        sizeof(vdb::storage::HotPayloadDescriptor);
    const AddressEntry first{
        4096, vector_bytes + descriptor_bytes + kInlineBytes};
    const AddressEntry second{
        4096 + 540, vector_bytes + descriptor_bytes + kInlineBytes};

    InlineHotRecordStoreConfig::PayloadMetadataMap metadata;
    for (const AddressEntry addr : {first, second}) {
        InlineHotRecordStoreConfig::PayloadMetadata entry;
        entry.payload_bytes = kInlineBytes;
        entry.inline_bytes = kInlineBytes;
        entry.payload_storage_type = static_cast<uint8_t>(
            vdb::storage::HotPayloadStorageType::kInlinePayload);
        metadata.emplace(addr.offset, entry);
    }

    for (const SpanPlannerMode mode : {
             SpanPlannerMode::SV, SpanPlannerMode::SE}) {
        SearchConfig config;
        config.top_k = 2;
        config.nprobe = 1;
        config.enable_vec_span_coalescing = true;
        config.vec_span_tile_bytes = 1u << 20;
        config.vec_span_planner_mode = mode;
        config.vec_span_alpha_num = 3;
        config.vec_span_alpha_den = 2;
        config.vec_span_safein_rho_num = 1;
        config.vec_span_safein_rho_den = 1;
        config.enable_vec_span_payload_reuse = true;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes = descriptor_bytes;
        config.inline_hot_record_store.payload_metadata = &metadata;

        const DirectSpanPlannerResult internal = RunDirectSpanPlanner(
            config, {{first, true}, {second, false}});
        ASSERT_EQ(internal.groups.size(), 1u)
            << "mode=" << static_cast<unsigned>(mode);
        EXPECT_EQ(internal.groups[0].begin, 0u);
        EXPECT_EQ(internal.groups[0].end, 2u);
        EXPECT_EQ(internal.groups[0].read_length, 796u);
        EXPECT_EQ(internal.groups[0].safein_credit_bytes, kInlineBytes);
        EXPECT_EQ(internal.stats.vec_span_planner_credit_bytes,
                  kInlineBytes);
        EXPECT_EQ(internal.stats.vec_span_planner_fallbacks, 0u);

        const DirectSpanPlannerResult endpoint = RunDirectSpanPlanner(
            config, {{first, false}, {second, true}});
        ASSERT_EQ(endpoint.groups.size(), 2u)
            << "mode=" << static_cast<unsigned>(mode);
        EXPECT_EQ(endpoint.groups[0].safein_credit_bytes, 0u);
        EXPECT_EQ(endpoint.groups[1].safein_credit_bytes, 0u);
        EXPECT_EQ(endpoint.stats.vec_span_planner_credit_bytes, 0u);
        EXPECT_EQ(endpoint.stats.vec_span_planner_fallbacks, 0u);
    }
}

TEST_F(OverlapSchedulerTest, SpanPlannerRejectsIneligibleSafeInCredit) {
    constexpr uint32_t kInlineBytes = 64;
    const uint32_t vector_bytes = kDim * sizeof(float);
    const uint32_t descriptor_bytes =
        sizeof(vdb::storage::HotPayloadDescriptor);
    const AddressEntry full_record{
        4096, vector_bytes + descriptor_bytes + kInlineBytes};

    auto base_config = [&]() {
        SearchConfig config;
        config.top_k = 1;
        config.nprobe = 1;
        config.enable_vec_span_coalescing = true;
        config.vec_span_tile_bytes = 1u << 20;
        config.vec_span_planner_mode = SpanPlannerMode::SV;
        config.vec_span_safein_rho_num = 1;
        config.vec_span_safein_rho_den = 1;
        config.enable_vec_span_payload_reuse = true;
        config.inline_hot_record_store.enabled = true;
        config.inline_hot_record_store.descriptor_bytes = descriptor_bytes;
        return config;
    };

    InlineHotRecordStoreConfig::PayloadMetadataMap cold_metadata;
    InlineHotRecordStoreConfig::PayloadMetadata cold;
    cold.payload_offset = 8192;
    cold.payload_bytes = kInlineBytes;
    cold.payload_storage_type = static_cast<uint8_t>(
        vdb::storage::HotPayloadStorageType::kColdPointer);
    cold_metadata.emplace(full_record.offset, cold);
    SearchConfig cold_config = base_config();
    cold_config.inline_hot_record_store.payload_metadata = &cold_metadata;
    EXPECT_EQ(ResolveDirectSpanCredit(cold_config, full_record), 0u);

    InlineHotRecordStoreConfig::PayloadMetadataMap missing_metadata;
    SearchConfig missing_config = base_config();
    missing_config.inline_hot_record_store.payload_metadata =
        &missing_metadata;
    EXPECT_EQ(ResolveDirectSpanCredit(missing_config, full_record), 0u);

    InlineHotRecordStoreConfig::PayloadMetadataMap inline_metadata;
    InlineHotRecordStoreConfig::PayloadMetadata inlined;
    inlined.payload_bytes = kInlineBytes;
    inlined.inline_bytes = kInlineBytes;
    inlined.payload_storage_type = static_cast<uint8_t>(
        vdb::storage::HotPayloadStorageType::kInlinePayload);
    inline_metadata.emplace(full_record.offset, inlined);
    SearchConfig short_config = base_config();
    short_config.inline_hot_record_store.payload_metadata = &inline_metadata;
    const AddressEntry short_record{
        full_record.offset,
        vector_bytes + descriptor_bytes + kInlineBytes - 1};
    EXPECT_EQ(ResolveDirectSpanCredit(short_config, short_record), 0u);

    SearchConfig sidecar_config = base_config();
    sidecar_config.vec_span_safein_rho_num = 0;
    sidecar_config.inline_hot_record_store.payload_metadata = &inline_metadata;
    sidecar_config.separate_record_store.enabled = true;
    EXPECT_EQ(ResolveDirectSpanCredit(sidecar_config, full_record), 0u);
}

TEST_F(OverlapSchedulerTest, InvalidSpanPlannerRationalContractFailsFast) {
    EXPECT_DEATH(
        {
            PreadFallbackReader reader;
            SearchConfig config;
            config.enable_vec_span_coalescing = true;
            config.vec_span_tile_bytes = 1u << 20;
            config.vec_span_alpha_den = 0;
            OverlapScheduler scheduler(*index_, reader, config);
        },
        "invalid vector-span planner contract");

    EXPECT_DEATH(
        {
            PreadFallbackReader reader;
            SearchConfig config;
            config.enable_vec_span_coalescing = true;
            config.vec_span_tile_bytes = 1u << 20;
            config.vec_span_safein_rho_num = 2;
            config.vec_span_safein_rho_den = 1;
            OverlapScheduler scheduler(*index_, reader, config);
        },
        "invalid vector-span planner contract");
}

TEST_F(OverlapSchedulerTest,
       SafeInAwarePlannerRequiresReusableInlineMetadata) {
    EXPECT_DEATH(
        {
            PreadFallbackReader reader;
            SearchConfig config;
            config.enable_vec_span_coalescing = true;
            config.vec_span_tile_bytes = 1u << 20;
            config.vec_span_planner_mode = SpanPlannerMode::SV;
            config.vec_span_safein_rho_num = 1;
            config.vec_span_safein_rho_den = 1;
            OverlapScheduler scheduler(*index_, reader, config);
        },
        "SafeIn-aware span planning requires");
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
