#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "rabitq_bench_calibration.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/npy_reader.h"
#include "vdb/rabitq/rabitq_estimator.h"

using namespace vdb;
using namespace vdb::bench;
using namespace vdb::index;

namespace fs = std::filesystem;

namespace {

std::string GetStringArg(int argc, char* argv[], const char* name,
                         const std::string& default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return default_val;
}

int GetIntArg(int argc, char* argv[], const char* name, int default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    }
    return default_val;
}

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::fflush(stdout);
}

std::vector<uint32_t> SampleUnique(uint32_t total, uint32_t requested,
                                   uint64_t seed) {
    requested = std::min(total, requested);
    std::vector<uint32_t> ids(total);
    std::iota(ids.begin(), ids.end(), 0u);
    std::mt19937_64 rng(seed == 0 ? std::random_device{}() : seed);
    std::shuffle(ids.begin(), ids.end(), rng);
    ids.resize(requested);
    return ids;
}

std::vector<uint32_t> SampleWithReplacement(uint32_t total, uint32_t requested,
                                            uint64_t seed) {
    std::vector<uint32_t> ids;
    if (total == 0 || requested == 0) return ids;
    ids.reserve(requested);
    std::mt19937_64 rng(seed == 0 ? std::random_device{}() : seed);
    std::uniform_int_distribution<uint32_t> dist(0, total - 1);
    for (uint32_t i = 0; i < requested; ++i) {
        ids.push_back(dist(rng));
    }
    return ids;
}

float Percentile(std::vector<float> values, float p) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    const float idx = p * static_cast<float>(values.size() - 1);
    const size_t k = static_cast<size_t>(
        std::min(idx, static_cast<float>(values.size() - 1)));
    return values[k];
}

float ComputeQueryKth(IvfIndex& index,
                      const float* query,
                      Dim dim,
                      uint32_t topk,
                      uint32_t nprobe,
                      rabitq::RaBitQEstimator& estimator) {
    rabitq::PreparedQuery pq;
    rabitq::ClusterPreparedScratch scratch;
    std::priority_queue<float> heap;
    uint64_t candidate_count = 0;

    const auto clusters = index.FindNearestClusters(query, nprobe);
    for (ClusterID cid : clusters) {
        auto s = index.segment().EnsureClusterLoaded(cid);
        if (!s.ok()) continue;

        const uint32_t count = index.segment().GetNumRecords(cid);
        if (count == 0) {
            (void)index.segment().UnloadCluster(cid);
            continue;
        }
        std::vector<uint32_t> indices(count);
        std::iota(indices.begin(), indices.end(), 0u);

        std::vector<rabitq::RaBitQCode> codes;
        s = index.segment().LoadCodes(cid, indices, codes);
        if (!s.ok()) {
            (void)index.segment().UnloadCluster(cid);
            continue;
        }

        estimator.PrepareQueryInto(
            query, index.centroids().data() + static_cast<size_t>(cid) * dim,
            index.rotation(), &pq, &scratch);
        for (const auto& code : codes) {
            const float d = estimator.EstimateDistanceMultiBit(pq, code);
            if (heap.size() < topk) {
                heap.push(d);
            } else if (d < heap.top()) {
                heap.pop();
                heap.push(d);
            }
            ++candidate_count;
        }
        (void)index.segment().UnloadCluster(cid);
    }

    if (candidate_count < topk || heap.size() < topk) {
        return std::numeric_limits<float>::quiet_NaN();
    }
    return heap.top();
}

void WriteSummary(const std::string& path,
                  const std::string& dataset,
                  const std::string& index_dir,
                  uint32_t available_queries,
                  uint32_t requested_samples,
                  uint32_t effective_samples,
                  const std::string& sampling_mode,
                  uint32_t topk,
                  uint32_t nprobe,
                  uint64_t seed,
                  double generation_time_ms,
                  uint32_t valid_query_count,
                  uint32_t insufficient_count,
                  const std::vector<float>& samples) {
    std::ofstream f(path);
    f << "{\n";
    f << "  \"dataset\": \"" << dataset << "\",\n";
    f << "  \"index_dir\": \"" << index_dir << "\",\n";
    f << "  \"available_queries\": " << available_queries << ",\n";
    f << "  \"requested_samples\": " << requested_samples << ",\n";
    f << "  \"effective_samples\": " << effective_samples << ",\n";
    f << "  \"sampling_mode\": \"" << sampling_mode << "\",\n";
    f << "  \"topk\": " << topk << ",\n";
    f << "  \"safein_dk_nprobe\": " << nprobe << ",\n";
    f << "  \"seed\": " << seed << ",\n";
    f << "  \"generation_time_ms\": " << generation_time_ms << ",\n";
    f << "  \"valid_query_count\": " << valid_query_count << ",\n";
    f << "  \"insufficient_candidate_count\": " << insufficient_count << ",\n";
    f << "  \"min\": " << (samples.empty() ? 0.0f : *std::min_element(samples.begin(), samples.end())) << ",\n";
    f << "  \"p50\": " << Percentile(samples, 0.50f) << ",\n";
    f << "  \"p90\": " << Percentile(samples, 0.90f) << ",\n";
    f << "  \"p95\": " << Percentile(samples, 0.95f) << ",\n";
    f << "  \"p97\": " << Percentile(samples, 0.97f) << ",\n";
    f << "  \"p98\": " << Percentile(samples, 0.98f) << ",\n";
    f << "  \"p99\": " << Percentile(samples, 0.99f) << ",\n";
    f << "  \"max\": " << (samples.empty() ? 0.0f : *std::max_element(samples.begin(), samples.end())) << "\n";
    f << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dataset = GetStringArg(argc, argv, "--dataset", "");
    const std::string index_dir = GetStringArg(argc, argv, "--index-dir", "");
    const std::string output = GetStringArg(argc, argv, "--output", "/tmp/safein_dk_samples");
    const std::string samples_output =
        GetStringArg(argc, argv, "--samples-output", output + "/dk_samples.npy");
    const int queries_arg = GetIntArg(argc, argv, "--queries", 0);
    const uint32_t topk = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));
    const uint32_t requested_samples =
        static_cast<uint32_t>(GetIntArg(argc, argv, "--safein-dk-samples", 0));
    const uint32_t nprobe =
        static_cast<uint32_t>(GetIntArg(argc, argv, "--safein-dk-nprobe", 256));
    const uint64_t seed = static_cast<uint64_t>(GetIntArg(argc, argv, "--seed", 42));
    const std::string mode_arg =
        GetStringArg(argc, argv, "--safein-dk-sampling-mode", "unique");

    if (dataset.empty() || index_dir.empty()) {
        std::fprintf(stderr, "--dataset and --index-dir are required\n");
        return 1;
    }
    auto mode_or = ParseSafeInDkSamplingModeArg(mode_arg);
    if (!mode_or.ok()) {
        std::fprintf(stderr, "%s\n", mode_or.status().ToString().c_str());
        return 1;
    }
    const SafeInDkSamplingMode mode = mode_or.value();

    fs::create_directories(output);

    auto q_or = io::LoadNpyFloat32(dataset + "/query_embeddings.npy");
    if (!q_or.ok()) {
        std::fprintf(stderr, "Failed to load query_embeddings.npy: %s\n",
                     q_or.status().ToString().c_str());
        return 1;
    }
    auto& queries = q_or.value();
    const uint32_t q_total = queries.rows;
    const uint32_t q = (queries_arg > 0 && static_cast<uint32_t>(queries_arg) < q_total)
        ? static_cast<uint32_t>(queries_arg)
        : q_total;
    const uint32_t sample_count =
        requested_samples > 0 ? requested_samples : q;
    const Dim dim = static_cast<Dim>(queries.cols);

    IvfIndex index;
    auto s = index.Open(index_dir, false);
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to open index: %s\n", s.ToString().c_str());
        return 1;
    }
    const uint8_t bits = index.segment().cluster_reader().rabitq_config().bits;
    if (bits <= 1) {
        std::fprintf(stderr, "Stage2 SafeIn d_k samples require bits > 1\n");
        return 1;
    }

    std::vector<uint32_t> sampled = (mode == SafeInDkSamplingMode::WithReplacement)
        ? SampleWithReplacement(q, sample_count, seed)
        : SampleUnique(q, sample_count, seed);
    if (sampled.empty()) {
        std::fprintf(stderr, "No query samples selected\n");
        return 1;
    }

    std::unordered_map<uint32_t, uint32_t> multiplicity;
    multiplicity.reserve(sampled.size());
    for (uint32_t qi : sampled) ++multiplicity[qi];

    Log("SafeIn d_k generator: queries=%u/%u requested_samples=%u effective_unique=%zu mode=%s nprobe=%u\n",
        q, q_total, sample_count, multiplicity.size(), mode_arg.c_str(), nprobe);

    auto start = std::chrono::steady_clock::now();
    rabitq::RaBitQEstimator estimator(dim, bits);
    std::vector<float> samples;
    samples.reserve(sampled.size());
    uint32_t valid_unique = 0;
    uint32_t insufficient = 0;
    uint32_t done = 0;

    for (const auto& kv : multiplicity) {
        const uint32_t qi = kv.first;
        const uint32_t mult = kv.second;
        const float* query = queries.data.data() + static_cast<size_t>(qi) * dim;
        const float kth = ComputeQueryKth(index, query, dim, topk, nprobe, estimator);
        if (std::isfinite(kth)) {
            ++valid_unique;
            for (uint32_t i = 0; i < mult; ++i) samples.push_back(kth);
        } else {
            ++insufficient;
        }
        ++done;
        if (done % 10 == 0 || done == multiplicity.size()) {
            Log("  processed unique query %u/%zu samples=%zu\n",
                done, multiplicity.size(), samples.size());
        }
    }

    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    if (samples.empty()) {
        std::fprintf(stderr, "No valid SafeIn d_k samples generated\n");
        return 1;
    }

    s = io::SaveNpyFloat32Vector(samples_output, samples);
    if (!s.ok()) {
        std::fprintf(stderr, "Failed to write samples: %s\n", s.ToString().c_str());
        return 1;
    }
    const std::string summary_path = output + "/dk_samples_summary.json";
    WriteSummary(summary_path, dataset, index_dir, q, sample_count,
                 static_cast<uint32_t>(samples.size()), mode_arg, topk, nprobe,
                 seed, elapsed_ms, valid_unique, insufficient, samples);

    Log("Wrote %s\n", samples_output.c_str());
    Log("Wrote %s\n", summary_path.c_str());
    Log("Samples: count=%zu min=%.6f p90=%.6f p95=%.6f p97=%.6f p98=%.6f p99=%.6f max=%.6f time=%.1f ms\n",
        samples.size(), *std::min_element(samples.begin(), samples.end()),
        Percentile(samples, 0.90f), Percentile(samples, 0.95f),
        Percentile(samples, 0.97f), Percentile(samples, 0.98f),
        Percentile(samples, 0.99f), *std::max_element(samples.begin(), samples.end()),
        elapsed_ms);
    return 0;
}
