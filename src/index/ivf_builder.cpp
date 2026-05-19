#include "vdb/index/ivf_builder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "superkmeans/superkmeans.h"
#include "superkmeans/hierarchical_superkmeans.h"

#include "vdb/index/conann.h"
#include "vdb/index/crc_calibrator.h"
#include "vdb/index/faiss_coarse_builder.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/fastscan.h"
#include "vdb/simd/popcount.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/pack_codes.h"
#include "vdb/storage/cluster_store.h"
#include "vdb/storage/data_file_writer.h"

// FlatBuffers generated header
#include "segment_meta_generated.h"

namespace vdb {
namespace index {

namespace {

constexpr uint32_t kDefaultFaissNiter = 10;

bool IsPowerOf2(uint32_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

uint32_t NextPowerOf2(uint32_t n) {
    if (n == 0) return 1;
    if (IsPowerOf2(n)) return n;
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

std::vector<float> PadRowsToDim(const float* vectors, uint32_t rows,
                                uint32_t old_dim, uint32_t new_dim) {
    std::vector<float> padded(static_cast<size_t>(rows) * new_dim, 0.0f);
    for (uint32_t i = 0; i < rows; ++i) {
        std::memcpy(padded.data() + static_cast<size_t>(i) * new_dim,
                    vectors + static_cast<size_t>(i) * old_dim,
                    static_cast<size_t>(old_dim) * sizeof(float));
    }
    return padded;
}

std::string JsonUIntArray(const std::vector<uint32_t>& values) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) oss << ", ";
        oss << values[i];
    }
    oss << "]";
    return oss.str();
}

void NormalizeRows(std::vector<float>* matrix, uint32_t rows, uint32_t dim) {
    for (uint32_t i = 0; i < rows; ++i) {
        float* row = matrix->data() + static_cast<size_t>(i) * dim;
        float norm_sq = 0.0f;
        for (uint32_t j = 0; j < dim; ++j) {
            norm_sq += row[j] * row[j];
        }
        const float norm = std::sqrt(norm_sq);
        if (norm <= 0.0f) {
            continue;
        }
        for (uint32_t j = 0; j < dim; ++j) {
            row[j] /= norm;
        }
    }
}

const char* CoarseBuilderName(CoarseBuilder builder) {
    switch (builder) {
        case CoarseBuilder::SuperKMeans:
            return "superkmeans";
        case CoarseBuilder::HierarchicalSuperKMeans:
            return "hierarchical_superkmeans";
        case CoarseBuilder::FaissKMeans:
            return "faiss_kmeans";
        case CoarseBuilder::Auto:
        default:
            return "auto";
    }
}

const char* AssignmentModeName(AssignmentMode mode) {
    switch (mode) {
        case AssignmentMode::RedundantTop2Naive:
            return "redundant_top2_naive";
        case AssignmentMode::RedundantTop2Rair:
            return "redundant_top2_rair";
        case AssignmentMode::Single:
        default:
            return "single";
    }
}

const char* ClusteringSourceName(ClusteringSource source) {
    switch (source) {
        case ClusteringSource::Precomputed:
            return "precomputed";
        case ClusteringSource::Auto:
        default:
            return "auto";
    }
}

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

IvfBuilder::IvfBuilder(const IvfBuilderConfig& config) : config_(config) {}
IvfBuilder::~IvfBuilder() = default;

// ============================================================================
// Build — orchestrate the full build pipeline
// ============================================================================

Status IvfBuilder::Build(const float* vectors, uint32_t N, Dim dim,
                         const std::string& output_dir,
                         PayloadFn payload_fn) {
    if (!vectors || N == 0 || dim == 0) {
        return Status::InvalidArgument("vectors, N, or dim is zero/null");
    }
    if (config_.nlist == 0) {
        return Status::InvalidArgument("nlist must be > 0");
    }
    if (config_.nlist > N) {
        return Status::InvalidArgument("nlist must be <= N");
    }
    if (config_.assignment_factor != 1 && config_.assignment_factor != 2) {
        return Status::InvalidArgument("assignment_factor must be 1 or 2");
    }

    if (config_.assignment_mode == AssignmentMode::RedundantTop2Rair &&
        config_.assignment_factor != 2) {
        return Status::InvalidArgument(
            "assignment_mode=redundant_top2_rair requires assignment_factor=2");
    }

    logical_dim_ = dim;
    effective_dim_ = dim;
    padding_mode_ = "none";
    rotation_mode_ = "random_matrix";

    std::vector<float> padded_vectors;
    std::vector<float> padded_calibration_queries;
    const float* build_vectors = vectors;
    const float* calibration_queries = config_.calibration_queries;
    Dim build_dim = dim;

    if (config_.pad_non_power_of_two_to_pow2 && !IsPowerOf2(dim)) {
        effective_dim_ = static_cast<Dim>(NextPowerOf2(dim));
        build_dim = effective_dim_;
        padding_mode_ = "zero_pad_to_pow2";
        padded_vectors = PadRowsToDim(vectors, N, dim, build_dim);
        build_vectors = padded_vectors.data();
        if (config_.calibration_queries != nullptr &&
            config_.num_calibration_queries > 0) {
            padded_calibration_queries = PadRowsToDim(
                config_.calibration_queries,
                config_.num_calibration_queries,
                dim,
                build_dim);
            calibration_queries = padded_calibration_queries.data();
        }
    }

    // Phase A: K-means clustering
    auto s = RunKMeans(build_vectors, N, build_dim);
    if (!s.ok()) return s;

    assignment_mode_ = config_.assignment_mode;
    if (assignment_mode_ == AssignmentMode::Single &&
        config_.assignment_factor == 2) {
        assignment_mode_ = AssignmentMode::RedundantTop2Naive;
    }
    rair_lambda_ = config_.rair_lambda;
    rair_strict_second_choice_ = config_.rair_strict_second_choice;

    s = DeriveSecondaryAssignments(build_vectors, N, build_dim);
    if (!s.ok()) return s;

    // Phase B: calibrate ConANN d_k
    const float* saved_queries = config_.calibration_queries;
    config_.calibration_queries = calibration_queries;
    CalibrateDk(build_vectors, N, build_dim);
    config_.calibration_queries = saved_queries;

    // Phase C+D: write everything to disk
    s = WriteIndex(vectors, build_vectors, N, build_dim, output_dir,
                   std::move(payload_fn));
    return s;
}

// ============================================================================
// Phase A: Clustering (SuperKMeans or precomputed)
// ============================================================================

Status IvfBuilder::RunKMeans(const float* vectors, uint32_t N, Dim dim) {
    const uint32_t K = config_.nlist;
    clustering_source_ = ClusteringSource::Auto;
    coarse_builder_used_ = config_.coarse_builder;
    effective_metric_ = config_.metric;
    const bool normalize_for_cosine = (config_.metric == "cosine");

    // Path 1: Load precomputed centroids and assignments
    if (!config_.centroids_path.empty() && !config_.assignments_path.empty()) {
        auto c_or = io::LoadVectors(config_.centroids_path);
        if (!c_or.ok()) {
            return Status::IOError("Failed to load centroids: " +
                                   c_or.status().ToString());
        }
        auto& c = c_or.value();
        if (c.cols == dim) {
            centroids_.assign(c.data.begin(), c.data.end());
        } else if (config_.pad_non_power_of_two_to_pow2 &&
                   c.cols == logical_dim_ &&
                   dim == effective_dim_) {
            centroids_ = PadRowsToDim(c.data.data(), c.rows, c.cols, dim);
        } else {
            return Status::InvalidArgument("Centroid dim mismatch");
        }

        auto a_or = io::LoadIvecs(config_.assignments_path);
        if (!a_or.ok()) {
            return Status::IOError("Failed to load assignments: " +
                                   a_or.status().ToString());
        }
        auto& a = a_or.value();
        if (a.rows != N) {
            return Status::InvalidArgument("Assignment count mismatch");
        }
        assignments_.resize(N);
        for (uint32_t i = 0; i < N; ++i) {
            assignments_[i] = static_cast<uint32_t>(a.data[i]);
        }
        clustering_source_ = ClusteringSource::Precomputed;
        if (config_.metric == "cosine") {
            effective_metric_ = "ip";
        } else if (config_.metric == "ip") {
            effective_metric_ = "ip";
        } else {
            effective_metric_ = "l2";
        }
        if (coarse_builder_used_ == CoarseBuilder::Auto) {
            coarse_builder_used_ = CoarseBuilder::HierarchicalSuperKMeans;
        }
        return Status::OK();
    }

    // Path 2: Automatic clustering.
    // Auto preserves the current repository behavior.
    if (config_.coarse_builder == CoarseBuilder::FaissKMeans) {
        FaissCoarseBuildConfig faiss_cfg;
        faiss_cfg.nlist = K;
        faiss_cfg.dim = dim;
        faiss_cfg.train_size = config_.faiss_train_size;
        faiss_cfg.niter = (config_.faiss_niter == 0)
            ? kDefaultFaissNiter
            : config_.faiss_niter;
        faiss_cfg.nredo = config_.faiss_nredo;
        faiss_cfg.seed = config_.seed;
        faiss_cfg.metric = config_.metric;

        FaissCoarseBuildResult faiss_result;
        VDB_RETURN_IF_ERROR(
            RunFaissCoarseBuilder(vectors, N, faiss_cfg, &faiss_result));
        centroids_ = std::move(faiss_result.centroids);
        assignments_ = std::move(faiss_result.assignments);
        effective_metric_ = std::move(faiss_result.effective_metric);
        coarse_builder_used_ = CoarseBuilder::FaissKMeans;
        clustering_source_ = ClusteringSource::Auto;

        if (!config_.save_centroids_path.empty()) {
            std::ofstream f(config_.save_centroids_path, std::ios::binary);
            if (!f.is_open()) {
                return Status::IOError("Failed to create " + config_.save_centroids_path);
            }
            const uint32_t d32 = dim;
            for (uint32_t i = 0; i < K; ++i) {
                f.write(reinterpret_cast<const char*>(&d32), 4);
                f.write(reinterpret_cast<const char*>(
                    centroids_.data() + static_cast<size_t>(i) * dim),
                    dim * sizeof(float));
            }
        }
        if (!config_.save_assignments_path.empty()) {
            std::ofstream f(config_.save_assignments_path, std::ios::binary);
            if (!f.is_open()) {
                return Status::IOError("Failed to create " + config_.save_assignments_path);
            }
            const uint32_t one = 1;
            for (uint32_t i = 0; i < N; ++i) {
                f.write(reinterpret_cast<const char*>(&one), 4);
                f.write(reinterpret_cast<const char*>(&assignments_[i]), 4);
            }
        }
        return Status::OK();
    }

    bool use_hierarchical = false;
    if (config_.coarse_builder == CoarseBuilder::Auto) {
        use_hierarchical = (K >= 128);
        coarse_builder_used_ = use_hierarchical
            ? CoarseBuilder::HierarchicalSuperKMeans
            : CoarseBuilder::SuperKMeans;
    } else {
        use_hierarchical =
            (config_.coarse_builder == CoarseBuilder::HierarchicalSuperKMeans);
        coarse_builder_used_ = config_.coarse_builder;
    }

    std::vector<float> normalized_vectors;
    const float* clustering_vectors = vectors;
    if (normalize_for_cosine) {
        normalized_vectors.assign(
            vectors, vectors + static_cast<size_t>(N) * dim);
        NormalizeRows(&normalized_vectors, N, dim);
        clustering_vectors = normalized_vectors.data();
    }

    if (use_hierarchical) {
        skmeans::HierarchicalSuperKMeansConfig hskm_cfg;
        hskm_cfg.iters_mesoclustering = 5;
        hskm_cfg.iters_fineclustering = config_.max_iterations;
        hskm_cfg.iters_refinement = 3;
        hskm_cfg.seed = static_cast<uint32_t>(config_.seed);
        hskm_cfg.verbose = false;
        hskm_cfg.tol = config_.tolerance;

        auto hskm = skmeans::HierarchicalSuperKMeans(K, dim, hskm_cfg);
        auto c = hskm.Train(clustering_vectors, N);
        auto a = hskm.Assign(clustering_vectors, c.data(), N, K);

        centroids_.assign(c.begin(), c.end());
        assignments_.assign(a.begin(), a.end());
        effective_metric_ = normalize_for_cosine ? "ip" : "l2";
    } else {
        skmeans::SuperKMeansConfig skm_cfg;
        skm_cfg.iters = config_.max_iterations;
        skm_cfg.seed = static_cast<uint32_t>(config_.seed);
        skm_cfg.verbose = false;
        skm_cfg.early_termination = true;
        skm_cfg.tol = config_.tolerance;

        auto skm = skmeans::SuperKMeans(K, dim, skm_cfg);
        auto c = skm.Train(clustering_vectors, N);
        auto a = skm.Assign(clustering_vectors, c.data(), N, K);

        centroids_.assign(c.begin(), c.end());
        assignments_.assign(a.begin(), a.end());
        effective_metric_ = normalize_for_cosine ? "ip" : "l2";
    }

    // Save clustering results if output paths are configured
    if (!config_.save_centroids_path.empty()) {
        std::ofstream f(config_.save_centroids_path, std::ios::binary);
        if (!f.is_open()) {
            return Status::IOError("Failed to create " + config_.save_centroids_path);
        }
        const uint32_t d32 = dim;
        for (uint32_t i = 0; i < K; ++i) {
            f.write(reinterpret_cast<const char*>(&d32), 4);
            f.write(reinterpret_cast<const char*>(
                centroids_.data() + static_cast<size_t>(i) * dim),
                dim * sizeof(float));
        }
    }
    if (!config_.save_assignments_path.empty()) {
        std::ofstream f(config_.save_assignments_path, std::ios::binary);
        if (!f.is_open()) {
            return Status::IOError("Failed to create " + config_.save_assignments_path);
        }
        const uint32_t one = 1;
        for (uint32_t i = 0; i < N; ++i) {
            f.write(reinterpret_cast<const char*>(&one), 4);
            f.write(reinterpret_cast<const char*>(&assignments_[i]), 4);
        }
    }

    return Status::OK();
}

Status IvfBuilder::DeriveSecondaryAssignments(const float* vectors,
                                              uint32_t N,
                                              Dim dim) {
    secondary_assignments_.assign(N, std::numeric_limits<uint32_t>::max());
    if (!(assignment_mode_ == AssignmentMode::Single ||
          config_.assignment_factor == 1)) {
        std::vector<float> normalized_vectors;
        const float* assignment_vectors = vectors;
        if (config_.metric == "cosine") {
            normalized_vectors.assign(
                vectors, vectors + static_cast<size_t>(N) * dim);
            NormalizeRows(&normalized_vectors, N, dim);
            assignment_vectors = normalized_vectors.data();
        }
        const uint32_t K = config_.nlist;
#pragma omp parallel for schedule(static)
        for (int64_t i = 0; i < static_cast<int64_t>(N); ++i) {
            const float* vec = assignment_vectors + static_cast<size_t>(i) * dim;
            float best_dist = std::numeric_limits<float>::max();
            float second_dist = std::numeric_limits<float>::max();
            uint32_t best_cluster = std::numeric_limits<uint32_t>::max();
            uint32_t second_cluster = std::numeric_limits<uint32_t>::max();

            for (uint32_t k = 0; k < K; ++k) {
                float d = simd::L2Sqr(vec,
                                      centroids_.data() + static_cast<size_t>(k) * dim,
                                      dim);
                if (d < best_dist) {
                    second_dist = best_dist;
                    second_cluster = best_cluster;
                    best_dist = d;
                    best_cluster = k;
                } else if (d < second_dist) {
                    second_dist = d;
                    second_cluster = k;
                }
            }

            if (best_cluster == std::numeric_limits<uint32_t>::max()) {
                continue;
            }

            const uint32_t primary = assignments_[static_cast<size_t>(i)];
            if (assignment_mode_ == AssignmentMode::RedundantTop2Naive) {
                if (primary == best_cluster) {
                    secondary_assignments_[static_cast<size_t>(i)] = second_cluster;
                } else if (primary == second_cluster) {
                    secondary_assignments_[static_cast<size_t>(i)] = best_cluster;
                } else {
                    secondary_assignments_[static_cast<size_t>(i)] = best_cluster;
                }
                continue;
            }

            const float* primary_centroid =
                centroids_.data() + static_cast<size_t>(primary) * dim;
            float best_air_loss = std::numeric_limits<float>::max();
            uint32_t best_air_cluster = std::numeric_limits<uint32_t>::max();

            for (uint32_t k = 0; k < K; ++k) {
                if (k == primary && rair_strict_second_choice_) {
                    continue;
                }

                const float* cand_centroid =
                    centroids_.data() + static_cast<size_t>(k) * dim;
                float residual_ip = 0.0f;
                float cand_dist = 0.0f;
                for (Dim j = 0; j < dim; ++j) {
                    const float r = primary_centroid[j] - vec[j];
                    const float rp = cand_centroid[j] - vec[j];
                    residual_ip += r * rp;
                    cand_dist += rp * rp;
                }
                const float loss = cand_dist + rair_lambda_ * residual_ip;
                if (loss < best_air_loss) {
                    best_air_loss = loss;
                    best_air_cluster = k;
                }
            }

            if (!rair_strict_second_choice_ && best_air_cluster == primary) {
                continue;
            }
            secondary_assignments_[static_cast<size_t>(i)] = best_air_cluster;
        }
    }

    if (!config_.save_secondary_assignments_path.empty()) {
        std::ofstream f(config_.save_secondary_assignments_path, std::ios::binary);
        if (!f.is_open()) {
            return Status::IOError(
                "Failed to create " + config_.save_secondary_assignments_path);
        }
        const uint32_t one = 1;
        for (uint32_t i = 0; i < N; ++i) {
            int32_t value = static_cast<int32_t>(
                secondary_assignments_[i] == std::numeric_limits<uint32_t>::max()
                    ? -1
                    : static_cast<int32_t>(secondary_assignments_[i]));
            f.write(reinterpret_cast<const char*>(&one), 4);
            f.write(reinterpret_cast<const char*>(&value), 4);
        }
    }

    return Status::OK();
}

// ============================================================================
// Phase B: Calibrate ConANN d_k
// ============================================================================

void IvfBuilder::CalibrateDk(const float* vectors, uint32_t N, Dim dim) {
    if (config_.calibration_queries != nullptr &&
        config_.num_calibration_queries > 0) {
        // Cross-modal: calibrate d_k from query→database distances
        calibrated_dk_ = ConANN::CalibrateDistanceThreshold(
            config_.calibration_queries, config_.num_calibration_queries,
            vectors, N, dim,
            config_.calibration_samples,
            config_.calibration_topk,
            config_.calibration_percentile,
            config_.seed);
    } else {
        // Same-modal fallback: database self-sampling
        calibrated_dk_ = ConANN::CalibrateDistanceThreshold(
            vectors, N, dim,
            config_.calibration_samples,
            config_.calibration_topk,
            config_.calibration_percentile,
            config_.seed);
    }
}

// ============================================================================
// CalibrateEpsilonIp — distance-error calibration for FastScan classification
// ============================================================================
//
// Measures |dist_fastscan - dist_true| / (2 * norm_oc * norm_qc) to calibrate
// eps_ip for the FastScan-based S1 classification path.
//
// The resulting eps_ip is used in the margin formula:
//   margin = 2 · r_max · norm_qc · eps_ip

static float CalibrateEpsilonIp(
    const std::vector<std::vector<rabitq::RaBitQCode>>& all_codes,
    const std::vector<std::vector<uint32_t>>& cluster_members,
    const float* vectors,
    const float* centroids,
    const rabitq::RotationMatrix& rotation,
    Dim dim,
    uint32_t K,
    uint32_t max_samples_per_cluster,
    float percentile,
    uint64_t seed,
    float d_k,
    uint8_t bits = 1) {

    const uint32_t packed_sz = storage::FastScanPackedSize(dim);

    // Truncation: only sample pairs with true_dist in [lo, hi] around d_k
    const float lo = 0.1f * d_k;
    const float hi = 10.0f * d_k;

    // Thread-local error vectors (merged at the end).
    // Avoids contention on a shared vector::push_back and matches the pattern
    // used by eps-calibration-simd in bench_vector_search.
#ifdef _OPENMP
    const int nt = omp_get_max_threads();
#else
    const int nt = 1;
#endif
    std::vector<std::vector<float>> tl_errors(nt);

#pragma omp parallel
    {
#ifdef _OPENMP
        int tid = omp_get_thread_num();
#else
        int tid = 0;
#endif
        auto& local_errors = tl_errors[tid];
        local_errors.reserve(max_samples_per_cluster * 300);

        // Per-thread estimator and packed_codes buffer (no false sharing,
        // no per-iteration malloc in the hot loop).
        rabitq::RaBitQEstimator estimator(dim, bits);
        std::vector<uint8_t> packed_codes(packed_sz, 0);

#pragma omp for schedule(dynamic, 16)
        for (int k = 0; k < static_cast<int>(K); ++k) {
            const auto& members = cluster_members[static_cast<size_t>(k)];
            const auto& codes = all_codes[static_cast<size_t>(k)];
            const uint32_t n_members = static_cast<uint32_t>(members.size());
            if (n_members < 2) continue;

            uint32_t n_queries = max_samples_per_cluster;
            if (n_members < 2 * max_samples_per_cluster) {
                n_queries = std::max(n_members / 2, 1u);
            }
            n_queries = std::min(n_queries, n_members);

            std::vector<uint32_t> indices(n_members);
            std::iota(indices.begin(), indices.end(), 0u);
            std::mt19937 rng(seed + static_cast<uint32_t>(k));
            std::shuffle(indices.begin(), indices.end(), rng);

            const float* centroid = centroids +
                static_cast<size_t>(k) * dim;

            for (uint32_t q = 0; q < n_queries; ++q) {
                const uint32_t q_idx = indices[q];
                const float* query = vectors +
                    static_cast<size_t>(members[q_idx]) * dim;

                auto pq = estimator.PrepareQuery(query, centroid, rotation);

                for (uint32_t t_base = 0; t_base < n_members; t_base += 32) {
                    uint32_t block_count = std::min(32u, n_members - t_base);

                    storage::PackSignBitsForFastScan(
                        &codes[t_base], block_count, dim, packed_codes.data());

                    float block_norms[32] = {};
                    for (uint32_t j = 0; j < block_count; ++j)
                        block_norms[j] = codes[t_base + j].norm;

                    alignas(64) float fs_dists[32];
                    estimator.EstimateDistanceFastScan(
                        pq, packed_codes.data(), block_norms, block_count, fs_dists);

                    for (uint32_t j = 0; j < block_count; ++j) {
                        uint32_t t = t_base + j;
                        if (t == q_idx) continue;

                        uint32_t global_id = members[t];
                        float true_dist = simd::L2Sqr(
                            query, vectors + static_cast<size_t>(global_id) * dim, dim);

                        // Truncate: skip pairs far from decision boundary
                        if (true_dist < lo || true_dist > hi) continue;

                        float dist_err = std::abs(fs_dists[j] - true_dist);
                        float denom = 2.0f * block_norms[j] * pq.norm_qc;
                        if (denom > 1e-10f) {
                            local_errors.push_back(dist_err / denom);
                        }
                    }
                }
            }
        }
    }

    // Merge thread-local error vectors.
    std::vector<float> normalized_errors;
    {
        size_t total = 0;
        for (const auto& v : tl_errors) total += v.size();
        normalized_errors.reserve(total);
        for (auto& v : tl_errors)
            normalized_errors.insert(normalized_errors.end(), v.begin(), v.end());
    }

    if (normalized_errors.empty()) return 0.0f;

    std::sort(normalized_errors.begin(), normalized_errors.end());
    float findex = percentile * static_cast<float>(normalized_errors.size() - 1);
    auto idx = static_cast<size_t>(std::min(
        findex, static_cast<float>(normalized_errors.size() - 1)));
    return normalized_errors[idx];
}

// ============================================================================
// Phase C+D: Write per-cluster files + global metadata
// ============================================================================

Status IvfBuilder::WriteIndex(const float* raw_vectors,
                              const float* encoded_vectors,
                              uint32_t N,
                              Dim dim,
                              const std::string& output_dir,
                              PayloadFn payload_fn) {
    const uint32_t K = config_.nlist;

    // --- Create output directory ---
    std::filesystem::create_directories(output_dir);

    // --- Generate rotation matrix ---
    // Prefer Hadamard rotation when dim is a power of 2 (O(L log L) Apply via
    // FWHT). Falls back to random Gaussian QR for non-power-of-2 dims.
    // Matches the selection strategy in bench_vector_search and aligns
    // bench_e2e recall with the direct-memory benchmark.
    rabitq::RotationMatrix rotation(dim);
    bool used_hadamard = false;
    if (dim > 0 && (dim & (dim - 1)) == 0) {
        used_hadamard = rotation.GenerateHadamard(config_.seed,
                                                  /*use_fast_transform=*/true);
        if (used_hadamard) {
            rotation_mode_ = (logical_dim_ != dim) ? "hadamard_padded" : "hadamard";
        }
    } else if (!config_.pad_non_power_of_two_to_pow2 &&
               config_.use_blocked_hadamard_permuted) {
        used_hadamard = rotation.GenerateBlockedHadamardPermuted(
            config_.seed, /*use_fast_transform=*/true);
        if (used_hadamard) {
            rotation_mode_ = "blocked_hadamard_permuted";
        }
    }

    if (!used_hadamard) {
        rotation.GenerateRandom(config_.seed);
        rotation_mode_ = "random_matrix";
    }
    auto s = rotation.Save(output_dir + "/rotation.bin");
    if (!s.ok()) return s;

    // --- Phase 2.1: Write rotated_centroids.bin (Hadamard path only) ---
    if (used_hadamard) {
        const std::string rc_path = output_dir + "/rotated_centroids.bin";
        std::vector<float> rot_cents(static_cast<size_t>(K) * dim);
        for (uint32_t k = 0; k < K; ++k) {
            const float* c = centroids_.data() + static_cast<size_t>(k) * dim;
            rotation.Apply(c, rot_cents.data() + static_cast<size_t>(k) * dim);
        }
        std::ofstream rc_file(rc_path, std::ios::binary);
        if (!rc_file.is_open()) {
            return Status::IOError("Failed to create rotated_centroids.bin: " + rc_path);
        }
        rc_file.write(reinterpret_cast<const char*>(rot_cents.data()),
                      static_cast<std::streamsize>(K) * dim * sizeof(float));
        if (!rc_file.good()) {
            return Status::IOError("Failed to write rotated_centroids.bin");
        }
    }

    // --- Write centroids.bin ---
    {
        const std::string path = output_dir + "/centroids.bin";
        std::ofstream f(path, std::ios::binary);
        if (!f.is_open()) {
            return Status::IOError("Failed to create centroids.bin: " + path);
        }
        f.write(reinterpret_cast<const char*>(centroids_.data()),
                static_cast<std::streamsize>(K) * dim * sizeof(float));
        if (!f.good()) {
            return Status::IOError("Failed to write centroids.bin");
        }
    }

    // --- Group vectors by cluster ---
    std::vector<std::vector<uint32_t>> cluster_members(K);
    for (uint32_t i = 0; i < N; ++i) {
        cluster_members[assignments_[i]].push_back(i);
        if (config_.assignment_factor == 2 &&
            secondary_assignments_[i] != std::numeric_limits<uint32_t>::max() &&
            secondary_assignments_[i] != assignments_[i]) {
            cluster_members[secondary_assignments_[i]].push_back(i);
        }
    }

    // --- Build encoder ---
    rabitq::RaBitQEncoder encoder(dim, rotation, config_.rabitq.bits);

    // =======================================================================
    // Unified file writing: single data.dat + single cluster.clu
    // =======================================================================

    const std::string dat_path = output_dir + "/data.dat";
    const std::string clu_path = output_dir + "/cluster.clu";

    // --- Phase 1: write all records into one DataFileWriter ---
    storage::DataFileWriter dat_writer;
    s = dat_writer.Open(dat_path, 0, logical_dim_, config_.payload_schemas,
                        config_.page_size);
    if (!s.ok()) return s;

    // Write each original vector exactly once. Cluster postings reuse the
    // same AddressEntry so duplicated assignments do not duplicate data.dat.
    std::vector<AddressEntry> addr_by_vec_id(N);

    for (uint32_t idx = 0; idx < N; ++idx) {
        const float* vec =
            raw_vectors + static_cast<size_t>(idx) * logical_dim_;
        auto pl = payload_fn ? payload_fn(idx) : std::vector<Datum>{};
        s = dat_writer.WriteRecord(vec, pl, addr_by_vec_id[idx]);
        if (!s.ok()) return s;
    }

    // addr_entries_per_cluster[k] = address entries for cluster k
    std::vector<std::vector<AddressEntry>> addr_entries_per_cluster(K);

    for (uint32_t k = 0; k < K; ++k) {
        const auto& members = cluster_members[k];
        addr_entries_per_cluster[k].reserve(members.size());

        for (uint32_t idx : members) {
            addr_entries_per_cluster[k].push_back(addr_by_vec_id[idx]);
        }
    }

    s = dat_writer.Finalize();
    if (!s.ok()) return s;

    // --- Phase 2a: RaBitQ encode all clusters, compute r_max ---
    std::vector<std::vector<rabitq::RaBitQCode>> all_codes(K);
    std::vector<float> r_max_per_cluster(K, 0.0f);

    for (uint32_t k = 0; k < K; ++k) {
        const auto& members = cluster_members[k];
        const uint32_t n_members = static_cast<uint32_t>(members.size());
        const float* centroid = centroids_.data() + static_cast<size_t>(k) * dim;

        // Gather member vectors contiguously
        std::vector<float> member_vecs(static_cast<size_t>(n_members) * dim);
        for (uint32_t m = 0; m < n_members; ++m) {
            std::memcpy(member_vecs.data() + static_cast<size_t>(m) * dim,
                         encoded_vectors + static_cast<size_t>(members[m]) * dim,
                         dim * sizeof(float));
        }

        all_codes[k] = encoder.EncodeBatch(member_vecs.data(), n_members, centroid);

        // r_max = max(‖o-c‖) in this cluster
        for (const auto& code : all_codes[k]) {
            r_max_per_cluster[k] = std::max(r_max_per_cluster[k], code.norm);
        }
    }

    // --- Phase 2b: calibrate global ε_ip ---
    // Distance-error calibration: |dist_fs - dist_true| / (2·norm_oc·norm_qc).
    // Used as: margin = 2 · r_max · norm_qc · eps_ip (per-cluster, per-query).
    calibrated_eps_ip_ = CalibrateEpsilonIp(
        all_codes, cluster_members, encoded_vectors, centroids_.data(),
        rotation, dim, K,
        config_.epsilon_samples, config_.epsilon_percentile, config_.seed,
        calibrated_dk_, config_.rabitq.bits);

    // --- Phase 2c: write unified cluster.clu ---
    storage::ClusterStoreWriter clu_writer;
    s = clu_writer.Open(clu_path, K, dim, config_.rabitq);
    if (!s.ok()) return s;

    for (uint32_t k = 0; k < K; ++k) {
        const auto& members = cluster_members[k];
        const uint32_t n_members = static_cast<uint32_t>(members.size());
        const float* centroid = centroids_.data() + static_cast<size_t>(k) * dim;

        // V9 raw address table encode
        auto addr_blocks = storage::AddressColumn::EncodeRawTableV2(
            addr_entries_per_cluster[k], config_.page_size);

        // Write cluster block (epsilon field stores r_max)
        s = clu_writer.BeginCluster(k, n_members, centroid, r_max_per_cluster[k]);
        if (!s.ok()) return s;

        s = clu_writer.WriteVectors(all_codes[k]);
        if (!s.ok()) return s;

        s = clu_writer.WriteAddressBlocks(addr_blocks);
        if (!s.ok()) return s;

        s = clu_writer.EndCluster();
        if (!s.ok()) return s;

        if (progress_cb_) {
            progress_cb_(k, K);
        }
    }

    s = clu_writer.Finalize("data.dat");
    if (!s.ok()) return s;

    // --- Write segment.meta (FlatBuffers) ---
    flatbuffers::FlatBufferBuilder fbb(4096);

    // IvfParams
    auto ivf_params = vdb::schema::CreateIvfParams(
        fbb,
        K,                         // nlist
        config_.nprobe,            // nprobe
        0, 0,                      // centroids_offset/length (using separate file)
        N,                         // training_vectors
        config_.max_iterations,    // kmeans_iterations
        0, 0, 0.0f,               // min/max/avg list size (not computed here)
        0.0f,                      // balance_factor (deprecated, always 0)
        assignment_mode_ == AssignmentMode::RedundantTop2Rair
            ? vdb::schema::AssignmentMode::REDUNDANT_TOP2_RAIR
            : assignment_mode_ == AssignmentMode::RedundantTop2Naive
                ? vdb::schema::AssignmentMode::REDUNDANT_TOP2
                : vdb::schema::AssignmentMode::SINGLE,
        config_.assignment_factor,
        clustering_source_ == ClusteringSource::Precomputed
            ? vdb::schema::ClusteringSource::PRECOMPUTED
            : vdb::schema::ClusteringSource::AUTO,
        rair_lambda_,
        rair_strict_second_choice_
    );

    // RaBitQParams
    auto rabitq_params = vdb::schema::CreateRaBitQParams(
        fbb,
        config_.rabitq.bits,
        config_.rabitq.block_size,
        config_.rabitq.c_factor,
        0, 0  // codebook offset/length (not applicable for 1-bit)
    );

    // ConANNParams
    auto conann_params = vdb::schema::CreateConANNParams(
        fbb,
        0.0f, 0.0f,                     // deprecated tau_in/tau_out factors
        calibrated_eps_ip_,              // epsilon = ε_ip (inner-product error bound)
        calibrated_dk_,                  // d_k
        config_.calibration_samples,     // calibration_samples
        config_.calibration_topk,        // calibration_topk
        config_.calibration_percentile,  // calibration_percentile
        calibrated_eps_ip_,              // eps_ip_fs (same as epsilon; explicit FastScan label)
        used_hadamard                    // has_rotated_centroids
    );

    // ClusterMeta array — per-cluster info from the lookup table
    const auto& global_info = clu_writer.info();
    std::vector<flatbuffers::Offset<vdb::schema::ClusterMeta>> cluster_offsets;
    cluster_offsets.reserve(K);
    for (uint32_t k = 0; k < K; ++k) {
        const auto& entry = global_info.lookup_table[k];

        // Build empty AddressColumnMeta (address info is in .clu mini-trailers)
        auto ab_vec = fbb.CreateVector(
            std::vector<flatbuffers::Offset<vdb::schema::AddressBlockMeta>>{});
        auto addr_col = vdb::schema::CreateAddressColumnMeta(
            fbb, 64 /*granularity*/, 0, ab_vec);

        auto dfp = fbb.CreateString("data.dat");

        cluster_offsets.push_back(vdb::schema::CreateClusterMeta(
            fbb,
            entry.cluster_id,
            entry.num_records,
            0, 0,                   // centroid_offset/length (in .clu lookup table)
            entry.block_offset,     // rabitq_data_offset → block_offset
            entry.block_size,       // rabitq_data_length → block_size
            addr_col,
            dfp,
            0  // checksum
        ));
    }
    auto clusters_vec = fbb.CreateVector(cluster_offsets);

    // PayloadColumnSchema array
    std::vector<flatbuffers::Offset<vdb::schema::PayloadColumnSchema>> ps_offsets;
    for (const auto& cs : config_.payload_schemas) {
        auto name_off = fbb.CreateString(cs.name);
        ps_offsets.push_back(vdb::schema::CreatePayloadColumnSchema(
            fbb, cs.id, name_off, static_cast<uint8_t>(cs.dtype), cs.nullable));
    }
    auto payload_schemas_vec = fbb.CreateVector(ps_offsets);

    // --- Phase E: CRC score precomputation (optional) ---
    flatbuffers::Offset<vdb::schema::CrcParams> crc_params_offset;
    if (config_.crc_top_k > 0 && config_.calibration_queries != nullptr &&
        config_.num_calibration_queries > 0) {
        // Build ClusterData for CRC score computation.
        // Flatten RaBitQCode objects into contiguous byte blocks.
        const uint32_t num_words = (dim + 63) / 64;
        const uint32_t entry_size = num_words * sizeof(uint64_t) +
                                    sizeof(float) + sizeof(uint32_t);

        std::vector<std::vector<uint8_t>> flat_codes(K);
        std::vector<std::vector<uint32_t>> cluster_ids(K);
        std::vector<ClusterData> crc_clusters(K);

        for (uint32_t k = 0; k < K; ++k) {
            const auto& members = cluster_members[k];
            uint32_t n_members = static_cast<uint32_t>(members.size());

            // Build flat code block
            flat_codes[k].resize(static_cast<size_t>(n_members) * entry_size);
            for (uint32_t m = 0; m < n_members; ++m) {
                uint8_t* dst = flat_codes[k].data() +
                               static_cast<size_t>(m) * entry_size;
                const auto& code = all_codes[k][m];
                std::memcpy(dst, code.code.data(),
                            num_words * sizeof(uint64_t));
                std::memcpy(dst + num_words * sizeof(uint64_t),
                            &code.norm, sizeof(float));
                std::memcpy(dst + num_words * sizeof(uint64_t) + sizeof(float),
                            &code.sum_x, sizeof(uint32_t));
            }

            // Build global IDs
            cluster_ids[k].resize(n_members);
            for (uint32_t m = 0; m < n_members; ++m) {
                cluster_ids[k][m] = members[m];
            }

            crc_clusters[k].vectors = nullptr;  // not needed for RaBitQ scoring
            crc_clusters[k].ids = cluster_ids[k].data();
            crc_clusters[k].count = n_members;
            crc_clusters[k].codes_block = flat_codes[k].data();
            crc_clusters[k].code_entry_size = entry_size;
        }

        // Compute QueryScores for all calibration queries
        auto crc_scores = CrcCalibrator::ComputeScoresRaBitQ(
            config_.calibration_queries, config_.num_calibration_queries,
            dim, centroids_.data(), K, crc_clusters,
            config_.crc_top_k, rotation);

        // Serialize to crc_scores.bin
        s = CrcCalibrator::WriteScores(
            output_dir + "/crc_scores.bin", crc_scores, K, config_.crc_top_k);
        if (!s.ok()) return s;

        // Build FlatBuffers CrcParams
        auto sf = fbb.CreateString("crc_scores.bin");
        crc_params_offset = vdb::schema::CreateCrcParams(
            fbb, sf, config_.num_calibration_queries, config_.crc_top_k);
    }

    // SegmentMeta (use builder pattern to include payload_schemas)
    vdb::schema::SegmentMetaBuilder smb(fbb);
    smb.add_segment_id(config_.segment_id);
    smb.add_version(1);
    smb.add_state(vdb::schema::SegmentState::ACTIVE);
    smb.add_dimension(dim);
    smb.add_metric_type(vdb::schema::MetricType::L2);
    smb.add_vector_dtype(vdb::schema::VectorDType::FLOAT32);
    smb.add_ivf_params(ivf_params);
    smb.add_rabitq_params(rabitq_params);
    smb.add_conann_params(conann_params);
    if (crc_params_offset.o) {
        smb.add_crc_params(crc_params_offset);
    }
    smb.add_clusters(clusters_vec);
    smb.add_payload_schemas(payload_schemas_vec);
    auto seg = smb.Finish();

    fbb.Finish(seg, vdb::schema::SegmentMetaIdentifier());

    // Write segment.meta
    {
        const std::string meta_path = output_dir + "/segment.meta";
        std::ofstream f(meta_path, std::ios::binary);
        if (!f.is_open()) {
            return Status::IOError("Failed to create segment.meta: " + meta_path);
        }
        f.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()),
                fbb.GetSize());
        if (!f.good()) {
            return Status::IOError("Failed to write segment.meta");
        }
    }

    {
        const std::string sidecar_path = output_dir + "/build_metadata.json";
        std::ofstream f(sidecar_path, std::ios::binary);
        if (!f.is_open()) {
            return Status::IOError(
                "Failed to create build_metadata.json: " + sidecar_path);
        }

        std::ostringstream oss;
        oss << "{\n"
            << "  \"coarse_builder\": \"" << CoarseBuilderName(coarse_builder_used_) << "\",\n"
            << "  \"requested_metric\": \"" << config_.metric << "\",\n"
            << "  \"effective_metric\": \"" << effective_metric_ << "\",\n"
            << "  \"logical_dim\": " << logical_dim_ << ",\n"
            << "  \"effective_dim\": " << effective_dim_ << ",\n"
            << "  \"padding_mode\": \"" << padding_mode_ << "\",\n"
            << "  \"rotation_mode\": \"" << rotation_mode_ << "\",\n"
            << "  \"rotation_seed\": " << rotation.seed() << ",\n"
            << "  \"rotation_block_sizes\": "
            << JsonUIntArray(rotation.block_sizes()) << ",\n"
            << "  \"nlist\": " << K << ",\n"
            << "  \"assignment_mode\": \"" << AssignmentModeName(assignment_mode_) << "\",\n"
            << "  \"assignment_factor\": " << config_.assignment_factor << ",\n"
            << "  \"clustering_source\": \"" << ClusteringSourceName(clustering_source_) << "\",\n"
            << "  \"faiss_train_size\": " << std::min(config_.faiss_train_size, N) << ",\n"
            << "  \"faiss_niter\": " << ((config_.faiss_niter == 0) ? kDefaultFaissNiter : config_.faiss_niter) << ",\n"
            << "  \"faiss_nredo\": " << config_.faiss_nredo << ",\n"
            << "  \"faiss_backend\": \"cpu\"\n"
            << "}\n";
        const std::string payload = oss.str();
        f.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!f.good()) {
            return Status::IOError("Failed to write build_metadata.json");
        }
    }

    return Status::OK();
}

}  // namespace index
}  // namespace vdb
