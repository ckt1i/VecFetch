#include "vdb/query/overlap_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "vdb/query/rerank_consumer.h"
#include "vdb/rabitq/rabitq_estimator.h"

namespace vdb {
namespace query {

namespace {
constexpr double kLowOverheadStage2Weight = 1.0;
constexpr uint32_t kSubmitIntervalLimit = 4;
constexpr uint32_t kSubmitTailMinBatch = 8;

inline size_t NextPowerOfTwo(size_t value) {
    size_t result = 1;
    while (result < value) result <<= 1;
    return result;
}

inline uint64_t MixOffset(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}

inline void CheckPrepRead(const Status& s, const char* context) {
    if (!s.ok()) {
        std::fprintf(stderr, "FATAL: PrepRead failed (%s): %s\n",
                     context, s.ToString().c_str());
        std::abort();
    }
}

inline const char* CluReadModeName(CluReadMode mode) {
    switch (mode) {
        case CluReadMode::Window:
            return "window";
        case CluReadMode::FullPreload:
            return "full_preload";
    }
    return "unknown";
}
}  // namespace

void OverlapScheduler::QueryDedupSet::Reserve(size_t expected) {
    size_t capacity = NextPowerOfTwo(std::max<size_t>(16, expected * 2));
    slots.assign(capacity, kEmpty);
    mask = capacity - 1;
    size = 0;
}

void OverlapScheduler::QueryDedupSet::Clear() {
    if (slots.empty()) return;
    std::fill(slots.begin(), slots.end(), kEmpty);
    size = 0;
}

bool OverlapScheduler::QueryDedupSet::Insert(uint64_t key) {
    if (slots.empty()) {
        Reserve(256);
    }
    size_t idx = static_cast<size_t>(MixOffset(key)) & mask;
    for (;;) {
        uint64_t& slot = slots[idx];
        if (slot == key) return false;
        if (slot == kEmpty) {
            slot = key;
            ++size;
            return true;
        }
        idx = (idx + 1) & mask;
    }
}

OverlapScheduler::OverlapScheduler(index::IvfIndex& index,
                                   AsyncReader& reader,
                                   const SearchConfig& config)
    : OverlapScheduler(index, reader, reader, config) {}

OverlapScheduler::OverlapScheduler(index::IvfIndex& index,
                                   AsyncReader& cluster_reader,
                                   AsyncReader& data_reader,
                                   const SearchConfig& config)
    : index_(index),
      cluster_reader_(cluster_reader),
      data_reader_(data_reader),
      isolated_submission_mode_(&cluster_reader != &data_reader ||
                                config.submission_mode == SubmissionMode::Isolated),
      config_(config),
      vec_bytes_(index.logical_dim() * sizeof(float)),
      est_top_k_(config.top_k),
      use_crc_(config.crc_params != nullptr),
      estimator_(index.dim(), index.segment().rabitq_config().bits),
      prober_(index.conann(), index.dim(),
              index.segment().rabitq_config().bits) {
    if (index.used_hadamard()) {
        query_wrapper_.rotated_q.resize(index.dim());
    }
    if (index.logical_dim() != index.dim()) {
        query_wrapper_.padded_q.resize(index.dim(), 0.0f);
    }
    query_wrapper_.scratch.quant_query.reserve(index.dim());
    query_wrapper_.scratch.fastscan_lut.reserve(static_cast<size_t>(index.dim()) * 8 + 63);
    ready_clusters_.reserve(std::max(1u, config_.prefetch_depth + config_.initial_prefetch));
    submitted_candidate_offsets_.Reserve(
        static_cast<size_t>(std::max(1u, config_.nprobe)) * 256);
    resident_scratch_.completions.resize(128);
    if (use_crc_) {
        crc_stopper_ = index::CrcStopper(*config.crc_params, index.nlist());
        est_heap_.reserve(est_top_k_);
    }
    // Stage 2: precompute margin divisor from bits
    uint8_t bits = index.segment().rabitq_config().bits;
    if (bits > 1) {
        has_s2_ = true;
        margin_s2_divisor_ = static_cast<float>(1u << (bits - 1));
    }
    buffer_pool_.Prime(vec_bytes_, std::max(1u, config_.io_queue_depth));
    InitializeDataBufferSlab();
}

OverlapScheduler::~OverlapScheduler() {
    CleanupPendingSlots();
    for (uint8_t* buf : fixed_vec_buffers_) {
        std::free(buf);
    }
}

SearchResults OverlapScheduler::Search(const float* query_vec) {
    // Reset per-query state
    ready_clusters_.clear();
    submitted_candidate_offsets_.Clear();
    CleanupPendingSlots();
    pending_all_plans_.clear();
    pending_vec_only_plans_.clear();
    next_to_submit_ = 0;
    inflight_clusters_ = 0;
    est_heap_.clear();

    auto t_search_start = std::chrono::steady_clock::now();
    const float* effective_query_vec = query_vec;
    if (index_.logical_dim() != index_.dim()) {
        std::fill(query_wrapper_.padded_q.begin(), query_wrapper_.padded_q.end(), 0.0f);
        std::memcpy(query_wrapper_.padded_q.data(), query_vec,
                    static_cast<size_t>(index_.logical_dim()) * sizeof(float));
        effective_query_vec = query_wrapper_.padded_q.data();
    }

    if (config_.use_resident_clusters) {
        if (!index_.segment().resident_preload_enabled()) {
            std::fprintf(stderr,
                         "FATAL: resident cluster mode requires preloaded clusters before search\n");
            std::abort();
        }
    } else if (config_.clu_read_mode == CluReadMode::FullPreload &&
               !index_.segment().resident_preload_enabled()) {
        Status s = index_.segment().PreloadAllClusters();
        if (!s.ok()) {
            std::fprintf(stderr,
                         "FATAL: failed to enable %s mode: %s\n",
                         CluReadModeName(config_.clu_read_mode),
                         s.ToString().c_str());
            std::abort();
        }
    }

    index_.SetUseCoarseSelectSimd(config_.enable_coarse_select_simd);
    index_.SetUseCoarseSelectPhase2(config_.enable_coarse_select_phase2);
    SearchContext ctx(effective_query_vec, config_);
    // Phase 1.3: precompute P^T × query once when Hadamard rotation is used
    if (index_.used_hadamard()) {
        auto t_rot0 = std::chrono::steady_clock::now();
        index_.rotation().Apply(effective_query_vec, query_wrapper_.rotated_q.data());
        ctx.stats().probe_prepare_rotation_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_rot0).count();
    }
    RerankConsumer reranker(ctx, index_.logical_dim());

    auto coarse_start = std::chrono::steady_clock::now();
    auto sorted_clusters = index_.FindNearestClusters(
        ctx.query_vec(), config_.nprobe);
    ctx.stats().coarse_score_ms = index_.last_coarse_score_ms();
    ctx.stats().coarse_topn_ms = index_.last_coarse_topn_ms();
    ctx.stats().coarse_select_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - coarse_start).count();

    PrefetchClusters(ctx, sorted_clusters);
    if (config_.use_resident_clusters &&
        config_.clu_read_mode == CluReadMode::FullPreload) {
        ProbeResidentThinPath(ctx, reranker, sorted_clusters);
    } else {
        ProbeAndDrainInterleaved(ctx, reranker, sorted_clusters);
    }
    FinalDrain(ctx, reranker);
    reranker.ExecuteBuffered();

    auto results = ctx.collector().Finalize();

    {
        auto tf0 = std::chrono::steady_clock::now();
        FetchMissingPayloads(ctx, reranker, results);
        ctx.stats().fetch_missing_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tf0).count();
        ctx.stats().remaining_payload_fetch_ms = ctx.stats().fetch_missing_ms;
    }
    auto sr = AssembleResults(reranker, results);
    sr.stats() = ctx.stats();
    sr.stats().total_time_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_search_start).count();
    return sr;
}

uint32_t OverlapScheduler::PendingDataRequestCount() const {
    return static_cast<uint32_t>(
        pending_all_plans_.size() + pending_vec_only_plans_.size());
}

void OverlapScheduler::EmitPendingDataRequests(SearchContext& ctx,
                                               uint32_t max_count) {
    if (max_count == 0) return;
    uint32_t remaining = std::min(max_count, PendingDataRequestCount());
    if (remaining == 0) return;

    const int dat_fd = index_.segment().data_reader().fd();

    auto emit_all = [&](uint32_t count) {
        if (count == 0) return;
        auto tp0 = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < count; ++i) {
            const ReadPlanEntry& plan = pending_all_plans_.front();
            uint8_t* buf = buffer_pool_.Acquire(plan.read_length);
            PendingIO pio;
            pio.type = PendingIO::Type::VEC_ALL;
            pio.addr = plan.addr;
            pio.read_offset = plan.addr.offset;
            pio.read_length = plan.read_length;
            const uint32_t slot_id =
                AllocatePendingSlot(pio, buf, PendingBufferCleanup::Pool);
            CheckPrepRead(data_reader_.PrepReadTagged(
                              dat_fd, buf, plan.read_length, plan.addr.offset,
                              slot_id),
                          "scheduler VEC_ALL");
            pending_all_plans_.pop_front();
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tp0).count();
        ctx.stats().uring_prep_ms += elapsed;
        ctx.stats().probe_submit_prepare_all_ms += elapsed;
        ctx.stats().probe_submit_emit_ms += elapsed;
        ctx.stats().probe_submit_ms += elapsed;
    };

    auto emit_vec_only = [&](uint32_t count) {
        if (count == 0) return;
        auto tp0 = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < count; ++i) {
            const ReadPlanEntry& plan = pending_vec_only_plans_.front();
            uint8_t* buf = nullptr;
            uint16_t fixed_idx = 0;
            const bool use_fixed_buffer =
                TryAcquireFixedVecBuffer(&buf, &fixed_idx);
            if (!use_fixed_buffer) {
                buf = buffer_pool_.Acquire(vec_bytes_);
            }
            PendingIO pio;
            pio.type = PendingIO::Type::VEC_ONLY;
            pio.addr = plan.addr;
            pio.read_offset = plan.addr.offset;
            pio.read_length = vec_bytes_;
            const uint32_t slot_id = AllocatePendingSlot(
                pio, buf,
                use_fixed_buffer ? PendingBufferCleanup::FixedVec
                                 : PendingBufferCleanup::Pool,
                fixed_idx);
            if (use_fixed_buffer && fixed_buffer_reader_ != nullptr) {
                CheckPrepRead(fixed_buffer_reader_->PrepReadRegisteredBufferTagged(
                                  dat_fd, buf, fixed_idx, vec_bytes_,
                                  plan.addr.offset, slot_id),
                              "scheduler VEC_ONLY fixed");
            } else {
                CheckPrepRead(data_reader_.PrepReadTagged(
                                  dat_fd, buf, vec_bytes_, plan.addr.offset,
                                  slot_id),
                              "scheduler VEC_ONLY");
            }
            pending_vec_only_plans_.pop_front();
        }
        const double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tp0).count();
        ctx.stats().uring_prep_ms += elapsed;
        ctx.stats().probe_submit_prepare_vec_only_ms += elapsed;
        ctx.stats().probe_submit_emit_ms += elapsed;
        ctx.stats().probe_submit_ms += elapsed;
    };

    const uint32_t all_take =
        std::min<uint32_t>(remaining, static_cast<uint32_t>(pending_all_plans_.size()));
    emit_all(all_take);
    remaining -= all_take;
    emit_vec_only(remaining);
    ctx.stats().probe_time_ms = ctx.stats().probe_prepare_ms +
        ctx.stats().probe_stage1_ms + ctx.stats().probe_stage2_ms +
        ctx.stats().probe_submit_ms;
}

void OverlapScheduler::ProbeResidentThinPath(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<ClusterID>& sorted_clusters) {
    static constexpr uint32_t kCrossClusterSubmitInterval = 4;
    static constexpr uint32_t kPendingRequestFlushThreshold = 32;
    auto& comps = resident_scratch_.completions;
    const uint32_t emit_threshold =
        (config_.submit_batch_size == 0)
            ? std::numeric_limits<uint32_t>::max()
            : config_.submit_batch_size;
    uint32_t clusters_since_submit = 0;

    auto record_submit = [&](AsyncReader& reader) {
        auto ts0 = std::chrono::steady_clock::now();
        uint32_t submitted = reader.Submit();
        ctx.stats().uring_submit_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ts0).count();
        ctx.stats().total_io_submitted += submitted;
        if (submitted > 0) {
            ctx.stats().total_submit_calls++;
        }
    };

    auto drain_reader = [&](AsyncReader& reader, bool wait_for_one) {
        uint32_t n = 0;
        auto tw0 = std::chrono::steady_clock::now();
        if (wait_for_one) {
            n = reader.WaitAndPoll(comps.data(),
                                   static_cast<uint32_t>(comps.size()));
            double wait_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
            ctx.stats().io_wait_time_ms += wait_ms;
        } else {
            n = reader.Poll(comps.data(), static_cast<uint32_t>(comps.size()));
        }
        for (uint32_t j = 0; j < n; ++j) {
            DispatchCompletion(comps[j].user_data, ctx, reranker);
        }
    };

    for (size_t i = 0; i < sorted_clusters.size(); ++i) {
        uint32_t cid = sorted_clusters[i];
        const ParsedCluster* cluster = GetResidentParsedCluster(cid);
        if (cluster == nullptr) {
            std::fprintf(stderr,
                         "FATAL: resident cluster view missing for cluster %u\n",
                         cid);
            std::abort();
        }

        auto tp0 = std::chrono::steady_clock::now();
        ProbeCluster(*cluster, cid, ctx, reranker);
        (void)tp0;
        ++clusters_since_submit;

        if (isolated_submission_mode_) {
            const bool vec_batch_ready =
                PendingDataRequestCount() >= emit_threshold;
            const bool interval_hit =
                clusters_since_submit >= kCrossClusterSubmitInterval;
            const bool pending_pressure =
                PendingDataRequestCount() >= kPendingRequestFlushThreshold;
            const bool final_cluster = (i + 1 == sorted_clusters.size());
            if ((vec_batch_ready || pending_pressure ||
                 (interval_hit && PendingDataRequestCount() >= emit_threshold) ||
                 final_cluster) &&
                PendingDataRequestCount() > 0) {
                EmitPendingDataRequests(ctx, PendingDataRequestCount());
                ctx.stats().total_submit_window_flushes++;
                if (final_cluster ||
                    (interval_hit && !pending_pressure && !vec_batch_ready)) {
                    ctx.stats().total_submit_window_tail_flushes++;
                }
                record_submit(data_reader_);
                drain_reader(data_reader_, /*wait_for_one=*/false);
                clusters_since_submit = 0;
            }
        } else {
            const bool vec_batch_ready =
                PendingDataRequestCount() >= emit_threshold;
            const bool interval_hit =
                clusters_since_submit >= kCrossClusterSubmitInterval;
            const bool pending_pressure =
                PendingDataRequestCount() >= kPendingRequestFlushThreshold;
            const bool final_cluster = (i + 1 == sorted_clusters.size());
            if ((vec_batch_ready || pending_pressure ||
                 (interval_hit && PendingDataRequestCount() >= emit_threshold) ||
                 final_cluster) &&
                PendingDataRequestCount() > 0) {
                EmitPendingDataRequests(ctx, PendingDataRequestCount());
                ctx.stats().total_submit_window_flushes++;
                if (final_cluster ||
                    (interval_hit && !pending_pressure && !vec_batch_ready)) {
                    ctx.stats().total_submit_window_tail_flushes++;
                }
                record_submit(cluster_reader_);
                drain_reader(cluster_reader_, /*wait_for_one=*/false);
                clusters_since_submit = 0;
            }
        }

        if (config_.early_stop) {
            if (use_crc_) {
                float est_kth = (est_heap_.size() >= est_top_k_)
                    ? est_heap_.front().first
                    : std::numeric_limits<float>::infinity();
                auto t_crc_decision = std::chrono::steady_clock::now();
                const bool should_stop = crc_stopper_.ShouldStop(
                    static_cast<uint32_t>(i + 1), est_kth);
                ctx.stats().crc_decision_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_crc_decision).count();
                if (should_stop) {
                    ctx.stats().total_crc_would_stop++;
                    if (!config_.crc_no_break) {
                        ctx.stats().early_stopped = true;
                        ctx.stats().clusters_skipped =
                            static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
                        ctx.stats().crc_clusters_probed =
                            static_cast<uint32_t>(i + 1);
                        break;
                    }
                }
            } else if (ctx.collector().Full() &&
                       ctx.collector().TopDistance() < index_.conann().d_k()) {
                ctx.stats().early_stopped = true;
                ctx.stats().clusters_skipped =
                    static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
                break;
            }
        }
    }

    if (isolated_submission_mode_) {
        if (PendingDataRequestCount() > 0) {
            EmitPendingDataRequests(ctx, PendingDataRequestCount());
            ctx.stats().total_submit_window_flushes++;
            ctx.stats().total_submit_window_tail_flushes++;
            record_submit(data_reader_);
        }
    } else if (PendingDataRequestCount() > 0) {
        EmitPendingDataRequests(ctx, PendingDataRequestCount());
        ctx.stats().total_submit_window_flushes++;
        ctx.stats().total_submit_window_tail_flushes++;
        record_submit(cluster_reader_);
    }
}

uint32_t OverlapScheduler::AllocatePendingSlot(PendingIO io, uint8_t* buffer,
                                               PendingBufferCleanup cleanup,
                                               uint16_t fixed_buffer_index) {
    uint32_t slot_id = 0;
    if (!free_pending_slots_.empty()) {
        slot_id = free_pending_slots_.back();
        free_pending_slots_.pop_back();
    } else {
        slot_id = static_cast<uint32_t>(pending_slots_.size());
        pending_slots_.emplace_back();
    }

    PendingSlot& slot = pending_slots_[slot_id];
    slot.in_use = true;
    slot.buffer = buffer;
    slot.fixed_buffer_index = fixed_buffer_index;
    slot.cleanup = cleanup;
    slot.io = std::move(io);
    return slot_id;
}

OverlapScheduler::PendingSlot* OverlapScheduler::GetPendingSlot(
    uint64_t slot_token) {
    uint32_t slot_id = static_cast<uint32_t>(slot_token);
    if (slot_token > std::numeric_limits<uint32_t>::max() ||
        slot_id >= pending_slots_.size()) {
        return nullptr;
    }
    PendingSlot& slot = pending_slots_[slot_id];
    return slot.in_use ? &slot : nullptr;
}

void OverlapScheduler::ReleasePendingSlot(uint32_t slot_id) {
    if (slot_id >= pending_slots_.size()) return;
    PendingSlot& slot = pending_slots_[slot_id];
    slot.in_use = false;
    slot.buffer = nullptr;
    slot.fixed_buffer_index = 0;
    slot.cleanup = PendingBufferCleanup::None;
    slot.io = PendingIO{};
    free_pending_slots_.push_back(slot_id);
}

void OverlapScheduler::ReleaseFixedVecBuffer(uint16_t buffer_index) {
    free_fixed_vec_buffers_.push_back(buffer_index);
}

void OverlapScheduler::CleanupPendingSlot(PendingSlot& slot) {
    if (!slot.in_use || !slot.buffer) return;

    switch (slot.cleanup) {
        case PendingBufferCleanup::Free:
            std::free(slot.buffer);
            break;
        case PendingBufferCleanup::Pool:
            buffer_pool_.Release(slot.buffer);
            break;
        case PendingBufferCleanup::FixedVec:
            ReleaseFixedVecBuffer(slot.fixed_buffer_index);
            break;
        case PendingBufferCleanup::None:
            break;
    }
    slot.buffer = nullptr;
    slot.cleanup = PendingBufferCleanup::None;
}

void OverlapScheduler::CleanupPendingSlots() {
    for (uint32_t slot_id = 0; slot_id < pending_slots_.size(); ++slot_id) {
        PendingSlot& slot = pending_slots_[slot_id];
        if (!slot.in_use) continue;
        CleanupPendingSlot(slot);
        ReleasePendingSlot(slot_id);
    }
}

bool OverlapScheduler::TryAcquireFixedVecBuffer(uint8_t** buffer,
                                                uint16_t* buffer_index) {
    if (!fixed_vec_buffers_enabled_ || free_fixed_vec_buffers_.empty()) {
        return false;
    }
    uint16_t idx = free_fixed_vec_buffers_.back();
    free_fixed_vec_buffers_.pop_back();
    *buffer = fixed_vec_buffers_[idx];
    *buffer_index = idx;
    return true;
}

void OverlapScheduler::InitializeDataBufferSlab() {
    auto* uring_reader = dynamic_cast<IoUringReader*>(&data_reader_);
    if (uring_reader == nullptr || config_.io_queue_depth == 0) {
        return;
    }

    fixed_vec_buffers_.reserve(config_.io_queue_depth);
    fixed_vec_buffer_capacities_.reserve(config_.io_queue_depth);
    free_fixed_vec_buffers_.reserve(config_.io_queue_depth);
    const uint32_t aligned_vec_bytes = (vec_bytes_ + 4095u) & ~4095u;
    for (uint32_t i = 0; i < config_.io_queue_depth; ++i) {
        uint8_t* buf = static_cast<uint8_t*>(
            std::aligned_alloc(4096, aligned_vec_bytes));
        if (!buf) {
            for (uint8_t* allocated : fixed_vec_buffers_) {
                std::free(allocated);
            }
            fixed_vec_buffers_.clear();
            fixed_vec_buffer_capacities_.clear();
            free_fixed_vec_buffers_.clear();
            return;
        }
        fixed_vec_buffers_.push_back(buf);
        fixed_vec_buffer_capacities_.push_back(aligned_vec_bytes);
        free_fixed_vec_buffers_.push_back(
            static_cast<uint16_t>(fixed_vec_buffers_.size() - 1));
    }

    std::vector<const uint8_t*> raw_ptrs(fixed_vec_buffers_.size());
    for (size_t i = 0; i < fixed_vec_buffers_.size(); ++i) {
        raw_ptrs[i] = fixed_vec_buffers_[i];
    }
    Status s = uring_reader->RegisterBuffers(raw_ptrs.data(),
                                             fixed_vec_buffer_capacities_.data(),
                                             static_cast<uint32_t>(raw_ptrs.size()));
    if (!s.ok()) {
        for (uint8_t* buf : fixed_vec_buffers_) {
            std::free(buf);
        }
        fixed_vec_buffers_.clear();
        fixed_vec_buffer_capacities_.clear();
        free_fixed_vec_buffers_.clear();
        return;
    }

    fixed_buffer_reader_ = uring_reader;
    fixed_vec_buffers_enabled_ = true;
}

// ============================================================================
// SubmitClusterRead + PrefetchClusters
// ============================================================================

void OverlapScheduler::SubmitClusterRead(uint32_t cluster_id) {
    auto loc = index_.segment().GetBlockLocation(cluster_id);
    if (!loc.has_value()) return;

    int clu_fd = index_.segment().clu_fd();
    uint32_t size32 = static_cast<uint32_t>(loc->size);
    // Round up read length to 4KB for O_DIRECT compatibility
    uint32_t read_size = (size32 + 4095u) & ~4095u;
    // 4KB-aligned buffer allocation for O_DIRECT compatibility
    uint8_t* buf = static_cast<uint8_t*>(
        std::aligned_alloc(4096, read_size));
    if (!buf) {
        std::fprintf(stderr, "FATAL: aligned_alloc failed for cluster block (%u bytes)\n", read_size);
        std::abort();
    }
    PendingIO io;
    io.type = PendingIO::Type::CLUSTER_BLOCK;
    io.cluster_id = cluster_id;
    io.block_size = size32;  // original size for parsing
    uint32_t slot_id = AllocatePendingSlot(std::move(io), buf,
                                           PendingBufferCleanup::Free);
    CheckPrepRead(cluster_reader_.PrepReadTagged(
                      clu_fd, buf, read_size, loc->offset, slot_id),
                  "cluster block");
    inflight_clusters_++;
}

void OverlapScheduler::PrefetchClusters(
    SearchContext& ctx,
    const std::vector<ClusterID>& sorted_clusters) {
    if (config_.clu_read_mode == CluReadMode::FullPreload) {
        next_to_submit_ = static_cast<uint32_t>(sorted_clusters.size());
        inflight_clusters_ = 0;
        (void)ctx;
        return;
    }
    uint32_t initial = std::min(
        use_crc_ ? config_.initial_prefetch : config_.prefetch_depth,
        static_cast<uint32_t>(sorted_clusters.size()));
    for (uint32_t i = 0; i < initial; ++i) {
        SubmitClusterRead(sorted_clusters[i]);
    }
    next_to_submit_ = initial;

    if (initial > 0) {
        auto ts0 = std::chrono::steady_clock::now();
        uint32_t submitted = cluster_reader_.Submit();
        ctx.stats().uring_submit_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ts0).count();
        ctx.stats().total_io_submitted += submitted;
        if (submitted > 0) {
            ctx.stats().total_submit_calls++;
        }
    }
}

const ParsedCluster* OverlapScheduler::GetResidentParsedCluster(
    uint32_t cluster_id) const {
    if (config_.use_resident_clusters) {
        return index_.segment().GetResidentParsedCluster(cluster_id);
    }
    const auto* resident = index_.segment().GetResidentClusterView(cluster_id);
    if (resident == nullptr) {
        return nullptr;
    }
    return index_.segment().GetResidentParsedCluster(cluster_id);
}

// ============================================================================
// DispatchCompletion
// ============================================================================

void OverlapScheduler::DispatchCompletion(
    uint64_t slot_token, SearchContext& ctx, RerankConsumer& reranker) {
    PendingSlot* slot = GetPendingSlot(slot_token);
    if (slot == nullptr) return;

    const uint32_t slot_id = static_cast<uint32_t>(slot_token);
    uint8_t* buf = slot->buffer;
    PendingIO io = slot->io;

    switch (io.type) {
        case PendingIO::Type::CLUSTER_BLOCK: {
            inflight_clusters_--;
            auto block_buf = AlignedBufPtr(buf);
            slot->buffer = nullptr;
            slot->cleanup = PendingBufferCleanup::None;
            ParsedCluster pc;
            {
                auto tp0 = std::chrono::steady_clock::now();
                auto s = index_.segment().ParseClusterBlock(
                    io.cluster_id, std::move(block_buf), io.block_size, pc);
                ctx.stats().parse_cluster_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tp0).count();
                if (s.ok()) {
                    ready_clusters_[io.cluster_id] = std::move(pc);
                }
            }
            // Do NOT Release to BufferPool — ownership moved to ParsedCluster
            break;
        }
        case PendingIO::Type::VEC_ONLY: {
            reranker.ConsumeVec(buf, io.addr);
            CleanupPendingSlot(*slot);
            break;
        }
        case PendingIO::Type::VEC_ALL: {
            reranker.ConsumeAll(buf, io.addr);
            CleanupPendingSlot(*slot);
            break;
        }
        case PendingIO::Type::PAYLOAD:
            reranker.ConsumePayload(buf, io.addr);
            slot->buffer = nullptr;
            slot->cleanup = PendingBufferCleanup::None;
            // ConsumePayload takes ownership — don't release
            break;
    }
    ReleasePendingSlot(slot_id);
}

// ============================================================================
// AsyncIOSink — ProbeResultSink that submits io_uring reads + tracks est_heap
// ============================================================================

class OverlapScheduler::AsyncIOSink : public index::ProbeResultSink {
 public:
    AsyncIOSink(OverlapScheduler& sched, SearchContext& ctx, int dat_fd)
        : sched_(sched), ctx_(ctx), dat_fd_(dat_fd),
          skip_dedup_(sched.config_.use_resident_clusters &&
                      sched.config_.clu_read_mode == CluReadMode::FullPreload &&
                      sched.index_.assignment_mode() == index::AssignmentMode::Single),
          scratch_(sched.submit_scratch_) {
        crc_estimates_.reserve(256);
    }

    void OnCandidates(const index::CandidateBatch& batch) override {
        if (batch.count == 0) return;

        ctx_.stats().total_candidate_batches++;
        scratch_.unique_count = 0;
        scratch_.safein_all_count = 0;
        scratch_.vec_only_count = 0;

        ScanAndPartitionBatch(batch);
        BuildReadPlans(batch);

        if (sched_.use_crc_ && !crc_estimates_.empty()) {
            crc_pending_ = true;
        }
    }

    void FinalizeCluster() {
        if (crc_pending_) {
            auto t_crc_merge = std::chrono::steady_clock::now();
            ctx_.stats().total_crc_estimates_merged +=
                static_cast<uint32_t>(crc_estimates_.size());
            for (float est_dist : crc_estimates_) {
                if (sched_.est_heap_.size() < sched_.est_top_k_) {
                    sched_.est_heap_.push_back({est_dist, 0u});
                    std::push_heap(sched_.est_heap_.begin(), sched_.est_heap_.end());
                } else if (est_dist < sched_.est_heap_.front().first) {
                    std::pop_heap(sched_.est_heap_.begin(), sched_.est_heap_.end());
                    sched_.est_heap_.back() = {est_dist, 0u};
                    std::push_heap(sched_.est_heap_.begin(), sched_.est_heap_.end());
                }
            }
            crc_pending_ = false;
            crc_estimates_.clear();
            ctx_.stats().crc_merge_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_crc_merge).count();
        }
    }

    double dedup_and_slot_ms_ = 0;
    double prepare_vec_only_ms_ = 0;
    double prepare_all_ms_ = 0;
    double submit_cpu_ms() const {
        return dedup_and_slot_ms_;
    }

 private:
    void ScanAndPartitionBatch(const index::CandidateBatch& batch) {
        auto t0 = std::chrono::steady_clock::now();
        for (uint32_t i = 0; i < batch.count; ++i) {
            const AddressEntry& addr = batch.decoded_addr[i];
            bool duplicate_in_batch = false;
            for (uint32_t u = 0; u < scratch_.unique_count; ++u) {
                if (batch.decoded_addr[scratch_.unique_indices[u]].offset == addr.offset) {
                    duplicate_in_batch = true;
                    break;
                }
            }
            if (duplicate_in_batch) {
                ctx_.stats().duplicate_candidates++;
                ctx_.stats().deduplicated_candidates++;
                continue;
            }

            if (!skip_dedup_ && !sched_.submitted_candidate_offsets_.Insert(addr.offset)) {
                ctx_.stats().duplicate_candidates++;
                ctx_.stats().deduplicated_candidates++;
                continue;
            }

            const uint32_t idx = scratch_.unique_count++;
            scratch_.unique_indices[idx] = i;
            ctx_.stats().unique_fetch_candidates++;

            if (sched_.use_crc_) {
                auto t_crc_buffer = std::chrono::steady_clock::now();
                crc_estimates_.push_back(batch.est_dist[i]);
                ctx_.stats().total_crc_estimates_buffered++;
                ctx_.stats().crc_buffer_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_crc_buffer).count();
            }

            const bool use_vec_all =
                batch.cls[i] == index::CandidateClass::SafeIn &&
                addr.size <= sched_.config_.safein_all_threshold;
            if (use_vec_all) {
                scratch_.safein_all_indices[scratch_.safein_all_count++] = idx;
            } else {
                scratch_.vec_only_indices[scratch_.vec_only_count++] = idx;
            }
        }
        dedup_and_slot_ms_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    void BuildReadPlans(const index::CandidateBatch& batch) {
        for (uint32_t pos = 0; pos < scratch_.safein_all_count; ++pos) {
            const uint32_t unique_idx = scratch_.safein_all_indices[pos];
            const AddressEntry& addr = batch.decoded_addr[scratch_.unique_indices[unique_idx]];
            ReadPlanEntry plan;
            plan.type = PendingIO::Type::VEC_ALL;
            plan.addr = addr;
            plan.read_length = addr.size;
            sched_.pending_all_plans_.push_back(plan);
            ctx_.stats().total_submit_window_requests++;
        }

        auto all_elapsed = std::chrono::steady_clock::now();
        prepare_all_ms_ += std::chrono::duration<double, std::milli>(
            all_elapsed - all_elapsed).count();
        for (uint32_t pos = 0; pos < scratch_.vec_only_count; ++pos) {
            const uint32_t unique_idx = scratch_.vec_only_indices[pos];
            const AddressEntry& addr = batch.decoded_addr[scratch_.unique_indices[unique_idx]];
            ReadPlanEntry plan;
            plan.type = PendingIO::Type::VEC_ONLY;
            plan.addr = addr;
            plan.read_length = sched_.vec_bytes_;
            sched_.pending_vec_only_plans_.push_back(plan);
            ctx_.stats().total_submit_window_requests++;
        }
    }

    OverlapScheduler& sched_;
    SearchContext& ctx_;
    int dat_fd_;
    bool skip_dedup_ = false;
    SubmitScratch& scratch_;
    bool crc_pending_ = false;
    std::vector<float> crc_estimates_;
};

// ============================================================================
// ProbeCluster — Phase 3: thin wrapper around ClusterProber + AsyncIOSink
// ============================================================================

OverlapScheduler::PreparedClusterQueryView
OverlapScheduler::PrepareClusterQueryView(const SearchContext& ctx,
                                          uint32_t cluster_id,
                                          rabitq::PrepareTimingBreakdown* timing) {
    PreparedClusterQueryView view;
    auto* pq = &query_wrapper_.prepared;
    if (index_.used_hadamard()) {
        estimator_.PrepareQueryRotatedInto(
            query_wrapper_.rotated_q.data(),
            index_.rotated_centroid(cluster_id),
            pq,
            &query_wrapper_.scratch,
            timing);
    } else {
        estimator_.PrepareQueryInto(
            ctx.query_vec(), index_.centroid(cluster_id), index_.rotation(), pq,
            &query_wrapper_.scratch,
            timing);
    }
    view.prepared = pq;
    view.scratch = &query_wrapper_.scratch;
    view.margin_factor = 2.0f * pq->norm_qc * index_.conann().epsilon();
    return view;
}

void OverlapScheduler::ProbeCluster(
    const ParsedCluster& pc, uint32_t cluster_id,
    SearchContext& ctx, RerankConsumer& /*reranker*/) {

    int dat_fd = index_.segment().data_reader().fd();
    ctx.stats().total_probed += pc.num_records;

    // Phase 1.3: fast path avoids per-cluster FWHT using precomputed rotated query
    // and precomputed rotated_centroid (P^T × c_k). Falls back to full rotation otherwise.
    auto prepare_start = std::chrono::steady_clock::now();
    rabitq::PrepareTimingBreakdown prep_timing;
    rabitq::PrepareTimingBreakdown* prep_timing_ptr =
        config_.enable_fine_grained_timing ? &prep_timing : nullptr;
    PreparedClusterQueryView prepared = PrepareClusterQueryView(
        ctx, cluster_id, prep_timing_ptr);
    ctx.stats().probe_prepare_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - prepare_start).count();
    if (config_.enable_fine_grained_timing) {
        ctx.stats().probe_prepare_subtract_ms += prep_timing.subtract_norm_ms;
        ctx.stats().probe_prepare_rotation_ms += prep_timing.rotation_ms;
        ctx.stats().probe_prepare_normalize_ms += prep_timing.normalize_sign_sum_maxabs_ms;
        ctx.stats().probe_prepare_quantize_ms += prep_timing.quantize_ms;
        ctx.stats().probe_prepare_lut_build_ms += prep_timing.lut_build_ms;
        ctx.stats().probe_prepare_quant_lut_ms +=
            prep_timing.quantize_ms + prep_timing.lut_build_ms;
    }

    // dynamic_d_k:
    // - When CRC is disabled, keep +inf to avoid any estimate-driven SafeOut.
    // - When CRC is enabled, use a conservative floor at the static ConANN d_k.
    //   This prevents under-estimated est_heap thresholds from over-pruning
    //   Stage1/Stage2 candidates before rerank has stabilized the frontier.
    float dynamic_d_k = std::numeric_limits<float>::infinity();
    if (use_crc_ && est_heap_.size() >= est_top_k_) {
        dynamic_d_k = std::max(est_heap_.front().first, index_.conann().d_k());
    }

    AsyncIOSink sink(*this, ctx, dat_fd);
    index::ProbeStats local_stats;
    auto classify_start = std::chrono::steady_clock::now();
    prober_.Probe(pc, prepared, prepared.margin_factor, dynamic_d_k,
                  config_.enable_address_decode_simd,
                  config_.enable_fine_grained_timing,
                  config_.enable_stage2_collect_block_first,
                  config_.enable_stage2_scatter_batch_classify,
                  sink, local_stats);
    const double classify_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - classify_start).count();
    if (config_.enable_fine_grained_timing) {
        ctx.stats().probe_stage1_ms += local_stats.stage1_ms;
        ctx.stats().probe_stage1_estimate_ms += local_stats.stage1_estimate_ms;
        ctx.stats().probe_stage1_mask_ms += local_stats.stage1_mask_ms;
        ctx.stats().probe_stage1_iterate_ms += local_stats.stage1_iterate_ms;
        ctx.stats().probe_stage1_classify_only_ms += local_stats.stage1_classify_ms;
        ctx.stats().probe_stage2_ms += local_stats.stage2_ms;
        ctx.stats().probe_stage2_collect_ms += local_stats.stage2_collect_ms;
        ctx.stats().probe_stage2_kernel_ms += local_stats.stage2_kernel_ms;
        ctx.stats().probe_stage2_scatter_ms += local_stats.stage2_scatter_ms;
        ctx.stats().probe_stage2_kernel_sign_flip_ms += local_stats.stage2_kernel_sign_flip_ms;
        ctx.stats().probe_stage2_kernel_abs_fma_ms += local_stats.stage2_kernel_abs_fma_ms;
        ctx.stats().probe_stage2_kernel_tail_ms += local_stats.stage2_kernel_tail_ms;
        ctx.stats().probe_stage2_kernel_reduce_ms += local_stats.stage2_kernel_reduce_ms;
    } else {
        const double stage1_unit =
            static_cast<double>(local_stats.num_stage1_blocks);
        const double stage2_unit =
            static_cast<double>(local_stats.num_stage2_candidates) *
            kLowOverheadStage2Weight;
        const double unit_sum = std::max(stage1_unit + stage2_unit, 1.0);
        const double stage2_ratio = stage2_unit / unit_sum;
        const double stage2_ms = classify_wall_ms * stage2_ratio;
        const double stage1_ms = std::max(0.0, classify_wall_ms - stage2_ms);
        ctx.stats().probe_stage1_ms += stage1_ms;
        ctx.stats().probe_stage2_ms += stage2_ms;
    }
    sink.FinalizeCluster();
    ctx.stats().probe_submit_ms += sink.submit_cpu_ms();
    ctx.stats().probe_submit_prepare_vec_only_ms += sink.prepare_vec_only_ms_;
    ctx.stats().probe_submit_prepare_all_ms += sink.prepare_all_ms_;
    ctx.stats().probe_classify_ms =
        ctx.stats().probe_stage1_ms + ctx.stats().probe_stage2_ms;
    ctx.stats().probe_time_ms = ctx.stats().probe_prepare_ms +
        ctx.stats().probe_stage1_ms + ctx.stats().probe_stage2_ms +
        ctx.stats().probe_submit_ms;

    // Aggregate classification statistics into SearchContext.
    // total_safe_in  = S1 SafeIn (I/O submitted)
    // total_safe_out = S1 SafeOut (skipped)
    // total_uncertain = final Uncertain candidates (S1 uncertain minus S2 eliminations)
    ctx.stats().total_safe_in   += local_stats.s1_safein;
    ctx.stats().total_safe_out  += local_stats.s1_safeout;
    ctx.stats().s2_safe_in      += local_stats.s2_safein;
    ctx.stats().s2_safe_out     += local_stats.s2_safeout;
    ctx.stats().s2_uncertain    += local_stats.s2_uncertain;
    ctx.stats().total_uncertain +=
        local_stats.s1_uncertain - local_stats.s2_safein - local_stats.s2_safeout;
}

// ============================================================================
// ProbeAndDrainInterleaved — unified event loop
// ============================================================================

void OverlapScheduler::ProbeAndDrainInterleaved(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<ClusterID>& sorted_clusters) {
    static constexpr uint32_t kPendingRequestFlushThreshold = 32;

    std::vector<IoCompletion> comps(128);
    const uint32_t reserve_slots = std::min(
        config_.cluster_submit_reserve,
        config_.io_queue_depth > 0 ? config_.io_queue_depth - 1 : 0);
    uint32_t vec_flush_threshold =
        (config_.submit_batch_size == 0)
            ? std::numeric_limits<uint32_t>::max()
            : config_.submit_batch_size;
    if (config_.io_queue_depth > 0) {
        const uint32_t capacity_for_vec =
            std::max(1u, config_.io_queue_depth - reserve_slots);
        vec_flush_threshold = std::min(vec_flush_threshold, capacity_for_vec);
    }
    uint32_t clusters_since_data_submit = 0;
    float ema_req_per_cluster = static_cast<float>(vec_flush_threshold);
    auto record_submit = [&](AsyncReader& reader) {
        auto ts0 = std::chrono::steady_clock::now();
        uint32_t submitted = reader.Submit();
        ctx.stats().uring_submit_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ts0).count();
        ctx.stats().total_io_submitted += submitted;
        if (submitted > 0) {
            ctx.stats().total_submit_calls++;
        }
    };

    auto drain_completions = [&](AsyncReader& reader, bool wait_for_one) {
        uint32_t n = 0;
        auto tw0 = std::chrono::steady_clock::now();
        if (wait_for_one) {
            n = reader.WaitAndPoll(comps.data(),
                                   static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
        } else {
            n = reader.Poll(comps.data(), static_cast<uint32_t>(comps.size()));
        }
        for (uint32_t j = 0; j < n; ++j) {
            DispatchCompletion(comps[j].user_data, ctx, reranker);
        }
    };

    auto flush_shared_reader = [&](bool force,
                                  bool prioritize_cluster_reads,
                                  bool vec_batch_ready) {
        if (cluster_reader_.prepped() == 0) return;
        if (!force && !prioritize_cluster_reads && !vec_batch_ready) return;
        record_submit(cluster_reader_);
    };

    auto flush_isolated_readers = [&](bool force,
                                      bool prioritize_cluster_reads,
                                      bool vec_batch_ready) {
        if (cluster_reader_.prepped() > 0 &&
            (force || prioritize_cluster_reads)) {
            record_submit(cluster_reader_);
        }
        if (data_reader_.prepped() > 0 &&
            (force || vec_batch_ready)) {
            record_submit(data_reader_);
        }
    };

    enum class PendingFlushReason : uint8_t {
        Hard,
        Soft,
        Tail,
        StopSensitive,
        Final,
    };

    auto emit_pending_with_reason = [&](PendingFlushReason reason) {
        const uint32_t pending = PendingDataRequestCount();
        if (pending == 0) return false;
        EmitPendingDataRequests(ctx, pending);
        ctx.stats().total_submit_window_flushes++;
        if (reason == PendingFlushReason::Tail ||
            reason == PendingFlushReason::Final) {
            ctx.stats().total_submit_window_tail_flushes++;
        }
        if (reason == PendingFlushReason::StopSensitive) {
            ctx.stats().total_submit_stop_flushes++;
        }
        return true;
    };

    auto force_flush_and_submit = [&](PendingFlushReason reason) {
        const bool emitted = emit_pending_with_reason(reason);
        if (isolated_submission_mode_) {
            flush_isolated_readers(/*force=*/true,
                                   /*prioritize_cluster_reads=*/false,
                                   /*vec_batch_ready=*/true);
        } else {
            flush_shared_reader(/*force=*/true,
                                /*prioritize_cluster_reads=*/false,
                                /*vec_batch_ready=*/true);
        }
        return emitted;
    };

    auto stop_safe_flush = [&]() {
        force_flush_and_submit(PendingFlushReason::StopSensitive);
    };

    auto apply_submit_policy = [&](bool prepared_cluster_reads,
                                   bool final_cluster,
                                   bool stop_sensitive,
                                   uint32_t remaining_clusters) {
        if (stop_sensitive) {
            stop_safe_flush();
            if (prepared_cluster_reads) {
                if (isolated_submission_mode_) {
                    flush_isolated_readers(/*force=*/true,
                                           /*prioritize_cluster_reads=*/true,
                                           /*vec_batch_ready=*/true);
                } else {
                    flush_shared_reader(/*force=*/true,
                                        /*prioritize_cluster_reads=*/true,
                                        /*vec_batch_ready=*/true);
                }
            }
            clusters_since_data_submit = 0;
            return;
        }

        const uint32_t pending = PendingDataRequestCount();
        uint32_t dynamic_flush_threshold = vec_flush_threshold;
        uint32_t dynamic_interval_limit = kSubmitIntervalLimit;
        if (config_.enable_online_submit_tuning) {
            const uint32_t tuned = static_cast<uint32_t>(std::lround(
                2.0f * std::max(1.0f, ema_req_per_cluster)));
            dynamic_flush_threshold = std::clamp(
                tuned, config_.submit_batch_min, config_.submit_batch_max);
            if (config_.io_queue_depth > 0) {
                const uint32_t capacity_for_vec =
                    std::max(1u, config_.io_queue_depth - reserve_slots);
                dynamic_flush_threshold =
                    std::min(dynamic_flush_threshold, capacity_for_vec);
            }
            if (ema_req_per_cluster < 8.0f) {
                dynamic_interval_limit = 2;
            } else if (ema_req_per_cluster < 16.0f) {
                dynamic_interval_limit = 3;
            }
        }
        const bool hard_flush =
            pending >= kPendingRequestFlushThreshold;
        const bool soft_flush = pending >= dynamic_flush_threshold;
        const bool likely_cannot_fill_next_batch =
            config_.enable_online_submit_tuning &&
            pending > 0 &&
            (static_cast<float>(pending) +
             ema_req_per_cluster * static_cast<float>(remaining_clusters) <
             static_cast<float>(dynamic_flush_threshold));
        const bool tail_flush =
            (final_cluster && pending > 0) ||
            (pending >= kSubmitTailMinBatch &&
             clusters_since_data_submit >= dynamic_interval_limit &&
             likely_cannot_fill_next_batch);

        bool emitted = false;
        if (hard_flush) {
            emitted = emit_pending_with_reason(PendingFlushReason::Hard);
        } else if (soft_flush) {
            emitted = emit_pending_with_reason(PendingFlushReason::Soft);
        } else if (tail_flush) {
            emitted = emit_pending_with_reason(PendingFlushReason::Tail);
        }

        if (isolated_submission_mode_) {
            const bool vec_batch_ready = hard_flush || soft_flush || tail_flush;
            flush_isolated_readers(/*force=*/hard_flush || final_cluster,
                                   /*prioritize_cluster_reads=*/prepared_cluster_reads,
                                   vec_batch_ready);
            if (emitted && data_reader_.prepped() == 0) {
                clusters_since_data_submit = 0;
            }
        } else {
            const bool force_submit = hard_flush || final_cluster;
            flush_shared_reader(/*force=*/force_submit,
                                /*prioritize_cluster_reads=*/prepared_cluster_reads,
                                /*vec_batch_ready=*/hard_flush || soft_flush || tail_flush);
            if (emitted && cluster_reader_.prepped() == 0) {
                clusters_since_data_submit = 0;
            }
        }
    };

    for (size_t i = 0; i < sorted_clusters.size(); ++i) {
        uint32_t cid = sorted_clusters[i];

        const ParsedCluster* cluster_to_probe = nullptr;
        if (config_.clu_read_mode == CluReadMode::FullPreload) {
            cluster_to_probe = GetResidentParsedCluster(cid);
            if (cluster_to_probe == nullptr) {
                std::fprintf(stderr,
                             "FATAL: resident cluster view missing for cluster %u\n",
                             cid);
                std::abort();
            }
        } else {
            // 1. Wait until target cluster is ready (consuming other CQEs meanwhile).
            while (ready_clusters_.find(cid) == ready_clusters_.end()) {
                if (isolated_submission_mode_) {
                    emit_pending_with_reason(PendingFlushReason::Final);
                    flush_isolated_readers(/*force=*/true,
                                           /*prioritize_cluster_reads=*/false,
                                           /*vec_batch_ready=*/true);
                    if (cluster_reader_.InFlight() > 0) {
                        drain_completions(cluster_reader_, /*wait_for_one=*/true);
                    } else {
                        drain_completions(cluster_reader_, /*wait_for_one=*/false);
                    }
                    drain_completions(data_reader_, /*wait_for_one=*/false);
                } else {
                    emit_pending_with_reason(PendingFlushReason::Final);
                    flush_shared_reader(/*force=*/true,
                                        /*prioritize_cluster_reads=*/false,
                                        /*vec_batch_ready=*/true);
                    drain_completions(cluster_reader_, /*wait_for_one=*/true);
                }
            }
            cluster_to_probe = &ready_clusters_[cid];
        }

        {
            auto tp0 = std::chrono::steady_clock::now();
            ProbeCluster(*cluster_to_probe, cid, ctx, reranker);
            (void)tp0;
        }
        ++clusters_since_data_submit;

        if (config_.clu_read_mode != CluReadMode::FullPreload) {
            ready_clusters_.erase(cid);
        }

        // Early stop
        if (config_.early_stop) {
            if (use_crc_) {
                stop_safe_flush();
                // CRC early stop: use RaBitQ estimate heap distance
                float est_kth = (est_heap_.size() >= est_top_k_)
                    ? est_heap_.front().first
                    : std::numeric_limits<float>::infinity();
                auto t_crc_decision = std::chrono::steady_clock::now();
                const bool should_stop = crc_stopper_.ShouldStop(
                    static_cast<uint32_t>(i + 1), est_kth);
                ctx.stats().crc_decision_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_crc_decision).count();
                if (should_stop) {
                    ctx.stats().total_crc_would_stop++;
                    if (!config_.crc_no_break) {
                        ctx.stats().early_stopped = true;
                        ctx.stats().clusters_skipped =
                            static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
                        ctx.stats().crc_clusters_probed =
                            static_cast<uint32_t>(i + 1);
                        stop_safe_flush();
                        if (isolated_submission_mode_) {
                            drain_completions(data_reader_, /*wait_for_one=*/false);
                            drain_completions(cluster_reader_, /*wait_for_one=*/false);
                        } else {
                            drain_completions(cluster_reader_, /*wait_for_one=*/false);
                        }
                        break;
                    }
                }
            } else {
                // Legacy early stop: exact L2 collector vs static d_k
                if (ctx.collector().Full() &&
                    ctx.collector().TopDistance() < index_.conann().d_k()) {
                    ctx.stats().early_stopped = true;
                    ctx.stats().clusters_skipped =
                        static_cast<uint32_t>(sorted_clusters.size() - 1 - i);
                    stop_safe_flush();
                    if (isolated_submission_mode_) {
                        drain_completions(data_reader_, /*wait_for_one=*/false);
                        drain_completions(cluster_reader_, /*wait_for_one=*/false);
                    } else {
                        drain_completions(cluster_reader_, /*wait_for_one=*/false);
                    }
                    break;
                }
            }
        }

        // 4. Sliding window refill — cluster reads are prepared first, then
        // coalesced with vec reads into a single submit when possible.
        bool prepared_cluster_reads = false;
        if (config_.clu_read_mode != CluReadMode::FullPreload) {
            if (inflight_clusters_ < config_.refill_threshold &&
                next_to_submit_ < sorted_clusters.size()) {
                uint32_t refill = std::min(
                    config_.refill_count,
                    static_cast<uint32_t>(sorted_clusters.size()) -
                        next_to_submit_);
                if (reserve_slots > 0) {
                    refill = std::min(refill, reserve_slots);
                }
                for (uint32_t r = 0; r < refill; ++r) {
                    SubmitClusterRead(sorted_clusters[next_to_submit_++]);
                }
                prepared_cluster_reads = refill > 0;
            }
        }
        const bool final_cluster = (i + 1 == sorted_clusters.size());
        const uint32_t pending_before_policy = PendingDataRequestCount();
        apply_submit_policy(prepared_cluster_reads,
                            final_cluster,
                            /*stop_sensitive=*/false,
                            static_cast<uint32_t>(sorted_clusters.size() - 1 - i));
        if (config_.enable_online_submit_tuning) {
            const float alpha = std::clamp(config_.submit_ema_alpha, 0.0f, 1.0f);
            ema_req_per_cluster =
                (1.0f - alpha) * ema_req_per_cluster +
                alpha * static_cast<float>(pending_before_policy);
        }
    }

    if (isolated_submission_mode_) {
        emit_pending_with_reason(PendingFlushReason::Final);
        flush_isolated_readers(/*force=*/true,
                               /*prioritize_cluster_reads=*/false,
                               /*vec_batch_ready=*/true);
    } else {
        emit_pending_with_reason(PendingFlushReason::Final);
            flush_shared_reader(/*force=*/true,
                                /*prioritize_cluster_reads=*/false,
                                /*vec_batch_ready=*/true);
    }
}

// ============================================================================
// FinalDrain
// ============================================================================

void OverlapScheduler::FinalDrain(SearchContext& ctx,
                                   RerankConsumer& reranker) {
    std::vector<IoCompletion> comps(128);
    auto drain_reader = [&](AsyncReader& reader) {
        while (reader.InFlight() > 0) {
            auto tw0 = std::chrono::steady_clock::now();
            uint32_t n = reader.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
            for (uint32_t j = 0; j < n; ++j) {
                DispatchCompletion(comps[j].user_data, ctx, reranker);
            }
        }
        for (;;) {
            uint32_t n = reader.Poll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            if (n == 0) break;
            for (uint32_t j = 0; j < n; ++j) {
                DispatchCompletion(comps[j].user_data, ctx, reranker);
            }
        }
    };

    drain_reader(cluster_reader_);
    if (isolated_submission_mode_) {
        drain_reader(data_reader_);
    }
    CleanupPendingSlots();
}

// ============================================================================
// FetchMissingPayloads
// ============================================================================

void OverlapScheduler::FetchMissingPayloads(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<CollectorEntry>& results) {
    int fd = index_.segment().data_reader().fd();

    for (const auto& entry : results) {
        if (reranker.HasPayload(entry.addr.offset)) continue;
        if (entry.addr.size <= vec_bytes_) continue;  // No payload data

        uint32_t payload_len = entry.addr.size - vec_bytes_;
        uint64_t payload_off = entry.addr.offset + vec_bytes_;
        uint8_t* buf = buffer_pool_.Acquire(payload_len);
        PendingIO pio;
        pio.type = PendingIO::Type::PAYLOAD;
        pio.addr = entry.addr;
        pio.read_offset = payload_off;
        pio.read_length = payload_len;
        uint32_t slot_id = AllocatePendingSlot(std::move(pio), buf,
                                               PendingBufferCleanup::Pool);
        CheckPrepRead(data_reader_.PrepReadTagged(
                          fd, buf, payload_len, payload_off, slot_id),
                      "payload");
    }

    if (data_reader_.prepped() > 0 || data_reader_.InFlight() > 0) {
        auto ts0 = std::chrono::steady_clock::now();
        uint32_t submitted = data_reader_.Submit();
        ctx.stats().uring_submit_ms += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - ts0).count();
        ctx.stats().total_io_submitted += submitted;
        ctx.stats().total_payload_fetched += submitted;
        if (submitted > 0) {
            ctx.stats().total_submit_calls++;
        }

        std::vector<IoCompletion> comps(128);
        while (data_reader_.InFlight() > 0) {
            auto tw0 = std::chrono::steady_clock::now();
            uint32_t n = data_reader_.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
            for (uint32_t i = 0; i < n; ++i) {
                DispatchCompletion(comps[i].user_data, ctx, reranker);
            }
        }
        // Drain sync completions (pread path)
        for (;;) {
            uint32_t n = data_reader_.Poll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            if (n == 0) break;
            for (uint32_t i = 0; i < n; ++i) {
                DispatchCompletion(comps[i].user_data, ctx, reranker);
            }
        }
    }
}

// ============================================================================
// AssembleResults
// ============================================================================

SearchResults OverlapScheduler::AssembleResults(
    RerankConsumer& reranker,
    const std::vector<CollectorEntry>& results) {
    SearchResults sr;
    sr.results().reserve(results.size());

    const auto& schemas = index_.segment().data_reader().payload_schemas();
    bool has_payload = !schemas.empty();

    for (const auto& entry : results) {
        SearchResult result;
        result.distance = entry.distance;

        if (has_payload) {
            auto payload_buf = reranker.TakePayload(entry.addr.offset);
            if (payload_buf && entry.addr.size > vec_bytes_) {
                uint32_t payload_len = entry.addr.size - vec_bytes_;
                index_.segment().data_reader().ParsePayload(
                    payload_buf.get(), payload_len, 0, result.payload);
            }
        }

        sr.results().push_back(std::move(result));
    }

    return sr;
}

}  // namespace query
}  // namespace vdb
