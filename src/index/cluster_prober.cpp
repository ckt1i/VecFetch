#include "vdb/index/cluster_prober.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "vdb/simd/stage2_classify.h"
#include "vdb/simd/fastscan.h"
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/storage/pack_codes.h"

#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
#include <immintrin.h>
#endif

namespace vdb {
namespace index {

namespace {

constexpr uint32_t kStage2BatchSize = 8;

bool DebugCompareCompactStage2Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("VDB_DEBUG_COMPARE_COMPACT_STAGE2");
        return env != nullptr && env[0] != '\0' && env[0] != '0';
    }();
    return enabled;
}

VDB_FORCE_INLINE uint32_t LaneMaskForCount(uint32_t count) {
    return (count >= 32u) ? 0xFFFFFFFFu : ((1u << count) - 1u);
}

VDB_FORCE_INLINE uint32_t CompactMaskToIndices(uint32_t mask,
                                               uint32_t* VDB_RESTRICT out_idx) {
    uint32_t n = 0;
    while (mask) {
        const uint32_t lane = static_cast<uint32_t>(__builtin_ctz(mask));
        out_idx[n++] = lane;
        mask &= (mask - 1u);
    }
    return n;
}

}  // namespace

ClusterProber::ClusterProber(const ConANN& conann, Dim dim, uint8_t bits)
    : conann_(conann),
      dim_(dim),
      bits_(bits),
      has_s2_(bits > 1),
      margin_s2_divisor_(bits > 1 ? static_cast<float>(1u << (bits - 1)) : 1.0f),
      estimator_(dim, bits) {}

ClusterProber::~ClusterProber() = default;

void ClusterProber::Probe(const query::ParsedCluster& pc,
                           const rabitq::PreparedClusterQueryView& view,
                           float margin_factor,
                           float dynamic_d_k,
                           bool enable_address_decode_simd,
                           bool enable_fine_grained_timing,
                           bool enable_stage2_collect_block_first,
                           bool enable_stage2_scatter_batch_classify,
                           ProbeResultSink& sink,
                           ProbeStats& stats) const {
    const rabitq::PreparedQuery& pq = *view.prepared;
    const uint32_t packed_sz = storage::FastScanPackedSize(dim_);
    const float safein_threshold_base = conann_.d_k();

    for (uint32_t b = 0; b < pc.num_fastscan_blocks; ++b) {
        ++stats.num_stage1_blocks;
        CandidateBatch batch;
        const uint32_t base_idx = b * 32;
        const uint32_t count = std::min(32u, pc.num_records - base_idx);

        const uint8_t* block_ptr =
            pc.fastscan_blocks + static_cast<size_t>(b) * pc.fastscan_block_size;
        const float* block_norms =
            reinterpret_cast<const float*>(block_ptr + packed_sz);

        // Stage 1a: batch-32 FastScan distance estimation
        alignas(64) float dists[32];
        if (enable_fine_grained_timing) {
            auto estimate_start = std::chrono::steady_clock::now();
            estimator_.EstimateDistanceFastScan(
                view, block_ptr, block_norms, count, dists);
            stats.stage1_estimate_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - estimate_start).count();
        } else {
            estimator_.EstimateDistanceFastScan(view, block_ptr, block_norms, count, dists);
        }

        // Stage 1b: batch SafeOut classification via SIMD bitmask.
        // Bit v set  ⟺  dists[v] > dynamic_d_k + 2 * margin_factor * block_norms[v]
        uint32_t so_mask = 0;
        if (enable_fine_grained_timing) {
            auto mask_start = std::chrono::steady_clock::now();
            so_mask = simd::FastScanSafeOutMask(
                dists, block_norms, count, dynamic_d_k, margin_factor);
            stats.stage1_mask_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - mask_start).count();
        } else {
            so_mask = simd::FastScanSafeOutMask(
                dists, block_norms, count, dynamic_d_k, margin_factor);
        }
        stats.s1_safeout += static_cast<uint32_t>(__builtin_popcount(so_mask));

        // Stage 1c/1d: compact survivors and classify them in mask batches.
        const uint32_t lane_valid = LaneMaskForCount(count);
        const uint32_t maybe_in = (~so_mask) & lane_valid;
        const uint32_t safein_mask = simd::FastScanSafeInMask(
            dists, block_norms, count, safein_threshold_base, margin_factor) & maybe_in;
        const uint32_t uncertain_mask = maybe_in & ~safein_mask;

        uint32_t survivor_lanes[32];
        uint32_t survivor_count = 0;
        uint32_t safein_count = 0;
        uint32_t uncertain_count = 0;
        if (enable_fine_grained_timing) {
            auto iterate_start = std::chrono::steady_clock::now();
            survivor_count = CompactMaskToIndices(maybe_in, survivor_lanes);
            safein_count = static_cast<uint32_t>(__builtin_popcount(safein_mask));
            uncertain_count = static_cast<uint32_t>(__builtin_popcount(uncertain_mask));
            stats.stage1_iterate_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - iterate_start).count();
        } else {
            survivor_count = CompactMaskToIndices(maybe_in, survivor_lanes);
            safein_count = static_cast<uint32_t>(__builtin_popcount(safein_mask));
            uncertain_count = static_cast<uint32_t>(__builtin_popcount(uncertain_mask));
        }

        if (enable_fine_grained_timing) {
            auto classify_start = std::chrono::steady_clock::now();
            stats.s1_safein += safein_count;
            stats.s1_uncertain += uncertain_count;
            stats.stage1_classify_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - classify_start).count();
        } else {
            stats.s1_safein += safein_count;
            stats.s1_uncertain += uncertain_count;
        }

        struct Stage2LaneEntry {
            bool present = false;
            uint32_t global_idx = 0;
            float est_dist_s1 = 0.0f;
            float margin_s1 = 0.0f;
            float norm_oc = 0.0f;
            const uint8_t* code_abs = nullptr;
            const uint8_t* sign = nullptr;
            float xipnorm = 0.0f;
            bool sign_packed = false;
        };
        struct Stage2BlockScratch {
            bool used = false;
            uint32_t block_id = 0;
            uint32_t lane_mask = 0;
            Stage2LaneEntry lanes[kStage2BatchSize];
        };
        Stage2BlockScratch stage2_blocks[4];
        const uint32_t base_stage2_block_id = base_idx / kStage2BatchSize;
        uint32_t stage2_block_count = 0;
        uint32_t stage2_candidate_count = 0;
        uint32_t stage2_block_lookups = 0;
        uint32_t stage2_block_reuses = 0;
        double stage2_collect_ms = 0.0;
        double stage2_kernel_ms = 0.0;
        double stage2_scatter_ms = 0.0;
        double stage2_kernel_sign_flip_ms = 0.0;
        double stage2_kernel_abs_fma_ms = 0.0;
        double stage2_kernel_tail_ms = 0.0;
        double stage2_kernel_reduce_ms = 0.0;

        for (uint32_t s = 0; s < survivor_count; ++s) {
            const uint32_t j = survivor_lanes[s];
            const uint32_t global_idx = base_idx + j;
            const float est_dist_s1 = dists[j];
            const float margin_s1 = margin_factor * block_norms[j];
            ResultClass rc_final = ((safein_mask >> j) & 1u) != 0
                ? ResultClass::SafeIn
                : ResultClass::Uncertain;

            // Stage 2: collect S1-Uncertain candidates and batch packed-sign boosting.
            if (rc_final == ResultClass::Uncertain && has_s2_ &&
                (pc.exrabitq_entries != nullptr || pc.exrabitq_batch_blocks != nullptr)) {
                auto collect_start = enable_fine_grained_timing
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                const uint32_t lane_id = global_idx % kStage2BatchSize;
                Stage2BlockScratch* block = nullptr;
                if (enable_stage2_collect_block_first) {
                    const uint32_t local_block_slot = j / kStage2BatchSize;
                    const uint32_t block_id = base_stage2_block_id + local_block_slot;
                    block = &stage2_blocks[local_block_slot];
                    if (!block->used) {
                        block->used = true;
                        block->block_id = block_id;
                        block->lane_mask = 0;
                        ++stage2_block_count;
                    } else {
                        ++stage2_block_reuses;
                    }
                } else {
                    const uint32_t block_id = global_idx / kStage2BatchSize;
                    for (uint32_t bi = 0; bi < stage2_block_count; ++bi) {
                        ++stage2_block_lookups;
                        if (stage2_blocks[bi].block_id == block_id) {
                            block = &stage2_blocks[bi];
                            ++stage2_block_reuses;
                            break;
                        }
                    }
                    if (block == nullptr) {
                        block = &stage2_blocks[stage2_block_count++];
                        block->used = true;
                        block->block_id = block_id;
                        block->lane_mask = 0;
                    }
                }
                Stage2LaneEntry& entry = block->lanes[lane_id];
                entry.present = true;
                entry.global_idx = global_idx;
                entry.est_dist_s1 = est_dist_s1;
                entry.margin_s1 = margin_s1;
                entry.norm_oc = block_norms[j];
                if (pc.exrabitq_storage_version >= 11) {
                    entry.xipnorm = 0.0f;
                    entry.sign_packed = true;
                } else {
                    const query::ParsedCluster::ExRaBitQView ex_view =
                        pc.exrabitq_view(global_idx, dim_);
                    entry.code_abs = ex_view.code_abs;
                    entry.sign = ex_view.sign;
                    entry.xipnorm = ex_view.xipnorm;
                    entry.sign_packed = ex_view.sign_packed;
                }
                block->lane_mask |= (1u << lane_id);
                ++stage2_candidate_count;
                if (enable_fine_grained_timing) {
                    stage2_collect_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - collect_start).count();
                }
                continue;
            }

            batch.global_idx[batch.count] = global_idx;
            batch.est_dist[batch.count] = est_dist_s1;
            batch.cls[batch.count] = (rc_final == ResultClass::SafeIn)
                ? CandidateClass::SafeIn
                : CandidateClass::Uncertain;
            batch.count++;
        }

        if (stage2_candidate_count > 0) {
            stats.num_stage2_candidates += stage2_candidate_count;

            auto stage2_start = enable_fine_grained_timing
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::time_point{};
            const float* const rotated_query = pq.rotated.data();

            for (uint32_t bi = 0; bi < 4; ++bi) {
                const Stage2BlockScratch& block = stage2_blocks[bi];
                if (!block.used) continue;
                alignas(64) float ip_raw_batch[kStage2BatchSize] = {};
                query::ParsedCluster::ExRaBitQBatchBlockView block_view;
                auto kernel_start = enable_fine_grained_timing
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                simd::IPExRaBitQBatchPackedSignCompactTiming kernel_timing;
                simd::IPExRaBitQBatchPackedSignCompactTiming* kernel_timing_ptr =
                    enable_fine_grained_timing ? &kernel_timing : nullptr;
                if (pc.exrabitq_storage_version >= 11) {
                    block_view = pc.exrabitq_batch_block_view(block.block_id);
                    const auto parallel_view =
                        pc.exrabitq_batch_parallel_block_view(block.block_id);
                    if (parallel_view.abs_slices != nullptr &&
                        parallel_view.sign_words != nullptr) {
                        simd::IPExRaBitQBatchPackedSignParallelCompact(
                            rotated_query,
                            parallel_view.abs_slices,
                            parallel_view.sign_words,
                            parallel_view.valid_count,
                            dim_,
                            pc.exrabitq_dim_block,
                            parallel_view.slices_per_dim_block,
                            ip_raw_batch,
                            kernel_timing_ptr);
                    } else {
                        simd::IPExRaBitQBatchPackedSignCompact(
                            rotated_query,
                            block_view.abs_blocks,
                            block_view.sign_blocks,
                            block_view.valid_count,
                            dim_,
                            pc.exrabitq_dim_block,
                            ip_raw_batch,
                            kernel_timing_ptr);
                    }
                    if (DebugCompareCompactStage2Enabled()) {
                        const uint32_t sign_block_bytes = pc.exrabitq_dim_block / 8;
                        std::vector<uint8_t> ref_abs(dim_);
                        std::vector<uint8_t> ref_sign((dim_ + 7) / 8);
                        float max_abs_diff = 0.0f;
                        uint32_t worst_lane = 0;
                        uint32_t compared = 0;
                        for (uint32_t lane = 0; lane < block_view.valid_count; ++lane) {
                            if ((block.lane_mask & (1u << lane)) == 0) continue;
                            std::fill(ref_abs.begin(), ref_abs.end(), 0);
                            std::fill(ref_sign.begin(), ref_sign.end(), 0);
                            for (uint32_t db = 0; db < pc.exrabitq_num_dim_blocks; ++db) {
                                const uint32_t dim_start = db * pc.exrabitq_dim_block;
                                const uint32_t copy = std::min(pc.exrabitq_dim_block, dim_ - dim_start);
                                const uint8_t* abs_ptr =
                                    block_view.abs_blocks +
                                    static_cast<size_t>(db) * pc.exrabitq_batch_size * pc.exrabitq_dim_block +
                                    lane * pc.exrabitq_dim_block;
                                std::memcpy(ref_abs.data() + dim_start, abs_ptr, copy);
                                const uint8_t* sign_ptr =
                                    block_view.sign_blocks +
                                    static_cast<size_t>(db) * pc.exrabitq_batch_size * sign_block_bytes +
                                    lane * sign_block_bytes;
                                std::memcpy(ref_sign.data() + dim_start / 8, sign_ptr, sign_block_bytes);
                            }
                            const float ref_ip = simd::IPExRaBitQPackedSign(
                                rotated_query, ref_abs.data(), ref_sign.data(), dim_);
                            const float diff = std::abs(ref_ip - ip_raw_batch[lane]);
                            if (diff > max_abs_diff) {
                                max_abs_diff = diff;
                                worst_lane = lane;
                            }
                            ++compared;
                        }
                        if (compared > 0 && max_abs_diff > 1e-3f) {
                            std::fprintf(stderr,
                                "[v11-stage2-debug] block=%u compared=%u worst_lane=%u max_abs_diff=%.6f valid=%u lane_mask=0x%02x\n",
                                block.block_id, compared, worst_lane, max_abs_diff,
                                block_view.valid_count, block.lane_mask);
                        }
                    }
                } else {
                    bool all_packed = true;
                    const uint8_t* code_abs_ptrs[kStage2BatchSize] = {};
                    const uint8_t* sign_ptrs[kStage2BatchSize] = {};
                    uint32_t chunk = 0;
                    for (uint32_t lane = 0; lane < kStage2BatchSize; ++lane) {
                        if ((block.lane_mask & (1u << lane)) == 0) continue;
                        const Stage2LaneEntry& entry = block.lanes[lane];
                        all_packed &= entry.sign_packed;
                        code_abs_ptrs[chunk] = entry.code_abs;
                        sign_ptrs[chunk] = entry.sign;
                        ++chunk;
                    }
                    if (all_packed) {
                        simd::IPExRaBitQBatchPackedSign(
                            rotated_query, code_abs_ptrs, sign_ptrs, chunk, dim_, ip_raw_batch);
                    } else {
                        uint32_t chunk_idx = 0;
                        for (uint32_t lane = 0; lane < kStage2BatchSize; ++lane) {
                            if ((block.lane_mask & (1u << lane)) == 0) continue;
                            const Stage2LaneEntry& entry = block.lanes[lane];
                            ip_raw_batch[chunk_idx++] = simd::IPExRaBitQ(
                                rotated_query, entry.code_abs, entry.sign, entry.sign_packed, dim_);
                        }
                    }
                }
                if (enable_fine_grained_timing) {
                    stage2_kernel_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - kernel_start).count();
                    stage2_kernel_sign_flip_ms += kernel_timing.sign_flip_ms;
                    stage2_kernel_abs_fma_ms += kernel_timing.abs_fma_ms;
                    stage2_kernel_tail_ms += kernel_timing.tail_ms;
                    stage2_kernel_reduce_ms += kernel_timing.reduce_ms;
                }

                auto scatter_start = enable_fine_grained_timing
                    ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
                alignas(32) float lane_ip_raw[kStage2BatchSize] = {};
                alignas(32) float lane_xipnorm[kStage2BatchSize] = {};
                alignas(32) float lane_norm_oc[kStage2BatchSize] = {};
                alignas(32) float lane_margin_s1[kStage2BatchSize] = {};
                uint32_t chunk_idx = 0;
                for (uint32_t lane = 0; lane < kStage2BatchSize; ++lane) {
                    if ((block.lane_mask & (1u << lane)) == 0) continue;
                    const Stage2LaneEntry& entry = block.lanes[lane];
                    lane_xipnorm[lane] = (pc.exrabitq_storage_version >= 11)
                        ? block_view.xipnorms[lane]
                        : entry.xipnorm;
                    lane_ip_raw[lane] = (pc.exrabitq_storage_version >= 11)
                        ? ip_raw_batch[lane]
                        : ip_raw_batch[chunk_idx++];
                    lane_norm_oc[lane] = entry.norm_oc;
                    lane_margin_s1[lane] = entry.margin_s1;
                }

                if (enable_stage2_scatter_batch_classify) {
                    const simd::Stage2ClassifyMasks classify_masks = simd::Stage2ClassifyBatch(
                        lane_ip_raw,
                        lane_xipnorm,
                        lane_norm_oc,
                        lane_margin_s1,
                        block.lane_mask,
                        pq.norm_qc,
                        pq.norm_qc_sq,
                        1.0f / margin_s2_divisor_,
                        conann_.d_k(),
                        dynamic_d_k);

                    stats.s2_safein += static_cast<uint32_t>(__builtin_popcount(classify_masks.safein));
                    stats.s2_safeout += static_cast<uint32_t>(__builtin_popcount(classify_masks.safeout));
                    stats.s2_uncertain += static_cast<uint32_t>(__builtin_popcount(classify_masks.uncertain));

                    const uint32_t keep_mask = classify_masks.safein | classify_masks.uncertain;
                    for (uint32_t lane = 0; lane < kStage2BatchSize; ++lane) {
                        if ((keep_mask & (1u << lane)) == 0) continue;
                        const Stage2LaneEntry& entry = block.lanes[lane];
                        batch.global_idx[batch.count] = entry.global_idx;
                        batch.est_dist[batch.count] = entry.est_dist_s1;
                        batch.cls[batch.count] = ((classify_masks.safein >> lane) & 1u) != 0
                            ? CandidateClass::SafeIn
                            : CandidateClass::Uncertain;
                        batch.count++;
                    }
                } else {
                    for (uint32_t lane = 0; lane < kStage2BatchSize; ++lane) {
                        if ((block.lane_mask & (1u << lane)) == 0) continue;
                        const Stage2LaneEntry& entry = block.lanes[lane];
                        const float xipnorm = lane_xipnorm[lane];
                        const float ip_raw = lane_ip_raw[lane];
                        const float ip_est = ip_raw * xipnorm;

                        float est_dist_s2 = entry.norm_oc * entry.norm_oc + pq.norm_qc_sq
                                          - 2.0f * entry.norm_oc * pq.norm_qc * ip_est;
                        est_dist_s2 = std::max(est_dist_s2, 0.0f);

                        const float margin_s2 = entry.margin_s1 / margin_s2_divisor_;
                        const ResultClass rc_s2 =
                            conann_.ClassifyAdaptive(est_dist_s2, margin_s2, dynamic_d_k);

                        if (rc_s2 == ResultClass::SafeIn) {
                            ++stats.s2_safein;
                        } else if (rc_s2 == ResultClass::SafeOut) {
                            ++stats.s2_safeout;
                            continue;
                        } else {
                            ++stats.s2_uncertain;
                        }

                        batch.global_idx[batch.count] = entry.global_idx;
                        batch.est_dist[batch.count] = entry.est_dist_s1;
                        batch.cls[batch.count] = (rc_s2 == ResultClass::SafeIn)
                            ? CandidateClass::SafeIn
                            : CandidateClass::Uncertain;
                        batch.count++;
                    }
                }
                if (enable_fine_grained_timing) {
                    stage2_scatter_ms += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - scatter_start).count();
                }
            }

            if (enable_fine_grained_timing) {
                stats.stage2_collect_ms += stage2_collect_ms;
                stats.stage2_kernel_ms += stage2_kernel_ms;
                stats.stage2_scatter_ms += stage2_scatter_ms;
                stats.stage2_kernel_sign_flip_ms += stage2_kernel_sign_flip_ms;
                stats.stage2_kernel_abs_fma_ms += stage2_kernel_abs_fma_ms;
                stats.stage2_kernel_tail_ms += stage2_kernel_tail_ms;
                stats.stage2_kernel_reduce_ms += stage2_kernel_reduce_ms;
                stats.num_stage2_block_lookups += stage2_block_lookups;
                stats.num_stage2_block_reuses += stage2_block_reuses;
                const double stage2_wall_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - stage2_start).count();
                const double stage2_breakdown_ms =
                    stage2_collect_ms + stage2_kernel_ms + stage2_scatter_ms;
                stats.stage2_ms += std::max(stage2_wall_ms, stage2_breakdown_ms);
            }
        }

        if (batch.count > 0) {
            if (enable_address_decode_simd) {
                pc.DecodeAddressBatch(batch.global_idx, batch.count, batch.decoded_addr);
            } else {
                for (uint32_t i = 0; i < batch.count; ++i) {
                    batch.decoded_addr[i] = pc.AddressAt(batch.global_idx[i]);
                }
            }
            sink.OnCandidates(batch);
        }
    }

    if (enable_fine_grained_timing) {
        stats.stage1_ms = stats.stage1_estimate_ms +
                          stats.stage1_mask_ms +
                          stats.stage1_iterate_ms +
                          stats.stage1_classify_ms;
    }
}

}  // namespace index
}  // namespace vdb
