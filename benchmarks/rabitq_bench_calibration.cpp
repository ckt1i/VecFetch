#include "rabitq_bench_calibration.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <random>

#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/simd/distance_l2.h"

namespace vdb {
namespace bench {

namespace {

struct RowLocation {
    uint32_t cluster_id = 0;
    uint32_t local_idx = 0;
};

float ConannPercentile(std::vector<float> values, float percentile) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    uint32_t k = static_cast<uint32_t>(
        std::floor((1.0f - percentile) * static_cast<float>(values.size())));
    k = std::min<uint32_t>(k, static_cast<uint32_t>(values.size() - 1));
    return values[k];
}

float UpperPercentile(std::vector<float> values, float percentile) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    const float findex = percentile * static_cast<float>(values.size() - 1);
    const uint32_t k = static_cast<uint32_t>(
        std::min(findex, static_cast<float>(values.size() - 1)));
    return values[k];
}

std::vector<uint32_t> SampleIndices(uint32_t total, uint32_t num_samples,
                                    uint64_t seed) {
    num_samples = std::min(num_samples, total);
    std::vector<uint32_t> ids(total);
    std::iota(ids.begin(), ids.end(), 0u);
    std::mt19937_64 rng(seed == 0 ? std::random_device{}() : seed);
    std::shuffle(ids.begin(), ids.end(), rng);
    ids.resize(num_samples);
    return ids;
}

std::vector<uint32_t> SampleIndicesWithReplacement(uint32_t total,
                                                   uint32_t num_samples,
                                                   uint64_t seed) {
    std::vector<uint32_t> ids;
    if (total == 0 || num_samples == 0) return ids;
    ids.reserve(num_samples);
    std::mt19937_64 rng(seed == 0 ? std::random_device{}() : seed);
    std::uniform_int_distribution<uint32_t> dist(0, total - 1);
    for (uint32_t i = 0; i < num_samples; ++i) {
        ids.push_back(dist(rng));
    }
    return ids;
}

}  // namespace

StatusOr<std::vector<uint32_t>> LoadAssignments(const std::string& path,
                                                uint32_t expected_rows) {
    auto a_or = io::LoadIvecs(path);
    if (!a_or.ok()) return a_or.status();
    const auto& a = a_or.value();
    if (a.rows != expected_rows) {
        return Status::InvalidArgument(
            "Assignment rows mismatch: expected " + std::to_string(expected_rows) +
            ", got " + std::to_string(a.rows));
    }
    std::vector<uint32_t> out(expected_rows);
    for (uint32_t i = 0; i < expected_rows; ++i) {
        out[i] = static_cast<uint32_t>(a.data[static_cast<size_t>(i) * a.cols]);
    }
    return out;
}

void BuildClusterMembers(const std::vector<uint32_t>& assignments,
                         uint32_t nlist,
                         std::vector<std::vector<uint32_t>>* cluster_members) {
    cluster_members->assign(nlist, {});
    for (uint32_t row = 0; row < assignments.size(); ++row) {
        const uint32_t cid = assignments[row];
        if (cid < nlist) (*cluster_members)[cid].push_back(row);
    }
}

bool ExtractPayloadId(const std::vector<Datum>& payload, int64_t* id) {
    if (payload.empty() || id == nullptr) return false;
    if (payload[0].dtype != DType::INT64) return false;
    *id = payload[0].fixed.i64;
    return true;
}

Status RecoverClusterMembersFromIndex(
    index::IvfIndex& index,
    const std::unordered_map<int64_t, uint32_t>& image_id_to_row,
    std::vector<std::vector<uint32_t>>* cluster_members) {
    if (cluster_members == nullptr) {
        return Status::InvalidArgument("cluster_members output is null");
    }
    if (image_id_to_row.empty()) {
        return Status::InvalidArgument("image_id_to_row is empty");
    }

    cluster_members->assign(index.nlist(), {});
    std::vector<float> scratch_vec(index.dim());
    std::vector<Datum> payload;

    for (uint32_t cid = 0; cid < index.nlist(); ++cid) {
        VDB_RETURN_IF_ERROR(index.segment().EnsureClusterLoaded(cid));
        const uint32_t count = index.segment().GetNumRecords(cid);
        auto& members = (*cluster_members)[cid];
        members.reserve(count);
        for (uint32_t ridx = 0; ridx < count; ++ridx) {
            const AddressEntry addr = index.segment().GetAddress(cid, ridx);
            payload.clear();
            VDB_RETURN_IF_ERROR(
                index.segment().ReadRecord(addr, scratch_vec.data(), payload));
            int64_t image_id = -1;
            if (!ExtractPayloadId(payload, &image_id)) {
                return Status::InvalidArgument(
                    "Index payload is missing INT64 id required for false stats");
            }
            auto it = image_id_to_row.find(image_id);
            if (it == image_id_to_row.end()) {
                return Status::InvalidArgument(
                    "Index payload id not found in image_ids map: " +
                    std::to_string(image_id));
            }
            members.push_back(it->second);
        }
    }
    return Status::OK();
}

void EncodeAllCodes(const std::vector<float>& base_data,
                    uint32_t n,
                    Dim dim,
                    const std::vector<std::vector<uint32_t>>& cluster_members,
                    const std::vector<float>& centroids,
                    const rabitq::RotationMatrix& rotation,
                    uint8_t bits,
                    const std::vector<uint32_t>* cluster_subset,
                    std::vector<std::vector<rabitq::RaBitQCode>>* all_codes) {
    (void)n;
    rabitq::RaBitQEncoder encoder(dim, rotation, bits);
    all_codes->assign(cluster_members.size(), {});
    const bool use_subset = cluster_subset != nullptr && !cluster_subset->empty();
    if (use_subset) {
        for (uint32_t cid : *cluster_subset) {
            if (cid >= cluster_members.size()) continue;
            const auto& members = cluster_members[cid];
            auto& codes = (*all_codes)[cid];
            if (members.empty()) continue;
            std::vector<float> member_vecs(static_cast<size_t>(members.size()) * dim);
            for (uint32_t m = 0; m < members.size(); ++m) {
                std::memcpy(member_vecs.data() + static_cast<size_t>(m) * dim,
                            base_data.data() + static_cast<size_t>(members[m]) * dim,
                            static_cast<size_t>(dim) * sizeof(float));
            }
            codes = encoder.EncodeBatch(member_vecs.data(),
                                        static_cast<uint32_t>(members.size()),
                                        centroids.data() + static_cast<size_t>(cid) * dim);
        }
        return;
    }
    for (uint32_t cid = 0; cid < cluster_members.size(); ++cid) {
        const auto& members = cluster_members[cid];
        auto& codes = (*all_codes)[cid];
        if (members.empty()) continue;
        std::vector<float> member_vecs(static_cast<size_t>(members.size()) * dim);
        for (uint32_t m = 0; m < members.size(); ++m) {
            std::memcpy(member_vecs.data() + static_cast<size_t>(m) * dim,
                        base_data.data() + static_cast<size_t>(members[m]) * dim,
                        static_cast<size_t>(dim) * sizeof(float));
        }
        codes = encoder.EncodeBatch(member_vecs.data(),
                                    static_cast<uint32_t>(members.size()),
                                    centroids.data() + static_cast<size_t>(cid) * dim);
    }
}

std::vector<float> GenerateExactSafeInDkSamples(
    const float* queries,
    uint32_t q,
    const float* database,
    uint32_t n,
    Dim dim,
    uint32_t top_k,
    uint32_t sample_queries,
    uint64_t seed,
    SafeInDkSamplingMode sampling_mode) {
    std::vector<float> query_kths;
    if (queries == nullptr || database == nullptr || q == 0 || n == 0 ||
        top_k == 0 || sample_queries == 0) {
        return query_kths;
    }

    const auto sampled = (sampling_mode == SafeInDkSamplingMode::WithReplacement)
        ? SampleIndicesWithReplacement(q, sample_queries, seed)
        : SampleIndices(q, sample_queries, seed);
    query_kths.reserve(sampled.size());

    std::vector<float> dists(n);
    for (uint32_t qi : sampled) {
        const float* query = queries + static_cast<size_t>(qi) * dim;
        for (uint32_t row = 0; row < n; ++row) {
            dists[row] = simd::L2Sqr(
                query, database + static_cast<size_t>(row) * dim, dim);
        }
        const uint32_t kth = std::min<uint32_t>(top_k - 1, n - 1);
        std::nth_element(dists.begin(), dists.begin() + kth, dists.end());
        query_kths.push_back(dists[kth]);
    }
    return query_kths;
}

std::vector<float> GenerateRabitqSafeInDkSamples(
    const float* queries,
    uint32_t q,
    Dim dim,
    uint32_t top_k,
    uint32_t sample_queries,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const std::vector<std::vector<rabitq::RaBitQCode>>& all_codes,
    const std::vector<float>& centroids,
    const rabitq::RotationMatrix& rotation,
    uint8_t bits,
    uint64_t seed,
    SafeInDkSamplingMode sampling_mode,
    const index::IvfIndex* index,
    index::SafeInDkSearchScope search_scope,
    uint32_t nprobe) {
    rabitq::RaBitQEstimator estimator(dim, bits);
    rabitq::PreparedQuery pq;
    rabitq::ClusterPreparedScratch scratch;
    const auto sampled = (sampling_mode == SafeInDkSamplingMode::WithReplacement)
        ? SampleIndicesWithReplacement(q, sample_queries, seed)
        : SampleIndices(q, sample_queries, seed);
    std::vector<float> query_kths;
    query_kths.reserve(sampled.size());

    for (uint32_t qi : sampled) {
        const float* query = queries + static_cast<size_t>(qi) * dim;
        std::vector<ClusterID> probed_clusters;
        if (search_scope == index::SafeInDkSearchScope::NProbe) {
            if (index == nullptr) continue;
            probed_clusters = index->FindNearestClusters(query, nprobe);
        }

        std::vector<float> dists;
        dists.reserve(100000);
        if (search_scope == index::SafeInDkSearchScope::NProbe) {
            for (ClusterID cid : probed_clusters) {
                estimator.PrepareQueryInto(
                    query, centroids.data() + static_cast<size_t>(cid) * dim,
                    rotation, &pq, &scratch);
                const auto& codes = all_codes[cid];
                for (const auto& code : codes) {
                    dists.push_back(estimator.EstimateDistanceMultiBit(pq, code));
                }
            }
        } else {
            for (uint32_t cid = 0; cid < cluster_members.size(); ++cid) {
                estimator.PrepareQueryInto(
                    query, centroids.data() + static_cast<size_t>(cid) * dim,
                    rotation, &pq, &scratch);
                const auto& codes = all_codes[cid];
                for (const auto& code : codes) {
                    dists.push_back(estimator.EstimateDistanceMultiBit(pq, code));
                }
            }
        }
        if (dists.size() < top_k) continue;
        const uint32_t kth =
            std::min<uint32_t>(top_k - 1, static_cast<uint32_t>(dists.size() - 1));
        std::nth_element(dists.begin(), dists.begin() + kth, dists.end());
        query_kths.push_back(dists[kth]);
    }
    return query_kths;
}

float SelectSafeInDkFromSamples(const std::vector<float>& samples,
                                float percentile) {
    return ConannPercentile(samples, percentile);
}

float CalibrateExactSafeInDk(
    const float* queries,
    uint32_t q,
    const float* database,
    uint32_t n,
    Dim dim,
    uint32_t top_k,
    uint32_t sample_queries,
    float percentile,
    uint64_t seed) {
    auto samples = GenerateExactSafeInDkSamples(
        queries, q, database, n, dim, top_k, sample_queries, seed,
        SafeInDkSamplingMode::Unique);
    return SelectSafeInDkFromSamples(samples, percentile);
}

float CalibrateRabitqSafeInDk(
    const float* queries,
    uint32_t q,
    Dim dim,
    uint32_t top_k,
    uint32_t sample_queries,
    float percentile,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const std::vector<std::vector<rabitq::RaBitQCode>>& all_codes,
    const std::vector<float>& centroids,
    const rabitq::RotationMatrix& rotation,
    uint8_t bits,
    uint64_t seed,
    const index::IvfIndex* index,
    index::SafeInDkSearchScope search_scope,
    uint32_t nprobe) {
    auto samples = GenerateRabitqSafeInDkSamples(
        queries, q, dim, top_k, sample_queries, cluster_members, all_codes,
        centroids, rotation, bits, seed, SafeInDkSamplingMode::Unique, index,
        search_scope, nprobe);
    return SelectSafeInDkFromSamples(samples, percentile);
}

const char* SafeInThresholdSourceName(SafeInThresholdSource source) {
    switch (source) {
        case SafeInThresholdSource::ExactL2:
            return "exact_l2";
        case SafeInThresholdSource::RabitqS2Kth:
            return "rabitq_s2_kth";
    }
    return "unknown";
}

float CalibrateSplitEpsilon(
    const std::vector<std::vector<rabitq::RaBitQCode>>& all_codes,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const float* vectors,
    const std::vector<float>& centroids,
    const rabitq::RotationMatrix& rotation,
    Dim dim,
    uint32_t max_samples_per_cluster,
    float percentile,
    uint64_t seed,
    float d_k,
    uint8_t bits,
    const std::vector<uint32_t>* cluster_subset,
    EpsilonSamplingMode sampling_mode,
    EpsilonCalibrationStats* stats) {
    const float lo = 0.1f * d_k;
    const float hi = 10.0f * d_k;
    std::vector<float> errors;
    rabitq::RaBitQEstimator estimator(dim, bits);
    std::vector<uint32_t> sample_ids;
    const bool use_subset = cluster_subset != nullptr && !cluster_subset->empty();
    if (stats != nullptr) {
        *stats = EpsilonCalibrationStats{};
        stats->sampling_mode = sampling_mode;
        stats->requested_samples = max_samples_per_cluster;
    }
    auto record_error = [&](const rabitq::PreparedQuery& pq,
                            const rabitq::RaBitQCode& code,
                            const float* query,
                            const float* target) -> bool {
        const float true_dist = simd::L2Sqr(query, target, dim);
        if (true_dist < lo || true_dist > hi) return false;
        const float est_dist = (bits > 1)
            ? estimator.EstimateDistanceMultiBit(pq, code)
            : estimator.EstimateDistanceAccurate(pq, code);
        const float denom = 2.0f * code.norm * pq.norm_qc;
        if (denom <= 0.0f) return false;
        errors.push_back(std::abs(est_dist - true_dist) / denom);
        return true;
    };
    if (sampling_mode == EpsilonSamplingMode::GlobalPair) {
        std::vector<RowLocation> rows;
        rows.reserve(cluster_members.size());
        auto add_cluster_rows = [&](uint32_t cid) {
            if (cid >= cluster_members.size() || cid >= all_codes.size()) return;
            const auto& members = cluster_members[cid];
            if (members.size() < 2) return;
            for (uint32_t local_idx = 0;
                 local_idx < static_cast<uint32_t>(members.size());
                 ++local_idx) {
                rows.push_back(RowLocation{cid, local_idx});
            }
        };
        if (use_subset) {
            for (uint32_t cid : *cluster_subset) add_cluster_rows(cid);
        } else {
            for (uint32_t cid = 0; cid < cluster_members.size(); ++cid) {
                add_cluster_rows(cid);
            }
        }
        if (rows.empty() || max_samples_per_cluster == 0) {
            if (stats != nullptr) {
                stats->valid_error_count = 0;
                stats->attempted_pairs = 0;
            }
            return 0.0f;
        }
        errors.reserve(max_samples_per_cluster);
        std::mt19937_64 rng(seed == 0 ? std::random_device{}() : seed);
        std::uniform_int_distribution<size_t> row_dist(0, rows.size() - 1);
        const uint32_t max_attempts =
            std::max<uint32_t>(max_samples_per_cluster + 1024u,
                               max_samples_per_cluster * 20u);
        uint32_t attempts = 0;
        rabitq::PreparedQuery pq;
        rabitq::ClusterPreparedScratch scratch;
        while (errors.size() < max_samples_per_cluster &&
               attempts < max_attempts) {
            ++attempts;
            const RowLocation loc = rows[row_dist(rng)];
            const auto& members = cluster_members[loc.cluster_id];
            const auto& codes = all_codes[loc.cluster_id];
            if (members.size() < 2 || loc.local_idx >= members.size() ||
                loc.local_idx >= codes.size()) {
                continue;
            }
            std::uniform_int_distribution<uint32_t> target_dist(
                0, static_cast<uint32_t>(members.size() - 2));
            uint32_t target_local = target_dist(rng);
            if (target_local >= loc.local_idx) ++target_local;
            if (target_local >= codes.size()) continue;
            const float* centroid =
                centroids.data() + static_cast<size_t>(loc.cluster_id) * dim;
            const float* query =
                vectors + static_cast<size_t>(members[loc.local_idx]) * dim;
            const float* target =
                vectors + static_cast<size_t>(members[target_local]) * dim;
            estimator.PrepareQueryInto(query, centroid, rotation, &pq, &scratch);
            record_error(pq, codes[target_local], query, target);
        }
        if (stats != nullptr) {
            stats->valid_error_count = static_cast<uint32_t>(errors.size());
            stats->attempted_pairs = attempts;
        }
        return UpperPercentile(std::move(errors), percentile);
    }
    const auto run_cluster = [&](uint32_t cid) {
        const auto& members = cluster_members[cid];
        const auto& codes = all_codes[cid];
        const uint32_t n_members = static_cast<uint32_t>(members.size());
        if (n_members < 2) return;
        uint32_t n_queries = max_samples_per_cluster;
        if (n_members < 2 * max_samples_per_cluster) {
            n_queries = std::max(n_members / 2, 1u);
        }
        n_queries = std::min(n_queries, n_members);
        sample_ids.resize(n_members);
        std::iota(sample_ids.begin(), sample_ids.end(), 0u);
        std::mt19937 rng(static_cast<uint32_t>(seed + cid));
        std::shuffle(sample_ids.begin(), sample_ids.end(), rng);
        const float* centroid = centroids.data() + static_cast<size_t>(cid) * dim;
        rabitq::PreparedQuery pq;
        rabitq::ClusterPreparedScratch scratch;
        for (uint32_t qi = 0; qi < n_queries; ++qi) {
            const uint32_t local_q = sample_ids[qi];
            const float* query = vectors + static_cast<size_t>(members[local_q]) * dim;
            estimator.PrepareQueryInto(query, centroid, rotation, &pq, &scratch);
            for (uint32_t j = 0; j < n_members; ++j) {
                if (j == local_q) continue;
                record_error(pq, codes[j], query,
                             vectors + static_cast<size_t>(members[j]) * dim);
            }
        }
    };
    if (use_subset) {
        for (uint32_t cid : *cluster_subset) {
            if (cid >= cluster_members.size()) continue;
            run_cluster(cid);
        }
    } else {
        for (uint32_t cid = 0; cid < cluster_members.size(); ++cid) {
            run_cluster(cid);
        }
    }
    if (stats != nullptr) {
        stats->valid_error_count = static_cast<uint32_t>(errors.size());
        stats->attempted_pairs = static_cast<uint32_t>(errors.size());
    }
    return UpperPercentile(std::move(errors), percentile);
}

const char* EpsilonSamplingModeName(EpsilonSamplingMode mode) {
    switch (mode) {
        case EpsilonSamplingMode::LegacyPerCluster:
            return "legacy_per_cluster";
        case EpsilonSamplingMode::GlobalPair:
            return "global_pair";
    }
    return "legacy_per_cluster";
}

StatusOr<EpsilonSamplingMode> ParseEpsilonSamplingModeArg(
    const std::string& value) {
    if (value == "legacy_per_cluster") {
        return EpsilonSamplingMode::LegacyPerCluster;
    }
    if (value == "global_pair") {
        return EpsilonSamplingMode::GlobalPair;
    }
    return Status::InvalidArgument(
        "Invalid epsilon_sampling_mode: " + value +
        " (expected legacy_per_cluster or global_pair)");
}

StatusOr<index::SafeInDkSearchScope> ParseSafeInDkSearchScopeArg(
    const std::string& value) {
    if (value == "full") {
        return index::SafeInDkSearchScope::FullDatabase;
    }
    if (value == "nprobe") {
        return index::SafeInDkSearchScope::NProbe;
    }
    return Status::InvalidArgument(
        "Invalid safein_dk_search_scope: " + value +
        " (expected full or nprobe)");
}

StatusOr<SafeInDkSamplingMode> ParseSafeInDkSamplingModeArg(
    const std::string& value) {
    if (value == "unique") return SafeInDkSamplingMode::Unique;
    if (value == "with_replacement") {
        return SafeInDkSamplingMode::WithReplacement;
    }
    return Status::InvalidArgument(
        "Invalid safein_dk_sampling_mode: " + value +
        " (expected unique or with_replacement)");
}

}  // namespace bench
}  // namespace vdb
