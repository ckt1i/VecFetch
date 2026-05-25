/// bench_phase_a_grid.cpp — Offline Phase A grid replay in Stage2 RabitQ space.
///
/// Usage:
///   bench_phase_a_grid --dataset <dir> --index-dir <dir>
///       [--base path] [--query path] [--centroids path] [--assignments path]
///       [--gt-file path] [--queries 200] [--dk-calibration-queries 500] [--topk 10]
///       [--nprobe 64] [--search-scope full|nprobe]
///       [--dk-percentiles 0.99,0.95,0.90]
///       [--eps-percentiles 0.99,0.95,0.90]
///       [--bits 4] [--outdir ./phase_a_grid]

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/npy_reader.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/simd/distance_l2.h"

using namespace vdb;
using namespace vdb::index;
using namespace vdb::rabitq;

namespace fs = std::filesystem;

namespace {

struct QueryTruth {
    std::unordered_set<uint32_t> row_ids;
    std::unordered_set<int64_t> external_ids;
};

struct CandidateEval {
    float est_dist_s2 = 0.0f;
    float margin_unit = 0.0f;  // 2 * norm_qc * norm_oc
    float normalized_err_s2 = 0.0f;
    bool is_true_topk = false;
};

struct QueryEval {
    uint32_t query_id = 0;
    uint64_t num_candidates = 0;
    uint64_t true_topk_in_scope = 0;
    float exact_kth_l2 = 0.0f;
    float rabitq_kth = 0.0f;
    std::vector<CandidateEval> candidates;
};

struct ComboSummary {
    double dk_percentile = 0.0;
    double eps_percentile = 0.0;
    double dk_value = 0.0;
    double eps_value = 0.0;
    uint64_t safein = 0;
    uint64_t false_safein = 0;
    uint64_t queries_ge1_safein = 0;
    uint64_t queries_ge2_safein = 0;
    uint64_t count_est_lt_dk = 0;
    uint64_t count_margin_blocked = 0;
};

static std::string GetArg(int argc, char* argv[], const char* name,
                          const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return def;
}

static int GetIntArg(int argc, char* argv[], const char* name, int def) {
    const auto s = GetArg(argc, argv, name, "");
    return s.empty() ? def : std::atoi(s.c_str());
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static std::string DefaultBasePath(const std::string& dataset_dir) {
    if (fs::exists(dataset_dir + "/image_embeddings.npy")) {
        return dataset_dir + "/image_embeddings.npy";
    }
    return dataset_dir + "/image_embeddings.fvecs";
}

static std::string DefaultQueryPath(const std::string& dataset_dir) {
    if (fs::exists(dataset_dir + "/query_embeddings.npy")) {
        return dataset_dir + "/query_embeddings.npy";
    }
    return dataset_dir + "/query_embeddings.fvecs";
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            default: out += c; break;
        }
    }
    return out;
}

static std::vector<double> ParseDoubles(const std::string& s) {
    std::vector<double> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty()) continue;
        double val = 0.0;
        auto [ptr, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), val);
        if (ec == std::errc()) out.push_back(val);
    }
    return out;
}

static float ConannLowPercentile(std::vector<float> values, float percentile) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    uint32_t k = static_cast<uint32_t>(
        std::floor((1.0f - percentile) * static_cast<float>(values.size())));
    k = std::min<uint32_t>(k, static_cast<uint32_t>(values.size() - 1));
    return values[k];
}

static float UpperPercentile(std::vector<float> values, float percentile) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    const float findex = percentile * static_cast<float>(values.size() - 1);
    const size_t idx = static_cast<size_t>(std::min(
        findex, static_cast<float>(values.size() - 1)));
    return values[idx];
}

static StatusOr<std::vector<uint32_t>> LoadAssignments(const std::string& path,
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

static bool InferClusteringPaths(const std::string& dataset_dir,
                                 const std::string& index_dir,
                                 uint32_t nlist,
                                 std::string* centroids_path,
                                 std::string* assignments_path) {
    if (centroids_path == nullptr || assignments_path == nullptr) return false;
    if (!centroids_path->empty() && !assignments_path->empty()) return true;
    const std::vector<std::pair<std::string, std::string>> candidates = {
        {dataset_dir + "/coco_centroid_" + std::to_string(nlist) + ".fvecs",
         dataset_dir + "/coco_cluster_id_" + std::to_string(nlist) + ".ivecs"},
        {"/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/msmarco_passage_centroid_" +
             std::to_string(nlist) + ".fvecs",
         "/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/msmarco_passage_cluster_id_" +
             std::to_string(nlist) + ".ivecs"},
        {"/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/msmarco_passage_centroid_" +
             std::to_string(nlist) + "_pad1024.fvecs",
         "/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/msmarco_passage_cluster_id_" +
             std::to_string(nlist) + ".ivecs"},
    };
    for (const auto& [c_path, a_path] : candidates) {
        if (fs::exists(c_path) && fs::exists(a_path)) {
            if (centroids_path->empty()) *centroids_path = c_path;
            if (assignments_path->empty()) *assignments_path = a_path;
            return true;
        }
    }
    const std::string exported_centroids = index_dir + "/centroids.fvecs";
    const std::string exported_assignments = index_dir + "/assignments.ivecs";
    if (fs::exists(exported_centroids) && fs::exists(exported_assignments)) {
        if (centroids_path->empty()) *centroids_path = exported_centroids;
        if (assignments_path->empty()) *assignments_path = exported_assignments;
        return true;
    }
    return false;
}

static QueryTruth BuildTruthFromRows(const std::vector<uint32_t>& rows,
                                     const std::vector<int64_t>* image_ids) {
    QueryTruth truth;
    truth.row_ids.insert(rows.begin(), rows.end());
    if (image_ids != nullptr && !image_ids->empty()) {
        for (uint32_t row : rows) {
            if (row < image_ids->size()) truth.external_ids.insert((*image_ids)[row]);
        }
    }
    return truth;
}

static std::vector<std::pair<float, uint32_t>> ComputeExactTopKRowsWithDists(
    const float* q_vec, const io::NpyArrayFloat& base, uint32_t top_k) {
    const uint32_t N = base.rows;
    const Dim dim = static_cast<Dim>(base.cols);
    std::vector<std::pair<float, uint32_t>> dists(N);
    for (uint32_t i = 0; i < N; ++i) {
        dists[i] = {
            simd::L2Sqr(q_vec, base.data.data() + static_cast<size_t>(i) * dim, dim),
            i};
    }
    const uint32_t k = std::min(top_k, N);
    std::partial_sort(dists.begin(), dists.begin() + k, dists.end());
    dists.resize(k);
    return dists;
}

static bool IsTrueTopK(const QueryTruth& truth, uint32_t row, int64_t external_id) {
    if (!truth.row_ids.empty()) return truth.row_ids.count(row) > 0;
    return truth.external_ids.count(external_id) > 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string dataset_dir = GetArg(argc, argv, "--dataset", "");
    const std::string index_dir = GetArg(argc, argv, "--index-dir", "");
    std::string base_path = GetArg(argc, argv, "--base", "");
    std::string query_path = GetArg(argc, argv, "--query", "");
    std::string centroids_path = GetArg(argc, argv, "--centroids", "");
    std::string assignments_path = GetArg(argc, argv, "--assignments", "");
    const std::string gt_file = GetArg(argc, argv, "--gt-file", "");
    const std::string outdir = GetArg(argc, argv, "--outdir", "./phase_a_grid");
    const std::string search_scope = GetArg(argc, argv, "--search-scope", "nprobe");
    const uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    const uint32_t nprobe = static_cast<uint32_t>(GetIntArg(argc, argv, "--nprobe", 64));
    const uint32_t q_limit = static_cast<uint32_t>(GetIntArg(argc, argv, "--queries", 200));
    const uint32_t dk_calib_limit = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--dk-calibration-queries", static_cast<int>(q_limit)));
    uint8_t bits = static_cast<uint8_t>(GetIntArg(argc, argv, "--bits", 0));
    const std::vector<double> dk_percentiles = ParseDoubles(
        GetArg(argc, argv, "--dk-percentiles", "0.99,0.95,0.90"));
    const std::vector<double> eps_percentiles = ParseDoubles(
        GetArg(argc, argv, "--eps-percentiles", "0.99,0.95,0.90"));

    if (dataset_dir.empty() || index_dir.empty()) {
        std::fprintf(stderr,
                     "Usage: bench_phase_a_grid --dataset <dir> --index-dir <dir> "
                     "[--queries 200] [--dk-calibration-queries 500] [--topk 10] [--nprobe 64] "
                     "[--search-scope full|nprobe] "
                     "[--dk-percentiles 0.99,0.95,0.90] "
                     "[--eps-percentiles 0.99,0.95,0.90] [--outdir ./phase_a_grid]\n");
        return 1;
    }
    if (dk_percentiles.empty() || eps_percentiles.empty()) {
        std::fprintf(stderr, "Empty dk/eps percentile grid.\n");
        return 1;
    }

    fs::create_directories(outdir);
    if (base_path.empty()) base_path = DefaultBasePath(dataset_dir);
    if (query_path.empty()) query_path = DefaultQueryPath(dataset_dir);

    Log("=== Phase A Grid ===\n");
    Log("Dataset: %s\n", dataset_dir.c_str());
    Log("Index:   %s\n", index_dir.c_str());
    Log("Scope:   %s\n", search_scope.c_str());
    Log("Outdir:  %s\n", outdir.c_str());

    IvfIndex index;
    Status s = index.Open(index_dir, false);
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to open index: %s\n", s.ToString().c_str());
        return 1;
    }
    if (bits == 0) bits = index.segment().rabitq_config().bits;
    if (bits <= 1) {
        std::fprintf(stderr, "Phase A grid requires bits > 1.\n");
        return 1;
    }
    if (!InferClusteringPaths(dataset_dir, index_dir, index.nlist(),
                              &centroids_path, &assignments_path)) {
        std::fprintf(stderr,
                     "Failed to infer centroids/assignments. Provide --centroids and --assignments.\n");
        return 1;
    }

    auto base_or = io::LoadVectors(base_path);
    auto query_or = io::LoadVectors(query_path);
    auto centroids_or = io::LoadVectors(centroids_path);
    auto assignments_or = LoadAssignments(assignments_path, base_or.ok() ? base_or.value().rows : 0);
    if (!base_or.ok() || !query_or.ok() || !centroids_or.ok() || !assignments_or.ok()) {
        std::fprintf(stderr, "Failed to load required inputs.\n");
        return 1;
    }

    const auto& base = base_or.value();
    const auto& qry = query_or.value();
    const auto& cents = centroids_or.value();
    const auto& assignments = assignments_or.value();
    const uint32_t Q = std::min(q_limit, qry.rows);
    const uint32_t Q_dk = std::min(dk_calib_limit, qry.rows);
    const uint32_t N = base.rows;
    const Dim dim = static_cast<Dim>(base.cols);

    std::vector<int64_t> image_ids;
    if (fs::exists(dataset_dir + "/image_ids.npy")) {
        auto ids_or = io::LoadNpyInt64(dataset_dir + "/image_ids.npy");
        if (ids_or.ok()) image_ids = ids_or.value().data;
    }

    std::vector<QueryTruth> truth_sets(Q);
    const bool truth_available = search_scope == "full";
    if (truth_available) {
        Log("GT mode: brute-force exact top-%u over %u base vectors\n", top_k, N);
        for (uint32_t qi = 0; qi < Q; ++qi) {
            const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
            auto exact_topk = ComputeExactTopKRowsWithDists(q_vec, base, top_k);
            std::vector<uint32_t> rows;
            rows.reserve(exact_topk.size());
            for (const auto& [dist, row] : exact_topk) rows.push_back(row);
            truth_sets[qi] = BuildTruthFromRows(rows, &image_ids);
        }
    } else {
        Log("GT mode: unavailable for non-full scope\n");
    }

    std::vector<std::vector<uint32_t>> cluster_members(index.nlist());
    for (uint32_t row = 0; row < assignments.size(); ++row) {
        const uint32_t cid = assignments[row];
        if (cid >= index.nlist()) {
            std::fprintf(stderr, "Assignment cluster id out of range: %u\n", cid);
            return 1;
        }
        cluster_members[cid].push_back(row);
    }

    const auto& rotation = index.rotation();
    RaBitQEncoder encoder(dim, rotation, bits);
    RaBitQEstimator estimator(dim, bits);
    std::vector<std::unique_ptr<std::vector<RaBitQCode>>> cluster_code_cache(index.nlist());
    auto ensure_cluster_codes = [&](uint32_t cid) -> const std::vector<RaBitQCode>& {
        if (cluster_code_cache[cid]) return *cluster_code_cache[cid];
        auto codes_ptr = std::make_unique<std::vector<RaBitQCode>>();
        const auto& members = cluster_members[cid];
        codes_ptr->reserve(members.size());
        const float* centroid = cents.data.data() + static_cast<size_t>(cid) * dim;
        for (uint32_t row : members) {
            codes_ptr->push_back(
                encoder.Encode(base.data.data() + static_cast<size_t>(row) * dim, centroid));
        }
        cluster_code_cache[cid] = std::move(codes_ptr);
        return *cluster_code_cache[cid];
    };

    std::vector<QueryEval> evals(Q);
    std::vector<float> rabitq_kths;
    std::vector<float> normalized_errors;
    rabitq_kths.reserve(Q_dk);

    for (uint32_t qi = 0; qi < Q_dk; ++qi) {
        if (qi % 10 == 0) Log("query %u/%u\n", qi, Q_dk);
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
        QueryEval* qeval_ptr = (qi < Q) ? &evals[qi] : nullptr;
        if (qeval_ptr != nullptr) qeval_ptr->query_id = qi;

        std::vector<uint32_t> clusters;
        if (search_scope == "full") {
            clusters.resize(index.nlist());
            std::iota(clusters.begin(), clusters.end(), 0u);
        } else {
            const std::vector<ClusterID> probed = index.FindNearestClusters(q_vec, nprobe);
            clusters.assign(probed.begin(), probed.end());
        }

        std::vector<float> s2_dists;
        for (uint32_t cid : clusters) {
            const float* centroid = cents.data.data() + static_cast<size_t>(cid) * dim;
            PreparedQuery pq = estimator.PrepareQuery(q_vec, centroid, rotation);
            const auto& codes = ensure_cluster_codes(cid);
            const auto& members = cluster_members[cid];
            if (qeval_ptr != nullptr) qeval_ptr->num_candidates += members.size();
            for (size_t local_idx = 0; local_idx < members.size(); ++local_idx) {
                const uint32_t row = members[local_idx];
                const int64_t external_id =
                    (row < image_ids.size()) ? image_ids[row] : static_cast<int64_t>(row);
                const bool is_true = (qeval_ptr != nullptr) &&
                    truth_available && IsTrueTopK(truth_sets[qi], row, external_id);
                if (qeval_ptr != nullptr && is_true) ++qeval_ptr->true_topk_in_scope;

                const RaBitQCode& code = codes[local_idx];
                const float est_s2 = estimator.EstimateDistanceMultiBit(pq, code);
                s2_dists.push_back(est_s2);
                if (qeval_ptr != nullptr) {
                    const float exact_dist = simd::L2Sqr(
                        q_vec, base.data.data() + static_cast<size_t>(row) * dim, dim);
                    const float margin_unit = 2.0f * std::max(1e-12f, pq.norm_qc * code.norm);
                    const float normalized_err = std::abs(est_s2 - exact_dist) / margin_unit;
                    qeval_ptr->candidates.push_back({est_s2, margin_unit, normalized_err, is_true});
                    normalized_errors.push_back(normalized_err);
                }
            }
        }

        if (qeval_ptr != nullptr && truth_available) {
            auto exact_topk = ComputeExactTopKRowsWithDists(q_vec, base, top_k);
            qeval_ptr->exact_kth_l2 = exact_topk.empty() ? 0.0f : exact_topk.back().first;
        }
        if (s2_dists.size() < top_k) {
            if (qeval_ptr != nullptr) qeval_ptr->rabitq_kth = std::numeric_limits<float>::infinity();
        } else {
            std::nth_element(s2_dists.begin(), s2_dists.begin() + (top_k - 1), s2_dists.end());
            const float kth = s2_dists[top_k - 1];
            if (qeval_ptr != nullptr) qeval_ptr->rabitq_kth = kth;
            rabitq_kths.push_back(kth);
        }
    }

    std::vector<ComboSummary> combos;
    for (double dk_p : dk_percentiles) {
        const float dk_value = ConannLowPercentile(rabitq_kths, static_cast<float>(dk_p));
        for (double eps_p : eps_percentiles) {
            const float eps_value = UpperPercentile(normalized_errors, static_cast<float>(eps_p));
            ComboSummary combo;
            combo.dk_percentile = dk_p;
            combo.eps_percentile = eps_p;
            combo.dk_value = dk_value;
            combo.eps_value = eps_value;

            for (const QueryEval& qeval : evals) {
                uint64_t query_safein = 0;
                for (const CandidateEval& cand : qeval.candidates) {
                    if (cand.est_dist_s2 < dk_value) {
                        ++combo.count_est_lt_dk;
                        const float margin = cand.margin_unit * eps_value;
                        if (cand.est_dist_s2 < dk_value - margin) {
                            ++combo.safein;
                            ++query_safein;
                            if (truth_available && !cand.is_true_topk) ++combo.false_safein;
                        } else {
                            ++combo.count_margin_blocked;
                        }
                    }
                }
                if (query_safein >= 1) ++combo.queries_ge1_safein;
                if (query_safein >= 2) ++combo.queries_ge2_safein;
            }
            combos.push_back(combo);
        }
    }

    {
        std::ofstream csv(outdir + "/phase_a_grid_summary.csv");
        csv << "dk_percentile,eps_percentile,dk_value,eps_value,safein,false_safein,"
            << "false_safein_rate,avg_safein_per_query,queries_ge1_safein,queries_ge2_safein,"
            << "avg_est_lt_dk_per_query,avg_margin_blocked_per_query,truth_available,scope\n";
        csv << std::fixed << std::setprecision(9);
        for (const ComboSummary& combo : combos) {
            const double safein_rate = combo.safein == 0 ? 0.0
                : static_cast<double>(combo.false_safein) / static_cast<double>(combo.safein);
            csv << combo.dk_percentile << ','
                << combo.eps_percentile << ','
                << combo.dk_value << ','
                << combo.eps_value << ','
                << combo.safein << ','
                << combo.false_safein << ','
                << safein_rate << ','
                << (static_cast<double>(combo.safein) / static_cast<double>(Q)) << ','
                << combo.queries_ge1_safein << ','
                << combo.queries_ge2_safein << ','
                << (static_cast<double>(combo.count_est_lt_dk) / static_cast<double>(Q)) << ','
                << (static_cast<double>(combo.count_margin_blocked) / static_cast<double>(Q)) << ','
                << (truth_available ? 1 : 0) << ','
                << search_scope << '\n';
        }
    }

    {
        std::ofstream json(outdir + "/phase_a_grid_summary.json");
        json << std::fixed << std::setprecision(9);
        json << "{\n";
        json << "  \"dataset\": \"" << JsonEscape(dataset_dir) << "\",\n";
        json << "  \"index_dir\": \"" << JsonEscape(index_dir) << "\",\n";
        json << "  \"scope\": \"" << JsonEscape(search_scope) << "\",\n";
        json << "  \"queries\": " << Q << ",\n";
        json << "  \"dk_calibration_queries\": " << Q_dk << ",\n";
        json << "  \"topk\": " << top_k << ",\n";
        json << "  \"nprobe\": " << nprobe << ",\n";
        json << "  \"bits\": " << static_cast<int>(bits) << ",\n";
        json << "  \"truth_available\": " << (truth_available ? "true" : "false") << ",\n";
        json << "  \"num_combos\": " << combos.size() << ",\n";
        json << "  \"rows\": [\n";
        for (size_t i = 0; i < combos.size(); ++i) {
            const ComboSummary& combo = combos[i];
            const double safein_rate = combo.safein == 0 ? 0.0
                : static_cast<double>(combo.false_safein) / static_cast<double>(combo.safein);
            json << "    {\n";
            json << "      \"dk_percentile\": " << combo.dk_percentile << ",\n";
            json << "      \"eps_percentile\": " << combo.eps_percentile << ",\n";
            json << "      \"dk_value\": " << combo.dk_value << ",\n";
            json << "      \"eps_value\": " << combo.eps_value << ",\n";
            json << "      \"safein\": " << combo.safein << ",\n";
            json << "      \"false_safein\": " << combo.false_safein << ",\n";
            json << "      \"false_safein_rate\": " << safein_rate << ",\n";
            json << "      \"avg_safein_per_query\": "
                 << (static_cast<double>(combo.safein) / static_cast<double>(Q)) << ",\n";
            json << "      \"queries_ge1_safein\": " << combo.queries_ge1_safein << ",\n";
            json << "      \"queries_ge2_safein\": " << combo.queries_ge2_safein << ",\n";
            json << "      \"avg_est_lt_dk_per_query\": "
                 << (static_cast<double>(combo.count_est_lt_dk) / static_cast<double>(Q)) << ",\n";
            json << "      \"avg_margin_blocked_per_query\": "
                 << (static_cast<double>(combo.count_margin_blocked) / static_cast<double>(Q)) << "\n";
            json << "    }" << (i + 1 == combos.size() ? "\n" : ",\n");
        }
        json << "  ]\n";
        json << "}\n";
    }

    Log("Wrote %s\n", (outdir + "/phase_a_grid_summary.csv").c_str());
    Log("Wrote %s\n", (outdir + "/phase_a_grid_summary.json").c_str());
    return 0;
}
