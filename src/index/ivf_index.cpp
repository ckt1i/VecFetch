#include "vdb/index/ivf_index.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <numeric>
#include <string_view>

#include "vdb/common/aligned_alloc.h"
#include "vdb/simd/coarse_ip_score.h"
#include "vdb/simd/coarse_select.h"
#include "vdb/simd/distance_l2.h"

#ifdef VDB_USE_MKL
#include <mkl.h>
#endif

// FlatBuffers generated header
#include "segment_meta_generated.h"

namespace vdb {
namespace index {

namespace {

CoarseBuilder ParseCoarseBuilder(std::string_view value) {
    if (value == "superkmeans") {
        return CoarseBuilder::SuperKMeans;
    }
    if (value == "hierarchical_superkmeans") {
        return CoarseBuilder::HierarchicalSuperKMeans;
    }
    if (value == "faiss_kmeans") {
        return CoarseBuilder::FaissKMeans;
    }
    return CoarseBuilder::Auto;
}

std::string ExtractJsonStringField(const std::string& contents, std::string_view key) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const size_t key_pos = contents.find(quoted_key);
    if (key_pos == std::string::npos) {
        return {};
    }
    const size_t colon = contents.find(':', key_pos + quoted_key.size());
    const size_t first_quote = contents.find('"', colon + 1);
    const size_t second_quote = contents.find('"', first_quote + 1);
    if (colon == std::string::npos ||
        first_quote == std::string::npos ||
        second_quote == std::string::npos ||
        second_quote <= first_quote + 1) {
        return {};
    }
    return contents.substr(first_quote + 1, second_quote - first_quote - 1);
}

uint64_t ExtractJsonUintField(const std::string& contents, std::string_view key,
                              uint64_t default_value) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const size_t key_pos = contents.find(quoted_key);
    if (key_pos == std::string::npos) {
        return default_value;
    }
    const size_t colon = contents.find(':', key_pos + quoted_key.size());
    if (colon == std::string::npos) {
        return default_value;
    }
    size_t begin = colon + 1;
    while (begin < contents.size() &&
           std::isspace(static_cast<unsigned char>(contents[begin]))) {
        ++begin;
    }
    size_t end = begin;
    while (end < contents.size() &&
           std::isdigit(static_cast<unsigned char>(contents[end]))) {
        ++end;
    }
    if (end == begin) {
        return default_value;
    }
    return std::strtoull(contents.substr(begin, end - begin).c_str(), nullptr, 10);
}

void NormalizeVector(const float* src, float* dst, Dim dim) {
    float norm_sq = 0.0f;
    for (Dim i = 0; i < dim; ++i) {
        norm_sq += src[i] * src[i];
    }
    const float norm = std::sqrt(norm_sq);
    if (norm <= 0.0f) {
        std::memcpy(dst, src, static_cast<size_t>(dim) * sizeof(float));
        return;
    }
    for (Dim i = 0; i < dim; ++i) {
        dst[i] = src[i] / norm;
    }
}

#if defined(VDB_USE_AVX512)
constexpr uint32_t kCoarseCentroidBlock = 8;
constexpr uint32_t kCoarseVecWidth = 16;
#elif defined(VDB_USE_AVX2)
constexpr uint32_t kCoarseCentroidBlock = 4;
constexpr uint32_t kCoarseVecWidth = 8;
#else
constexpr uint32_t kCoarseCentroidBlock = 1;
constexpr uint32_t kCoarseVecWidth = 1;
#endif

void BuildCoarsePackedLayout(const std::vector<float>& src,
                             uint32_t nlist,
                             Dim dim,
                             IvfIndex::CoarsePackedLayout* layout) {
    if (layout == nullptr) return;
    layout->centroid_block = kCoarseCentroidBlock;
    layout->vec_width = kCoarseVecWidth;
    layout->num_centroid_blocks = (nlist + kCoarseCentroidBlock - 1) / kCoarseCentroidBlock;
    layout->num_dim_blocks =
        (static_cast<uint32_t>(dim) + kCoarseVecWidth - 1) / kCoarseVecWidth;
    layout->packed_dim = static_cast<size_t>(layout->num_dim_blocks) * kCoarseVecWidth;

    const size_t total_floats = static_cast<size_t>(layout->num_centroid_blocks) *
                                layout->num_dim_blocks *
                                kCoarseCentroidBlock * kCoarseVecWidth;
    layout->data.assign(total_floats, 0.0f);

    for (uint32_t cb = 0; cb < layout->num_centroid_blocks; ++cb) {
        for (uint32_t db = 0; db < layout->num_dim_blocks; ++db) {
            const uint32_t dim_base = db * kCoarseVecWidth;
            if (dim_base >= static_cast<uint32_t>(dim)) {
                continue;
            }
            const uint32_t copy_count = std::min<uint32_t>(
                kCoarseVecWidth, static_cast<uint32_t>(dim) - dim_base);
            for (uint32_t lane = 0; lane < kCoarseCentroidBlock; ++lane) {
                const uint32_t centroid_idx = cb * kCoarseCentroidBlock + lane;
                if (centroid_idx >= nlist) {
                    continue;
                }
                const size_t packed_base =
                    ((static_cast<size_t>(cb) * layout->num_dim_blocks + db) *
                         kCoarseCentroidBlock +
                     lane) *
                    kCoarseVecWidth;
                const float* centroid_ptr =
                    src.data() + static_cast<size_t>(centroid_idx) * dim;
                std::memcpy(layout->data.data() + packed_base,
                            centroid_ptr + dim_base,
                            static_cast<size_t>(copy_count) * sizeof(float));
            }
        }
    }
}

void ComputeCoarseIPScoresDispatch(const float* query,
                                   const IvfIndex::CoarsePackedLayout& layout,
                                   uint32_t nlist,
                                   float* scores) {
#if defined(VDB_USE_AVX512) || defined(VDB_USE_AVX2)
    simd::ComputeCoarseIPScoresPacked(
        query, layout.data.data(), nlist, scores, layout.num_dim_blocks);
#else
    simd::ComputeCoarseIPScoresPackedScalar(
        query, layout.data.data(), nlist, layout.centroid_block,
        layout.num_dim_blocks, layout.vec_width, scores);
#endif
}

}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

IvfIndex::IvfIndex() = default;
IvfIndex::~IvfIndex() = default;

// ============================================================================
// Open — load index from directory
// ============================================================================

Status IvfIndex::Open(const std::string& dir, bool use_direct_io) {
    dir_ = dir;

    // --- 1. Read segment.meta (FlatBuffers) ---
    const std::string meta_path = dir + "/segment.meta";
    std::ifstream meta_file(meta_path, std::ios::binary | std::ios::ate);
    if (!meta_file.is_open()) {
        return Status::IOError("Failed to open segment.meta: " + meta_path);
    }
    const auto meta_size = meta_file.tellg();
    meta_file.seekg(0);
    std::vector<uint8_t> meta_buf(static_cast<size_t>(meta_size));
    meta_file.read(reinterpret_cast<char*>(meta_buf.data()), meta_size);
    if (!meta_file.good()) {
        return Status::IOError("Failed to read segment.meta");
    }
    meta_file.close();

    // Verify FlatBuffer
    auto verifier = flatbuffers::Verifier(meta_buf.data(), meta_buf.size());
    if (!vdb::schema::VerifySegmentMetaBuffer(verifier)) {
        return Status::Corruption("Invalid segment.meta FlatBuffer");
    }

    const auto* seg_meta = vdb::schema::GetSegmentMeta(meta_buf.data());
    if (!seg_meta) {
        return Status::Corruption("Failed to parse SegmentMeta");
    }

    dim_ = seg_meta->dimension();
    logical_dim_ = dim_;
    if (dim_ == 0) {
        return Status::InvalidArgument("SegmentMeta dimension is 0");
    }

    // --- 2. Load IVF params ---
    const auto* ivf = seg_meta->ivf_params();
    if (!ivf) {
        return Status::InvalidArgument("SegmentMeta missing ivf_params");
    }
    nlist_ = ivf->nlist();
    if (nlist_ == 0) {
        return Status::InvalidArgument("IvfParams nlist is 0");
    }
    assignment_mode_ = AssignmentMode::Single;
    switch (ivf->assignment_mode()) {
        case vdb::schema::AssignmentMode::REDUNDANT_TOP2:
            assignment_mode_ = AssignmentMode::RedundantTop2Naive;
            break;
        case vdb::schema::AssignmentMode::REDUNDANT_TOP2_RAIR:
            assignment_mode_ = AssignmentMode::RedundantTop2Rair;
            break;
        case vdb::schema::AssignmentMode::SINGLE:
        default:
            assignment_mode_ = AssignmentMode::Single;
            break;
    }
    assignment_factor_ = ivf->assignment_factor();
    rair_lambda_ = ivf->rair_lambda();
    rair_strict_second_choice_ = ivf->rair_strict_second_choice();
    clustering_source_ = (ivf->clustering_source() ==
                          vdb::schema::ClusteringSource::PRECOMPUTED)
        ? ClusteringSource::Precomputed
        : ClusteringSource::Auto;
    coarse_builder_ = CoarseBuilder::Auto;
    {
        const std::string sidecar_path = dir + "/build_metadata.json";
        std::ifstream sidecar(sidecar_path);
        if (sidecar.is_open()) {
            std::string contents((std::istreambuf_iterator<char>(sidecar)),
                                 std::istreambuf_iterator<char>());
            const std::string coarse_builder =
                ExtractJsonStringField(contents, "coarse_builder");
            if (!coarse_builder.empty()) {
                coarse_builder_ = ParseCoarseBuilder(coarse_builder);
            }
            requested_metric_ = ExtractJsonStringField(contents, "requested_metric");
            effective_metric_ = ExtractJsonStringField(contents, "effective_metric");
            if (requested_metric_.empty()) {
                requested_metric_ = "l2";
            }
            if (effective_metric_.empty()) {
                effective_metric_ = "l2";
            }
            logical_dim_ = static_cast<Dim>(
                ExtractJsonUintField(contents, "logical_dim", dim_));
            padding_mode_ = ExtractJsonStringField(contents, "padding_mode");
            rotation_mode_ = ExtractJsonStringField(contents, "rotation_mode");
            if (padding_mode_.empty()) {
                padding_mode_ = "none";
            }
            if (rotation_mode_.empty()) {
                rotation_mode_ = "random_matrix";
            }
        }
    }

    // --- 3. Load centroids.bin ---
    const std::string centroids_path = dir + "/centroids.bin";
    std::ifstream cent_file(centroids_path, std::ios::binary);
    if (!cent_file.is_open()) {
        return Status::IOError("Failed to open centroids.bin: " + centroids_path);
    }
    const size_t cent_size = static_cast<size_t>(nlist_) * dim_ * sizeof(float);
    centroids_.resize(static_cast<size_t>(nlist_) * dim_);
    cent_file.read(reinterpret_cast<char*>(centroids_.data()), cent_size);
    if (!cent_file.good()) {
        return Status::IOError("Failed to read centroids.bin");
    }
    cent_file.close();
    if (effective_metric_ == "ip" && requested_metric_ == "cosine") {
        normalized_centroids_.resize(centroids_.size());
        for (uint32_t i = 0; i < nlist_; ++i) {
            NormalizeVector(
                centroids_.data() + static_cast<size_t>(i) * dim_,
                normalized_centroids_.data() + static_cast<size_t>(i) * dim_,
                dim_);
        }
    }
    BuildCoarsePackedLayout(centroids_, nlist_, dim_, &packed_centroids_);
    if (!normalized_centroids_.empty()) {
        BuildCoarsePackedLayout(
            normalized_centroids_, nlist_, dim_, &packed_normalized_centroids_);
    }

    // --- 4. Load rotation.bin ---
    const std::string rotation_path = dir + "/rotation.bin";
    auto rot_result = rabitq::RotationMatrix::Load(rotation_path, dim_);
    if (!rot_result.ok()) {
        return rot_result.status();
    }
    rotation_ = std::make_unique<rabitq::RotationMatrix>(std::move(rot_result.value()));
    if (rotation_mode_ == "random_matrix") {
        if (rotation_->is_blocked_hadamard_permuted()) {
            rotation_mode_ = "blocked_hadamard_permuted";
        } else if (rotation_->is_fast_hadamard()) {
            rotation_mode_ = (logical_dim_ != dim_) ? "hadamard_padded" : "hadamard";
        }
    }

    // --- 5. Load ConANN params ---
    // Global epsilon may be 0 when per-cluster epsilon is used (stored in .clu
    // lookup table). d_k is always global and must be loaded.
    const auto* conann_params = seg_meta->conann_params();
    if (conann_params) {
        float eps = conann_params->epsilon();
        float dk = conann_params->d_k();
        conann_ = ConANN(eps, dk);
    }

    // --- Phase 2: Load rotated_centroids.bin from disk ---
    // When has_rotated_centroids=true (new Phase-2 builder), the file must be present.
    // When false (non-Hadamard dim or pre-Phase-2 index), skip loading.
    if (conann_params && conann_params->has_rotated_centroids()) {
        const std::string rc_path = dir + "/rotated_centroids.bin";
        std::ifstream rc_file(rc_path, std::ios::binary);
        if (!rc_file.is_open()) {
            return Status::IOError(
                "Index declares has_rotated_centroids=true but file is missing: " + rc_path);
        }
        used_hadamard_ = true;
        rotated_centroids_.resize(static_cast<size_t>(nlist_) * dim_);
        rc_file.read(reinterpret_cast<char*>(rotated_centroids_.data()),
                     static_cast<std::streamsize>(nlist_) * dim_ * sizeof(float));
        if (!rc_file.good()) {
            return Status::IOError("Failed to read rotated_centroids.bin");
        }
        if (rotation_mode_ == "random_matrix") {
            if (rotation_->is_blocked_hadamard_permuted()) {
                rotation_mode_ = "blocked_hadamard_permuted";
            } else {
                rotation_mode_ = (logical_dim_ != dim_) ? "hadamard_padded" : "hadamard";
            }
        }
    }

    // --- 6. Load payload schemas ---
    const auto* ps = seg_meta->payload_schemas();
    if (ps) {
        payload_schemas_.reserve(ps->size());
        for (uint32_t i = 0; i < ps->size(); ++i) {
            const auto* entry = ps->Get(i);
            if (!entry) continue;
            ColumnSchema cs;
            cs.id = entry->id();
            cs.name = entry->name() ? entry->name()->str() : "";
            cs.dtype = static_cast<DType>(entry->dtype());
            cs.nullable = entry->nullable();
            payload_schemas_.push_back(cs);
        }
    }

    // --- 7. Open segment (unified cluster.clu + data.dat) ---
    auto seg_status = segment_.Open(dir, payload_schemas_, use_direct_io,
                                    logical_dim_ > 0 ? std::optional<Dim>(logical_dim_)
                                                     : std::nullopt);
    if (!seg_status.ok()) {
        return seg_status;
    }

    // Build cluster_ids from segment
    const auto* clusters = seg_meta->clusters();
    if (clusters) {
        for (uint32_t i = 0; i < clusters->size(); ++i) {
            const auto* cm = clusters->Get(i);
            if (!cm) continue;
            cluster_ids_.push_back(cm->cluster_id());
        }
    }

    // Sort cluster_ids for consistent ordering
    std::sort(cluster_ids_.begin(), cluster_ids_.end());

#ifdef VDB_USE_MKL
    // Precompute ||c||² for each centroid (used by MKL-accelerated distance)
    centroid_norms_.resize(nlist_);
    for (uint32_t i = 0; i < nlist_; ++i) {
        const float* c = centroid(i);
        centroid_norms_[i] = cblas_sdot(dim_, c, 1, c, 1);
    }
#endif

    return Status::OK();
}

// ============================================================================
// FindNearestClusters — metric-aware coarse ranking over centroids
// ============================================================================

std::vector<ClusterID> IvfIndex::FindNearestClusters(
    const float* query, uint32_t nprobe) const {
    if (nprobe == 0 || cluster_ids_.empty()) {
        return {};
    }

    const uint32_t actual_nprobe = std::min(nprobe, nlist_);
    if (!coarse_scratch_) {
        coarse_scratch_ = std::make_unique<CoarseScratch>();
    }
    CoarseScratch& scratch = *coarse_scratch_;
    scratch.scores.resize(nlist_);
    scratch.order.resize(nlist_);

    // Compute scores/distances to all centroids
    auto coarse_score_start = std::chrono::steady_clock::now();
    if (effective_metric_ == "ip") {
        const CoarsePackedLayout* packed_layout = &packed_centroids_;
        const size_t packed_dim = packed_layout->packed_dim;
        scratch.query_buffer.assign(packed_dim, 0.0f);
        const float* query_ip = scratch.query_buffer.data();
        if (requested_metric_ == "cosine") {
            NormalizeVector(query, scratch.query_buffer.data(), dim_);
            if (!packed_normalized_centroids_.empty()) {
                packed_layout = &packed_normalized_centroids_;
            }
        } else {
            std::memcpy(scratch.query_buffer.data(), query,
                        static_cast<size_t>(dim_) * sizeof(float));
        }
        ComputeCoarseIPScoresDispatch(
            query_ip, *packed_layout, nlist_, scratch.scores.data());
    } else {
#ifdef VDB_USE_MKL
        scratch.query_buffer.resize(nlist_);
        cblas_sgemv(CblasRowMajor, CblasNoTrans,
                    nlist_, dim_,
                    1.0f, centroids_.data(), dim_,
                    query, 1,
                    0.0f, scratch.query_buffer.data(), 1);

        const float q_norm = cblas_sdot(dim_, query, 1, query, 1);

        for (uint32_t i = 0; i < nlist_; ++i) {
            scratch.scores[i] =
                q_norm + centroid_norms_[i] - 2.0f * scratch.query_buffer[i];
        }
#else
        for (uint32_t i = 0; i < nlist_; ++i) {
            scratch.scores[i] = simd::L2Sqr(query, centroid(i), dim_);
        }
#endif
    }
    last_coarse_score_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - coarse_score_start).count();

    // Partial sort to find the nprobe nearest
    auto coarse_topn_start = std::chrono::steady_clock::now();
    if (use_coarse_select_simd_) {
        if (use_coarse_select_phase2_) {
            simd::SelectTopNProbeSmallSpecialized(
                scratch.scores.data(), nlist_, actual_nprobe, scratch.order.data());
        } else {
            simd::SelectTopNProbeSmall(
                scratch.scores.data(), nlist_, actual_nprobe, scratch.order.data());
        }
    } else {
        for (uint32_t i = 0; i < nlist_; ++i) {
            scratch.order[i] = i;
        }
        auto score_less = [&](uint32_t lhs, uint32_t rhs) {
            return scratch.scores[lhs] < scratch.scores[rhs];
        };
        if (actual_nprobe < nlist_) {
            std::nth_element(scratch.order.begin(),
                             scratch.order.begin() + actual_nprobe,
                             scratch.order.end(),
                             score_less);
        }
        std::sort(scratch.order.begin(),
                  scratch.order.begin() + actual_nprobe,
                  score_less);
    }
    last_coarse_topn_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - coarse_topn_start).count();

    // Map centroid indices to cluster IDs
    std::vector<ClusterID> result(actual_nprobe);
    for (uint32_t i = 0; i < actual_nprobe; ++i) {
        result[i] = cluster_ids_[scratch.order[i]];
    }

    return result;
}

}  // namespace index
}  // namespace vdb
