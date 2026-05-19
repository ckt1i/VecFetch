#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "vdb/index/ivf_index.h"
#include "vdb/io/npy_reader.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/index/cluster_prober.h"
#include "vdb/simd/fastscan.h"
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/storage/pack_codes.h"

using namespace vdb;

namespace {

std::string GetStringArg(int argc, char* argv[], const char* name,
                         const std::string& def = "") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return def;
}

int GetIntArg(int argc, char* argv[], const char* name, int def = 0) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    }
    return def;
}

void PrintUsage() {
    std::fprintf(stderr,
                 "Usage: bench_compare_stage_paths "
                 "--index-a <dir> --index-b <dir> --dataset <dir> "
                 "[--query-idx 0] [--nprobe 64] [--probe-rank 0] [--cluster-id <id>]\n");
}

struct PreparedState {
    rabitq::RaBitQEstimator estimator;
    rabitq::PreparedQuery prepared;
    rabitq::ClusterPreparedScratch scratch;
    PreparedState(Dim dim, uint8_t bits) : estimator(dim, bits) {}
};

struct Stage1Masks {
    uint32_t so_mask = 0;
    uint32_t safein_mask = 0;
    uint32_t uncertain_mask = 0;
};

struct Stage2Values {
    float ip_raw = 0.0f;
    float ip_est = 0.0f;
    float est_dist_s2 = 0.0f;
    ResultClass rc_s2 = ResultClass::Uncertain;
};

struct CollectSink : public index::ProbeResultSink {
    std::vector<float> est_dists;
    std::vector<uint32_t> global_indices;
    void OnCandidates(const index::CandidateBatch& batch) override {
        for (uint32_t i = 0; i < batch.count; ++i) {
            est_dists.push_back(batch.est_dist[i]);
            global_indices.push_back(batch.global_idx[i]);
        }
    }
};

VDB_FORCE_INLINE uint32_t LaneMaskForCount(uint32_t count) {
    return (count >= 32u) ? 0xFFFFFFFFu : ((1u << count) - 1u);
}

VDB_FORCE_INLINE uint32_t Stage1SafeInMask(const float* dists,
                                           const float* block_norms,
                                           uint32_t count,
                                           float safein_threshold_base,
                                           float margin_factor) {
    const float threshold_mul = 2.0f * margin_factor;
    uint32_t mask = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const float threshold = safein_threshold_base - threshold_mul * block_norms[i];
        if (dists[i] < threshold) {
            mask |= (1u << i);
        }
    }
    return mask & LaneMaskForCount(count);
}

bool PrepareForCluster(const index::IvfIndex& index,
                       const float* query,
                       uint32_t cluster_id,
                       PreparedState* state,
                       float* margin_factor) {
    if (state == nullptr || margin_factor == nullptr) return false;
    if (index.used_hadamard()) {
        state->estimator.PrepareQueryRotatedInto(
            query,
            index.rotated_centroid(cluster_id),
            &state->prepared,
            &state->scratch,
            nullptr);
    } else {
        state->estimator.PrepareQueryInto(
            query,
            index.centroid(cluster_id),
            index.rotation(),
            &state->prepared,
            &state->scratch,
            nullptr);
    }
    *margin_factor = 2.0f * state->prepared.norm_qc * index.conann().epsilon();
    return true;
}

Stage1Masks ComputeStage1Masks(const index::IvfIndex& index,
                               const rabitq::PreparedClusterQueryView& view,
                               const query::ParsedCluster& pc,
                               uint32_t block_idx,
                               float dynamic_d_k,
                               uint8_t bits,
                               float* out_dists) {
    Stage1Masks out;
    const uint32_t packed_sz = storage::FastScanPackedSize(index.dim());
    const uint32_t base_idx = block_idx * 32;
    const uint32_t count = std::min(32u, pc.num_records - base_idx);
    const uint8_t* block_ptr =
        pc.fastscan_blocks + static_cast<size_t>(block_idx) * pc.fastscan_block_size;
    const float* block_norms =
        reinterpret_cast<const float*>(block_ptr + packed_sz);
    rabitq::RaBitQEstimator est(index.dim(), bits);
    est.EstimateDistanceFastScan(view, block_ptr, block_norms, count, out_dists);
    out.so_mask = simd::FastScanSafeOutMask(
        out_dists, block_norms, count, dynamic_d_k, view.margin_factor);
    const uint32_t lane_valid = LaneMaskForCount(count);
    const uint32_t maybe_in = (~out.so_mask) & lane_valid;
    out.safein_mask = Stage1SafeInMask(
        out_dists, block_norms, count, index.conann().d_k(), view.margin_factor) & maybe_in;
    out.uncertain_mask = maybe_in & ~out.safein_mask;
    return out;
}

Stage2Values ComputeV10Stage2(const index::IvfIndex& index,
                              const rabitq::PreparedQuery& pq,
                              const query::ParsedCluster& pc,
                              uint32_t global_idx,
                              float norm_oc,
                              float margin_s1,
                              float dynamic_d_k,
                              uint8_t bits) {
    Stage2Values v;
    const auto ex = pc.exrabitq_view(global_idx, index.dim());
    v.ip_raw = ex.sign_packed
        ? simd::IPExRaBitQPackedSign(pq.rotated.data(), ex.code_abs, ex.sign, index.dim())
        : simd::IPExRaBitQ(pq.rotated.data(), ex.code_abs, ex.sign, false, index.dim());
    v.ip_est = v.ip_raw * ex.xipnorm;
    v.est_dist_s2 = norm_oc * norm_oc + pq.norm_qc_sq
                  - 2.0f * norm_oc * pq.norm_qc * v.ip_est;
    v.est_dist_s2 = std::max(v.est_dist_s2, 0.0f);
    const float margin_s2 = margin_s1 / static_cast<float>(1u << (bits - 1));
    v.rc_s2 = index.conann().ClassifyAdaptive(v.est_dist_s2, margin_s2, dynamic_d_k);
    return v;
}

Stage2Values ComputeV11Stage2(const index::IvfIndex& index,
                              const rabitq::PreparedQuery& pq,
                              const query::ParsedCluster& pc,
                              uint32_t global_idx,
                              float norm_oc,
                              float margin_s1,
                              float dynamic_d_k,
                              uint8_t bits) {
    Stage2Values v;
    const uint32_t block_id = global_idx / 8u;
    const uint32_t lane = global_idx % 8u;
    const auto block = pc.exrabitq_batch_block_view(block_id);
    alignas(64) float ip_raw_batch[8] = {};
    simd::IPExRaBitQBatchPackedSignCompact(
        pq.rotated.data(),
        block.abs_blocks,
        block.sign_blocks,
        block.valid_count,
        index.dim(),
        pc.exrabitq_dim_block,
        ip_raw_batch);
    v.ip_raw = ip_raw_batch[lane];
    v.ip_est = v.ip_raw * block.xipnorms[lane];
    v.est_dist_s2 = norm_oc * norm_oc + pq.norm_qc_sq
                  - 2.0f * norm_oc * pq.norm_qc * v.ip_est;
    v.est_dist_s2 = std::max(v.est_dist_s2, 0.0f);
    const float margin_s2 = margin_s1 / static_cast<float>(1u << (bits - 1));
    v.rc_s2 = index.conann().ClassifyAdaptive(v.est_dist_s2, margin_s2, dynamic_d_k);
    return v;
}

const char* RcName(ResultClass rc) {
    switch (rc) {
        case ResultClass::SafeIn: return "SafeIn";
        case ResultClass::SafeOut: return "SafeOut";
        default: return "Uncertain";
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string index_a_dir = GetStringArg(argc, argv, "--index-a");
    const std::string index_b_dir = GetStringArg(argc, argv, "--index-b");
    const std::string dataset_dir = GetStringArg(argc, argv, "--dataset");
    const int query_idx = GetIntArg(argc, argv, "--query-idx", 0);
    const int nprobe = GetIntArg(argc, argv, "--nprobe", 64);
    const int probe_rank = GetIntArg(argc, argv, "--probe-rank", 0);
    const int cluster_id_arg = GetIntArg(argc, argv, "--cluster-id", -1);
    const int simulate_crc = GetIntArg(argc, argv, "--simulate-crc", 0);
    if (index_a_dir.empty() || index_b_dir.empty() || dataset_dir.empty()) {
        PrintUsage();
        return 1;
    }

    auto qry_or = io::LoadNpyFloat32(dataset_dir + "/query_embeddings.npy");
    if (!qry_or.ok()) {
        std::fprintf(stderr, "Failed to load query_embeddings: %s\n",
                     qry_or.status().ToString().c_str());
        return 1;
    }
    const auto& queries = qry_or.value();
    if (query_idx < 0 || static_cast<uint32_t>(query_idx) >= queries.rows) {
        std::fprintf(stderr, "query_idx out of range\n");
        return 1;
    }

    index::IvfIndex index_a;
    index::IvfIndex index_b;
    Status s = index_a.Open(index_a_dir, false);
    if (!s.ok()) {
        std::fprintf(stderr, "Open index_a failed: %s\n", s.ToString().c_str());
        return 1;
    }
    s = index_b.Open(index_b_dir, false);
    if (!s.ok()) {
        std::fprintf(stderr, "Open index_b failed: %s\n", s.ToString().c_str());
        return 1;
    }
    if (index_a.dim() != index_b.dim() || index_a.dim() != queries.cols) {
        std::fprintf(stderr, "dimension mismatch\n");
        return 1;
    }

    s = index_a.segment().PreloadAllClusters();
    if (!s.ok()) {
        std::fprintf(stderr, "Preload index_a failed: %s\n", s.ToString().c_str());
        return 1;
    }
    s = index_b.segment().PreloadAllClusters();
    if (!s.ok()) {
        std::fprintf(stderr, "Preload index_b failed: %s\n", s.ToString().c_str());
        return 1;
    }

    const float* query = queries.data.data() + static_cast<size_t>(query_idx) * queries.cols;
    const auto clusters_a = index_a.FindNearestClusters(query, nprobe);
    const auto clusters_b = index_b.FindNearestClusters(query, nprobe);
    const uint8_t bits_a = index_a.segment().rabitq_config().bits;
    const uint8_t bits_b = index_b.segment().rabitq_config().bits;
    std::printf("query_idx=%d nprobe=%d\n", query_idx, nprobe);
    std::printf("top clusters a[0]=%u b[0]=%u\n",
                clusters_a.empty() ? 0u : clusters_a[0],
                clusters_b.empty() ? 0u : clusters_b[0]);
    if (simulate_crc != 0) {
        index::ClusterProber prober_a(index_a.conann(), index_a.dim(), index_a.segment().rabitq_config().bits);
        index::ClusterProber prober_b(index_b.conann(), index_b.dim(), index_b.segment().rabitq_config().bits);
        std::vector<std::pair<float, uint32_t>> heap_a;
        std::vector<std::pair<float, uint32_t>> heap_b;
        heap_a.reserve(10);
        heap_b.reserve(10);
        for (int pr = 0; pr < nprobe; ++pr) {
            const uint32_t cid_a = clusters_a[static_cast<size_t>(pr)];
            const uint32_t cid_b = clusters_b[static_cast<size_t>(pr)];
            if (cid_a != cid_b) {
                std::printf("PROBE_LIST_DIFF rank=%d cid_a=%u cid_b=%u\n", pr, cid_a, cid_b);
                return 0;
            }
            const auto* cpa = index_a.segment().GetResidentParsedCluster(cid_a);
            const auto* cpb = index_b.segment().GetResidentParsedCluster(cid_b);
            PreparedState psa(index_a.dim(), bits_a);
            PreparedState psb(index_b.dim(), bits_b);
            float mf_a = 0.0f;
            float mf_b = 0.0f;
            PrepareForCluster(index_a, query, cid_a, &psa, &mf_a);
            PrepareForCluster(index_b, query, cid_b, &psb, &mf_b);
            rabitq::PreparedClusterQueryView va{&psa.prepared, &psa.scratch, mf_a};
            rabitq::PreparedClusterQueryView vb{&psb.prepared, &psb.scratch, mf_b};
            const float dyn_a = (heap_a.size() >= 10) ? std::max(heap_a.front().first, index_a.conann().d_k())
                                                      : std::numeric_limits<float>::infinity();
            const float dyn_b = (heap_b.size() >= 10) ? std::max(heap_b.front().first, index_b.conann().d_k())
                                                      : std::numeric_limits<float>::infinity();
            CollectSink sink_a;
            CollectSink sink_b;
            index::ProbeStats stats_a;
            index::ProbeStats stats_b;
            prober_a.Probe(*cpa, va, mf_a, dyn_a,
                           false, false, false, false, sink_a, stats_a);
            prober_b.Probe(*cpb, vb, mf_b, dyn_b,
                           false, false, false, false, sink_b, stats_b);
            if (sink_a.est_dists.size() != sink_b.est_dists.size()) {
                std::printf("CRC_PROBE_DIFF rank=%d cid=%u dyn(a,b)=%.9f,%.9f emitted_count(a,b)=%zu,%zu\n",
                            pr, cid_a, dyn_a, dyn_b, sink_a.est_dists.size(), sink_b.est_dists.size());
                return 0;
            }
            for (size_t i = 0; i < sink_a.est_dists.size(); ++i) {
                if (std::abs(sink_a.est_dists[i] - sink_b.est_dists[i]) > 1e-5f ||
                    sink_a.global_indices[i] != sink_b.global_indices[i]) {
                    std::printf("CRC_EST_DIFF rank=%d cid=%u pos=%zu dyn(a,b)=%.9f,%.9f est(a,b)=%.9f,%.9f idx(a,b)=%u,%u\n",
                                pr, cid_a, i, dyn_a, dyn_b,
                                sink_a.est_dists[i], sink_b.est_dists[i],
                                sink_a.global_indices[i], sink_b.global_indices[i]);
                    return 0;
                }
            }
            for (float est : sink_a.est_dists) {
                if (heap_a.size() < 10) {
                    heap_a.push_back({est, 0u});
                    std::push_heap(heap_a.begin(), heap_a.end());
                } else if (est < heap_a.front().first) {
                    std::pop_heap(heap_a.begin(), heap_a.end());
                    heap_a.back() = {est, 0u};
                    std::push_heap(heap_a.begin(), heap_a.end());
                }
            }
            for (float est : sink_b.est_dists) {
                if (heap_b.size() < 10) {
                    heap_b.push_back({est, 0u});
                    std::push_heap(heap_b.begin(), heap_b.end());
                } else if (est < heap_b.front().first) {
                    std::pop_heap(heap_b.begin(), heap_b.end());
                    heap_b.back() = {est, 0u};
                    std::push_heap(heap_b.begin(), heap_b.end());
                }
            }
            if (heap_a.size() != heap_b.size()) {
                std::printf("CRC_HEAP_SIZE_DIFF rank=%d size(a,b)=%zu,%zu\n",
                            pr, heap_a.size(), heap_b.size());
                return 0;
            }
            auto sorted_a = heap_a;
            auto sorted_b = heap_b;
            std::sort(sorted_a.begin(), sorted_a.end());
            std::sort(sorted_b.begin(), sorted_b.end());
            for (size_t i = 0; i < sorted_a.size(); ++i) {
                if (std::abs(sorted_a[i].first - sorted_b[i].first) > 1e-5f) {
                    std::printf("CRC_HEAP_DIFF rank=%d cid=%u heap_pos=%zu val(a,b)=%.9f,%.9f\n",
                                pr, cid_a, i, sorted_a[i].first, sorted_b[i].first);
                    return 0;
                }
            }
        }
        std::printf("NO_CRC_PROBE_DIFF_FOUND query_idx=%d nprobe=%d\n", query_idx, nprobe);
        return 0;
    }

    const uint32_t cluster_id = cluster_id_arg >= 0
        ? static_cast<uint32_t>(cluster_id_arg)
        : clusters_a.at(static_cast<size_t>(probe_rank));
    std::printf("cluster_id=%u probe_rank=%d\n", cluster_id, probe_rank);

    const auto* pc_a = index_a.segment().GetResidentParsedCluster(cluster_id);
    const auto* pc_b = index_b.segment().GetResidentParsedCluster(cluster_id);
    if (pc_a == nullptr || pc_b == nullptr) {
        std::fprintf(stderr, "resident cluster missing\n");
        return 1;
    }
    std::printf("storage_versions: a=%u b=%u records: a=%u b=%u\n",
                pc_a->exrabitq_storage_version,
                pc_b->exrabitq_storage_version,
                pc_a->num_records, pc_b->num_records);

    PreparedState state_a(index_a.dim(), index_a.segment().rabitq_config().bits);
    PreparedState state_b(index_b.dim(), index_b.segment().rabitq_config().bits);
    float margin_factor_a = 0.0f;
    float margin_factor_b = 0.0f;
    PrepareForCluster(index_a, query, cluster_id, &state_a, &margin_factor_a);
    PrepareForCluster(index_b, query, cluster_id, &state_b, &margin_factor_b);

    std::printf("prepared: norm_qc a=%.9f b=%.9f | norm_qc_sq a=%.9f b=%.9f | sum_q a=%.9f b=%.9f | margin_factor a=%.9f b=%.9f\n",
                state_a.prepared.norm_qc, state_b.prepared.norm_qc,
                state_a.prepared.norm_qc_sq, state_b.prepared.norm_qc_sq,
                state_a.prepared.sum_q, state_b.prepared.sum_q,
                margin_factor_a, margin_factor_b);

    rabitq::PreparedClusterQueryView view_a{&state_a.prepared, &state_a.scratch, margin_factor_a};
    rabitq::PreparedClusterQueryView view_b{&state_b.prepared, &state_b.scratch, margin_factor_b};
    const float dynamic_d_k = std::max(index_a.conann().d_k(), index_b.conann().d_k());

    alignas(64) float dists_a[32];
    alignas(64) float dists_b[32];
    bool any_diff = false;
    const uint32_t num_blocks = std::min(pc_a->num_fastscan_blocks, pc_b->num_fastscan_blocks);
    for (uint32_t b = 0; b < num_blocks; ++b) {
        const Stage1Masks m_a = ComputeStage1Masks(index_a, view_a, *pc_a, b, dynamic_d_k, bits_a, dists_a);
        const Stage1Masks m_b = ComputeStage1Masks(index_b, view_b, *pc_b, b, dynamic_d_k, bits_b, dists_b);
        const uint32_t base_idx = b * 32;
        const uint32_t count = std::min(32u, pc_a->num_records - base_idx);
        if (m_a.so_mask != m_b.so_mask ||
            m_a.safein_mask != m_b.safein_mask ||
            m_a.uncertain_mask != m_b.uncertain_mask) {
            any_diff = true;
            std::printf("STAGE1_MASK_DIFF block=%u base_idx=%u count=%u so(a,b)=0x%08x,0x%08x safein(a,b)=0x%08x,0x%08x uncertain(a,b)=0x%08x,0x%08x\n",
                        b, base_idx, count, m_a.so_mask, m_b.so_mask,
                        m_a.safein_mask, m_b.safein_mask,
                        m_a.uncertain_mask, m_b.uncertain_mask);
            for (uint32_t j = 0; j < count; ++j) {
                const float* norms_a = reinterpret_cast<const float*>(
                    pc_a->fastscan_blocks + static_cast<size_t>(b) * pc_a->fastscan_block_size +
                    (pc_a->fastscan_block_size - 32 * sizeof(float)));
                const float* norms_b = reinterpret_cast<const float*>(
                    pc_b->fastscan_blocks + static_cast<size_t>(b) * pc_b->fastscan_block_size +
                    (pc_b->fastscan_block_size - 32 * sizeof(float)));
                const uint32_t bit = 1u << j;
                if (((m_a.so_mask ^ m_b.so_mask) & bit) ||
                    ((m_a.safein_mask ^ m_b.safein_mask) & bit) ||
                    ((m_a.uncertain_mask ^ m_b.uncertain_mask) & bit)) {
                    std::printf(" lane=%u global_idx=%u dist(a,b)=%.9f,%.9f norm(a,b)=%.9f,%.9f flags so=%u/%u safein=%u/%u uncertain=%u/%u\n",
                                j, base_idx + j, dists_a[j], dists_b[j],
                                norms_a[j], norms_b[j],
                                (m_a.so_mask & bit) ? 1u : 0u, (m_b.so_mask & bit) ? 1u : 0u,
                                (m_a.safein_mask & bit) ? 1u : 0u, (m_b.safein_mask & bit) ? 1u : 0u,
                                (m_a.uncertain_mask & bit) ? 1u : 0u, (m_b.uncertain_mask & bit) ? 1u : 0u);
                }
            }
            break;
        }

        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t bit = 1u << j;
            if ((m_a.uncertain_mask & bit) == 0 && (m_b.uncertain_mask & bit) == 0) continue;
            const uint32_t global_idx = base_idx + j;
            const float norm_oc_a = pc_a->norm_oc(b, j);
            const float norm_oc_b = pc_b->norm_oc(b, j);
            const float margin_s1_a = margin_factor_a * norm_oc_a;
            const float margin_s1_b = margin_factor_b * norm_oc_b;
            const Stage2Values s2_a = ComputeV10Stage2(
                index_a, state_a.prepared, *pc_a, global_idx, norm_oc_a, margin_s1_a, dynamic_d_k, bits_a);
            const Stage2Values s2_b = ComputeV11Stage2(
                index_b, state_b.prepared, *pc_b, global_idx, norm_oc_b, margin_s1_b, dynamic_d_k, bits_b);
            if (std::abs(s2_a.ip_raw - s2_b.ip_raw) > 1e-4f ||
                std::abs(s2_a.ip_est - s2_b.ip_est) > 1e-4f ||
                std::abs(s2_a.est_dist_s2 - s2_b.est_dist_s2) > 1e-4f ||
                s2_a.rc_s2 != s2_b.rc_s2) {
                any_diff = true;
                std::printf("STAGE2_DIFF block=%u lane=%u global_idx=%u\n", b, j, global_idx);
                std::printf("  ip_raw a=%.9f b=%.9f\n", s2_a.ip_raw, s2_b.ip_raw);
                std::printf("  ip_est a=%.9f b=%.9f\n", s2_a.ip_est, s2_b.ip_est);
                std::printf("  est_dist_s2 a=%.9f b=%.9f\n", s2_a.est_dist_s2, s2_b.est_dist_s2);
                std::printf("  rc_s2 a=%s b=%s\n", RcName(s2_a.rc_s2), RcName(s2_b.rc_s2));
                std::printf("  norm_oc a=%.9f b=%.9f margin_s1 a=%.9f b=%.9f\n",
                            norm_oc_a, norm_oc_b, margin_s1_a, margin_s1_b);
                break;
            }
        }
        if (any_diff) break;
    }

    if (!any_diff) {
        std::printf("NO_STAGE_DIFF_FOUND_FOR_SELECTED_QUERY_CLUSTER\n");
    }
    return 0;
}
