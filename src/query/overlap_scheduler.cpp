#include "vdb/query/overlap_scheduler.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unistd.h>

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

inline void CheckReadStatus(const Status& s, const char* context) {
    if (!s.ok()) {
        std::fprintf(stderr, "FATAL: synchronous read failed (%s): %s\n",
                     context, s.ToString().c_str());
        std::abort();
    }
}

inline Status ReadExactFd(int fd, uint64_t offset, uint32_t length,
                          uint8_t* out_buffer) {
    if (fd < 0) {
        return Status::InvalidArgument("ReadExactFd invalid fd");
    }
    uint32_t total_read = 0;
    while (total_read < length) {
        ssize_t n = ::pread(fd, out_buffer + total_read,
                            length - total_read,
                            static_cast<off_t>(offset + total_read));
        if (n < 0) {
            return Status::IOError("pread failed");
        }
        if (n == 0) {
            return Status::IOError("unexpected EOF");
        }
        total_read += static_cast<uint32_t>(n);
    }
    return Status::OK();
}

inline uint8_t ResolveActiveExBits(const RaBitQConfig& rabitq_config,
                                   const SearchConfig& search_config) {
    const uint8_t stored_ex_bits = rabitq_config.stage2_payload_bits();
    if (!search_config.rabitq_active_ex_bits_set) {
        return stored_ex_bits;
    }
    return std::min<uint8_t>(search_config.rabitq_active_ex_bits,
                             stored_ex_bits);
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
      aligned_vec_bytes_((vec_bytes_ + 4095u) & ~4095u),
      est_top_k_(config.top_k),
      use_dynamic_safeout_(config.enable_dynamic_safeout),
      use_dynamic_safein_(config.dynamic_safein_mode != DynamicSafeInMode::Static),
      use_estimate_frontier_(use_dynamic_safeout_ || use_dynamic_safein_),
      estimator_(index.dim(), index.segment().rabitq_config().active_code_bits()),
		      prober_(index.conann(), index.dim(),
		              index.segment().rabitq_config().active_code_bits(),
		              index.segment().rabitq_config().effective_total_bits(),
			              ResolveActiveExBits(index.segment().rabitq_config(), config)) {
    if (index.used_hadamard()) {
        query_wrapper_.rotated_q.resize(index.dim());
    }
    if (index.logical_dim() != index.dim()) {
        query_wrapper_.padded_q.resize(index.dim(), 0.0f);
    }
    query_wrapper_.scratch.quant_query.reserve(index.dim());
    query_wrapper_.scratch.fastscan_lut.reserve(static_cast<size_t>(index.dim()) * 8 + 63);
    submitted_candidate_offsets_.Reserve(
        static_cast<size_t>(std::max(1u, config_.nprobe)) * 256);
    resident_scratch_.completions.resize(128);
    if (use_estimate_frontier_) {
        safeout_frontier_heap_.reserve(est_top_k_);
        safein_frontier_heap_.reserve(est_top_k_);
    }
    if (config_.dynamic_safein_defer_initial_clusters > 0 ||
        config_.dynamic_safein_defer_until_ready) {
        const uint32_t reserve_deferred =
            config_.dynamic_safein_defer_max_candidates > 0
                ? config_.dynamic_safein_defer_max_candidates
                : std::max(256u, config_.nprobe * 64u);
        deferred_safein_plans_.reserve(reserve_deferred);
    }
    if (config_.non_safeout_candidate_budget > 0) {
        budgeted_read_plan_heap_.reserve(config_.non_safeout_candidate_budget);
        pending_vec_only_plans_.reserve(config_.non_safeout_candidate_budget);
        if (config_.budgeted_prefetch_limit > 0) {
            pending_spec_vec_plans_.reserve(config_.budgeted_prefetch_limit);
            speculative_prefetch_offsets_.reserve(config_.budgeted_prefetch_limit * 2u);
            speculative_final_offsets_.reserve(config_.budgeted_prefetch_limit);
            speculative_vector_cache_.reserve(config_.budgeted_prefetch_limit);
        }
        pending_slots_.reserve(config_.non_safeout_candidate_budget);
    }
    // Stage 2: precompute margin divisor from effective total bits.
    const uint8_t active_ex_bits =
        ResolveActiveExBits(index.segment().rabitq_config(), config_);
    if (index.segment().rabitq_config().effective_total_bits() > 1 &&
        active_ex_bits > 0) {
        has_s2_ = true;
        margin_s2_divisor_ = static_cast<float>(
            1u << (index.segment().rabitq_config().effective_total_bits() - 1));
    }
    buffer_pool_.Prime(vec_bytes_, std::max(1u, config_.io_queue_depth));
    InitializeDataBufferSlab();
}

OverlapScheduler::~OverlapScheduler() {
    CleanupPendingSlots();
    for (uint8_t* buf : fixed_vec_buffers_) {
        std::free(buf);
    }
    for (uint8_t* buf : vec_only_owned_buffers_) {
        std::free(buf);
    }
}

void OverlapScheduler::SetFalseStatsTrueTopKRows(
    const std::unordered_set<uint32_t>* rows) {
    config_.false_stats_true_topk_rows = rows;
}

void OverlapScheduler::SetVectorReadTraceQueryIndex(uint32_t query_index) {
    vector_read_trace_query_index_ = query_index;
}

bool OverlapScheduler::UseSeparateRecordStore() const {
    return config_.separate_record_store.enabled &&
           config_.separate_record_store.redirect_payload_reads;
}

bool OverlapScheduler::UseVectorSidecarStore() const {
    return config_.separate_record_store.enabled;
}

bool OverlapScheduler::UseInlineHotRecordStore() const {
    return config_.inline_hot_record_store.enabled;
}

const SeparateRecordLocation& OverlapScheduler::LookupSeparateRecordOrAbort(
    SearchContext& ctx, AddressEntry addr) const {
    const SeparateRecordMap* map = config_.separate_record_store.address_map;
    if (map == nullptr) {
        std::fprintf(stderr,
                     "FATAL: separate record store enabled without address map\n");
        std::abort();
    }
    auto it = map->find(addr.offset);
    if (it == map->end()) {
        ctx.stats().separate_store_lookup_misses++;
        std::fprintf(stderr,
                     "FATAL: no separate-store mapping for record offset=%llu\n",
                     static_cast<unsigned long long>(addr.offset));
        std::abort();
    }
    return it->second;
}

uint64_t OverlapScheduler::SeparateVectorOffset(
    const SeparateRecordLocation& loc) const {
    return loc.row_id * static_cast<uint64_t>(vec_bytes_);
}

void OverlapScheduler::RecordVectorReadTrace(AddressEntry addr,
                                             bool from_vec_all) {
    if (config_.vector_read_trace == nullptr) return;
    VectorReadTraceEntry entry;
    entry.query_index = vector_read_trace_query_index_;
    entry.request_index = vector_read_trace_request_index_++;
    entry.combined_offset = addr.offset;
    entry.read_length = vec_bytes_;
    entry.request_type = from_vec_all ? 1 : 0;
    config_.vector_read_trace->push_back(entry);
}

void OverlapScheduler::MarkSafeInConfidenceTraceSelectedTopK(
    const std::vector<CollectorEntry>& results) {
    auto* trace = config_.safein_confidence_trace;
    if (trace == nullptr || safein_confidence_trace_query_start_ >= trace->size()) {
        return;
    }
    std::unordered_set<uint64_t> selected_offsets;
    selected_offsets.reserve(results.size() * 2u + 1u);
    for (const auto& result : results) {
        selected_offsets.insert(result.addr.offset);
    }
    for (size_t i = safein_confidence_trace_query_start_; i < trace->size(); ++i) {
        (*trace)[i].selected_topk =
            selected_offsets.count((*trace)[i].candidate_offset) != 0;
    }
}

storage::HotPayloadDescriptor
OverlapScheduler::ReadInlinePayloadDescriptorOrAbort(
    SearchContext& ctx, AddressEntry addr) const {
    const uint32_t descriptor_bytes =
        config_.inline_hot_record_store.descriptor_bytes;
    if (descriptor_bytes != sizeof(storage::HotPayloadDescriptor) ||
        addr.size < vec_bytes_ + descriptor_bytes) {
        ctx.stats().inline_descriptor_errors++;
        std::fprintf(stderr,
                     "FATAL: invalid inline hot-record descriptor config "
                     "addr_offset=%llu addr_size=%u vec_bytes=%u desc_bytes=%u\n",
                     static_cast<unsigned long long>(addr.offset), addr.size,
                     vec_bytes_, descriptor_bytes);
        std::abort();
    }

    uint8_t desc_buf[sizeof(storage::HotPayloadDescriptor)] = {};
    const int descriptor_fd =
        config_.inline_hot_record_store.buffered_hot_record_fd >= 0
            ? config_.inline_hot_record_store.buffered_hot_record_fd
            : index_.segment().data_reader().fd();
    Status s = ReadExactFd(descriptor_fd,
                           addr.offset + vec_bytes_, descriptor_bytes,
                           desc_buf);
    ctx.stats().inline_descriptor_read_requests++;
    if (!s.ok()) {
        ctx.stats().inline_descriptor_errors++;
        std::fprintf(stderr,
                     "FATAL: failed to read inline hot-record descriptor "
                     "offset=%llu: %s\n",
                     static_cast<unsigned long long>(addr.offset + vec_bytes_),
                     s.ToString().c_str());
        std::abort();
    }

    auto desc = storage::DecodeHotPayloadDescriptor(desc_buf);
    s = storage::ValidateHotPayloadDescriptor(desc);
    if (!s.ok()) {
        ctx.stats().inline_descriptor_errors++;
        std::fprintf(stderr,
                     "FATAL: invalid inline hot-record descriptor "
                     "addr_offset=%llu: %s\n",
                     static_cast<unsigned long long>(addr.offset),
                     s.ToString().c_str());
        std::abort();
    }
    return desc;
}

OverlapScheduler::PayloadLocation
OverlapScheduler::PayloadLocationFromDescriptorOrAbort(
    SearchContext& ctx, AddressEntry addr,
    const storage::HotPayloadDescriptor& desc) const {
        Status s = storage::ValidateHotPayloadDescriptor(desc);
        if (!s.ok()) {
            ctx.stats().inline_descriptor_errors++;
            std::fprintf(stderr,
                         "FATAL: invalid inline hot-record descriptor "
                         "addr_offset=%llu: %s\n",
                         static_cast<unsigned long long>(addr.offset),
                         s.ToString().c_str());
            std::abort();
        }
        if (desc.payload_bytes > static_cast<uint64_t>(UINT32_MAX)) {
            ctx.stats().inline_descriptor_errors++;
            std::fprintf(stderr,
                         "FATAL: inline hot-record payload too large "
                         "addr_offset=%llu payload_bytes=%llu\n",
                         static_cast<unsigned long long>(addr.offset),
                         static_cast<unsigned long long>(desc.payload_bytes));
            std::abort();
        }

        PayloadLocation loc;
        loc.length = static_cast<uint32_t>(desc.payload_bytes);
        if (desc.payload_storage_type ==
            static_cast<uint8_t>(storage::HotPayloadStorageType::kInlinePayload)) {
            loc.fd = config_.inline_hot_record_store.buffered_hot_record_fd >= 0
                ? config_.inline_hot_record_store.buffered_hot_record_fd
                : index_.segment().data_reader().fd();
            loc.offset = addr.offset + vec_bytes_ +
                config_.inline_hot_record_store.descriptor_bytes;
            loc.from_inline_hot_record = true;
            return loc;
        }

        if (desc.payload_storage_type == static_cast<uint8_t>(
                storage::HotPayloadStorageType::kPrefixColdPointer)) {
            loc.inline_prefix_length = desc.inline_bytes;
            loc.inline_prefix_offset = addr.offset + vec_bytes_ +
                config_.inline_hot_record_store.descriptor_bytes;
            loc.inline_prefix_fd =
                config_.inline_hot_record_store.buffered_hot_record_fd >= 0
                    ? config_.inline_hot_record_store.buffered_hot_record_fd
                    : index_.segment().data_reader().fd();
        }

        if (config_.inline_hot_record_store.cold_payload_fd < 0 &&
            loc.length > 0) {
            ctx.stats().inline_descriptor_errors++;
            std::fprintf(stderr,
                         "FATAL: inline hot-record cold payload requested "
                         "without payload.cold.dat fd\n");
            std::abort();
        }
        loc.fd = config_.inline_hot_record_store.cold_payload_fd;
        loc.offset = desc.payload_offset;
        loc.from_cold_payload = true;
        return loc;
}

void OverlapScheduler::CacheInlinePayloadLocationFromRecord(
    SearchContext& ctx, AddressEntry addr, const uint8_t* record,
    uint32_t record_bytes) {
    if (!UseInlineHotRecordStore() ||
        payload_location_cache_.count(addr.offset) > 0) {
        return;
    }
    const uint32_t descriptor_bytes =
        config_.inline_hot_record_store.descriptor_bytes;
    if (descriptor_bytes != sizeof(storage::HotPayloadDescriptor) ||
        record_bytes < vec_bytes_ + descriptor_bytes) {
        return;
    }
    const auto desc = storage::DecodeHotPayloadDescriptor(record + vec_bytes_);
    payload_location_cache_.emplace(
        addr.offset, PayloadLocationFromDescriptorOrAbort(ctx, addr, desc));
}

OverlapScheduler::PayloadLocation OverlapScheduler::LocatePayloadOrAbort(
    SearchContext& ctx, AddressEntry addr) {
    const auto cached = payload_location_cache_.find(addr.offset);
    if (cached != payload_location_cache_.end()) {
        return cached->second;
    }

    if (UseInlineHotRecordStore()) {
        const auto* metadata = config_.inline_hot_record_store.payload_metadata;
        if (metadata != nullptr) {
            const auto found = metadata->find(addr.offset);
            if (found == metadata->end()) {
                ctx.stats().inline_descriptor_errors++;
                std::fprintf(stderr,
                             "FATAL: inline payload metadata missing for "
                             "addr_offset=%llu\n",
                             static_cast<unsigned long long>(addr.offset));
                std::abort();
            }
            storage::HotPayloadDescriptor desc{};
            desc.payload_storage_type = found->second.payload_storage_type;
            desc.header_size = sizeof(storage::HotPayloadDescriptor);
            desc.inline_bytes = found->second.inline_bytes;
            desc.payload_offset = found->second.payload_offset;
            desc.payload_bytes = found->second.payload_bytes;
            PayloadLocation loc =
                PayloadLocationFromDescriptorOrAbort(ctx, addr, desc);
            payload_location_cache_.emplace(addr.offset, loc);
            return loc;
        }
        const auto desc = ReadInlinePayloadDescriptorOrAbort(ctx, addr);
        PayloadLocation loc =
            PayloadLocationFromDescriptorOrAbort(ctx, addr, desc);
        payload_location_cache_.emplace(addr.offset, loc);
        return loc;
    }

    PayloadLocation loc;
    if (UseSeparateRecordStore()) {
        const SeparateRecordLocation& sep =
            LookupSeparateRecordOrAbort(ctx, addr);
        loc.length = sep.payload_bytes;
        loc.offset = sep.payload_offset;
        loc.fd = config_.separate_record_store.payload_fd;
        payload_location_cache_.emplace(addr.offset, loc);
        return loc;
    }

    if (addr.size <= vec_bytes_) {
        payload_location_cache_.emplace(addr.offset, loc);
        return loc;
    }
    loc.length = addr.size - vec_bytes_;
    loc.offset = addr.offset + vec_bytes_;
    loc.fd = index_.segment().data_reader().fd();
    payload_location_cache_.emplace(addr.offset, loc);
    return loc;
}

bool OverlapScheduler::AddSafeOutFrontierEstimate(
    const EstimateHeapEntry& estimate) {
    if (!use_estimate_frontier_ || est_top_k_ == 0) return false;
    SafeOutFrontierEntry entry;
    entry.distance = estimate.distance;
    entry.error_bound = estimate.error_bound;
    entry.upper_bound = estimate.distance + estimate.error_bound;
    if (safeout_frontier_heap_.size() < est_top_k_) {
        safeout_frontier_heap_.push_back(entry);
        std::push_heap(safeout_frontier_heap_.begin(),
                       safeout_frontier_heap_.end());
        return true;
    } else if (entry.upper_bound < safeout_frontier_heap_.front().upper_bound) {
        std::pop_heap(safeout_frontier_heap_.begin(),
                      safeout_frontier_heap_.end());
        safeout_frontier_heap_.back() = entry;
        std::push_heap(safeout_frontier_heap_.begin(),
                       safeout_frontier_heap_.end());
        return true;
    }
    return false;
}

bool OverlapScheduler::AddSafeInFrontierEstimate(float lower_bound) {
    if (!use_estimate_frontier_ || est_top_k_ == 0 ||
        !std::isfinite(lower_bound)) {
        return false;
    }
    SafeInFrontierEntry entry;
    entry.lower_bound = lower_bound;
    if (safein_frontier_heap_.size() < est_top_k_) {
        safein_frontier_heap_.push_back(entry);
        std::push_heap(safein_frontier_heap_.begin(),
                       safein_frontier_heap_.end());
        return true;
    } else if (entry.lower_bound < safein_frontier_heap_.front().lower_bound) {
        std::pop_heap(safein_frontier_heap_.begin(),
                      safein_frontier_heap_.end());
        safein_frontier_heap_.back() = entry;
        std::push_heap(safein_frontier_heap_.begin(),
                       safein_frontier_heap_.end());
        return true;
    }
    return false;
}

float OverlapScheduler::SafeOutFrontierUpper() const {
    if (!use_estimate_frontier_ || est_top_k_ == 0 ||
        safeout_frontier_heap_.size() < est_top_k_) {
        return std::numeric_limits<float>::infinity();
    }
    return safeout_frontier_heap_.front().upper_bound;
}

float OverlapScheduler::SafeInFrontierLower() const {
    if (!use_estimate_frontier_ || est_top_k_ == 0 ||
        safein_frontier_heap_.size() < est_top_k_) {
        return std::numeric_limits<float>::infinity();
    }
    return safein_frontier_heap_.front().lower_bound;
}

float OverlapScheduler::DynamicSafeInReferenceFrontier() const {
    switch (config_.dynamic_safein_mode) {
        case DynamicSafeInMode::Static:
        case DynamicSafeInMode::Frontier:
            return SafeInFrontierLower();
    }
    return SafeInFrontierLower();
}

void OverlapScheduler::UpdateDynamicSafeInState(SearchContext& ctx,
                                                bool advance_probe) {
    if (!use_dynamic_safein_) return;

    const bool was_ready = dynamic_safein_ready_;
    if (advance_probe) {
        ++dynamic_safein_probes_seen_;
    }
    const float frontier = DynamicSafeInReferenceFrontier();
    dynamic_safein_current_frontier_ = frontier;

    if (std::isfinite(frontier)) {
        if (std::isfinite(dynamic_safein_last_frontier_)) {
            const float diff = std::abs(frontier - dynamic_safein_last_frontier_);
            const float denom = std::max(std::abs(dynamic_safein_last_frontier_), 1e-12f);
            const float rel = diff / denom;
            if (diff <= config_.dynamic_safein_abs_tol ||
                rel <= config_.dynamic_safein_rel_tol) {
                ++dynamic_safein_stable_count_;
            } else {
                dynamic_safein_stable_count_ = 1;
            }
        } else {
            dynamic_safein_stable_count_ = 1;
        }
        dynamic_safein_last_frontier_ = frontier;
    } else {
        dynamic_safein_stable_count_ = 0;
    }

    const bool has_frontier = std::isfinite(frontier);
    const bool min_probe_ready =
        dynamic_safein_probes_seen_ >= config_.dynamic_safein_min_probes;
    const bool stable_ready =
        dynamic_safein_stable_count_ >=
        std::max<uint32_t>(1u, config_.dynamic_safein_stable_probes);

    switch (config_.dynamic_safein_mode) {
        case DynamicSafeInMode::Static:
            dynamic_safein_ready_ = true;
            break;
        case DynamicSafeInMode::Frontier:
            dynamic_safein_ready_ =
                has_frontier && min_probe_ready && stable_ready;
            break;
    }

    if (!was_ready && dynamic_safein_ready_) {
        ctx.stats().dynamic_safein_ready_transitions++;
    }
}

float OverlapScheduler::SafeInThresholdForProbe() const {
    if (!use_dynamic_safein_ ||
        config_.dynamic_safein_mode == DynamicSafeInMode::Static) {
        return index_.conann().safein_d_k();
    }

    const float disabled = -std::numeric_limits<float>::infinity();

    switch (config_.dynamic_safein_mode) {
        case DynamicSafeInMode::Static:
            return index_.conann().safein_d_k();
        case DynamicSafeInMode::Frontier:
            return dynamic_safein_ready_ &&
                std::isfinite(dynamic_safein_current_frontier_)
                    ? dynamic_safein_current_frontier_
                    : disabled;
    }
    return disabled;
}

void OverlapScheduler::RecordDynamicSafeInStats(SearchContext& ctx,
                                                float threshold,
                                                float frontier) {
    if (!use_dynamic_safein_) return;

    auto& stats = ctx.stats();
    stats.dynamic_safein_clusters++;
    if (std::isfinite(frontier)) {
        stats.dynamic_safein_frontier_samples++;
        stats.dynamic_safein_frontier_sum += frontier;
        stats.dynamic_safein_final_frontier = frontier;
    }
    if (std::isfinite(threshold)) {
        stats.dynamic_safein_active_clusters++;
        stats.dynamic_safein_threshold_samples++;
        stats.dynamic_safein_threshold_sum += threshold;
        stats.dynamic_safein_final_threshold = threshold;
        if (!std::isfinite(dynamic_safein_current_threshold_) ||
            std::abs(threshold - dynamic_safein_current_threshold_) > 1e-6f) {
            stats.dynamic_safein_threshold_changed_clusters++;
        }
        dynamic_safein_current_threshold_ = threshold;
    } else {
        stats.dynamic_safein_disabled_clusters++;
        dynamic_safein_current_threshold_ = threshold;
    }
}

bool OverlapScheduler::ShouldDeferSafeInPlans() const {
    if (!use_dynamic_safein_) return false;
    if (config_.dynamic_safein_defer_initial_clusters == 0 &&
        !config_.dynamic_safein_defer_until_ready) {
        return false;
    }
    if (config_.dynamic_safein_defer_max_candidates > 0 &&
        deferred_safein_plans_.size() >= config_.dynamic_safein_defer_max_candidates) {
        return false;
    }
    if (config_.dynamic_safein_defer_initial_clusters > 0 &&
        dynamic_safein_probes_seen_ <= config_.dynamic_safein_defer_initial_clusters) {
        return true;
    }
    if (dynamic_safein_small_pool_extra_defer_ &&
        config_.dynamic_safein_defer_initial_clusters > 0 &&
        dynamic_safein_probes_seen_ <=
            config_.dynamic_safein_defer_initial_clusters + 1) {
        return true;
    }
    if (config_.dynamic_safein_defer_until_ready && !dynamic_safein_ready_) {
        return true;
    }
    return false;
}

bool OverlapScheduler::ShouldHoldDeferredSafeInPlans() {
    if (config_.safein_prefetch_global_window > 0 &&
        config_.safein_prefetch_order != SafeInPrefetchOrder::Arrival &&
        !deferred_safein_plans_.empty()) {
        return true;
    }
    if (!use_dynamic_safein_) return false;
    if (deferred_safein_plans_.empty()) return false;
    if (config_.dynamic_safein_defer_initial_clusters > 0 &&
        dynamic_safein_probes_seen_ <
            config_.dynamic_safein_defer_initial_clusters) {
        return true;
    }
    if (config_.dynamic_safein_defer_initial_clusters > 0 &&
        dynamic_safein_probes_seen_ ==
            config_.dynamic_safein_defer_initial_clusters) {
        const uint32_t submit_batch =
            config_.submit_batch_size == 0 ? 32u : config_.submit_batch_size;
        const uint32_t small_pool_limit = std::max(128u, submit_batch * 4u);
        if (deferred_safein_plans_.size() < small_pool_limit) {
            dynamic_safein_small_pool_extra_defer_ = true;
            return true;
        }
    }
    if (config_.dynamic_safein_defer_until_ready && !dynamic_safein_ready_) {
        return true;
    }
    dynamic_safein_small_pool_extra_defer_ = false;
    return false;
}

void OverlapScheduler::RecordSafeInPrefetchDecision(
    SearchContext& ctx, bool has_truth, bool is_true_topk) const {
    auto& stats = ctx.stats();
    stats.safein_prefetch_candidates++;
    if (!has_truth) {
        stats.safein_prefetch_unknown++;
    } else if (is_true_topk) {
        stats.safein_prefetch_true_topk++;
    } else {
        stats.safein_prefetch_false++;
    }
}

uint32_t OverlapScheduler::SafeInReadLength(AddressEntry addr) const {
    if (UseInlineHotRecordStore()) {
        return addr.size;
    }
    const uint32_t min_read = std::min(addr.size, vec_bytes_);
    const uint32_t target = std::max(config_.safein_threshold_bytes, min_read);
    return std::min(addr.size, target);
}

bool OverlapScheduler::ShouldScheduleSafeInPrefetch(
    SearchContext& ctx, uint32_t read_length, uint32_t extra_bytes) {
    auto& stats = ctx.stats();
    stats.safein_prefetch_considered++;

    if (config_.safein_prefetch_max_count > 0 &&
        safein_prefetch_scheduled_count_ >= config_.safein_prefetch_max_count) {
        stats.safein_prefetch_skipped_count_limit++;
        return false;
    }

    const uint64_t next_bytes =
        safein_prefetch_scheduled_bytes_ + static_cast<uint64_t>(read_length);
    if (config_.safein_prefetch_max_bytes > 0 &&
        next_bytes > config_.safein_prefetch_max_bytes) {
        stats.safein_prefetch_skipped_byte_limit++;
        return false;
    }

    const uint64_t next_extra_bytes = safein_prefetch_scheduled_extra_bytes_ +
        static_cast<uint64_t>(extra_bytes);
    if (config_.safein_query_extra_bytes > 0 &&
        next_extra_bytes > config_.safein_query_extra_bytes) {
        stats.safein_prefetch_skipped_byte_limit++;
        return false;
    }
    if (config_.safein_max_full_payload_bytes > 0 &&
        extra_bytes > config_.safein_max_full_payload_bytes) {
        stats.safein_prefetch_skipped_byte_limit++;
        return false;
    }

    safein_prefetch_scheduled_count_++;
    safein_prefetch_scheduled_bytes_ = next_bytes;
    safein_prefetch_scheduled_extra_bytes_ = next_extra_bytes;
    stats.safein_prefetch_scheduled_bytes += read_length;
    return true;
}

bool OverlapScheduler::UseNonSafeOutCandidateBudget() const {
    return config_.non_safeout_candidate_budget > 0;
}

void OverlapScheduler::AddBudgetedReadPlan(SearchContext& ctx,
                                           const ReadPlanEntry& plan,
                                           float rank_key) {
    if (!UseNonSafeOutCandidateBudget()) return;
    if (!std::isfinite(rank_key)) {
        rank_key = std::numeric_limits<float>::infinity();
    }

    auto& stats = ctx.stats();
    stats.candidate_budget_seen++;
    BudgetedReadPlan entry{rank_key, plan};
    const uint32_t budget = config_.non_safeout_candidate_budget;
    bool accepted = false;
    if (budgeted_read_plan_heap_.size() < budget) {
        budgeted_read_plan_heap_.push_back(entry);
        std::push_heap(budgeted_read_plan_heap_.begin(),
                       budgeted_read_plan_heap_.end());
        accepted = true;
    } else if (!budgeted_read_plan_heap_.empty() &&
        rank_key < budgeted_read_plan_heap_.front().rank_key) {
        std::pop_heap(budgeted_read_plan_heap_.begin(),
                      budgeted_read_plan_heap_.end());
        budgeted_read_plan_heap_.back() = entry;
        std::push_heap(budgeted_read_plan_heap_.begin(),
                       budgeted_read_plan_heap_.end());
        accepted = true;
    }
    if (accepted) {
        MaybeScheduleBudgetedPrefetch(ctx, plan);
    }
}

void OverlapScheduler::MaybeScheduleBudgetedPrefetch(
    SearchContext& ctx, const ReadPlanEntry& plan) {
    if (!UseNonSafeOutCandidateBudget() ||
        config_.budgeted_prefetch_limit == 0 ||
        config_.execution_mode == QueryExecutionMode::SerialNoOverlap) {
        return;
    }
    auto& stats = ctx.stats();
    stats.budgeted_prefetch_considered++;
    if (stats.budgeted_prefetch_scheduled >= config_.budgeted_prefetch_limit) {
        stats.budgeted_prefetch_skipped_limit++;
        return;
    }
    const uint64_t key = plan.addr.offset;
    if (!speculative_prefetch_offsets_.insert(key).second) {
        stats.budgeted_prefetch_duplicates++;
        return;
    }
    pending_spec_vec_plans_.push_back(VecOnlyReadPlan{plan.addr});
    stats.budgeted_prefetch_scheduled++;
    stats.total_submit_window_requests++;
}

bool OverlapScheduler::TryUseSpeculativeVector(
    SearchContext& ctx, RerankConsumer& reranker, AddressEntry addr) {
    if (config_.budgeted_prefetch_limit == 0) return false;
    const uint64_t key = addr.offset;
    auto cached = speculative_vector_cache_.find(key);
    if (cached != speculative_vector_cache_.end()) {
        reranker.ConsumeOwnedVec(std::move(cached->second.storage), addr);
        speculative_vector_cache_.erase(cached);
        ctx.stats().budgeted_prefetch_cache_hits++;
        ctx.stats().budgeted_prefetch_used++;
        return true;
    }
    if (speculative_prefetch_offsets_.count(key) > 0) {
        speculative_final_offsets_.insert(key);
        ctx.stats().budgeted_prefetch_inflight_uses++;
        return true;
    }
    return false;
}

void OverlapScheduler::CacheSpeculativeVector(
    SearchContext& ctx, AddressEntry addr, const uint8_t* vec) {
    uint8_t* copy = static_cast<uint8_t*>(
        std::aligned_alloc(4096, aligned_vec_bytes_));
    if (copy == nullptr) {
        std::fprintf(stderr,
                     "FATAL: aligned_alloc failed for speculative vector cache (%u bytes)\n",
                     aligned_vec_bytes_);
        std::abort();
    }
    std::memcpy(copy, vec, vec_bytes_);
    speculative_vector_cache_[addr.offset] =
        SpeculativeVector{addr, AlignedBufPtr(copy)};
    ctx.stats().budgeted_prefetch_completed++;
}

void OverlapScheduler::FinalizeBudgetedPrefetchStats(SearchContext& ctx) {
    if (config_.budgeted_prefetch_limit == 0) return;
    auto& stats = ctx.stats();
    if (stats.budgeted_prefetch_scheduled >= stats.budgeted_prefetch_used) {
        stats.budgeted_prefetch_wasted =
            stats.budgeted_prefetch_scheduled - stats.budgeted_prefetch_used;
    }
    speculative_vector_cache_.clear();
    speculative_prefetch_offsets_.clear();
    speculative_final_offsets_.clear();
}

void OverlapScheduler::MaterializeBudgetedReadPlans(SearchContext& ctx,
                                                    RerankConsumer& reranker) {
    if (!UseNonSafeOutCandidateBudget() || budgeted_read_plan_heap_.empty()) {
        return;
    }
    std::sort_heap(budgeted_read_plan_heap_.begin(),
                   budgeted_read_plan_heap_.end());

    auto& stats = ctx.stats();
    stats.candidate_budget_selected =
        static_cast<uint32_t>(budgeted_read_plan_heap_.size());
    stats.candidate_budget_dropped =
        stats.candidate_budget_seen > stats.candidate_budget_selected
            ? stats.candidate_budget_seen - stats.candidate_budget_selected
            : 0;

    for (const auto& entry : budgeted_read_plan_heap_) {
        const ReadPlanEntry& plan = entry.plan;
        if (TryUseSpeculativeVector(ctx, reranker, plan.addr)) {
            stats.unique_fetch_candidates++;
            continue;
        }
        if (plan.type == PendingIO::Type::VEC_ALL &&
            ShouldScheduleSafeInPrefetch(
                ctx, plan.read_length,
                plan.read_length > vec_bytes_ ? plan.read_length - vec_bytes_ : 0)) {
            pending_all_plans_.push_back(plan);
            RecordSafeInPrefetchDecision(ctx, plan.has_truth,
                                         plan.is_true_topk);
        } else {
            pending_vec_only_plans_.push_back(VecOnlyReadPlan{plan.addr});
        }
        stats.total_submit_window_requests++;
        stats.unique_fetch_candidates++;
    }
    budgeted_read_plan_heap_.clear();
}

void OverlapScheduler::FlushDeferredSafeInPlans(SearchContext& ctx,
                                                float threshold,
                                                bool force) {
    if (deferred_safein_plans_.empty()) return;
    if (!force && !std::isfinite(threshold)) return;

    if (config_.safein_prefetch_order != SafeInPrefetchOrder::Arrival) {
        const auto order = config_.safein_prefetch_order;
        std::stable_sort(
            deferred_safein_plans_.begin(), deferred_safein_plans_.end(),
            [order](const DeferredSafeInPlan& a,
                    const DeferredSafeInPlan& b) {
                const float a_score = order == SafeInPrefetchOrder::ConfidencePerByte
                    ? a.confidence / static_cast<float>(std::max(1u, a.extra_bytes))
                    : a.confidence;
                const float b_score = order == SafeInPrefetchOrder::ConfidencePerByte
                    ? b.confidence / static_cast<float>(std::max(1u, b.extra_bytes))
                    : b.confidence;
                return a_score > b_score;
            });
    }

    const bool threshold_active = std::isfinite(threshold);
    for (const auto& plan : deferred_safein_plans_) {
        const bool would_use_vec_all =
            threshold_active &&
            plan.safein_upper_bound < threshold;
        if (would_use_vec_all) {
            ctx.stats().dynamic_safein_deferred_safein++;
        }
        if (would_use_vec_all && !config_.late_materialization_enabled()) {
            const uint32_t read_length = plan.read_length > 0
                ? plan.read_length
                : SafeInReadLength(plan.addr);
            ReadPlanEntry read;
            read.type = PendingIO::Type::VEC_ALL;
            read.addr = plan.addr;
            read.read_length = read_length;
            read.has_truth = plan.has_truth;
            read.is_true_topk = plan.is_true_topk;
            if (UseNonSafeOutCandidateBudget()) {
                AddBudgetedReadPlan(ctx, read, plan.rank_key);
            } else if (ShouldScheduleSafeInPrefetch(
                           ctx, read_length,
                           read_length > vec_bytes_ ? read_length - vec_bytes_ : 0)) {
                pending_all_plans_.push_back(read);
                ctx.stats().total_submit_window_requests++;
                RecordSafeInPrefetchDecision(ctx, plan.has_truth,
                                             plan.is_true_topk);
            } else {
                pending_vec_only_plans_.push_back(VecOnlyReadPlan{plan.addr});
                ctx.stats().total_submit_window_requests++;
            }
        } else {
            if (UseNonSafeOutCandidateBudget()) {
                ReadPlanEntry read;
                read.type = PendingIO::Type::VEC_ONLY;
                read.addr = plan.addr;
                read.read_length = vec_bytes_;
                read.has_truth = plan.has_truth;
                read.is_true_topk = plan.is_true_topk;
                AddBudgetedReadPlan(ctx, read, plan.rank_key);
            } else {
                pending_vec_only_plans_.push_back(VecOnlyReadPlan{plan.addr});
                ctx.stats().total_submit_window_requests++;
            }
        }
    }
    ctx.stats().dynamic_safein_deferred_flushes++;
    deferred_safein_plans_.clear();
}

SearchResults OverlapScheduler::Search(const float* query_vec) {
    // Reset per-query state
    vector_read_trace_request_index_ = 0;
    submitted_candidate_offsets_.Clear();
    CleanupPendingSlots();
    pending_all_plans_.clear();
    pending_vec_only_plans_.clear();
    candidate_order_by_offset_.clear();
    payload_location_cache_.clear();
    next_candidate_order_ = 0;
    pending_spec_vec_plans_.clear();
    deferred_safein_plans_.clear();
    budgeted_read_plan_heap_.clear();
    pending_vec_only_head_ = 0;
    pending_spec_vec_head_ = 0;
    speculative_vector_cache_.clear();
    speculative_prefetch_offsets_.clear();
    speculative_final_offsets_.clear();
    safeout_frontier_heap_.clear();
    safein_frontier_heap_.clear();
    dynamic_safein_probes_seen_ = 0;
    dynamic_safein_stable_count_ = 0;
    dynamic_safein_ready_ = false;
    dynamic_safein_small_pool_extra_defer_ = false;
    dynamic_safein_last_frontier_ = std::numeric_limits<float>::infinity();
    dynamic_safein_current_frontier_ = std::numeric_limits<float>::infinity();
    dynamic_safein_current_threshold_ = std::numeric_limits<float>::infinity();
    safein_prefetch_scheduled_count_ = 0;
    safein_prefetch_scheduled_bytes_ = 0;
    safein_prefetch_scheduled_extra_bytes_ = 0;
    safein_confidence_trace_query_start_ =
        config_.safein_confidence_trace != nullptr
            ? config_.safein_confidence_trace->size()
            : 0;
    bextra_probe_cluster_ema_ms_ = 0.0;
    bextra_inflight_bytes_ = 0;
    bextra_window_index_ = 0;
    bextra_before_final_drain_ = true;

    auto t_search_start = std::chrono::steady_clock::now();
    const float* effective_query_vec = query_vec;
    if (index_.logical_dim() != index_.dim()) {
        std::fill(query_wrapper_.padded_q.begin(), query_wrapper_.padded_q.end(), 0.0f);
        std::memcpy(query_wrapper_.padded_q.data(), query_vec,
                    static_cast<size_t>(index_.logical_dim()) * sizeof(float));
        effective_query_vec = query_wrapper_.padded_q.data();
    }

    if (!index_.segment().resident_preload_enabled()) {
        Status s = index_.segment().PreloadAllClusters();
        if (!s.ok()) {
            std::fprintf(stderr,
                         "FATAL: failed to preload resident clusters before search: %s\n",
                         s.ToString().c_str());
            std::abort();
        }
    }

    index_.SetUseCoarseSelectSimd(config_.enable_coarse_select_simd);
    index_.SetUseCoarseSelectPhase2(config_.enable_coarse_select_phase2);
    index_.SetTwoLevelCoarseRouting(config_.enable_two_level_coarse_routing,
                                    config_.two_level_coarse_threshold,
                                    config_.two_level_coarse_super_count,
                                    config_.two_level_coarse_super_factor,
                                    config_.two_level_coarse_budget_factor,
                                    config_.enable_two_level_coarse_exact_overlap,
                                    config_.two_level_coarse_budget_cap);
    SearchContext ctx(effective_query_vec, config_);
    // Phase 1.3: precompute P^T × query once when Hadamard rotation is used
    if (index_.used_hadamard()) {
        auto t_rot0 = std::chrono::steady_clock::now();
        index_.rotation().Apply(effective_query_vec, query_wrapper_.rotated_q.data());
        ctx.stats().probe_prepare_rotation_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_rot0).count();
    }
    RerankConsumer reranker(ctx, index_.logical_dim(),
                            &candidate_order_by_offset_);

    auto coarse_start = std::chrono::steady_clock::now();
    auto sorted_clusters = index_.FindNearestClusters(
        ctx.query_vec(), config_.nprobe);
    ctx.stats().coarse_score_ms = index_.last_coarse_score_ms();
    ctx.stats().coarse_topn_ms = index_.last_coarse_topn_ms();
    ctx.stats().coarse_routing_mode = index_.last_coarse_routing_mode();
    ctx.stats().coarse_super_count = index_.last_coarse_super_count();
    ctx.stats().coarse_super_probes = index_.last_coarse_super_probes();
    ctx.stats().coarse_child_candidates_scored =
        index_.last_coarse_child_candidates_scored();
    ctx.stats().coarse_candidate_budget = index_.last_coarse_candidate_budget();
    ctx.stats().coarse_exact_fallback = index_.last_coarse_exact_fallback();
    ctx.stats().coarse_exact_overlap = index_.last_coarse_exact_overlap();
    ctx.stats().coarse_hierarchy_build_ms =
        index_.last_coarse_hierarchy_build_ms();
	    ctx.stats().coarse_select_ms = std::chrono::duration<double, std::milli>(
	        std::chrono::steady_clock::now() - coarse_start).count();

		    ProbeResidentClusters(ctx, reranker, sorted_clusters);
		    if (config_.execution_mode == QueryExecutionMode::SerialNoOverlap) {
		        ExecuteSerialDataReads(ctx, reranker);
		    } else {
		        auto t0 = std::chrono::steady_clock::now();
		        FinalDrain(ctx, reranker);
		        ctx.stats().final_drain_ms = std::chrono::duration<double, std::milli>(
		            std::chrono::steady_clock::now() - t0).count();
		    }
	    FinalizeBudgetedPrefetchStats(ctx);
	    {
	        auto t0 = std::chrono::steady_clock::now();
	        reranker.ExecuteBuffered();
	        ctx.stats().execute_buffered_ms = std::chrono::duration<double, std::milli>(
	            std::chrono::steady_clock::now() - t0).count();
	    }

	    std::vector<CollectorEntry> results;
	    {
	        auto t0 = std::chrono::steady_clock::now();
		        results = ctx.collector().Finalize();
	        ctx.stats().collector_finalize_ms =
	            std::chrono::duration<double, std::milli>(
	                std::chrono::steady_clock::now() - t0).count();
	    }
	    MarkSafeInConfidenceTraceSelectedTopK(results);

	    {
	        auto tf0 = std::chrono::steady_clock::now();
	        if (config_.execution_mode == QueryExecutionMode::SerialNoOverlap) {
	            FetchMissingPayloadsSerial(ctx, reranker, results);
	        } else {
	            FetchMissingPayloads(ctx, reranker, results);
	        }
        ctx.stats().fetch_missing_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - tf0).count();
	        ctx.stats().remaining_payload_fetch_ms = ctx.stats().fetch_missing_ms;
	    }
	    auto ta0 = std::chrono::steady_clock::now();
	    auto sr = AssembleResults(ctx, reranker, results);
	    ctx.stats().assemble_results_ms = std::chrono::duration<double, std::milli>(
	        std::chrono::steady_clock::now() - ta0).count();
	    sr.stats() = ctx.stats();
	    const double total_ms = std::chrono::duration<double, std::milli>(
	        std::chrono::steady_clock::now() - t_search_start).count();
	    sr.stats().total_time_ms = total_ms;
	    sr.stats().search_unaccounted_ms = total_ms -
	        (sr.stats().coarse_select_ms + sr.stats().probe_time_ms +
	         sr.stats().final_drain_ms + sr.stats().execute_buffered_ms +
	         sr.stats().collector_finalize_ms + sr.stats().fetch_missing_ms +
	         sr.stats().assemble_results_ms);
	    return sr;
	}

uint32_t OverlapScheduler::PendingDataRequestCount() const {
    return static_cast<uint32_t>(
        pending_all_plans_.size() +
        (pending_spec_vec_plans_.size() - pending_spec_vec_head_) +
        (pending_vec_only_plans_.size() - pending_vec_only_head_));
}

void OverlapScheduler::ApplyBextraWindowBudget(
    SearchContext& ctx, uint32_t clusters_processed,
    uint32_t clusters_remaining) {
    if (!config_.enable_safein_bextra_probe_budget ||
        pending_all_plans_.empty()) {
        return;
    }

    const double hide_time_ms =
        bextra_probe_cluster_ema_ms_ * static_cast<double>(clusters_remaining);
    const double service_double = std::max(
        0.0, config_.safein_bextra_rho *
                 config_.safein_bextra_bytes_per_ms * hide_time_ms);
    const uint64_t predicted_service = static_cast<uint64_t>(
        std::min(service_double,
                 static_cast<double>(std::numeric_limits<uint64_t>::max())));

    const uint64_t pending_count = static_cast<uint64_t>(
        pending_all_plans_.size() +
        (pending_vec_only_plans_.size() - pending_vec_only_head_) +
        (pending_spec_vec_plans_.size() - pending_spec_vec_head_));
    const uint64_t mandatory_pending = pending_count * vec_bytes_;
    const double candidates_per_cluster = clusters_processed > 0
        ? static_cast<double>(ctx.stats().unique_fetch_candidates) /
              static_cast<double>(clusters_processed)
        : 0.0;
    const uint64_t mandatory_future = static_cast<uint64_t>(
        candidates_per_cluster * static_cast<double>(clusters_remaining) *
        static_cast<double>(vec_bytes_));
    // Future candidate density changes sharply as the SafeOut frontier
    // tightens. Reserve only work already pending or in flight; later windows
    // account for future mandatory vectors when they actually materialize.
    const uint64_t committed = bextra_inflight_bytes_ + mandatory_pending;
    uint64_t extra_budget =
        predicted_service > committed ? predicted_service - committed : 0;

    BextraWindowTraceEntry trace;
    trace.query_index = vector_read_trace_query_index_;
    trace.window_index = bextra_window_index_++;
    trace.clusters_processed = clusters_processed;
    trace.clusters_remaining = clusters_remaining;
    trace.rho = config_.safein_bextra_rho;
    trace.probe_cluster_ema_ms = bextra_probe_cluster_ema_ms_;
    trace.hide_time_ms = hide_time_ms;
    trace.predicted_service_bytes = predicted_service;
    trace.inflight_bytes = bextra_inflight_bytes_;
    trace.mandatory_pending_bytes = mandatory_pending;
    trace.mandatory_future_bytes = mandatory_future;
    trace.predicted_extra_bytes = extra_budget;

    for (const auto& plan : pending_all_plans_) {
        trace.eligible_candidates++;
        trace.eligible_extra_bytes +=
            plan.read_length > vec_bytes_ ? plan.read_length - vec_bytes_ : 0;
    }

    size_t trace_index = std::numeric_limits<size_t>::max();
    if (config_.bextra_window_trace != nullptr) {
        trace_index = config_.bextra_window_trace->size();
        config_.bextra_window_trace->push_back(trace);
    }

    std::deque<ReadPlanEntry> admitted;
    while (!pending_all_plans_.empty()) {
        ReadPlanEntry plan = pending_all_plans_.front();
        pending_all_plans_.pop_front();
        const uint64_t extra =
            plan.read_length > vec_bytes_ ? plan.read_length - vec_bytes_ : 0;
        if (extra <= extra_budget) {
            extra_budget -= extra;
            plan.bextra_extra_bytes = extra;
            plan.bextra_trace_index = trace_index;
            admitted.push_back(plan);
            trace.scheduled_candidates++;
            trace.scheduled_extra_bytes += extra;
        } else {
            pending_vec_only_plans_.push_back(VecOnlyReadPlan{plan.addr});
        }
    }
    pending_all_plans_.swap(admitted);

    if (trace_index != std::numeric_limits<size_t>::max()) {
        (*config_.bextra_window_trace)[trace_index] = trace;
    }
    auto& stats = ctx.stats();
    stats.bextra_windows++;
    stats.bextra_eligible_candidates += trace.eligible_candidates;
    stats.bextra_scheduled_candidates += trace.scheduled_candidates;
    stats.bextra_predicted_service_bytes += trace.predicted_service_bytes;
    stats.bextra_predicted_extra_bytes += trace.predicted_extra_bytes;
    stats.bextra_eligible_extra_bytes += trace.eligible_extra_bytes;
    stats.bextra_scheduled_extra_bytes += trace.scheduled_extra_bytes;
}

void OverlapScheduler::RecordBextraCompletion(const PendingIO& io,
                                               SearchContext& ctx) {
    if (!config_.enable_safein_bextra_probe_budget) return;
    bextra_inflight_bytes_ = io.read_length >= bextra_inflight_bytes_
        ? 0
        : bextra_inflight_bytes_ - io.read_length;
    if (io.bextra_extra_bytes == 0) return;

    const bool hidden = bextra_before_final_drain_;
    if (hidden) {
        ctx.stats().bextra_completed_before_final_drain_bytes +=
            io.bextra_extra_bytes;
    } else {
        ctx.stats().bextra_spilled_to_final_drain_bytes += io.bextra_extra_bytes;
    }
    if (config_.bextra_window_trace != nullptr &&
        io.bextra_trace_index < config_.bextra_window_trace->size()) {
        auto& trace = (*config_.bextra_window_trace)[io.bextra_trace_index];
        if (hidden) {
            trace.completed_before_final_drain_bytes += io.bextra_extra_bytes;
        } else {
            trace.spilled_to_final_drain_bytes += io.bextra_extra_bytes;
        }
    }
}

void OverlapScheduler::EmitPendingDataRequests(SearchContext& ctx,
                                               uint32_t max_count) {
    if (max_count == 0) return;
    uint32_t remaining = std::min(max_count, PendingDataRequestCount());
    if (remaining == 0) return;

    const int dat_fd = index_.segment().data_reader().fd();
    const bool separate_store = UseSeparateRecordStore();
    const bool vector_sidecar = UseVectorSidecarStore();
    const int vec_fd = vector_sidecar
        ? config_.separate_record_store.vector_fd
        : dat_fd;
    const int payload_fd = separate_store ? config_.separate_record_store.payload_fd : dat_fd;
    const bool detailed_timing = config_.enable_hotpath_detailed_timing;

    auto emit_all = [&](uint32_t count) {
        if (count == 0) return;
        for (uint32_t i = 0; i < count; ++i) {
            const ReadPlanEntry& plan = pending_all_plans_.front();
            if (separate_store) {
                const SeparateRecordLocation& loc =
                    LookupSeparateRecordOrAbort(ctx, plan.addr);
                const uint32_t payload_prefix_len =
                    plan.read_length > vec_bytes_
                        ? std::min<uint32_t>(loc.payload_bytes,
                                             plan.read_length - vec_bytes_)
                        : 0;
                uint8_t* vec_buf = nullptr;
                uint16_t fixed_idx = 0;
                const bool use_fixed_buffer =
                    TryAcquireFixedVecBuffer(&vec_buf, &fixed_idx);
                if (use_fixed_buffer) {
                    ctx.stats().fixed_vec_buffer_hits++;
                } else {
                    ctx.stats().fixed_vec_buffer_misses++;
                    vec_buf = AcquireVecOnlyBuffer();
                }
                uint32_t vec_slot_id = AllocateVectorOnlyPendingSlot(
                    plan.addr, vec_buf,
                    use_fixed_buffer ? PendingBufferCleanup::FixedVec
                                     : PendingBufferCleanup::VecPool,
                    fixed_idx);
                const uint64_t vec_off = SeparateVectorOffset(loc);
                RecordVectorReadTrace(plan.addr, true);
                if (use_fixed_buffer && fixed_buffer_reader_ != nullptr) {
                    CheckPrepRead(fixed_buffer_reader_->PrepReadRegisteredBufferTagged(
                                      vec_fd, vec_buf, fixed_idx, vec_bytes_,
                                      vec_off, vec_slot_id),
                                  "scheduler NoCombine VEC_ALL vector");
                } else {
                    CheckPrepRead(data_reader_.PrepReadTagged(
                                      vec_fd, vec_buf, vec_bytes_, vec_off,
                                      vec_slot_id),
                                  "scheduler NoCombine VEC_ALL vector");
                }
                ctx.stats().vec_only_read_requests++;
                ctx.stats().vec_only_read_bytes += vec_bytes_;

                if (payload_prefix_len > 0) {
                    uint8_t* payload_buf = buffer_pool_.Acquire(payload_prefix_len);
                    PendingIO pio;
                    pio.type = PendingIO::Type::PAYLOAD_PREFIX;
                    pio.addr = plan.addr;
                    pio.read_offset = loc.payload_offset;
                    pio.read_length = payload_prefix_len;
                    pio.payload_total_length = loc.payload_bytes;
                    pio.payload_prefix_length = payload_prefix_len;
                    uint32_t payload_slot_id = AllocatePendingSlot(
                        pio, payload_buf, PendingBufferCleanup::Pool);
                    CheckPrepRead(data_reader_.PrepReadTagged(
                                      payload_fd, payload_buf, payload_prefix_len,
                                      loc.payload_offset, payload_slot_id),
                                  "scheduler NoCombine SafeIn payload prefix");
                    ctx.stats().payload_read_requests++;
                    ctx.stats().payload_read_bytes += payload_prefix_len;
                }
                const uint32_t scheduled_bytes = vec_bytes_ + payload_prefix_len;
                if (payload_prefix_len >= loc.payload_bytes) {
                    ctx.stats().safein_full_read_requests++;
                    ctx.stats().safein_full_read_bytes += scheduled_bytes;
                } else {
                    ctx.stats().safein_prefix_read_requests++;
                    ctx.stats().safein_prefix_read_bytes += scheduled_bytes;
                }
                pending_all_plans_.pop_front();
                continue;
            }

            uint8_t* buf = buffer_pool_.Acquire(plan.read_length);
            PendingIO pio;
            pio.type = PendingIO::Type::VEC_ALL;
            pio.addr = plan.addr;
            pio.read_offset = plan.addr.offset;
            pio.read_length = plan.read_length;
            pio.bextra_extra_bytes = plan.bextra_extra_bytes;
            pio.bextra_trace_index = plan.bextra_trace_index;
            RecordVectorReadTrace(plan.addr, true);
            uint32_t slot_id = 0;
            if (detailed_timing) {
                auto alloc_start = std::chrono::steady_clock::now();
                slot_id = AllocatePendingSlot(
                    pio, buf, PendingBufferCleanup::Pool);
                ctx.stats().probe_submit_pending_slot_alloc_ms +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - alloc_start).count();
                auto prep_start = std::chrono::steady_clock::now();
                CheckPrepRead(data_reader_.PrepReadTagged(
                                  dat_fd, buf, plan.read_length, plan.addr.offset,
                                  slot_id),
                              "scheduler VEC_ALL");
                const double prep_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - prep_start).count();
                ctx.stats().uring_prep_ms += prep_ms;
                ctx.stats().probe_submit_prep_read_ms += prep_ms;
            } else {
                slot_id = AllocatePendingSlot(
                    pio, buf, PendingBufferCleanup::Pool);
                CheckPrepRead(data_reader_.PrepReadTagged(
                                  dat_fd, buf, plan.read_length, plan.addr.offset,
                                  slot_id),
                              "scheduler VEC_ALL");
            }
            ctx.stats().all_read_requests++;
            ctx.stats().all_read_bytes += plan.read_length;
            if (config_.enable_safein_bextra_probe_budget) {
                bextra_inflight_bytes_ += plan.read_length;
            }
            if (plan.read_length >= plan.addr.size) {
                ctx.stats().safein_full_read_requests++;
                ctx.stats().safein_full_read_bytes += plan.read_length;
            } else {
                ctx.stats().safein_prefix_read_requests++;
                ctx.stats().safein_prefix_read_bytes += plan.read_length;
            }
            pending_all_plans_.pop_front();
        }
    };

    auto emit_vec_plans = [&](std::vector<VecOnlyReadPlan>& plans,
                              size_t& head,
                              uint32_t count,
                              PendingIO::Type io_type,
                              const char* context,
                              bool speculative) {
        if (count == 0) return;
        auto emit_start = std::chrono::steady_clock::now();
        auto emit_single = [&](const VecOnlyReadPlan& plan) {
            uint64_t read_offset = plan.addr.offset;
            if (vector_sidecar) {
                const SeparateRecordLocation& loc =
                    LookupSeparateRecordOrAbort(ctx, plan.addr);
                read_offset = SeparateVectorOffset(loc);
            }
            RecordVectorReadTrace(plan.addr, false);
            uint8_t* buf = nullptr;
            uint16_t fixed_idx = 0;
            const bool use_fixed_buffer =
                TryAcquireFixedVecBuffer(&buf, &fixed_idx);
            if (use_fixed_buffer) {
                ctx.stats().fixed_vec_buffer_hits++;
            } else {
                ctx.stats().fixed_vec_buffer_misses++;
            }
            if (!use_fixed_buffer) {
                buf = AcquireVecOnlyBuffer();
            }
            uint32_t slot_id = 0;
            if (detailed_timing) {
                auto alloc_start = std::chrono::steady_clock::now();
                slot_id = AllocateVectorOnlyPendingSlot(
                    plan.addr, buf,
                    use_fixed_buffer ? PendingBufferCleanup::FixedVec
                                     : PendingBufferCleanup::VecPool,
                    fixed_idx, io_type);
                ctx.stats().probe_submit_pending_slot_alloc_ms +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - alloc_start).count();
                auto prep_start = std::chrono::steady_clock::now();
                if (use_fixed_buffer && fixed_buffer_reader_ != nullptr &&
                    data_fd_registered_index_ >= 0) {
                    CheckPrepRead(fixed_buffer_reader_->PrepReadRegisteredBufferFixedFileTagged(
                                      data_fd_registered_index_, buf, fixed_idx,
                                      vec_bytes_, read_offset, slot_id),
                                  context);
                } else if (use_fixed_buffer && fixed_buffer_reader_ != nullptr) {
                    CheckPrepRead(fixed_buffer_reader_->PrepReadRegisteredBufferTagged(
                                      vec_fd, buf, fixed_idx, vec_bytes_,
                                      read_offset, slot_id),
                                  context);
                } else {
                    CheckPrepRead(data_reader_.PrepReadTagged(
                                      vec_fd, buf, vec_bytes_, read_offset,
                                      slot_id),
                                  context);
                }
                const double prep_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - prep_start).count();
                ctx.stats().uring_prep_ms += prep_ms;
                ctx.stats().probe_submit_prep_read_ms += prep_ms;
            } else {
                slot_id = AllocateVectorOnlyPendingSlot(
                    plan.addr, buf,
                    use_fixed_buffer ? PendingBufferCleanup::FixedVec
                                     : PendingBufferCleanup::VecPool,
                    fixed_idx, io_type);
                if (use_fixed_buffer && fixed_buffer_reader_ != nullptr &&
                    data_fd_registered_index_ >= 0) {
                    CheckPrepRead(fixed_buffer_reader_->PrepReadRegisteredBufferFixedFileTagged(
                                      data_fd_registered_index_, buf, fixed_idx,
                                      vec_bytes_, read_offset, slot_id),
                                  context);
                } else if (use_fixed_buffer && fixed_buffer_reader_ != nullptr) {
                    CheckPrepRead(fixed_buffer_reader_->PrepReadRegisteredBufferTagged(
                                      vec_fd, buf, fixed_idx, vec_bytes_,
                                      read_offset, slot_id),
                                  context);
                } else {
                    CheckPrepRead(data_reader_.PrepReadTagged(
                                      vec_fd, buf, vec_bytes_, read_offset,
                                      slot_id),
                                  context);
                }
            }
            ctx.stats().vec_only_read_requests++;
            ctx.stats().vec_only_read_bytes += vec_bytes_;
            if (config_.enable_safein_bextra_probe_budget) {
                bextra_inflight_bytes_ += vec_bytes_;
            }
            if (speculative) {
                ctx.stats().budgeted_prefetch_read_bytes += vec_bytes_;
            }
        };

        auto emit_span = [&](const std::vector<VecOnlyReadPlan>& group) {
            if (group.size() < 2) {
                emit_single(group.front());
                return;
            }
            const uint64_t read_offset = group.front().addr.offset;
            const uint64_t read_end = group.back().addr.offset + vec_bytes_;
            const uint64_t span64 = read_end - read_offset;
            if (span64 > UINT32_MAX) {
                for (const auto& plan : group) emit_single(plan);
                return;
            }
            const uint32_t span_bytes = static_cast<uint32_t>(span64);
            uint8_t* buf = buffer_pool_.Acquire(span_bytes);
            PendingIO pio;
            pio.type = PendingIO::Type::VEC_SPAN;
            pio.addr = group.front().addr;
            pio.read_offset = read_offset;
            pio.read_length = span_bytes;
            pio.span_members.reserve(group.size());
            for (const auto& plan : group) {
                RecordVectorReadTrace(plan.addr, false);
                pio.span_members.push_back(VecSpanMember{
                    plan.addr,
                    static_cast<uint32_t>(plan.addr.offset - read_offset)});
            }
            const uint32_t slot_id = AllocatePendingSlot(
                std::move(pio), buf, PendingBufferCleanup::Pool);
            CheckPrepRead(data_reader_.PrepReadTagged(
                              dat_fd, buf, span_bytes, read_offset, slot_id),
                          "scheduler VEC_SPAN");
            ctx.stats().vec_only_read_requests++;
            ctx.stats().vec_only_read_bytes += span_bytes;
            ctx.stats().vec_span_read_requests++;
            ctx.stats().vec_span_candidates +=
                static_cast<uint32_t>(group.size());
            ctx.stats().vec_span_read_bytes += span_bytes;
        };

        const bool can_coalesce =
            io_type == PendingIO::Type::VEC_ONLY && !speculative &&
            !vector_sidecar && config_.enable_vec_span_coalescing &&
            config_.vec_span_tile_bytes >= vec_bytes_ &&
            config_.vec_span_max_byte_amplification >= 1.0f;
        if (!can_coalesce) {
            for (uint32_t i = 0; i < count; ++i) {
                emit_single(plans[head++]);
            }
        } else {
            std::vector<VecOnlyReadPlan> selected(
                plans.begin() + static_cast<std::ptrdiff_t>(head),
                plans.begin() + static_cast<std::ptrdiff_t>(head + count));
            head += count;
            std::sort(selected.begin(), selected.end(),
                      [](const VecOnlyReadPlan& lhs,
                         const VecOnlyReadPlan& rhs) {
                          return lhs.addr.offset < rhs.addr.offset;
                      });

            std::vector<VecOnlyReadPlan> group;
            group.reserve(selected.size());
            auto flush_group = [&]() {
                if (group.empty()) return;
                emit_span(group);
                group.clear();
            };
            uint64_t group_tile = std::numeric_limits<uint64_t>::max();
            for (const auto& plan : selected) {
                const uint64_t tile =
                    plan.addr.offset / config_.vec_span_tile_bytes;
                const uint64_t vector_last = plan.addr.offset + vec_bytes_ - 1;
                const bool crosses_tile =
                    vector_last / config_.vec_span_tile_bytes != tile;
                if (crosses_tile) {
                    flush_group();
                    emit_single(plan);
                    group_tile = std::numeric_limits<uint64_t>::max();
                    continue;
                }
                if (!group.empty() && tile != group_tile) {
                    flush_group();
                }
                if (group.empty()) group_tile = tile;

                const uint64_t proposed_span =
                    plan.addr.offset + vec_bytes_ -
                    (group.empty() ? plan.addr.offset
                                   : group.front().addr.offset);
                const double allowed =
                    static_cast<double>(group.size() + 1) * vec_bytes_ *
                    static_cast<double>(
                        config_.vec_span_max_byte_amplification);
                if (!group.empty() &&
                    static_cast<double>(proposed_span) > allowed) {
                    flush_group();
                    group_tile = tile;
                }
                group.push_back(plan);
            }
            flush_group();
        }
        ctx.stats().probe_submit_vec_only_emit_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - emit_start).count();
    };

    const uint32_t pending_vec_count = static_cast<uint32_t>(
        (pending_spec_vec_plans_.size() - pending_spec_vec_head_) +
        (pending_vec_only_plans_.size() - pending_vec_only_head_));
    uint32_t all_take_limit =
        static_cast<uint32_t>(pending_all_plans_.size());
    if (config_.safein_prefetch_emit_quantum > 0 && pending_vec_count > 0) {
        all_take_limit =
            std::min(all_take_limit, config_.safein_prefetch_emit_quantum);
    }
    const uint32_t all_take = std::min<uint32_t>(remaining, all_take_limit);
    auto emit_start = std::chrono::steady_clock::now();
    emit_all(all_take);
    remaining -= all_take;
    const uint32_t spec_take = std::min<uint32_t>(
        remaining,
        static_cast<uint32_t>(pending_spec_vec_plans_.size() -
                              pending_spec_vec_head_));
    emit_vec_plans(pending_spec_vec_plans_, pending_spec_vec_head_,
                   spec_take, PendingIO::Type::SPEC_VEC_ONLY,
                   "scheduler SPEC_VEC_ONLY", /*speculative=*/true);
    remaining -= spec_take;
    const uint32_t vec_take = std::min<uint32_t>(
        remaining,
        static_cast<uint32_t>(pending_vec_only_plans_.size() -
                              pending_vec_only_head_));
    emit_vec_plans(pending_vec_only_plans_, pending_vec_only_head_,
                   vec_take, PendingIO::Type::VEC_ONLY,
                   "scheduler VEC_ONLY", /*speculative=*/false);
    remaining -= vec_take;
    const uint32_t tail_all_take = std::min<uint32_t>(
        remaining, static_cast<uint32_t>(pending_all_plans_.size()));
    emit_all(tail_all_take);
    const double emit_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - emit_start).count();
    ctx.stats().probe_submit_emit_ms += emit_ms;
    ctx.stats().probe_submit_ms += emit_ms;
    ctx.stats().probe_time_ms = ctx.stats().probe_prepare_ms +
        ctx.stats().probe_stage1_ms + ctx.stats().probe_stage2_ms +
        ctx.stats().probe_submit_ms;
}

void OverlapScheduler::ProbeResidentClusters(
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
            DispatchCompletion(comps[j].user_data, comps[j].result,
                               ctx, reranker);
        }
    };
    auto drain_reader_until_idle = [&](AsyncReader& reader) {
        while (reader.InFlight() > 0) {
            drain_reader(reader, /*wait_for_one=*/true);
        }
        drain_reader(reader, /*wait_for_one=*/false);
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
		        const double cluster_probe_ms =
		            std::chrono::duration<double, std::milli>(
		                std::chrono::steady_clock::now() - tp0).count();
		        if (bextra_probe_cluster_ema_ms_ == 0.0) {
		            bextra_probe_cluster_ema_ms_ = cluster_probe_ms;
		        } else {
		            const double alpha = std::clamp<double>(
		                config_.safein_bextra_ema_alpha, 0.0, 1.0);
		            bextra_probe_cluster_ema_ms_ =
		                alpha * cluster_probe_ms +
		                (1.0 - alpha) * bextra_probe_cluster_ema_ms_;
		        }
		        ++clusters_since_submit;

	        if (config_.execution_mode == QueryExecutionMode::SerialNoOverlap) {
	            continue;
	        }

        if (config_.serial_data_drains) {
            if (PendingDataRequestCount() > 0) {
                AsyncReader& data_submission_reader =
                    isolated_submission_mode_ ? data_reader_ : cluster_reader_;
                EmitPendingDataRequests(ctx, PendingDataRequestCount());
                ctx.stats().total_submit_window_flushes++;
                ctx.stats().total_submit_window_tail_flushes++;
                record_submit(data_submission_reader);
                drain_reader_until_idle(data_submission_reader);
                clusters_since_submit = 0;
            }
            continue;
        }

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
                ApplyBextraWindowBudget(
                    ctx, static_cast<uint32_t>(i + 1),
                    static_cast<uint32_t>(sorted_clusters.size() - i - 1));
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
                ApplyBextraWindowBudget(
                    ctx, static_cast<uint32_t>(i + 1),
                    static_cast<uint32_t>(sorted_clusters.size() - i - 1));
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
    }

	    FlushDeferredSafeInPlans(ctx, SafeInThresholdForProbe(), /*force=*/true);
	    MaterializeBudgetedReadPlans(ctx, reranker);

	    if (config_.execution_mode == QueryExecutionMode::SerialNoOverlap) {
	        return;
	    }

    if (isolated_submission_mode_) {
        if (PendingDataRequestCount() > 0) {
            ApplyBextraWindowBudget(
                ctx, static_cast<uint32_t>(sorted_clusters.size()), 0);
            EmitPendingDataRequests(ctx, PendingDataRequestCount());
            ctx.stats().total_submit_window_flushes++;
            ctx.stats().total_submit_window_tail_flushes++;
            record_submit(data_reader_);
        }
    } else if (PendingDataRequestCount() > 0) {
        ApplyBextraWindowBudget(
            ctx, static_cast<uint32_t>(sorted_clusters.size()), 0);
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

uint32_t OverlapScheduler::AllocateVectorOnlyPendingSlot(
    AddressEntry addr, uint8_t* buffer, PendingBufferCleanup cleanup,
    uint16_t fixed_buffer_index, PendingIO::Type type) {
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
    slot.io.type = type;
    slot.io.addr = addr;
    slot.io.read_offset = addr.offset;
    slot.io.read_length = vec_bytes_;
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

uint8_t* OverlapScheduler::AcquireVecOnlyBuffer() {
    if (!free_vec_only_buffers_.empty()) {
        uint8_t* buf = free_vec_only_buffers_.back();
        free_vec_only_buffers_.pop_back();
        return buf;
    }
    uint8_t* raw = static_cast<uint8_t*>(
        std::aligned_alloc(4096, aligned_vec_bytes_));
    if (!raw) {
        std::fprintf(stderr,
                     "FATAL: aligned_alloc failed for vec-only buffer (%u bytes)\n",
                     aligned_vec_bytes_);
        std::abort();
    }
    vec_only_owned_buffers_.push_back(raw);
    return raw;
}

void OverlapScheduler::ReleaseVecOnlyBuffer(uint8_t* buf) {
    if (buf == nullptr) return;
    free_vec_only_buffers_.push_back(buf);
}

void OverlapScheduler::ReleaseVectorOnlyPendingSlot(uint32_t slot_id) {
    if (slot_id >= pending_slots_.size()) return;
    PendingSlot& slot = pending_slots_[slot_id];
    if (!slot.in_use) return;

    if (slot.buffer != nullptr) {
        switch (slot.cleanup) {
            case PendingBufferCleanup::FixedVec:
                ReleaseFixedVecBuffer(slot.fixed_buffer_index);
                break;
            case PendingBufferCleanup::VecPool:
                ReleaseVecOnlyBuffer(slot.buffer);
                break;
            case PendingBufferCleanup::Pool:
                buffer_pool_.Release(slot.buffer);
                break;
            case PendingBufferCleanup::Free:
                std::free(slot.buffer);
                break;
            case PendingBufferCleanup::None:
                break;
        }
    }

    slot.in_use = false;
    slot.buffer = nullptr;
    slot.fixed_buffer_index = 0;
    slot.cleanup = PendingBufferCleanup::None;
    slot.io = PendingIO{};
    free_pending_slots_.push_back(slot_id);
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
        case PendingBufferCleanup::VecPool:
            ReleaseVecOnlyBuffer(slot.buffer);
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

    const uint32_t fixed_buffer_count =
        config_.fixed_vec_buffer_count > 0
            ? config_.fixed_vec_buffer_count
            : config_.io_queue_depth;
    if (fixed_buffer_count == 0) {
        return;
    }

    fixed_vec_buffers_.reserve(fixed_buffer_count);
    fixed_vec_buffer_capacities_.reserve(fixed_buffer_count);
    free_fixed_vec_buffers_.reserve(fixed_buffer_count);
    for (uint32_t i = 0; i < fixed_buffer_count; ++i) {
        uint8_t* buf = static_cast<uint8_t*>(
            std::aligned_alloc(4096, aligned_vec_bytes_));
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
        fixed_vec_buffer_capacities_.push_back(aligned_vec_bytes_);
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
    data_fd_registered_index_ = UseVectorSidecarStore()
        ? -1
        : uring_reader->RegisteredFileIndex(index_.segment().data_reader().fd());
}

const ParsedCluster* OverlapScheduler::GetResidentParsedCluster(
    uint32_t cluster_id) const {
    return index_.segment().GetResidentParsedCluster(cluster_id);
}

// ============================================================================
// DispatchCompletion
// ============================================================================

void OverlapScheduler::DispatchCompletion(
    uint64_t slot_token, int32_t result, SearchContext& ctx,
    RerankConsumer& reranker) {
    PendingSlot* slot = GetPendingSlot(slot_token);
    if (slot == nullptr) return;

    const uint32_t slot_id = static_cast<uint32_t>(slot_token);
    uint8_t* buf = slot->buffer;
    PendingIO io = slot->io;
    bool release_slot = true;
    if (result < 0 || static_cast<uint32_t>(result) != io.read_length) {
        CleanupPendingSlot(*slot);
        ReleasePendingSlot(slot_id);
        std::fprintf(stderr,
                     "FATAL: short or failed async read type=%u offset=%llu "
                     "expected=%u result=%d\n",
                     static_cast<unsigned>(io.type),
                     static_cast<unsigned long long>(io.read_offset),
                     io.read_length, result);
        std::abort();
    }
    RecordBextraCompletion(io, ctx);

    switch (io.type) {
        case PendingIO::Type::VEC_ONLY: {
            reranker.ConsumeVec(buf, io.addr);
            ReleaseVectorOnlyPendingSlot(slot_id);
            release_slot = false;
            break;
        }
        case PendingIO::Type::SPEC_VEC_ONLY: {
            if (speculative_final_offsets_.erase(io.addr.offset) > 0) {
                reranker.ConsumeVec(buf, io.addr);
                ctx.stats().budgeted_prefetch_completed++;
                ctx.stats().budgeted_prefetch_used++;
            } else {
                CacheSpeculativeVector(ctx, io.addr, buf);
            }
            ReleaseVectorOnlyPendingSlot(slot_id);
            release_slot = false;
            break;
        }
        case PendingIO::Type::VEC_SPAN: {
            for (const auto& member : io.span_members) {
                if (member.buffer_offset + vec_bytes_ > io.read_length) {
                    std::fprintf(stderr,
                                 "FATAL: invalid VEC_SPAN member offset=%u "
                                 "vec_bytes=%u read_length=%u\n",
                                 member.buffer_offset, vec_bytes_,
                                 io.read_length);
                    std::abort();
                }
                reranker.ConsumeVec(buf + member.buffer_offset, member.addr);
            }
            CleanupPendingSlot(*slot);
            break;
        }
        case PendingIO::Type::VEC_ALL: {
            CacheInlinePayloadLocationFromRecord(
                ctx, io.addr, buf, io.read_length);
            reranker.ConsumeAll(buf, io.addr, io.read_length);
            CleanupPendingSlot(*slot);
            break;
        }
        case PendingIO::Type::PAYLOAD_PREFIX:
            reranker.ConsumePayloadPrefix(buf, io.addr,
                                          io.payload_prefix_length,
                                          &buffer_pool_);
            slot->buffer = nullptr;
            slot->cleanup = PendingBufferCleanup::None;
            break;
        case PendingIO::Type::PAYLOAD:
            reranker.ConsumePayload(buf, io.addr, io.payload_total_length,
                                    &buffer_pool_);
            slot->buffer = nullptr;
            slot->cleanup = PendingBufferCleanup::None;
            // RerankConsumer returns this buffer to buffer_pool_ after parse.
            break;
        case PendingIO::Type::PAYLOAD_DESCRIPTOR: {
            const auto desc = storage::DecodeHotPayloadDescriptor(buf);
            payload_location_cache_[io.addr.offset] =
                PayloadLocationFromDescriptorOrAbort(ctx, io.addr, desc);
            CleanupPendingSlot(*slot);
            break;
        }
    }
    if (release_slot) {
        ReleasePendingSlot(slot_id);
    }
}

// ============================================================================
// AsyncIOSink — submits io_uring reads and buffers candidate estimates for
// dynamic SafeOut frontier updates.
// ============================================================================

class OverlapScheduler::AsyncIOSink : public index::ProbeResultSink {
 public:
    AsyncIOSink(OverlapScheduler& sched, SearchContext& ctx, int dat_fd,
                uint32_t cluster_id, float safein_prefetch_threshold)
        : sched_(sched), ctx_(ctx), dat_fd_(dat_fd),
          cluster_id_(cluster_id),
          safein_prefetch_threshold_(safein_prefetch_threshold),
          skip_dedup_(sched.index_.assignment_mode() == index::AssignmentMode::Single),
          scratch_(sched.submit_scratch_) {
        candidate_estimates_.reserve(256);
    }

    void OnCandidates(const index::CandidateBatch& batch) override {
        if (batch.count == 0) return;

        ctx_.stats().total_candidate_batches++;
        scratch_.unique_count = 0;
        scratch_.safein_all_count = 0;
        scratch_.vec_only_count = 0;

        ScanAndPartitionBatch(batch);
        if (sched_.config_.safein_prefetch_global_window > 0 &&
            sched_.config_.safein_prefetch_order !=
                SafeInPrefetchOrder::Arrival) {
            BuildGlobalRankedPlans(batch);
        } else if (sched_.ShouldDeferSafeInPlans()) {
            BuildDeferredPlans(batch);
        } else {
            BuildReadPlans(batch);
        }

        if (!candidate_estimates_.empty()) {
            estimates_pending_ = true;
        }
    }

    void FinalizeCluster() {
        if (estimates_pending_) {
            if (sched_.use_estimate_frontier_) {
                auto t_frontier_merge = std::chrono::steady_clock::now();
                ctx_.stats().total_safeout_frontier_estimates_merged +=
                    static_cast<uint32_t>(candidate_estimates_.size());
                for (const auto& estimate : candidate_estimates_) {
                    if (sched_.AddSafeOutFrontierEstimate(estimate)) {
                        ctx_.stats().total_safeout_frontier_updates++;
                    }
                    sched_.AddSafeInFrontierEstimate(estimate.lower_bound);
                }
                ctx_.stats().safeout_frontier_merge_ms +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t_frontier_merge).count();
            }
            estimates_pending_ = false;
            candidate_estimates_.clear();
        }
    }

    double dedup_and_slot_ms_ = 0;
    double prepare_vec_only_ms_ = 0;
    double prepare_all_ms_ = 0;
    double submit_cpu_ms() const {
        return dedup_and_slot_ms_;
    }

 private:
    bool CandidateTruth(uint32_t local_idx, bool* is_true_topk) const {
        if (is_true_topk != nullptr) {
            *is_true_topk = false;
        }
        const auto* members = sched_.config_.false_stats_cluster_members;
        const auto* truth = sched_.config_.false_stats_true_topk_rows;
        if (members == nullptr || truth == nullptr ||
            cluster_id_ >= members->size()) {
            return false;
        }
        const auto& cluster_members = (*members)[cluster_id_];
        if (local_idx >= cluster_members.size()) {
            return false;
        }
        if (is_true_topk != nullptr) {
            *is_true_topk = truth->count(cluster_members[local_idx]) != 0;
        }
        return true;
    }

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
            sched_.candidate_order_by_offset_.emplace(
                addr.offset, sched_.next_candidate_order_++);
            if (!sched_.UseNonSafeOutCandidateBudget()) {
                ctx_.stats().unique_fetch_candidates++;
            }

            if (sched_.use_estimate_frontier_) {
                auto t_estimate_buffer = std::chrono::steady_clock::now();
                candidate_estimates_.push_back({batch.est_dist[i], batch.est_error[i],
                                                batch.estimate_lower_bound[i]});
                const double buffer_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_estimate_buffer).count();
                ctx_.stats().total_safeout_frontier_estimates_buffered++;
                ctx_.stats().safeout_frontier_buffer_ms += buffer_ms;
            }

            if (batch.cls[i] == index::CandidateClass::SafeIn &&
                sched_.config_.safein_confidence_trace != nullptr &&
                std::isfinite(safein_prefetch_threshold_)) {
                bool is_true_topk = false;
                const bool has_truth =
                    CandidateTruth(batch.global_idx[i], &is_true_topk);
                SafeInConfidenceTraceEntry entry;
                entry.query_index = sched_.vector_read_trace_query_index_;
                entry.probe_index = sched_.dynamic_safein_probes_seen_;
                entry.cluster_id = cluster_id_;
                entry.cluster_local_index = batch.global_idx[i];
                entry.candidate_offset = addr.offset;
                entry.record_bytes = addr.size;
                entry.classification_stage = batch.classification_stage[i];
                entry.frontier_ready = sched_.dynamic_safein_ready_;
                entry.has_gt_label = has_truth;
                entry.gt_topk = is_true_topk;
                entry.est_dist = batch.est_dist[i];
                entry.est_error = batch.est_error[i];
                entry.safein_upper_bound = batch.safein_upper_bound[i];
                entry.safein_margin = std::max(
                    0.0f, entry.safein_upper_bound - entry.est_dist);
                entry.safein_threshold = safein_prefetch_threshold_;
                entry.safein_frontier = sched_.dynamic_safein_current_frontier_;
                entry.raw_slack =
                    entry.safein_threshold - entry.safein_upper_bound;
                entry.normalized_slack_error = entry.raw_slack /
                    std::max(std::abs(entry.est_error), 1e-12f);
                entry.normalized_slack_safein_margin = entry.raw_slack /
                    std::max(entry.safein_margin, 1e-12f);
                sched_.config_.safein_confidence_trace->push_back(entry);
            }

            const bool use_vec_all =
                batch.cls[i] == index::CandidateClass::SafeIn &&
                !sched_.config_.late_materialization_enabled();
            if (use_vec_all) {
                scratch_.safein_all_indices[scratch_.safein_all_count++] = idx;
            } else {
                scratch_.vec_only_indices[scratch_.vec_only_count++] = idx;
            }
        }
        dedup_and_slot_ms_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    }

    void BuildDeferredPlans(const index::CandidateBatch& batch) {
        for (uint32_t pos = 0; pos < scratch_.unique_count; ++pos) {
            const uint32_t batch_idx = scratch_.unique_indices[pos];
            bool is_true_topk = false;
            const bool has_truth =
                CandidateTruth(batch.global_idx[batch_idx], &is_true_topk);
            DeferredSafeInPlan plan;
            plan.addr = batch.decoded_addr[batch_idx];
            plan.rank_key = batch.est_dist[batch_idx];
            plan.safein_upper_bound = batch.safein_upper_bound[batch_idx];
            plan.confidence =
                (safein_prefetch_threshold_ - plan.safein_upper_bound) /
                std::max(std::abs(batch.est_error[batch_idx]), 1e-12f);
            plan.read_length = sched_.SafeInReadLength(plan.addr);
            plan.extra_bytes = plan.read_length > sched_.vec_bytes_
                ? plan.read_length - sched_.vec_bytes_
                : 0;
            plan.has_truth = has_truth;
            plan.is_true_topk = is_true_topk;
            sched_.deferred_safein_plans_.push_back(plan);
            ctx_.stats().dynamic_safein_deferred_candidates++;
        }
    }

    void BuildVecOnlyPlans(const index::CandidateBatch& batch) {
        auto vec_start = std::chrono::steady_clock::now();
        for (uint32_t pos = 0; pos < scratch_.vec_only_count; ++pos) {
            const uint32_t unique_idx = scratch_.vec_only_indices[pos];
            const uint32_t batch_idx = scratch_.unique_indices[unique_idx];
            const AddressEntry& addr = batch.decoded_addr[batch_idx];
            if (sched_.UseNonSafeOutCandidateBudget()) {
                ReadPlanEntry plan;
                plan.type = PendingIO::Type::VEC_ONLY;
                plan.addr = addr;
                plan.read_length = sched_.vec_bytes_;
                bool is_true_topk = false;
                plan.has_truth =
                    CandidateTruth(batch.global_idx[batch_idx], &is_true_topk);
                plan.is_true_topk = is_true_topk;
                sched_.AddBudgetedReadPlan(ctx_, plan, batch.est_dist[batch_idx]);
            } else {
                sched_.pending_vec_only_plans_.push_back(VecOnlyReadPlan{addr});
                ctx_.stats().total_submit_window_requests++;
            }
        }
        prepare_vec_only_ms_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - vec_start).count();
    }

    void BuildGlobalRankedPlans(const index::CandidateBatch& batch) {
        for (uint32_t pos = 0; pos < scratch_.safein_all_count; ++pos) {
            const uint32_t unique_idx = scratch_.safein_all_indices[pos];
            const uint32_t batch_idx = scratch_.unique_indices[unique_idx];
            bool is_true_topk = false;
            const bool has_truth =
                CandidateTruth(batch.global_idx[batch_idx], &is_true_topk);
            DeferredSafeInPlan plan;
            plan.addr = batch.decoded_addr[batch_idx];
            plan.rank_key = batch.est_dist[batch_idx];
            plan.safein_upper_bound = batch.safein_upper_bound[batch_idx];
            plan.confidence =
                (safein_prefetch_threshold_ - plan.safein_upper_bound) /
                std::max(std::abs(batch.est_error[batch_idx]), 1e-12f);
            plan.read_length = sched_.SafeInReadLength(plan.addr);
            plan.extra_bytes = plan.read_length > sched_.vec_bytes_
                ? plan.read_length - sched_.vec_bytes_
                : 0;
            plan.has_truth = has_truth;
            plan.is_true_topk = is_true_topk;
            sched_.deferred_safein_plans_.push_back(plan);
            ctx_.stats().dynamic_safein_deferred_candidates++;
            if (sched_.deferred_safein_plans_.size() >=
                sched_.config_.safein_prefetch_global_window) {
                sched_.FlushDeferredSafeInPlans(
                    ctx_, safein_prefetch_threshold_, /*force=*/true);
            }
        }
        BuildVecOnlyPlans(batch);
    }

    void BuildReadPlans(const index::CandidateBatch& batch) {
        auto all_start = std::chrono::steady_clock::now();
        std::vector<uint32_t> safein_order(scratch_.safein_all_count);
        for (uint32_t pos = 0; pos < scratch_.safein_all_count; ++pos) {
            safein_order[pos] = pos;
        }
        const auto order = sched_.config_.safein_prefetch_order;
        if (order != SafeInPrefetchOrder::Arrival) {
            const uint32_t rank_batch = std::max(
                1u, sched_.config_.safein_prefetch_rank_batch_size);
            for (uint32_t begin = 0; begin < safein_order.size();
                 begin += rank_batch) {
                const uint32_t end = std::min<uint32_t>(
                    static_cast<uint32_t>(safein_order.size()),
                    begin + rank_batch);
                std::stable_sort(
                    safein_order.begin() + begin, safein_order.begin() + end,
                    [&](uint32_t lhs_pos, uint32_t rhs_pos) {
                        const uint32_t lhs_unique =
                            scratch_.safein_all_indices[lhs_pos];
                        const uint32_t rhs_unique =
                            scratch_.safein_all_indices[rhs_pos];
                        const uint32_t lhs = scratch_.unique_indices[lhs_unique];
                        const uint32_t rhs = scratch_.unique_indices[rhs_unique];
                        auto score = [&](uint32_t i) {
                            const float confidence =
                                (safein_prefetch_threshold_ -
                                 batch.safein_upper_bound[i]) /
                                std::max(std::abs(batch.est_error[i]), 1e-12f);
                            if (order == SafeInPrefetchOrder::ConfidencePerByte) {
                                const uint32_t read_length =
                                    sched_.SafeInReadLength(batch.decoded_addr[i]);
                                const uint32_t extra = read_length > sched_.vec_bytes_
                                    ? read_length - sched_.vec_bytes_
                                    : 0;
                                return confidence /
                                    static_cast<float>(std::max(1u, extra));
                            }
                            return confidence;
                        };
                        return score(lhs) > score(rhs);
                    });
            }
        }
        for (uint32_t ordered_pos : safein_order) {
            const uint32_t pos = ordered_pos;
            const uint32_t unique_idx = scratch_.safein_all_indices[pos];
            const uint32_t batch_idx = scratch_.unique_indices[unique_idx];
            const AddressEntry& addr = batch.decoded_addr[batch_idx];
            bool is_true_topk = false;
            const bool has_truth =
                CandidateTruth(batch.global_idx[batch_idx], &is_true_topk);
            ReadPlanEntry plan;
            plan.type = PendingIO::Type::VEC_ALL;
            plan.addr = addr;
            plan.read_length = sched_.SafeInReadLength(addr);
            plan.has_truth = has_truth;
            plan.is_true_topk = is_true_topk;
            if (sched_.UseNonSafeOutCandidateBudget()) {
                sched_.AddBudgetedReadPlan(ctx_, plan, batch.est_dist[batch_idx]);
            } else if (sched_.ShouldScheduleSafeInPrefetch(
                           ctx_, plan.read_length,
                           plan.read_length > sched_.vec_bytes_
                               ? plan.read_length - sched_.vec_bytes_
                               : 0)) {
                sched_.pending_all_plans_.push_back(plan);
                ctx_.stats().total_submit_window_requests++;
                sched_.RecordSafeInPrefetchDecision(ctx_, has_truth,
                                                    is_true_topk);
            } else {
                sched_.pending_vec_only_plans_.push_back(VecOnlyReadPlan{addr});
                ctx_.stats().total_submit_window_requests++;
            }
        }
        prepare_all_ms_ += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - all_start).count();
        BuildVecOnlyPlans(batch);
    }

    OverlapScheduler& sched_;
    SearchContext& ctx_;
    int dat_fd_;
    uint32_t cluster_id_ = 0;
    float safein_prefetch_threshold_ = std::numeric_limits<float>::infinity();
    bool skip_dedup_ = false;
    SubmitScratch& scratch_;
    bool estimates_pending_ = false;
    std::vector<EstimateHeapEntry> candidate_estimates_;
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
    const float safein_eps = (config_.safein_epsilon_override >= 0.0f)
        ? config_.safein_epsilon_override
        : index_.conann().epsilon();
    const float safeout_eps = (config_.safeout_epsilon_override >= 0.0f)
        ? config_.safeout_epsilon_override
        : index_.conann().epsilon();
    view.safein_margin_factor = 2.0f * pq->norm_qc * safein_eps;
    view.safeout_margin_factor = 2.0f * pq->norm_qc * safeout_eps;
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

    // Dynamic SafeOut and query-adaptive SafeIn share an estimate frontier
    // snapshot; SafeOut still stays disabled when its flag is off.
    const float estimate_frontier_upper = SafeOutFrontierUpper();
    const float safeout_frontier_upper = use_dynamic_safeout_
        ? estimate_frontier_upper
        : std::numeric_limits<float>::infinity();
    UpdateDynamicSafeInState(ctx, /*advance_probe=*/true);
    const float safein_prefetch_threshold = SafeInThresholdForProbe();
    RecordDynamicSafeInStats(ctx, safein_prefetch_threshold,
                             dynamic_safein_current_frontier_);

    AsyncIOSink sink(*this, ctx, dat_fd, cluster_id, safein_prefetch_threshold);
    index::ProbeStats local_stats;
    auto classify_start = std::chrono::steady_clock::now();
    prober_.Probe(pc, cluster_id, prepared, safeout_frontier_upper,
                  safein_prefetch_threshold,
                  config_.enable_address_decode_simd,
                  config_.enable_fine_grained_timing,
                  config_.enable_stage1_safein,
                  config_.enable_stage2_collect_block_first,
                  config_.enable_stage2_scatter_batch_classify,
                  config_.false_stats_cluster_members,
                  config_.false_stats_true_topk_rows,
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
        ctx.stats().probe_stage2_decode_ms += local_stats.stage2_decode_ms;
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
    ctx.stats().stage1_fused_blocks += local_stats.stage1_fused_blocks;
    ctx.stats().stage1_fused_safeout_lanes += local_stats.stage1_fused_safeout_lanes;
    ctx.stats().stage1_fused_safein_lanes += local_stats.stage1_fused_safein_lanes;
    ctx.stats().stage2_masked_kernel_calls += local_stats.stage2_masked_kernel_calls;
    ctx.stats().stage2_lanes_requested += local_stats.stage2_lanes_requested;
    ctx.stats().stage2_lanes_skipped += local_stats.stage2_lanes_skipped;
    ctx.stats().stage2_lanes_total_valid += local_stats.stage2_lanes_total_valid;
    ctx.stats().stage2_decode_blocks += local_stats.stage2_decode_blocks;
    ctx.stats().stage2_decode_input_bytes += local_stats.stage2_decode_input_bytes;
    ctx.stats().stage2_decode_output_bytes += local_stats.stage2_decode_output_bytes;
    ctx.stats().stage2_active_ex_bits_sum += local_stats.stage2_active_ex_bits_sum;
    ctx.stats().stage2_stored_ex_bits_sum += local_stats.stage2_stored_ex_bits_sum;
    sink.FinalizeCluster();
    UpdateDynamicSafeInState(ctx, /*advance_probe=*/false);
    if (!ShouldHoldDeferredSafeInPlans()) {
        FlushDeferredSafeInPlans(ctx, SafeInThresholdForProbe(), /*force=*/false);
    }
    ctx.stats().probe_submit_ms += sink.submit_cpu_ms();
    ctx.stats().probe_submit_prepare_vec_only_ms += sink.prepare_vec_only_ms_;
    ctx.stats().probe_submit_prepare_all_ms += sink.prepare_all_ms_;
    ctx.stats().probe_classify_ms =
        ctx.stats().probe_stage1_ms + ctx.stats().probe_stage2_ms;
    ctx.stats().probe_time_ms = ctx.stats().probe_prepare_ms +
        ctx.stats().probe_stage1_ms + ctx.stats().probe_stage2_ms +
        ctx.stats().probe_submit_ms;

    // Aggregate classification statistics into SearchContext.
    // total_safe_in    = S1 SafeIn (I/O submitted)
    // total_safe_out   = S1 SafeOut (skipped)
    // s1_uncertain_raw = raw Stage 1 Uncertain population (input to Stage 2)
    // total_uncertain  = final Uncertain candidates after Stage 2 elimination
    ctx.stats().total_safe_in   += local_stats.s1_safein;
    ctx.stats().total_safe_out  += local_stats.s1_safeout;
    ctx.stats().s1_uncertain_raw += local_stats.s1_uncertain;
    ctx.stats().s2_safe_in      += local_stats.s2_safein;
    ctx.stats().s2_safe_out     += local_stats.s2_safeout;
    ctx.stats().s2_uncertain    += local_stats.s2_uncertain;
    ctx.stats().s1_false_safe_in += local_stats.s1_false_safein;
    ctx.stats().s1_false_safe_out += local_stats.s1_false_safeout;
    ctx.stats().s2_false_safe_in += local_stats.s2_false_safein;
    ctx.stats().s2_false_safe_out += local_stats.s2_false_safeout;
	    ctx.stats().total_uncertain +=
	        local_stats.s1_uncertain - local_stats.s2_safein - local_stats.s2_safeout;
	}

void OverlapScheduler::ExecuteSerialDataReads(SearchContext& ctx,
                                              RerankConsumer& reranker) {
    if (cluster_reader_.prepped() > 0 || cluster_reader_.InFlight() > 0 ||
        data_reader_.prepped() > 0 || data_reader_.InFlight() > 0) {
        std::fprintf(stderr,
                     "FATAL: serial-no-overlap entered read phase with async I/O pending\n");
        std::abort();
    }
    if (pending_spec_vec_head_ < pending_spec_vec_plans_.size()) {
        std::fprintf(stderr,
                     "FATAL: serial-no-overlap must not emit SPEC_VEC_ONLY plans\n");
        std::abort();
    }

    const bool separate_store = UseSeparateRecordStore();
    const bool vector_sidecar = UseVectorSidecarStore();
    const int vec_fd = vector_sidecar
        ? config_.separate_record_store.vector_fd
        : index_.segment().data_reader().fd();
    const int payload_fd = separate_store
        ? config_.separate_record_store.payload_fd
        : index_.segment().data_reader().fd();

    auto read_vec_only = [&](AddressEntry addr) {
        uint64_t read_offset = addr.offset;
        if (vector_sidecar) {
            const SeparateRecordLocation& loc =
                LookupSeparateRecordOrAbort(ctx, addr);
            read_offset = SeparateVectorOffset(loc);
        }
        RecordVectorReadTrace(addr, false);
        uint8_t* buf = AcquireVecOnlyBuffer();
        auto t0 = std::chrono::steady_clock::now();
        if (vector_sidecar) {
            CheckReadStatus(ReadExactFd(vec_fd, read_offset, vec_bytes_, buf),
                            "serial VEC_ONLY vector sidecar");
        } else {
            CheckReadStatus(index_.segment().data_reader().ReadRaw(
                                read_offset, vec_bytes_, buf),
                            "serial VEC_ONLY");
        }
        ctx.stats().serial_vector_read_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        ctx.stats().vec_only_read_requests++;
        ctx.stats().vec_only_read_bytes += vec_bytes_;
        ctx.stats().serial_vector_read_requests++;
        ctx.stats().serial_vector_read_bytes += vec_bytes_;
        reranker.ConsumeVec(buf, addr);
        ReleaseVecOnlyBuffer(buf);
    };

    while (!pending_all_plans_.empty()) {
        const ReadPlanEntry plan = pending_all_plans_.front();
        pending_all_plans_.pop_front();
        if (separate_store) {
            const SeparateRecordLocation& loc =
                LookupSeparateRecordOrAbort(ctx, plan.addr);
            const uint32_t payload_prefix_len =
                plan.read_length > vec_bytes_
                    ? std::min<uint32_t>(loc.payload_bytes,
                                         plan.read_length - vec_bytes_)
                    : 0;
            uint8_t* vec_buf = AcquireVecOnlyBuffer();
            const uint64_t vec_off = SeparateVectorOffset(loc);
            RecordVectorReadTrace(plan.addr, true);
            auto tv0 = std::chrono::steady_clock::now();
            CheckReadStatus(ReadExactFd(vec_fd, vec_off, vec_bytes_, vec_buf),
                            "serial VEC_ALL separate vector");
            ctx.stats().serial_vector_read_ms +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - tv0).count();
            ctx.stats().vec_only_read_requests++;
            ctx.stats().vec_only_read_bytes += vec_bytes_;
            ctx.stats().serial_vector_read_requests++;
            ctx.stats().serial_vector_read_bytes += vec_bytes_;
            reranker.ConsumeVec(vec_buf, plan.addr);
            ReleaseVecOnlyBuffer(vec_buf);

                if (payload_prefix_len > 0) {
                uint8_t* payload_buf = buffer_pool_.Acquire(payload_prefix_len);
                auto tp0 = std::chrono::steady_clock::now();
                CheckReadStatus(ReadExactFd(payload_fd, loc.payload_offset,
                                            payload_prefix_len, payload_buf),
                                "serial SafeIn separate payload prefix");
                ctx.stats().serial_payload_read_ms +=
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - tp0).count();
                ctx.stats().payload_read_requests++;
                ctx.stats().payload_read_bytes += payload_prefix_len;
                ctx.stats().serial_payload_read_requests++;
                ctx.stats().serial_payload_read_bytes += payload_prefix_len;
                reranker.ConsumePayloadPrefix(payload_buf, plan.addr,
                                              payload_prefix_len,
                                              &buffer_pool_);
            }
            const uint32_t scheduled_bytes = vec_bytes_ + payload_prefix_len;
            if (payload_prefix_len >= loc.payload_bytes) {
                ctx.stats().safein_full_read_requests++;
                ctx.stats().safein_full_read_bytes += scheduled_bytes;
            } else {
                ctx.stats().safein_prefix_read_requests++;
                ctx.stats().safein_prefix_read_bytes += scheduled_bytes;
            }
            continue;
        }

        uint8_t* buf = buffer_pool_.Acquire(plan.read_length);
        RecordVectorReadTrace(plan.addr, true);
        auto t0 = std::chrono::steady_clock::now();
        CheckReadStatus(index_.segment().data_reader().ReadRaw(
                            plan.addr.offset, plan.read_length, buf),
                        "serial VEC_ALL");
        ctx.stats().serial_full_record_read_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
        ctx.stats().all_read_requests++;
        ctx.stats().all_read_bytes += plan.read_length;
        ctx.stats().serial_full_record_read_requests++;
        ctx.stats().serial_full_record_read_bytes += plan.read_length;
        if (plan.read_length >= plan.addr.size) {
            ctx.stats().safein_full_read_requests++;
            ctx.stats().safein_full_read_bytes += plan.read_length;
        } else {
            ctx.stats().safein_prefix_read_requests++;
            ctx.stats().safein_prefix_read_bytes += plan.read_length;
        }
        CacheInlinePayloadLocationFromRecord(
            ctx, plan.addr, buf, plan.read_length);
        reranker.ConsumeAll(buf, plan.addr, plan.read_length);
        buffer_pool_.Release(buf);
    }

    while (pending_vec_only_head_ < pending_vec_only_plans_.size()) {
        read_vec_only(pending_vec_only_plans_[pending_vec_only_head_].addr);
        ++pending_vec_only_head_;
    }
    pending_vec_only_plans_.clear();
    pending_vec_only_head_ = 0;
    pending_spec_vec_plans_.clear();
    pending_spec_vec_head_ = 0;
}

	// ============================================================================
	// FinalDrain
	// ============================================================================

void OverlapScheduler::FinalDrain(SearchContext& ctx,
                                   RerankConsumer& reranker) {
    std::vector<IoCompletion> comps(128);
    if (config_.enable_safein_bextra_probe_budget) {
        auto poll_ready = [&](AsyncReader& reader) {
            for (;;) {
                const uint32_t n = reader.Poll(
                    comps.data(), static_cast<uint32_t>(comps.size()));
                if (n == 0) break;
                for (uint32_t i = 0; i < n; ++i) {
                    DispatchCompletion(comps[i].user_data, comps[i].result,
                                       ctx, reranker);
                }
            }
        };
        poll_ready(cluster_reader_);
        if (isolated_submission_mode_) {
            poll_ready(data_reader_);
        }
        bextra_before_final_drain_ = false;
    }
    auto drain_reader = [&](AsyncReader& reader) {
        while (reader.InFlight() > 0) {
            auto tw0 = std::chrono::steady_clock::now();
            uint32_t n = reader.WaitAndPoll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            ctx.stats().io_wait_time_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - tw0).count();
            for (uint32_t j = 0; j < n; ++j) {
                DispatchCompletion(comps[j].user_data, comps[j].result,
                                   ctx, reranker);
            }
        }
        for (;;) {
            uint32_t n = reader.Poll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            if (n == 0) break;
            for (uint32_t j = 0; j < n; ++j) {
                DispatchCompletion(comps[j].user_data, comps[j].result,
                                   ctx, reranker);
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

void OverlapScheduler::ResolvePayloadLocations(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<CollectorEntry>& results) {
    (void)reranker;
    if (!UseInlineHotRecordStore()) {
        for (const auto& entry : results) {
            (void)LocatePayloadOrAbort(ctx, entry.addr);
        }
        return;
    }

    if (config_.inline_hot_record_store.payload_metadata != nullptr) {
        for (const auto& entry : results) {
            (void)LocatePayloadOrAbort(ctx, entry.addr);
        }
        return;
    }

    const uint32_t descriptor_bytes =
        config_.inline_hot_record_store.descriptor_bytes;
    const int descriptor_fd =
        config_.inline_hot_record_store.buffered_hot_record_fd;

    // A tiny unaligned descriptor read cannot use the O_DIRECT data fd. Keep
    // the existing synchronous fallback when no buffered descriptor fd exists.
    if (descriptor_fd < 0) {
        for (const auto& entry : results) {
            (void)LocatePayloadOrAbort(ctx, entry.addr);
        }
        return;
    }

    uint32_t pending_descriptors = 0;
    for (const auto& entry : results) {
        if (payload_location_cache_.count(entry.addr.offset) > 0) continue;
        if (descriptor_bytes != sizeof(storage::HotPayloadDescriptor) ||
            entry.addr.size < vec_bytes_ + descriptor_bytes) {
            (void)LocatePayloadOrAbort(ctx, entry.addr);
            continue;
        }

        uint8_t* buf = buffer_pool_.Acquire(descriptor_bytes);
        PendingIO pio;
        pio.type = PendingIO::Type::PAYLOAD_DESCRIPTOR;
        pio.addr = entry.addr;
        pio.read_offset = entry.addr.offset + vec_bytes_;
        pio.read_length = descriptor_bytes;
        const uint32_t slot_id = AllocatePendingSlot(
            std::move(pio), buf, PendingBufferCleanup::Pool);
        CheckPrepRead(data_reader_.PrepReadTagged(
                          descriptor_fd, buf, descriptor_bytes,
                          entry.addr.offset + vec_bytes_, slot_id),
                      "inline payload descriptor batch");
        ctx.stats().inline_descriptor_read_requests++;
        ++pending_descriptors;
    }

    if (pending_descriptors == 0) return;

    const auto submit_start = std::chrono::steady_clock::now();
    const uint32_t submitted = data_reader_.Submit();
    ctx.stats().uring_submit_ms += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - submit_start).count();
    ctx.stats().total_io_submitted += submitted;
    if (submitted > 0) {
        ctx.stats().total_submit_calls++;
    }

    std::vector<IoCompletion> comps(
        std::max<uint32_t>(128u, pending_descriptors));
    while (data_reader_.InFlight() > 0) {
        const auto wait_start = std::chrono::steady_clock::now();
        const uint32_t n = data_reader_.WaitAndPoll(
            comps.data(), static_cast<uint32_t>(comps.size()));
        ctx.stats().io_wait_time_ms +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - wait_start).count();
        for (uint32_t i = 0; i < n; ++i) {
            DispatchCompletion(comps[i].user_data, comps[i].result,
                               ctx, reranker);
        }
    }
    for (;;) {
        const uint32_t n = data_reader_.Poll(
            comps.data(), static_cast<uint32_t>(comps.size()));
        if (n == 0) break;
        for (uint32_t i = 0; i < n; ++i) {
            DispatchCompletion(comps[i].user_data, comps[i].result,
                               ctx, reranker);
        }
    }
}

void OverlapScheduler::FetchMissingPayloads(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<CollectorEntry>& results) {
    ResolvePayloadLocations(ctx, reranker, results);
    for (const auto& entry : results) {
        const PayloadLocation payload =
            LocatePayloadOrAbort(ctx, entry.addr);
        if (payload.length == 0) continue;
        const uint32_t cached_prefix =
            std::min(reranker.CachedPayloadBytes(entry.addr.offset),
                     payload.length);
        if (cached_prefix >= payload.length) {
            ctx.stats().inline_payload_cache_hits +=
                payload.from_inline_hot_record ? 1u : 0u;
            continue;
        }
        uint8_t* buf = buffer_pool_.Acquire(payload.length);
        if (cached_prefix > 0) {
            const uint8_t* cached = reranker.CachedPayloadData(entry.addr.offset);
            if (cached != nullptr) {
                std::memcpy(buf, cached, cached_prefix);
            }
        }
        uint32_t ready_prefix = cached_prefix;
        if (ready_prefix < payload.inline_prefix_length) {
            const uint32_t hot_prefix_bytes =
                payload.inline_prefix_length - ready_prefix;
            CheckReadStatus(
                ReadExactFd(payload.inline_prefix_fd,
                            payload.inline_prefix_offset + ready_prefix,
                            hot_prefix_bytes, buf + ready_prefix),
                "inline hot payload prefix");
            ctx.stats().payload_read_requests++;
            ctx.stats().payload_read_bytes += hot_prefix_bytes;
            ready_prefix = payload.inline_prefix_length;
        }
        const uint32_t backing_skip = ready_prefix > payload.inline_prefix_length
            ? ready_prefix - payload.inline_prefix_length
            : (payload.inline_prefix_length == 0 ? ready_prefix : 0u);
        const uint32_t suffix_len = payload.length - ready_prefix;
        if (suffix_len == 0) {
            reranker.ConsumePayload(buf, entry.addr, payload.length);
            continue;
        }
        PendingIO pio;
        pio.type = PendingIO::Type::PAYLOAD;
        pio.addr = entry.addr;
        pio.read_offset = payload.offset + backing_skip;
        pio.read_length = suffix_len;
        pio.payload_total_length = payload.length;
        pio.payload_prefix_length = ready_prefix;
        uint32_t slot_id = AllocatePendingSlot(std::move(pio), buf,
                                               PendingBufferCleanup::Pool);
        CheckPrepRead(data_reader_.PrepReadTagged(
                          payload.fd, buf + ready_prefix, suffix_len,
                          payload.offset + backing_skip, slot_id),
                      "payload");
        ctx.stats().payload_read_requests++;
        ctx.stats().payload_read_bytes += suffix_len;
        if (ready_prefix > 0) {
            ctx.stats().safein_suffix_read_requests++;
            ctx.stats().safein_suffix_read_bytes += suffix_len;
        }
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
                DispatchCompletion(comps[i].user_data, comps[i].result,
                                   ctx, reranker);
            }
        }
        // Drain sync completions (pread path)
        for (;;) {
            uint32_t n = data_reader_.Poll(
                comps.data(), static_cast<uint32_t>(comps.size()));
            if (n == 0) break;
            for (uint32_t i = 0; i < n; ++i) {
                DispatchCompletion(comps[i].user_data, comps[i].result,
                                   ctx, reranker);
            }
        }
    }
}

void OverlapScheduler::FetchMissingPayloadsSerial(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<CollectorEntry>& results) {
    for (const auto& entry : results) {
        const PayloadLocation payload =
            LocatePayloadOrAbort(ctx, entry.addr);
        if (payload.length == 0) continue;
        const uint32_t cached_prefix =
            std::min(reranker.CachedPayloadBytes(entry.addr.offset),
                     payload.length);
        if (cached_prefix >= payload.length) {
            ctx.stats().inline_payload_cache_hits +=
                payload.from_inline_hot_record ? 1u : 0u;
            continue;
        }

        uint8_t* buf = buffer_pool_.Acquire(payload.length);
        if (cached_prefix > 0) {
            const uint8_t* cached = reranker.CachedPayloadData(entry.addr.offset);
            if (cached != nullptr) {
                std::memcpy(buf, cached, cached_prefix);
            }
        }
        uint32_t ready_prefix = cached_prefix;
        if (ready_prefix < payload.inline_prefix_length) {
            const uint32_t hot_prefix_bytes =
                payload.inline_prefix_length - ready_prefix;
            auto hot_t0 = std::chrono::steady_clock::now();
            CheckReadStatus(
                ReadExactFd(payload.inline_prefix_fd,
                            payload.inline_prefix_offset + ready_prefix,
                            hot_prefix_bytes, buf + ready_prefix),
                "serial inline hot payload prefix");
            ctx.stats().serial_payload_read_ms +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - hot_t0).count();
            ctx.stats().payload_read_requests++;
            ctx.stats().payload_read_bytes += hot_prefix_bytes;
            ctx.stats().serial_payload_read_requests++;
            ctx.stats().serial_payload_read_bytes += hot_prefix_bytes;
            ready_prefix = payload.inline_prefix_length;
        }
        const uint32_t backing_skip = ready_prefix > payload.inline_prefix_length
            ? ready_prefix - payload.inline_prefix_length
            : (payload.inline_prefix_length == 0 ? ready_prefix : 0u);
        const uint32_t suffix_len = payload.length - ready_prefix;
        auto t0 = std::chrono::steady_clock::now();
        if (suffix_len > 0) {
            CheckReadStatus(ReadExactFd(payload.fd,
                                        payload.offset + backing_skip,
                                        suffix_len, buf + ready_prefix),
                            "serial final payload");
            ctx.stats().serial_payload_read_ms +=
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
            ctx.stats().payload_read_requests++;
            ctx.stats().payload_read_bytes += suffix_len;
            ctx.stats().serial_payload_read_requests++;
            ctx.stats().serial_payload_read_bytes += suffix_len;
        }
        if (ready_prefix > 0) {
            ctx.stats().safein_suffix_read_requests++;
            ctx.stats().safein_suffix_read_bytes += suffix_len;
        }
        ctx.stats().total_payload_fetched++;
        reranker.ConsumePayload(buf, entry.addr, payload.length,
                                &buffer_pool_);
    }
}

// ============================================================================
// AssembleResults
// ============================================================================

SearchResults OverlapScheduler::AssembleResults(
    SearchContext& ctx, RerankConsumer& reranker,
    const std::vector<CollectorEntry>& results) {
    SearchResults sr;
    sr.results().reserve(results.size());

    const auto& schemas = index_.segment().data_reader().payload_schemas();
    bool has_payload = !schemas.empty();

    for (const auto& entry : results) {
        SearchResult result;
        result.distance = entry.distance;

        if (has_payload) {
            const uint8_t* payload_buf =
                reranker.CachedPayloadData(entry.addr.offset);
            if (payload_buf != nullptr) {
                const PayloadLocation payload =
                    LocatePayloadOrAbort(ctx, entry.addr);
                if (payload.length == 0) {
                    reranker.ReleasePayload(entry.addr.offset);
                    sr.results().push_back(std::move(result));
                    continue;
                }
                CheckReadStatus(
                    index_.segment().data_reader().ParsePayload(
                        payload_buf, payload.length, 0, result.payload),
                    "final payload parse");
                reranker.ReleasePayload(entry.addr.offset);
            }
        }

        sr.results().push_back(std::move(result));
    }

    return sr;
}

}  // namespace query
}  // namespace vdb
