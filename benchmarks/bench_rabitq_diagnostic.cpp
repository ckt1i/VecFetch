/// bench_rabitq_diagnostic.cpp — Offline diagnostic benchmark for RaBitQ S1/S2
/// distance estimation and ConANN classification on real serving-style probe
/// candidates.
///
/// Usage:
///   bench_rabitq_diagnostic --dataset <dir> --index-dir <dir>
///       [--base path] [--query path] [--centroids path] [--assignments path]
///       [--gt-file path] [--queries 200] [--topk 10] [--nprobe 64]
///       [--bits 4] [--outdir ./diag_output]

#include <algorithm>
#include <chrono>
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

struct CandidateMetrics {
    uint32_t query_id = 0;
    uint32_t cluster_rank = 0;
    uint32_t cluster_id = 0;
    uint32_t vector_row = 0;
    int64_t vector_external_id = -1;
    bool is_true_topk = false;
    float exact_dist = 0.0f;
    float est_dist_s1 = 0.0f;
    bool stage2_evaluated = false;
    float est_dist_s2 = 0.0f;
    float norm_qc = 0.0f;
    float norm_oc = 0.0f;
    float margin_s1 = 0.0f;
    float margin_s2 = 0.0f;
    float d_k_static = 0.0f;
    float dynamic_d_k_before_cluster = 0.0f;
    float abs_err_s1 = 0.0f;
    float abs_err_s2 = 0.0f;
    float normalized_err_s1 = 0.0f;
    float normalized_err_s2 = 0.0f;
    float safein_gap_s1 = 0.0f;
    float safein_gap_s2 = 0.0f;
    float safeout_gap_s1 = 0.0f;
    float safeout_gap_s2 = 0.0f;
    float safein_ratio_s1 = 0.0f;
    float safein_ratio_s2 = 0.0f;
    ResultClass s1_class = ResultClass::Uncertain;
    ResultClass s2_class = ResultClass::Uncertain;
    ResultClass final_class = ResultClass::Uncertain;
};

struct SummaryStats {
    uint64_t candidates = 0;
    uint64_t stage2_candidates = 0;
    uint64_t true_topk_candidates = 0;
    uint64_t final_false_safeout = 0;
    uint64_t s1_safein = 0;
    uint64_t s1_safeout = 0;
    uint64_t s1_uncertain = 0;
    uint64_t s2_safein = 0;
    uint64_t s2_safeout = 0;
    uint64_t s2_uncertain = 0;
    uint64_t final_safein = 0;
    uint64_t final_safeout = 0;
    uint64_t final_uncertain = 0;
    std::vector<float> abs_err_s1;
    std::vector<float> abs_err_s2;
    std::vector<float> normalized_err_s1;
    std::vector<float> normalized_err_s2;
};

struct EstimateHeapEntry {
    float distance = 0.0f;
    float error_bound = 0.0f;
    uint32_t row = 0;

    bool operator<(const EstimateHeapEntry& other) const {
        return distance < other.distance;
    }
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

static bool HasFlag(int argc, char* argv[], const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static const char* ClassToStr(ResultClass rc) {
    switch (rc) {
        case ResultClass::SafeIn: return "SafeIn";
        case ResultClass::SafeOut: return "SafeOut";
        case ResultClass::Uncertain: return "Uncertain";
    }
    return "Unknown";
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

static float Percentile(std::vector<float> values, float p) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    const float idx = p * static_cast<float>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return values[lo];
    const float frac = idx - static_cast<float>(lo);
    return values[lo] * (1.0f - frac) + values[hi] * frac;
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

    if (!index_dir.empty()) {
        const std::string exported_centroids = index_dir + "/centroids.fvecs";
        const std::string exported_assignments = index_dir + "/assignments.ivecs";
        if (fs::exists(exported_centroids) && fs::exists(exported_assignments)) {
            if (centroids_path->empty()) *centroids_path = exported_centroids;
            if (assignments_path->empty()) *assignments_path = exported_assignments;
            return true;
        }
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

static QueryTruth BuildTruthFromExternal(const std::vector<int64_t>& external_ids,
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

static std::vector<uint32_t> ComputeExactTopKRows(const float* q_vec,
                                                  const io::NpyArrayFloat& base,
                                                  uint32_t top_k) {
    const uint32_t N = base.rows;
    const Dim dim = static_cast<Dim>(base.cols);
    std::vector<std::pair<float, uint32_t>> dists(N);
    for (uint32_t i = 0; i < N; ++i) {
        dists[i] = {simd::L2Sqr(q_vec, base.data.data() + static_cast<size_t>(i) * dim, dim), i};
    }
    const uint32_t k = std::min(top_k, N);
    std::partial_sort(dists.begin(), dists.begin() + k, dists.end());
    std::vector<uint32_t> rows(k);
    for (uint32_t i = 0; i < k; ++i) rows[i] = dists[i].second;
    return rows;
}

static void WritePerCandidateRow(std::ofstream& csv, const CandidateMetrics& m) {
    csv << m.query_id << ','
        << m.cluster_rank << ','
        << m.cluster_id << ','
        << m.vector_row << ','
        << m.vector_external_id << ','
        << (m.is_true_topk ? 1 : 0) << ','
        << m.exact_dist << ','
        << m.est_dist_s1 << ',';
    if (m.stage2_evaluated) {
        csv << m.est_dist_s2;
    }
    csv << ','
        << m.norm_qc << ','
        << m.norm_oc << ','
        << m.margin_s1 << ',';
    if (m.stage2_evaluated) {
        csv << m.margin_s2;
    }
    csv << ','
        << m.d_k_static << ','
        << m.dynamic_d_k_before_cluster << ','
        << m.abs_err_s1 << ',';
    if (m.stage2_evaluated) csv << m.abs_err_s2;
    csv << ','
        << m.normalized_err_s1 << ',';
    if (m.stage2_evaluated) csv << m.normalized_err_s2;
    csv << ','
        << m.safein_gap_s1 << ',';
    if (m.stage2_evaluated) csv << m.safein_gap_s2;
    csv << ','
        << m.safeout_gap_s1 << ',';
    if (m.stage2_evaluated) csv << m.safeout_gap_s2;
    csv << ','
        << m.safein_ratio_s1 << ',';
    if (m.stage2_evaluated) csv << m.safein_ratio_s2;
    csv << ','
        << ClassToStr(m.s1_class) << ','
        << (m.stage2_evaluated ? ClassToStr(m.s2_class) : "") << ','
        << ClassToStr(m.final_class) << '\n';
}

static void WriteSummaryJson(const std::string& path,
                             const std::string& dataset_name,
                             const std::string& index_dir,
                             uint32_t queries,
                             uint32_t top_k,
                             uint32_t nprobe,
                             uint8_t bits,
                             float legacy_d_k,
                             float safein_d_k,
                             const std::string& safein_dk_space,
                             const std::string& safeout_threshold_mode,
                             const SummaryStats& stats) {
    std::ofstream f(path);
    f << "{\n";
    f << "  \"dataset\": \"" << JsonEscape(dataset_name) << "\",\n";
    f << "  \"index_dir\": \"" << JsonEscape(index_dir) << "\",\n";
    f << "  \"queries\": " << queries << ",\n";
    f << "  \"topk\": " << top_k << ",\n";
    f << "  \"nprobe\": " << nprobe << ",\n";
    f << "  \"bits\": " << static_cast<int>(bits) << ",\n";
    f << "  \"legacy_d_k\": " << legacy_d_k << ",\n";
    f << "  \"safein_d_k\": " << safein_d_k << ",\n";
    f << "  \"safein_dk_space\": \"" << safein_dk_space << "\",\n";
    f << "  \"safeout_threshold_mode\": \"" << safeout_threshold_mode << "\",\n";
    f << "  \"candidates\": " << stats.candidates << ",\n";
    f << "  \"stage2_candidates\": " << stats.stage2_candidates << ",\n";
    f << "  \"true_topk_candidates\": " << stats.true_topk_candidates << ",\n";
    f << "  \"final_false_safeout\": " << stats.final_false_safeout << ",\n";
    const auto pct = [](uint64_t v, uint64_t denom) -> double {
        return denom == 0 ? 0.0 : 100.0 * static_cast<double>(v) / static_cast<double>(denom);
    };
    f << std::fixed << std::setprecision(6);
    f << "  \"ratios\": {\n";
    f << "    \"stage2_candidate_pct\": " << pct(stats.stage2_candidates, stats.candidates) << ",\n";
    f << "    \"s1_safein_pct\": " << pct(stats.s1_safein, stats.candidates) << ",\n";
    f << "    \"s1_safeout_pct\": " << pct(stats.s1_safeout, stats.candidates) << ",\n";
    f << "    \"s1_uncertain_pct\": " << pct(stats.s1_uncertain, stats.candidates) << ",\n";
    f << "    \"s2_safein_from_uncertain_pct\": " << pct(stats.s2_safein, stats.stage2_candidates) << ",\n";
    f << "    \"s2_safeout_from_uncertain_pct\": " << pct(stats.s2_safeout, stats.stage2_candidates) << ",\n";
    f << "    \"s2_uncertain_from_uncertain_pct\": " << pct(stats.s2_uncertain, stats.stage2_candidates) << ",\n";
    f << "    \"final_safein_pct\": " << pct(stats.final_safein, stats.candidates) << ",\n";
    f << "    \"final_safeout_pct\": " << pct(stats.final_safeout, stats.candidates) << ",\n";
    f << "    \"final_uncertain_pct\": " << pct(stats.final_uncertain, stats.candidates) << "\n";
    f << "  },\n";
    auto write_percentiles = [&](const char* key, const std::vector<float>& vals) {
        f << "  \"" << key << "\": {\n";
        f << "    \"p50\": " << Percentile(vals, 0.50f) << ",\n";
        f << "    \"p90\": " << Percentile(vals, 0.90f) << ",\n";
        f << "    \"p95\": " << Percentile(vals, 0.95f) << ",\n";
        f << "    \"p99\": " << Percentile(vals, 0.99f) << ",\n";
        f << "    \"p999\": " << Percentile(vals, 0.999f) << "\n";
        f << "  }";
    };
    write_percentiles("abs_err_s1", stats.abs_err_s1);
    f << ",\n";
    write_percentiles("abs_err_s2", stats.abs_err_s2);
    f << ",\n";
    write_percentiles("normalized_err_s1", stats.normalized_err_s1);
    f << ",\n";
    write_percentiles("normalized_err_s2", stats.normalized_err_s2);
    f << "\n}\n";
}

static float RecomputeMaxError(const std::vector<EstimateHeapEntry>& heap) {
    float max_error = 0.0f;
    for (const EstimateHeapEntry& entry : heap) {
        max_error = std::max(max_error, entry.error_bound);
    }
    return max_error;
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
    const std::string outdir = GetArg(argc, argv, "--outdir", "./diag_output");
    const uint32_t top_k = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    const uint32_t nprobe = static_cast<uint32_t>(GetIntArg(argc, argv, "--nprobe", 64));
    const uint32_t q_limit = static_cast<uint32_t>(GetIntArg(argc, argv, "--queries", 200));
    uint8_t bits = static_cast<uint8_t>(GetIntArg(argc, argv, "--bits", 0));
    const bool use_bruteforce_gt = gt_file.empty() && !HasFlag(argc, argv, "--no-bruteforce-gt");

    if (dataset_dir.empty() || index_dir.empty()) {
        std::fprintf(stderr,
                     "Usage: bench_rabitq_diagnostic --dataset <dir> --index-dir <dir> "
                     "[--base path] [--query path] [--centroids path] [--assignments path] "
                     "[--gt-file path] [--queries 200] [--topk 10] [--nprobe 64] [--bits 4] "
                     "[--outdir ./diag_output]\n");
        return 1;
    }

    fs::create_directories(outdir);
    if (base_path.empty()) base_path = DefaultBasePath(dataset_dir);
    if (query_path.empty()) query_path = DefaultQueryPath(dataset_dir);

    Log("=== RaBitQ Offline Diagnostic ===\n");
    Log("Dataset:   %s\n", dataset_dir.c_str());
    Log("Index:     %s\n", index_dir.c_str());
    Log("Base:      %s\n", base_path.c_str());
    Log("Query:     %s\n", query_path.c_str());
    Log("Outdir:    %s\n", outdir.c_str());

    IvfIndex index;
    Status s = index.Open(index_dir, false);
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to open index: %s\n", s.ToString().c_str());
        return 1;
    }
    if (bits == 0) bits = index.segment().rabitq_config().bits;
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
    } else if (use_bruteforce_gt) {
        Log("GT mode: brute-force exact top-%u over %u base vectors\n", top_k, N);
        for (uint32_t qi = 0; qi < Q; ++qi) {
            const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
            truth_sets[qi] = BuildTruthFromRows(ComputeExactTopKRows(q_vec, base, top_k), &image_ids);
        }
    } else {
        std::fprintf(stderr, "No GT mode available.\n");
        return 1;
    }

    std::vector<std::vector<uint32_t>> cluster_members(index.nlist());
    cluster_members.reserve(index.nlist());
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
    std::vector<float> per_cluster_r_max(index.nlist(), -1.0f);
    auto ensure_cluster_codes = [&](uint32_t cid) -> const std::vector<RaBitQCode>& {
        if (cluster_code_cache[cid]) return *cluster_code_cache[cid];
        auto codes_ptr = std::make_unique<std::vector<RaBitQCode>>();
        const auto& members = cluster_members[cid];
        codes_ptr->reserve(members.size());
        float r_max = 0.0f;
        const float* centroid = cents.data.data() + static_cast<size_t>(cid) * dim;
        for (uint32_t row : members) {
            RaBitQCode code = encoder.Encode(
                base.data.data() + static_cast<size_t>(row) * dim, centroid);
            r_max = std::max(r_max, code.norm);
            codes_ptr->push_back(std::move(code));
        }
        per_cluster_r_max[cid] = r_max;
        cluster_code_cache[cid] = std::move(codes_ptr);
        return *cluster_code_cache[cid];
    };

    std::ofstream per_candidate(outdir + "/per_candidate.csv");
    per_candidate
        << "query_id,cluster_rank,cluster_id,vector_row,vector_external_id,is_true_topk,"
        << "exact_dist,est_dist_s1,est_dist_s2,norm_qc,norm_oc,margin_s1,margin_s2_current,"
        << "d_k_static,dynamic_d_k_before_cluster,abs_err_s1,abs_err_s2,normalized_err_s1,normalized_err_s2,"
        << "safein_gap_s1,safein_gap_s2,safeout_gap_s1,safeout_gap_s2,safein_ratio_s1,safein_ratio_s2,"
        << "s1_class,s2_class,final_class\n";

    std::ofstream classification(outdir + "/classification.csv");
    classification
        << "query_id,vector_row,cluster_id,cluster_rank,exact_dist,est_dist_s1,est_dist_s2,"
        << "margin_s1,margin_s2_current,dynamic_d_k_before_cluster,s1_class,s2_class,final_class,is_true_topk\n";

    std::ofstream kth(outdir + "/kth_convergence.csv");
    kth << "query_id,probed_clusters,est_kth\n";

    SummaryStats stats;
    stats.abs_err_s1.reserve(static_cast<size_t>(Q) * nprobe * 1024u);
    stats.abs_err_s2.reserve(static_cast<size_t>(Q) * nprobe * 1024u / 4u);
    stats.normalized_err_s1.reserve(static_cast<size_t>(Q) * nprobe * 1024u);
    stats.normalized_err_s2.reserve(static_cast<size_t>(Q) * nprobe * 1024u / 4u);

    const float d_k_static = index.conann().safein_d_k();
    const float legacy_d_k = index.conann().legacy_d_k();
    const float eps_ip = index.conann().epsilon();
    const float margin_s2_divisor = static_cast<float>(1u << (bits - 1));

    auto run_start = std::chrono::steady_clock::now();
    for (uint32_t qi = 0; qi < Q; ++qi) {
        if (qi % 10 == 0) Log("query %u/%u\n", qi, Q);
        const float* q_vec = qry.data.data() + static_cast<size_t>(qi) * dim;
        const std::vector<ClusterID> probed = index.FindNearestClusters(q_vec, nprobe);
        std::vector<EstimateHeapEntry> est_heap;
        est_heap.reserve(top_k + 1);
        float max_error_in_est_heap = 0.0f;

        for (uint32_t rank = 0; rank < probed.size(); ++rank) {
            const uint32_t cid = probed[rank];
            const float* centroid = cents.data.data() + static_cast<size_t>(cid) * dim;
            PreparedQuery pq = estimator.PrepareQuery(q_vec, centroid, rotation);
            const auto& cluster_codes = ensure_cluster_codes(cid);
            const auto& members = cluster_members[cid];
            const float margin_factor = 2.0f * pq.norm_qc * eps_ip;
            const float safeout_frontier_upper =
                (est_heap.size() >= top_k)
                    ? est_heap.front().distance + max_error_in_est_heap
                    : std::numeric_limits<float>::infinity();

            for (size_t local_idx = 0; local_idx < members.size(); ++local_idx) {
                const uint32_t row = members[local_idx];
                const RaBitQCode& code = cluster_codes[local_idx];
                CandidateMetrics m;
                m.query_id = qi;
                m.cluster_rank = rank + 1;
                m.cluster_id = cid;
                m.vector_row = row;
                m.vector_external_id =
                    (row < image_ids.size()) ? image_ids[row] : static_cast<int64_t>(row);
                m.is_true_topk = !truth_sets[qi].row_ids.empty()
                    ? (truth_sets[qi].row_ids.count(row) > 0)
                    : (truth_sets[qi].external_ids.count(m.vector_external_id) > 0);
                m.norm_qc = pq.norm_qc;
                m.norm_oc = code.norm;
                m.d_k_static = d_k_static;
                m.dynamic_d_k_before_cluster = safeout_frontier_upper;
                m.exact_dist = simd::L2Sqr(q_vec,
                    base.data.data() + static_cast<size_t>(row) * dim, dim);
                m.est_dist_s1 = estimator.EstimateDistance(pq, code);
                m.margin_s1 = margin_factor * code.norm;
                m.abs_err_s1 = std::abs(m.est_dist_s1 - m.exact_dist);
                const float denom_s1 = 2.0f * std::max(1e-12f, pq.norm_qc * code.norm);
                m.normalized_err_s1 = m.abs_err_s1 / denom_s1;
                m.safein_gap_s1 = d_k_static - m.est_dist_s1;
                m.safeout_gap_s1 = m.est_dist_s1 - safeout_frontier_upper;
                m.safein_ratio_s1 = m.safein_gap_s1 / std::max(1e-12f, m.margin_s1);
                m.s1_class = index.conann().ClassifyAdaptive(
                    m.est_dist_s1, m.margin_s1, safeout_frontier_upper);
                m.final_class = m.s1_class;

                ++stats.candidates;
                if (m.is_true_topk) ++stats.true_topk_candidates;
                stats.abs_err_s1.push_back(m.abs_err_s1);
                stats.normalized_err_s1.push_back(m.normalized_err_s1);
                if (m.s1_class == ResultClass::SafeIn) ++stats.s1_safein;
                else if (m.s1_class == ResultClass::SafeOut) ++stats.s1_safeout;
                else ++stats.s1_uncertain;

                if (m.s1_class == ResultClass::Uncertain && bits > 1) {
                    m.stage2_evaluated = true;
                    m.est_dist_s2 = estimator.EstimateDistanceMultiBit(pq, code);
                    m.margin_s2 = m.margin_s1 / margin_s2_divisor;
                    m.abs_err_s2 = std::abs(m.est_dist_s2 - m.exact_dist);
                    m.normalized_err_s2 = m.abs_err_s2 / denom_s1;
                    m.safein_gap_s2 = d_k_static - m.est_dist_s2;
                    m.safeout_gap_s2 = m.est_dist_s2 - safeout_frontier_upper;
                    m.safein_ratio_s2 = m.safein_gap_s2 / std::max(1e-12f, m.margin_s2);
                    m.s2_class = index.conann().ClassifyAdaptive(
                        m.est_dist_s2, m.margin_s2, safeout_frontier_upper);
                    m.final_class = m.s2_class;

                    ++stats.stage2_candidates;
                    stats.abs_err_s2.push_back(m.abs_err_s2);
                    stats.normalized_err_s2.push_back(m.normalized_err_s2);
                    if (m.s2_class == ResultClass::SafeIn) ++stats.s2_safein;
                    else if (m.s2_class == ResultClass::SafeOut) ++stats.s2_safeout;
                    else ++stats.s2_uncertain;
                }

                if (m.final_class == ResultClass::SafeIn) ++stats.final_safein;
                else if (m.final_class == ResultClass::SafeOut) {
                    ++stats.final_safeout;
                    if (m.is_true_topk) ++stats.final_false_safeout;
                } else {
                    ++stats.final_uncertain;
                }

                WritePerCandidateRow(per_candidate, m);
                classification
                    << m.query_id << ',' << m.vector_row << ',' << m.cluster_id << ','
                    << m.cluster_rank << ',' << m.exact_dist << ',' << m.est_dist_s1 << ',';
                if (m.stage2_evaluated) classification << m.est_dist_s2;
                classification << ',' << m.margin_s1 << ',';
                if (m.stage2_evaluated) classification << m.margin_s2;
                classification << ',' << m.dynamic_d_k_before_cluster << ','
                               << ClassToStr(m.s1_class) << ','
                               << (m.stage2_evaluated ? ClassToStr(m.s2_class) : "") << ','
                               << ClassToStr(m.final_class) << ','
                               << (m.is_true_topk ? 1 : 0) << '\n';

                if (m.final_class != ResultClass::SafeOut) {
                    const EstimateHeapEntry estimate{
                        m.est_dist_s1, m.margin_s1, row};
                    if (est_heap.size() < top_k) {
                        est_heap.push_back(estimate);
                        std::push_heap(est_heap.begin(), est_heap.end());
                        max_error_in_est_heap =
                            std::max(max_error_in_est_heap, estimate.error_bound);
                    } else if (estimate.distance < est_heap.front().distance) {
                        std::pop_heap(est_heap.begin(), est_heap.end());
                        est_heap.back() = estimate;
                        std::push_heap(est_heap.begin(), est_heap.end());
                        max_error_in_est_heap = RecomputeMaxError(est_heap);
                    }
                }
            }

            const float est_kth = est_heap.size() >= top_k
                ? est_heap.front().distance
                : std::numeric_limits<float>::infinity();
            kth << qi << ',' << (rank + 1) << ',' << est_kth << '\n';
        }
    }

    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - run_start).count();
    Log("Finished in %.2f ms\n", elapsed_ms);

    WriteSummaryJson(outdir + "/summary.json",
                     fs::path(dataset_dir).filename().string(),
                     index_dir,
                     Q,
                     top_k,
                     nprobe,
                     bits,
                     legacy_d_k,
                     d_k_static,
                     index.conann().has_safein_d_k() ? "rabitq_s2_or_explicit" : "legacy_exact_l2",
                     "query_time_estimated_kth_plus_margin",
                     stats);

    Log("Wrote %s\n", (outdir + "/per_candidate.csv").c_str());
    Log("Wrote %s\n", (outdir + "/summary.json").c_str());
    return 0;
}
