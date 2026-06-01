/// bench_dk_space_compare.cpp — compare exact-space d_k calibration against a
/// serving-probe RabitQ Stage2 d_k calibration.
///
/// Usage:
///   bench_dk_space_compare --dataset <dir> --index-dir <dir>
///       [--base path] [--query path] [--centroids path] [--assignments path]
///       [--gt-file path] [--queries 200] [--topk 10] [--nprobe 64]
///       [--percentile 0.99] [--bits 4] [--outdir ./dk_space_compare]

#include <algorithm>
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
#include "vdb/index/conann.h"
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

struct S2Record {
    float est_dist = 0.0f;
    float margin = 0.0f;
    bool is_true_topk = false;
};

struct QueryStats {
    uint32_t query_id = 0;
    uint64_t num_probe_candidates = 0;
    uint64_t num_s2_candidates = 0;
    uint64_t true_topk_in_probe = 0;
    uint64_t count_s2_lt_dk_exact = 0;
    uint64_t count_s2_lt_dk_rabitq = 0;
    uint64_t count_safein_current_exact_dk = 0;
    uint64_t count_safein_current_rabitq_dk = 0;
    uint64_t false_safein_exact_dk = 0;
    uint64_t false_safein_rabitq_dk = 0;
    float exact_kth_l2 = 0.0f;
    float rabitq_s2_probe_kth = 0.0f;
    bool insufficient_s2_candidates = false;
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

static float GetFloatArg(int argc, char* argv[], const char* name, float def) {
    const auto s = GetArg(argc, argv, name, "");
    return s.empty() ? def : std::strtof(s.c_str(), nullptr);
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
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

static float ConannPercentile(std::vector<float> values, float percentile) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    uint32_t k = static_cast<uint32_t>(
        std::floor((1.0f - percentile) * static_cast<float>(values.size())));
    k = std::min<uint32_t>(k, static_cast<uint32_t>(values.size() - 1));
    return values[k];
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

static StatusOr<std::vector<std::vector<int64_t>>> LoadExternalGroundTruth(
    const std::string& gt_file, uint32_t q_count, uint32_t gt_k) {
    std::vector<std::vector<int64_t>> gt_topk;
    if (gt_file.size() >= 6 && gt_file.substr(gt_file.size() - 6) == ".ivecs") {
        auto gt_or = io::LoadIvecs(gt_file);
        if (!gt_or.ok()) return gt_or.status();
        const auto& gt = gt_or.value();
        if (gt.rows < q_count || gt.cols < gt_k) {
            return Status::InvalidArgument("External GT shape is smaller than requested.");
        }
        gt_topk.resize(q_count);
        for (uint32_t qi = 0; qi < q_count; ++qi) {
            gt_topk[qi].resize(gt_k);
            const int32_t* row = gt.data.data() + static_cast<size_t>(qi) * gt.cols;
            for (uint32_t k = 0; k < gt_k; ++k) gt_topk[qi][k] = static_cast<int64_t>(row[k]);
        }
        return gt_topk;
    }
    auto gt_or = io::LoadNpyInt64Matrix(gt_file);
    if (!gt_or.ok()) return gt_or.status();
    const auto& gt = gt_or.value();
    if (gt.rows < q_count || gt.cols < gt_k) {
        return Status::InvalidArgument("External GT shape is smaller than requested.");
    }
    gt_topk.resize(q_count);
    for (uint32_t qi = 0; qi < q_count; ++qi) {
        gt_topk[qi].resize(gt_k);
        const int64_t* row = gt.data.data() + static_cast<size_t>(qi) * gt.cols;
        for (uint32_t k = 0; k < gt_k; ++k) gt_topk[qi][k] = row[k];
    }
    return gt_topk;
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

static QueryTruth BuildTruthFromExternal(
    const std::vector<int64_t>& external_ids,
    const std::unordered_map<int64_t, uint32_t>* id_to_row) {
    QueryTruth truth;
    truth.external_ids.insert(external_ids.begin(), external_ids.end());
    if (id_to_row != nullptr) {
        for (int64_t id : external_ids) {
            auto it = id_to_row->find(id);
            if (it != id_to_row->end()) truth.row_ids.insert(it->second);
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

static double SafeDiv(double num, double den) {
    return den == 0.0 ? 0.0 : num / den;
}

static void WritePerQueryCsv(const std::string& path,
                             const std::vector<QueryStats>& stats) {
    std::ofstream f(path);
    f << "query_id,num_probe_candidates,num_s2_candidates,exact_kth_l2,"
      << "rabitq_s2_probe_kth,count_s2_lt_dk_exact,count_s2_lt_dk_rabitq,"
      << "count_safein_current_exact_dk,count_safein_current_rabitq_dk,"
      << "true_topk_in_probe,false_safein_exact_dk,false_safein_rabitq_dk,"
      << "insufficient_s2_candidates\n";
    f << std::fixed << std::setprecision(9);
    for (const QueryStats& q : stats) {
        f << q.query_id << ','
          << q.num_probe_candidates << ','
          << q.num_s2_candidates << ','
          << q.exact_kth_l2 << ',';
        if (!q.insufficient_s2_candidates) f << q.rabitq_s2_probe_kth;
        f << ','
          << q.count_s2_lt_dk_exact << ','
          << q.count_s2_lt_dk_rabitq << ','
          << q.count_safein_current_exact_dk << ','
          << q.count_safein_current_rabitq_dk << ','
          << q.true_topk_in_probe << ','
          << q.false_safein_exact_dk << ','
          << q.false_safein_rabitq_dk << ','
          << (q.insufficient_s2_candidates ? 1 : 0) << '\n';
    }
}

static void WriteSummaryJson(const std::string& path,
                             const std::string& dataset_dir,
                             const std::string& index_dir,
                             uint32_t queries,
                             uint32_t top_k,
                             uint32_t nprobe,
                             uint8_t bits,
                             float percentile,
                             float d_k_exact,
                             float index_d_k,
                             float d_k_rabitq,
                             bool truth_available,
                             const std::vector<QueryStats>& stats) {
    uint64_t total_probe = 0;
    uint64_t total_s2 = 0;
    uint64_t insufficient = 0;
    uint64_t true_topk_in_probe = 0;
    uint64_t count_lt_exact = 0;
    uint64_t count_lt_rabitq = 0;
    uint64_t safein_exact = 0;
    uint64_t safein_rabitq = 0;
    uint64_t false_safein_exact = 0;
    uint64_t false_safein_rabitq = 0;
    uint64_t queries_ge1_exact = 0;
    uint64_t queries_ge1_rabitq = 0;
    std::vector<float> exact_kths;
    std::vector<float> rabitq_kths;
    exact_kths.reserve(stats.size());
    rabitq_kths.reserve(stats.size());
    for (const QueryStats& q : stats) {
        total_probe += q.num_probe_candidates;
        total_s2 += q.num_s2_candidates;
        true_topk_in_probe += q.true_topk_in_probe;
        count_lt_exact += q.count_s2_lt_dk_exact;
        count_lt_rabitq += q.count_s2_lt_dk_rabitq;
        safein_exact += q.count_safein_current_exact_dk;
        safein_rabitq += q.count_safein_current_rabitq_dk;
        false_safein_exact += q.false_safein_exact_dk;
        false_safein_rabitq += q.false_safein_rabitq_dk;
        if (q.count_safein_current_exact_dk > 0) ++queries_ge1_exact;
        if (q.count_safein_current_rabitq_dk > 0) ++queries_ge1_rabitq;
        if (q.insufficient_s2_candidates) {
            ++insufficient;
        } else {
            rabitq_kths.push_back(q.rabitq_s2_probe_kth);
        }
        exact_kths.push_back(q.exact_kth_l2);
    }

    std::ofstream f(path);
    f << std::fixed << std::setprecision(9);
    f << "{\n";
    f << "  \"dataset\": \"" << JsonEscape(dataset_dir) << "\",\n";
    f << "  \"index_dir\": \"" << JsonEscape(index_dir) << "\",\n";
    f << "  \"queries\": " << queries << ",\n";
    f << "  \"topk\": " << top_k << ",\n";
    f << "  \"nprobe\": " << nprobe << ",\n";
    f << "  \"bits\": " << static_cast<int>(bits) << ",\n";
    f << "  \"percentile\": " << percentile << ",\n";
    f << "  \"d_k_exact\": " << d_k_exact << ",\n";
    f << "  \"index_d_k\": " << index_d_k << ",\n";
    f << "  \"d_k_rabitq_s2_probe\": " << d_k_rabitq << ",\n";
    f << "  \"ratio_rabitq_to_exact\": " << SafeDiv(d_k_rabitq, d_k_exact) << ",\n";
    f << "  \"truth_available\": " << (truth_available ? "true" : "false") << ",\n";
    f << "  \"insufficient_s2_queries\": " << insufficient << ",\n";
    f << "  \"total_probe_candidates\": " << total_probe << ",\n";
    f << "  \"total_s2_candidates\": " << total_s2 << ",\n";
    f << "  \"true_topk_in_probe\": " << true_topk_in_probe << ",\n";
    f << "  \"avg_count_s2_lt_dk_exact\": " << SafeDiv(count_lt_exact, stats.size()) << ",\n";
    f << "  \"avg_count_s2_lt_dk_rabitq\": " << SafeDiv(count_lt_rabitq, stats.size()) << ",\n";
    f << "  \"avg_safein_current_exact_dk\": " << SafeDiv(safein_exact, stats.size()) << ",\n";
    f << "  \"avg_safein_current_rabitq_dk\": " << SafeDiv(safein_rabitq, stats.size()) << ",\n";
    f << "  \"false_safein_exact_dk\": " << false_safein_exact << ",\n";
    f << "  \"false_safein_rabitq_dk\": " << false_safein_rabitq << ",\n";
    f << "  \"false_safein_rate_exact_dk\": " << SafeDiv(false_safein_exact, safein_exact) << ",\n";
    f << "  \"false_safein_rate_rabitq_dk\": " << SafeDiv(false_safein_rabitq, safein_rabitq) << ",\n";
    f << "  \"queries_with_ge_1_safein_exact_dk\": " << queries_ge1_exact << ",\n";
    f << "  \"queries_with_ge_1_safein_rabitq_dk\": " << queries_ge1_rabitq << ",\n";
    f << "  \"exact_kth_l2_p50\": " << ConannPercentile(exact_kths, 0.50f) << ",\n";
    f << "  \"rabitq_s2_probe_kth_p50\": " << ConannPercentile(rabitq_kths, 0.50f) << "\n";
    f << "}\n";
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
    const std::string outdir = GetArg(argc, argv, "--outdir", "./dk_space_compare");
    const std::string exact_dk_mode = GetArg(argc, argv, "--exact-dk-mode", "recompute");
    const uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    const uint32_t nprobe = static_cast<uint32_t>(GetIntArg(argc, argv, "--nprobe", 64));
    const uint32_t q_limit = static_cast<uint32_t>(GetIntArg(argc, argv, "--queries", 200));
    const float percentile = GetFloatArg(argc, argv, "--percentile", 0.99f);
    const bool write_candidates = GetIntArg(argc, argv, "--write-candidates", 0) != 0;
    uint8_t bits = static_cast<uint8_t>(GetIntArg(argc, argv, "--bits", 0));

    if (dataset_dir.empty() || index_dir.empty()) {
        std::fprintf(stderr,
                     "Usage: bench_dk_space_compare --dataset <dir> --index-dir <dir> "
                     "[--base path] [--query path] [--centroids path] [--assignments path] "
                     "[--gt-file path] [--queries 200] [--topk 10] [--nprobe 64] "
                     "[--percentile 0.99] [--bits 4] [--write-candidates 0|1] "
                     "[--outdir ./dk_space_compare]\n");
        return 1;
    }

    fs::create_directories(outdir);
    if (base_path.empty()) base_path = DefaultBasePath(dataset_dir);
    if (query_path.empty()) query_path = DefaultQueryPath(dataset_dir);

    Log("=== d_k Space Compare ===\n");
    Log("Dataset:    %s\n", dataset_dir.c_str());
    Log("Index:      %s\n", index_dir.c_str());
    Log("Base:       %s\n", base_path.c_str());
    Log("Query:      %s\n", query_path.c_str());
    Log("Outdir:     %s\n", outdir.c_str());
    Log("topk=%u nprobe=%u queries=%u percentile=%.4f\n",
        top_k, nprobe, q_limit, percentile);
    Log("exact_dk_mode=%s\n", exact_dk_mode.c_str());

    IvfIndex index;
    Status s = index.Open(index_dir, false);
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to open index: %s\n", s.ToString().c_str());
        return 1;
    }
    if (bits == 0) bits = index.segment().rabitq_config().bits;
    if (bits <= 1) {
        std::fprintf(stderr, "Stage2 RabitQ d_k requires bits > 1.\n");
        return 1;
    }
    if (!InferClusteringPaths(dataset_dir, index_dir, index.nlist(),
                              &centroids_path, &assignments_path)) {
        std::fprintf(stderr,
                     "Failed to infer centroids/assignments. Provide --centroids and --assignments.\n");
        return 1;
    }

    auto base_or = io::LoadVectors(base_path);
    if (!base_or.ok()) {
        std::fprintf(stderr, "Failed to load base: %s\n",
                     base_or.status().ToString().c_str());
        return 1;
    }
    auto query_or = io::LoadVectors(query_path);
    if (!query_or.ok()) {
        std::fprintf(stderr, "Failed to load query: %s\n",
                     query_or.status().ToString().c_str());
        return 1;
    }
    auto centroids_or = io::LoadVectors(centroids_path);
    if (!centroids_or.ok()) {
        std::fprintf(stderr, "Failed to load centroids: %s\n",
                     centroids_or.status().ToString().c_str());
        return 1;
    }
    auto assignments_or = LoadAssignments(assignments_path, base_or.value().rows);
    if (!assignments_or.ok()) {
        std::fprintf(stderr, "Failed to load assignments: %s\n",
                     assignments_or.status().ToString().c_str());
        return 1;
    }

    const auto& base = base_or.value();
    const auto& qry = query_or.value();
    const auto& cents = centroids_or.value();
    const auto& assignments = assignments_or.value();
    const uint32_t Q = std::min(q_limit, qry.rows);
    const uint32_t N = base.rows;
    const Dim dim = static_cast<Dim>(base.cols);
    if (qry.cols != base.cols || cents.cols != base.cols || cents.rows != index.nlist()) {
        std::fprintf(stderr, "Dimension or centroid-count mismatch with index.\n");
        return 1;
    }

    std::vector<int64_t> image_ids;
    std::unordered_map<int64_t, uint32_t> image_id_to_row;
    if (fs::exists(dataset_dir + "/image_ids.npy")) {
        auto ids_or = io::LoadNpyInt64(dataset_dir + "/image_ids.npy");
        if (!ids_or.ok()) {
            std::fprintf(stderr, "Failed to load image_ids.npy: %s\n",
                         ids_or.status().ToString().c_str());
            return 1;
        }
        image_ids = ids_or.value().data;
        image_id_to_row.reserve(image_ids.size() * 2);
        for (uint32_t i = 0; i < image_ids.size(); ++i) image_id_to_row[image_ids[i]] = i;
    }

    std::vector<QueryTruth> truth_sets(Q);
    if (!gt_file.empty()) {
        auto gt_or = LoadExternalGroundTruth(gt_file, Q, top_k);
        if (!gt_or.ok()) {
            std::fprintf(stderr, "Failed to load external GT: %s\n",
                         gt_or.status().ToString().c_str());
            return 1;
        }
        for (uint32_t qi = 0; qi < Q; ++qi) {
            truth_sets[qi] = BuildTruthFromExternal(gt_or.value()[qi], &image_id_to_row);
        }
        Log("GT mode: external (%s)\n", gt_file.c_str());
    } else {
        if (exact_dk_mode == "index") {
            Log("GT mode: unavailable (exact-dk-mode=index skips brute-force truth)\n");
        } else {
            Log("GT mode: brute-force exact top-%u over %u base vectors\n", top_k, N);
        }
    }
    const bool truth_available = !gt_file.empty() || exact_dk_mode != "index";

    std::vector<QueryStats> per_query(Q);
    if (exact_dk_mode != "index") {
        for (uint32_t qi = 0; qi < Q; ++qi) {
            const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
            auto exact_topk = ComputeExactTopKRowsWithDists(q_vec, base, top_k);
            if (exact_topk.empty()) {
                std::fprintf(stderr, "Empty exact topk for query %u.\n", qi);
                return 1;
            }
            per_query[qi].exact_kth_l2 = exact_topk.back().first;
            if (gt_file.empty()) {
                std::vector<uint32_t> rows;
                rows.reserve(exact_topk.size());
                for (const auto& [dist, row] : exact_topk) {
                    (void)dist;
                    rows.push_back(row);
                }
                truth_sets[qi] = BuildTruthFromRows(rows, &image_ids);
            }
        }
    }
    for (uint32_t qi = 0; qi < Q; ++qi) {
        per_query[qi].query_id = qi;
    }

    const float index_d_k = index.conann().d_k();
    const float d_k_exact = (exact_dk_mode == "index")
        ? index_d_k
        : ConANN::CalibrateDistanceThreshold(
              qry.data.data(), Q, base.data.data(), N, dim, Q, top_k, percentile, 42);
    Log("d_k_exact=%.9f index_d_k=%.9f\n", d_k_exact, index_d_k);

    std::ofstream candidate_csv;
    if (write_candidates) {
        candidate_csv.open(outdir + "/dk_space_compare_candidates_exact.csv");
        candidate_csv
            << "query_id,vector_row,cluster_id,cluster_rank,is_true_topk,"
            << "exact_kth_l2,T_minus_Rq,exact_dist,est_s2,margin_s2,"
            << "upper_bound_s2,static_safein_exact_dk,oracle_safein_exact_Rq,"
            << "static_false_mode\n";
        candidate_csv << std::fixed << std::setprecision(9);
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

    const float eps_ip = index.conann().epsilon();
    const float margin_s2_divisor = static_cast<float>(1u << (bits - 1));
    std::vector<float> rabitq_query_kths;
    rabitq_query_kths.reserve(Q);

    for (uint32_t qi = 0; qi < Q; ++qi) {
        if (qi % 10 == 0) Log("query %u/%u\n", qi, Q);
        QueryStats& qs = per_query[qi];
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
        const std::vector<ClusterID> probed = index.FindNearestClusters(q_vec, nprobe);
        std::vector<float> s2_dists;

        for (uint32_t rank = 0; rank < probed.size(); ++rank) {
            (void)rank;
            const uint32_t cid = probed[rank];
            const float* centroid = cents.data.data() + static_cast<size_t>(cid) * dim;
            PreparedQuery pq = estimator.PrepareQuery(q_vec, centroid, rotation);
            const auto& codes = ensure_cluster_codes(cid);
            const auto& members = cluster_members[cid];
            const float margin_factor = 2.0f * pq.norm_qc * eps_ip;
            qs.num_probe_candidates += members.size();

            for (size_t local_idx = 0; local_idx < members.size(); ++local_idx) {
                const uint32_t row = members[local_idx];
                const int64_t external_id =
                    (row < image_ids.size()) ? image_ids[row] : static_cast<int64_t>(row);
                const bool is_true = IsTrueTopK(truth_sets[qi], row, external_id);
                if (truth_available && is_true) ++qs.true_topk_in_probe;

                const RaBitQCode& code = codes[local_idx];
                const float est_s2 = estimator.EstimateDistanceMultiBit(pq, code);
                const float margin_s1 = margin_factor * code.norm;
                const float margin_s2 = margin_s1 / margin_s2_divisor;
                const float upper_bound_s2 = est_s2 + margin_s2;
                s2_dists.push_back(est_s2);
                if (est_s2 < d_k_exact) ++qs.count_s2_lt_dk_exact;
                const bool safein_exact = upper_bound_s2 < d_k_exact;
                if (safein_exact) {
                    ++qs.count_safein_current_exact_dk;
                    if (truth_available && !is_true) ++qs.false_safein_exact_dk;
                }
                if (candidate_csv.is_open()) {
                    const float exact_dist = simd::L2Sqr(
                        q_vec, base.data.data() + static_cast<size_t>(row) * dim, dim);
                    const float exact_kth = qs.exact_kth_l2;
                    const bool oracle_safein =
                        exact_dk_mode != "index" && upper_bound_s2 < exact_kth;
                    const char* false_mode = "";
                    if (safein_exact && truth_available && !is_true) {
                        if (exact_dist > d_k_exact) {
                            false_mode = "bound_violation_exact_gt_T";
                        } else if (exact_dk_mode != "index" && exact_dist > exact_kth) {
                            false_mode = "static_T_above_query_Rq";
                        } else {
                            false_mode = "gt_tie_or_id_mismatch";
                        }
                    }
                    candidate_csv << qi << ','
                                  << row << ','
                                  << cid << ','
                                  << (rank + 1) << ','
                                  << (is_true ? 1 : 0) << ','
                                  << exact_kth << ','
                                  << (d_k_exact - exact_kth) << ','
                                  << exact_dist << ','
                                  << est_s2 << ','
                                  << margin_s2 << ','
                                  << upper_bound_s2 << ','
                                  << (safein_exact ? 1 : 0) << ','
                                  << (oracle_safein ? 1 : 0) << ','
                                  << false_mode << '\n';
                }
            }
        }

        qs.num_s2_candidates = s2_dists.size();
        if (s2_dists.size() < top_k) {
            qs.insufficient_s2_candidates = true;
            qs.rabitq_s2_probe_kth = 0.0f;
        } else {
            std::nth_element(s2_dists.begin(), s2_dists.begin() + (top_k - 1), s2_dists.end());
            qs.rabitq_s2_probe_kth = s2_dists[top_k - 1];
            rabitq_query_kths.push_back(qs.rabitq_s2_probe_kth);
        }
    }

    const float d_k_rabitq = ConannPercentile(rabitq_query_kths, percentile);
    Log("d_k_rabitq_s2_probe=%.9f ratio=%.6f\n",
        d_k_rabitq, SafeDiv(d_k_rabitq, d_k_exact));

    for (uint32_t qi = 0; qi < Q; ++qi) {
        if (qi % 10 == 0) Log("replay query %u/%u\n", qi, Q);
        QueryStats& qs = per_query[qi];
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
        const std::vector<ClusterID> probed = index.FindNearestClusters(q_vec, nprobe);

        for (uint32_t rank = 0; rank < probed.size(); ++rank) {
            (void)rank;
            const uint32_t cid = probed[rank];
            const float* centroid = cents.data.data() + static_cast<size_t>(cid) * dim;
            PreparedQuery pq = estimator.PrepareQuery(q_vec, centroid, rotation);
            const auto& codes = ensure_cluster_codes(cid);
            const auto& members = cluster_members[cid];
            const float margin_factor = 2.0f * pq.norm_qc * eps_ip;

            for (size_t local_idx = 0; local_idx < members.size(); ++local_idx) {
                const uint32_t row = members[local_idx];
                const int64_t external_id =
                    (row < image_ids.size()) ? image_ids[row] : static_cast<int64_t>(row);
                const bool is_true = IsTrueTopK(truth_sets[qi], row, external_id);
                const RaBitQCode& code = codes[local_idx];
                const float est_s2 = estimator.EstimateDistanceMultiBit(pq, code);
                const float margin_s1 = margin_factor * code.norm;
                const float margin_s2 = margin_s1 / margin_s2_divisor;
                if (est_s2 < d_k_rabitq) ++qs.count_s2_lt_dk_rabitq;
                const bool safein_rabitq = est_s2 < d_k_rabitq - margin_s2;
                if (safein_rabitq) {
                    ++qs.count_safein_current_rabitq_dk;
                    if (truth_available && !is_true) ++qs.false_safein_rabitq_dk;
                }
            }
        }
    }

    WritePerQueryCsv(outdir + "/dk_space_compare_per_query.csv", per_query);
    WriteSummaryJson(outdir + "/dk_space_compare_summary.json",
                     dataset_dir, index_dir, Q, top_k, nprobe, bits, percentile,
                     d_k_exact, index_d_k, d_k_rabitq, truth_available, per_query);

    Log("Wrote %s\n", (outdir + "/dk_space_compare_per_query.csv").c_str());
    Log("Wrote %s\n", (outdir + "/dk_space_compare_summary.json").c_str());
    return 0;
}
