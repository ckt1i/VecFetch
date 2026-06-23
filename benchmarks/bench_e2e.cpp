/// bench_e2e.cpp — End-to-end benchmark on real COCO datasets.
///
/// Measures: recall@{1,5,10}, query latency (avg/p50/p95/p99),
///           build time, brute-force GT time, IO/CPU pipeline utilization.
///
/// Usage:
///   bench_e2e [--dataset /path/to/coco_1k]
///
/// Output:
///   /home/zcq/VDB/test/{dataset}_{timestamp}/
///     index/         — built IVF index files
///     config.json    — parameter snapshot
///     results.json   — metrics + pipeline stats + per-query samples

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>

#include "vdb/common/distance.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/jsonl_reader.h"
#include "vdb/io/npy_reader.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"
#include "vdb/simd/distance_l2.h"
#include "rabitq_bench_calibration.h"

using namespace vdb;
using namespace vdb::bench;
using namespace vdb::index;
using namespace vdb::query;

namespace fs = std::filesystem;

// ============================================================================
// Command-line helpers
// ============================================================================

static std::string GetStringArg(int argc, char* argv[], const char* name,
                                const std::string& default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return default_val;
}

static bool HasFlag(int argc, char* argv[], const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return true;
    }
    return false;
}

static int GetIntArg(int argc, char* argv[], const char* name, int default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    }
    return default_val;
}

static float GetFloatArg(int argc, char* argv[], const char* name, float default_val) {
    for (int i = 1; i < argc - 1; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::strtof(argv[i + 1], nullptr);
    }
    return default_val;
}

static bool ParseDynamicSafeInModeArg(const std::string& value,
                                      DynamicSafeInMode* out) {
    if (value == "static" || value == "off") {
        *out = DynamicSafeInMode::Static;
        return true;
    }
    if (value == "frontier") {
        *out = DynamicSafeInMode::Frontier;
        return true;
    }
    return false;
}

static const char* DynamicSafeInModeName(DynamicSafeInMode mode) {
    switch (mode) {
        case DynamicSafeInMode::Static:
            return "static";
        case DynamicSafeInMode::Frontier:
            return "frontier";
    }
    return "unknown";
}

static bool RejectDeletedDynamicSafeInFlag(int argc, char* argv[]) {
    static constexpr const char* kDeletedFlags[] = {
        "--dynamic-safein-scale",
        "--dynamic-safein-scale-cap-static",
        "--dynamic-safein-payload-only",
        "--dynamic-safein-gap-rel-tol",
        "--dynamic-safein-gap-abs-tol",
    };
    for (const char* flag : kDeletedFlags) {
        if (HasFlag(argc, argv, flag)) {
            std::fprintf(stderr,
                         "Unsupported Dynamic SafeIn option %s: "
                         "use --dynamic-safein frontier without scale, "
                         "gap, cap, or payload-only flags.\n",
                         flag);
            return true;
        }
    }
    return false;
}

static bool RejectDeletedClusterLoadingFlag(int argc, char* argv[]) {
    static constexpr const char* kDeletedFlags[] = {
        "--clu-read-mode",
        "--use-resident-clusters",
        "--prefetch-depth",
        "--refill-threshold",
        "--refill-count",
    };
    for (const char* flag : kDeletedFlags) {
        if (HasFlag(argc, argv, flag)) {
            std::fprintf(stderr,
                         "Unsupported cluster loading option %s: "
                         "cluster.clu is always served from resident full preload.\n",
                         flag);
            return true;
        }
    }
    return false;
}

static bool RejectDeletedLegacySearchModeFlag(int argc, char* argv[]) {
    static constexpr const char* kDeletedFlags[] = {
        "--hnsw-coarse-routing",
        "--hnsw-coarse-m",
        "--hnsw-coarse-ef-construction",
        "--hnsw-coarse-ef-search",
        "--assignment-mode",
        "--assignment-factor",
        "--rair-lambda",
        "--rair-strict-second-choice",
        "--save-secondary-assignments",
        "--pad-to-pow2",
        "--blocked-hadamard-permuted",
        "--fht-kac-rotator",
    };
    for (const char* flag : kDeletedFlags) {
        if (HasFlag(argc, argv, flag)) {
            std::fprintf(stderr,
                         "Unsupported legacy search/build option %s: "
                         "formal runs use single assignment, exact/two-level "
                         "coarse routing, and automatic Hadamard/FHT-Kac rotation.\n",
                         flag);
            return true;
        }
    }
    return false;
}

// ============================================================================
// Timestamp + dataset name
// ============================================================================

static std::string Timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%S", std::localtime(&t));
    return buf;
}

static std::string DatasetName(const std::string& path) {
    return fs::path(path).filename().string();
}

static std::string FormatEpsilonTag(float epsilon_percentile) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << epsilon_percentile;
    return oss.str();
}

static std::string ResolveBenchIndexDir(const std::string& dataset_name,
                                        const std::string& output_dir,
                                        int nlist,
                                        const RaBitQConfig& rabitq_config,
                                        float epsilon_percentile,
                                        const std::string& requested_index_dir) {
    if (!requested_index_dir.empty()) {
        return requested_index_dir;
    }
    if (dataset_name == "coco_100k" && nlist == 2048) {
        std::string dir = "/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_";
        if (rabitq_config.uses_official_1_plus_n()) {
            dir += RaBitQFormatKey(rabitq_config);
        } else {
            dir += "bits" + std::to_string(rabitq_config.effective_total_bits());
        }
        dir += "_eps" + FormatEpsilonTag(epsilon_percentile);
        return dir;
    }
    if (rabitq_config.uses_official_1_plus_n()) {
        return output_dir + "/index_" + RaBitQFormatKey(rabitq_config);
    }
    return output_dir + "/index";
}

enum class RaBitQValidationMode {
    Auto,
    Official,
    Legacy,
};

static bool ParseRaBitQValidationModeArg(const std::string& value,
                                         RaBitQValidationMode* out) {
    if (value == "auto") {
        *out = RaBitQValidationMode::Auto;
        return true;
    }
    if (value == "official" || value == "official_1_plus_n") {
        *out = RaBitQValidationMode::Official;
        return true;
    }
    if (value == "legacy" || value == "legacy_signed_magnitude") {
        *out = RaBitQValidationMode::Legacy;
        return true;
    }
    return false;
}

static const char* RaBitQValidationModeName(RaBitQValidationMode mode) {
    switch (mode) {
        case RaBitQValidationMode::Auto:
            return "auto";
        case RaBitQValidationMode::Official:
            return "official_1_plus_n";
        case RaBitQValidationMode::Legacy:
            return "legacy_signed_magnitude";
    }
    return "auto";
}

static bool ValidateRaBitQMode(const RaBitQConfig& config,
                               RaBitQValidationMode requested,
                               const char* flag_name) {
    if (requested == RaBitQValidationMode::Auto) return true;
    const bool official = config.uses_official_1_plus_n();
    if (requested == RaBitQValidationMode::Official && !official) {
        std::fprintf(stderr,
                     "%s=official_1_plus_n requested, but index is %s\n",
                     flag_name,
                     std::string(RaBitQEstimatorModeName(
                         config.estimator_mode)).c_str());
        return false;
    }
    if (requested == RaBitQValidationMode::Legacy && official) {
        std::fprintf(stderr,
                     "%s=legacy_signed_magnitude requested, but index is %s\n",
                     flag_name,
                     std::string(RaBitQEstimatorModeName(
                         config.estimator_mode)).c_str());
        return false;
    }
    return true;
}

static bool ResolveRaBitQBuildConfig(int argc, char* argv[],
                                     int arg_bits,
                                     int arg_block_size,
                                     float arg_c_factor,
                                     RaBitQConfig* out) {
    std::string mode_arg =
        GetStringArg(argc, argv, "--rabitq-estimator-mode", "");
    if (mode_arg.empty()) {
        mode_arg = GetStringArg(argc, argv, "--rabitq-mode",
                                "legacy_signed_magnitude");
    }
    RaBitQEstimatorMode mode = RaBitQEstimatorMode::kLegacySignedMagnitude;
    if (!ParseRaBitQEstimatorMode(mode_arg, &mode)) {
        std::fprintf(stderr,
                     "Invalid --rabitq-estimator-mode: %s "
                     "(expected legacy_signed_magnitude or official_1_plus_n)\n",
                     mode_arg.c_str());
        return false;
    }
    const bool layout_explicit = HasFlag(argc, argv, "--rabitq-exdata-layout");
    const std::string layout_arg =
        GetStringArg(argc, argv, "--rabitq-exdata-layout", "generic_packed");
    RaBitQExDataLayout exdata_layout = RaBitQExDataLayout::kGenericPacked;
    if (!ParseRaBitQExDataLayout(layout_arg, &exdata_layout)) {
        std::fprintf(stderr,
                     "Invalid --rabitq-exdata-layout: %s "
                     "(expected generic_packed, split1_bitplane, split2_bitplanes, "
                     "split3_2plus1, split3_bitplanes, split3_trimmed_bitplanes, "
                     "split3_zero_plane_elide, vector_bitmajor_tiles, or selected_direct)\n",
                     layout_arg.c_str());
        return false;
    }

    RaBitQConfig config;
    config.block_size = static_cast<uint32_t>(arg_block_size);
    config.c_factor = arg_c_factor;
    config.estimator_mode = mode;
    config.exdata_layout = exdata_layout;
    if (mode == RaBitQEstimatorMode::kOfficial1PlusN) {
        const int total_bits =
            GetIntArg(argc, argv, "--rabitq-total-bits",
                      GetIntArg(argc, argv, "--total-bits", arg_bits));
        const int ex_bits =
            GetIntArg(argc, argv, "--rabitq-ex-bits",
                      GetIntArg(argc, argv, "--ex-bits", total_bits - 1));
        if (total_bits < 1 || total_bits > 8 || ex_bits < 0 || ex_bits > 7) {
            std::fprintf(stderr,
                         "Invalid official RaBitQ bits: total_bits=%d ex_bits=%d\n",
                         total_bits, ex_bits);
            return false;
        }
        config.total_bits = static_cast<uint8_t>(total_bits);
        config.ex_bits = static_cast<uint8_t>(ex_bits);
        config.bits = config.ex_bits > 0 ? config.ex_bits : 1;
        if (!layout_explicit) {
            config.exdata_layout =
                RaBitQDefaultOfficialExDataLayoutForBits(config.ex_bits);
        }
        if (!config.official_bits_valid()) {
            std::fprintf(stderr,
                         "official RaBitQ requires total_bits == ex_bits + 1 "
                         "(got total_bits=%d ex_bits=%d)\n",
                         total_bits, ex_bits);
            return false;
        }
        if (!(ex_bits == 0 || ex_bits == 1 || ex_bits == 2 || ex_bits == 3 || ex_bits == 4)) {
            std::fprintf(stderr,
                         "official RaBitQ supports ex_bits=0,1,2,3,4 "
                         "(got ex_bits=%d)\n",
                         ex_bits);
            return false;
        }
        if (!config.exdata_layout_valid()) {
            std::fprintf(stderr,
                         "optimized --rabitq-exdata-layout is incompatible with official bits "
                         "(got layout=%s total_bits=%d ex_bits=%d)\n",
                         std::string(RaBitQExDataLayoutName(exdata_layout)).c_str(),
                         total_bits, ex_bits);
            return false;
        }
    } else {
        if (HasFlag(argc, argv, "--rabitq-total-bits") ||
            HasFlag(argc, argv, "--total-bits") ||
            HasFlag(argc, argv, "--rabitq-ex-bits") ||
            HasFlag(argc, argv, "--ex-bits")) {
            std::fprintf(stderr,
                         "total/ex bit flags require "
                         "--rabitq-estimator-mode official_1_plus_n\n");
            return false;
        }
        if (exdata_layout != RaBitQExDataLayout::kGenericPacked) {
            std::fprintf(stderr,
                         "--rabitq-exdata-layout=%s requires "
                         "--rabitq-estimator-mode official_1_plus_n\n",
                         std::string(RaBitQExDataLayoutName(exdata_layout)).c_str());
            return false;
        }
        config.bits = static_cast<uint8_t>(std::max(1, arg_bits));
        config.total_bits = config.bits;
        config.ex_bits = config.bits > 1 ? config.bits : 0;
        config.exdata_layout = RaBitQExDataLayout::kGenericPacked;
    }
    *out = config;
    return true;
}

static std::string NormalizePath(const std::string& path) {
    std::error_code ec;
    fs::path normalized = fs::absolute(fs::path(path), ec);
    if (ec) {
        return path;
    }
    return normalized.lexically_normal().string();
}

static double FileSizeBytesOrZero(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) return 0.0;
    return static_cast<double>(size);
}

static bool EndsWith(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string InferGtMode(const std::string& gt_file) {
    if (gt_file.empty()) return "";
    if (EndsWith(gt_file, ".ivecs")) return "ivecs";
    if (EndsWith(gt_file, ".npy")) return "npy";
    return "";
}

struct FlatstorPayloadReader {
    std::vector<int64_t> offsets_and_lengths;
    uint32_t rows = 0;
    uint32_t cols = 0;
    int fd = -1;
    std::string index_path;
    std::string data_path;
    uint64_t total_payload_bytes = 0;

    ~FlatstorPayloadReader() {
        if (fd >= 0) {
            close(fd);
        }
    }

    Status Open(const std::string& index_file,
                const std::string& data_file,
                uint32_t expected_rows) {
        index_path = index_file;
        data_path = data_file;

        auto idx_or = io::LoadNpyInt64Matrix(index_file);
        if (!idx_or.ok()) return idx_or.status();
        auto idx = std::move(idx_or.value());
        if (idx.rows < expected_rows) {
            return Status::InvalidArgument(
                "Payload index rows (" + std::to_string(idx.rows) +
                ") smaller than dataset rows (" +
                std::to_string(expected_rows) + ")");
        }
        if (idx.cols < 2) {
            return Status::InvalidArgument(
                "Payload index must have at least two columns: offset,length");
        }

        std::error_code ec;
        const uint64_t data_bytes = fs::file_size(data_file, ec);
        if (ec) {
            return Status::IOError("Failed to stat payload data file: " + data_file);
        }

        total_payload_bytes = 0;
        for (uint32_t row = 0; row < expected_rows; ++row) {
            const int64_t* entry =
                idx.data.data() + static_cast<size_t>(row) * idx.cols;
            const int64_t offset = entry[0];
            const int64_t length = entry[1];
            if (offset < 0 || length < 0) {
                return Status::InvalidArgument(
                    "Payload index contains negative offset/length at row " +
                    std::to_string(row));
            }
            if (static_cast<uint64_t>(length) >
                static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
                return Status::InvalidArgument(
                    "Payload length exceeds uint32_t at row " +
                    std::to_string(row));
            }
            const uint64_t begin = static_cast<uint64_t>(offset);
            const uint64_t len = static_cast<uint64_t>(length);
            if (begin > data_bytes || len > data_bytes - begin) {
                return Status::InvalidArgument(
                    "Payload index points outside data file at row " +
                    std::to_string(row));
            }
            total_payload_bytes += len;
        }

        offsets_and_lengths = std::move(idx.data);
        rows = idx.rows;
        cols = idx.cols;
        fd = open(data_file.c_str(), O_RDONLY);
        if (fd < 0) {
            return Status::IOError("Failed to open payload data file: " + data_file);
        }
        return Status::OK();
    }

    std::string Read(uint32_t row) const {
        if (row >= rows || fd < 0) {
            throw std::runtime_error("FlatStor payload reader is not ready");
        }
        const int64_t* entry =
            offsets_and_lengths.data() + static_cast<size_t>(row) * cols;
        const off_t offset = static_cast<off_t>(entry[0]);
        const size_t length = static_cast<size_t>(entry[1]);
        std::string payload(length, '\0');
        size_t done = 0;
        while (done < length) {
            ssize_t n = pread(fd, payload.data() + done, length - done,
                              offset + static_cast<off_t>(done));
            if (n <= 0) {
                throw std::runtime_error(
                    "Failed to read FlatStor payload row " +
                    std::to_string(row) + ": " + std::strerror(errno));
            }
            done += static_cast<size_t>(n);
        }
        return payload;
    }
};

static StatusOr<std::vector<std::vector<int64_t>>> LoadExternalGroundTruth(
    const std::string& gt_file, uint32_t q_count, uint32_t gt_k) {
    std::vector<std::vector<int64_t>> gt_topk;
    const std::string mode = InferGtMode(gt_file);
    if (mode.empty()) {
        return Status::InvalidArgument(
            "Unsupported ground-truth file format: " + gt_file);
    }

    if (mode == "ivecs") {
        auto gt_or = io::LoadIvecs(gt_file);
        if (!gt_or.ok()) return gt_or.status();
        const auto& gt = gt_or.value();
        if (gt.rows < q_count) {
            return Status::InvalidArgument(
                "Ground-truth rows (" + std::to_string(gt.rows) +
                ") smaller than query count (" + std::to_string(q_count) + ")");
        }
        if (gt.cols < gt_k) {
            return Status::InvalidArgument(
                "Ground-truth cols (" + std::to_string(gt.cols) +
                ") smaller than required top-k (" + std::to_string(gt_k) + ")");
        }
        gt_topk.resize(q_count);
        for (uint32_t qi = 0; qi < q_count; ++qi) {
            gt_topk[qi].resize(gt_k);
            const int32_t* row = gt.data.data() + static_cast<size_t>(qi) * gt.cols;
            for (uint32_t k = 0; k < gt_k; ++k) {
                gt_topk[qi][k] = static_cast<int64_t>(row[k]);
            }
        }
        return gt_topk;
    }

    auto gt_or = io::LoadNpyInt64Matrix(gt_file);
    if (!gt_or.ok()) return gt_or.status();
    const auto& gt = gt_or.value();
    if (gt.rows < q_count) {
        return Status::InvalidArgument(
            "Ground-truth rows (" + std::to_string(gt.rows) +
            ") smaller than query count (" + std::to_string(q_count) + ")");
    }
    if (gt.cols < gt_k) {
        return Status::InvalidArgument(
            "Ground-truth cols (" + std::to_string(gt.cols) +
            ") smaller than required top-k (" + std::to_string(gt_k) + ")");
    }
    gt_topk.resize(q_count);
    for (uint32_t qi = 0; qi < q_count; ++qi) {
        gt_topk[qi].resize(gt_k);
        const int64_t* row = gt.data.data() + static_cast<size_t>(qi) * gt.cols;
        for (uint32_t k = 0; k < gt_k; ++k) {
            gt_topk[qi][k] = row[k];
        }
    }
    return gt_topk;
}

static const char* ExRaBitQStorageFormatName(uint32_t clu_file_version) {
    if (clu_file_version >= 14) return "official_1_plus_n_direct_compact_exdata";
    if (clu_file_version >= 13) return "official_1_plus_n_packed_exdata";
    if (clu_file_version >= 12) return "compact_blocked_packed_magnitude";
    if (clu_file_version >= 11) return "compact_blocked";
    if (clu_file_version >= 10) return "packed_sign";
    return "legacy_byte_sign";
}

// ============================================================================
// JSONL field parsing (no JSON library)
// ============================================================================

static int64_t ParseImageId(std::string_view line) {
    auto pos = line.find("\"image_id\":");
    if (pos == std::string_view::npos) return -1;
    pos += 11;  // skip "image_id":
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    int64_t val = 0;
    bool neg = false;
    if (pos < line.size() && line[pos] == '-') { neg = true; ++pos; }
    while (pos < line.size() && line[pos] >= '0' && line[pos] <= '9') {
        val = val * 10 + (line[pos] - '0');
        ++pos;
    }
    return neg ? -val : val;
}

static std::string ParseCaption(std::string_view line) {
    auto pos = line.find("\"caption\":");
    if (pos == std::string_view::npos) return "";
    pos += 10;  // skip "caption":
    // find opening quote
    pos = line.find('"', pos);
    if (pos == std::string_view::npos) return "";
    ++pos;  // skip opening quote
    auto end = line.find('"', pos);
    if (end == std::string_view::npos) return "";
    return std::string(line.substr(pos, end - pos));
}

// ============================================================================
// JSON output helpers (hand-crafted)
// ============================================================================

static std::string JStr(const std::string& k, const std::string& v) {
    return "\"" + k + "\": \"" + v + "\"";
}

static std::string JNum(const std::string& k, double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return "\"" + k + "\": " + buf;
}

static std::string JInt(const std::string& k, int64_t v) {
    return "\"" + k + "\": " + std::to_string(v);
}

static std::string JBool(const std::string& k, bool v) {
    return "\"" + k + "\": " + (v ? "true" : "false");
}

static float SelectPercentileForLog(std::vector<float> values, float percentile) {
    if (values.empty()) return 0.0f;
    std::sort(values.begin(), values.end());
    const float index = percentile * static_cast<float>(values.size() - 1);
    const size_t k = static_cast<size_t>(
        std::min(index, static_cast<float>(values.size() - 1)));
    return values[k];
}

static std::string JArr64(const std::string& k, const std::vector<int64_t>& v) {
    std::string s = "\"" + k + "\": [";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += ", ";
        s += std::to_string(v[i]);
    }
    s += "]";
    return s;
}

static std::string JArrF(const std::string& k, const std::vector<float>& v) {
    std::string s = "\"" + k + "\": [";
    char buf[32];
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) s += ", ";
        std::snprintf(buf, sizeof(buf), "%.6f", v[i]);
        s += buf;
    }
    s += "]";
    return s;
}

// ============================================================================
// Percentile (on pre-sorted vector)
// ============================================================================

static double Percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(idx);
    size_t hi = lo + 1;
    if (hi >= sorted.size()) return sorted.back();
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// ============================================================================
// Recall computation
// ============================================================================

static double ComputeRecallAtK(const std::vector<int64_t>& predicted,
                                const std::vector<int64_t>& gt,
                                uint32_t K) {
    uint32_t pk = std::min(K, static_cast<uint32_t>(predicted.size()));
    uint32_t gk = std::min(K, static_cast<uint32_t>(gt.size()));
    std::unordered_set<int64_t> gt_set(gt.begin(), gt.begin() + gk);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < pk; ++i) {
        if (gt_set.count(predicted[i])) ++hits;
    }
    return static_cast<double>(hits) / static_cast<double>(gk);
}

// ============================================================================
// Log
// ============================================================================

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ============================================================================
// Per-query result struct (used by both rounds)
// ============================================================================

struct QueryResult {
    int64_t query_id;
    std::vector<int64_t> predicted_ids;
    std::vector<float> predicted_dists;
    double query_time_ms;
    double io_wait_ms;
    double coarse_select_ms = 0;
    double coarse_score_ms = 0;
    double coarse_topn_ms = 0;
    uint32_t coarse_routing_mode = 0;
    uint32_t coarse_super_count = 0;
    uint32_t coarse_super_probes = 0;
    uint32_t coarse_child_candidates_scored = 0;
    uint32_t coarse_candidate_budget = 0;
    uint32_t coarse_exact_fallback = 0;
    uint32_t coarse_exact_overlap = 0;
    double coarse_hierarchy_build_ms = 0;
    double probe_time_ms;
    double probe_prepare_ms = 0;
    double probe_prepare_rotation_ms = 0;
    double probe_prepare_subtract_ms = 0;
    double probe_prepare_normalize_ms = 0;
    double probe_prepare_quantize_ms = 0;
    double probe_prepare_lut_build_ms = 0;
    double probe_prepare_quant_lut_ms = 0;
    double probe_stage1_ms = 0;
    double probe_stage1_estimate_ms = 0;
    double probe_stage1_mask_ms = 0;
    double probe_stage1_iterate_ms = 0;
    double probe_stage1_classify_only_ms = 0;
    double probe_stage2_ms = 0;
    double probe_stage2_collect_ms = 0;
    double probe_stage2_kernel_ms = 0;
    double probe_stage2_scatter_ms = 0;
    double probe_stage2_kernel_sign_flip_ms = 0;
    double probe_stage2_kernel_abs_fma_ms = 0;
    double probe_stage2_kernel_tail_ms = 0;
    double probe_stage2_kernel_reduce_ms = 0;
    double probe_stage2_decode_ms = 0;
    uint32_t stage1_fused_blocks = 0;
    uint32_t stage1_fused_safeout_lanes = 0;
    uint32_t stage1_fused_safein_lanes = 0;
    uint64_t stage2_masked_kernel_calls = 0;
    uint64_t stage2_lanes_requested = 0;
    uint64_t stage2_lanes_skipped = 0;
    uint64_t stage2_lanes_total_valid = 0;
    uint64_t stage2_decode_blocks = 0;
    uint64_t stage2_decode_input_bytes = 0;
    uint64_t stage2_decode_output_bytes = 0;
    double probe_classify_ms = 0;
    double probe_submit_ms = 0;
    double probe_submit_prepare_vec_only_ms = 0;
    double probe_submit_prepare_all_ms = 0;
    double probe_submit_emit_ms = 0;
    double probe_submit_vec_only_emit_ms = 0;
    double probe_submit_pending_slot_alloc_ms = 0;
    double probe_submit_prep_read_ms = 0;
    double rerank_cpu_ms = 0;
    double safein_payload_prefetch_ms = 0;
    double candidate_collect_ms = 0;
    double pool_vector_read_ms = 0;
    double rerank_compute_ms = 0;
    double rerank_vec_alloc_ms = 0;
    double rerank_vec_copy_ms = 0;
    double remaining_payload_fetch_ms = 0;
    double uring_prep_ms = 0;
    double uring_submit_ms = 0;
    double fetch_missing_ms = 0;
    uint32_t submit_calls = 0;
    uint32_t submit_window_flushes = 0;
    uint32_t submit_window_tail_flushes = 0;
    uint32_t vec_only_read_requests = 0;
    uint32_t all_read_requests = 0;
    uint32_t payload_read_requests = 0;
    uint32_t fixed_vec_buffer_hits = 0;
    uint32_t fixed_vec_buffer_misses = 0;
    double submit_window_requests = 0;
    double candidate_batches_per_cluster = 0;
    double safeout_frontier_estimates_buffered_per_cluster = 0;
    double safeout_frontier_estimates_merged_per_cluster = 0;
    double safeout_frontier_updates_per_cluster = 0;
    double stage2_block_lookups = 0;
    double stage2_block_reuses = 0;
    double safeout_frontier_buffer_ms = 0;
    double safeout_frontier_merge_ms = 0;
    double safeout_frontier_online_ms = 0;
    uint32_t dynamic_safein_clusters = 0;
    uint32_t dynamic_safein_active_clusters = 0;
    uint32_t dynamic_safein_disabled_clusters = 0;
    double dynamic_safein_threshold_avg = 0;
    double dynamic_safein_final_threshold = 0;
    double dynamic_safein_final_frontier = 0;
    uint32_t dynamic_safein_deferred_candidates = 0;
    uint32_t dynamic_safein_deferred_flushes = 0;
    uint32_t dynamic_safein_deferred_safein = 0;
    uint32_t safein_prefetch_candidates = 0;
    uint32_t safein_prefetch_true_topk = 0;
    uint32_t safein_prefetch_false = 0;
    uint32_t safein_prefetch_unknown = 0;
    uint32_t probed_clusters = 0;
    uint32_t total_probed;
    uint32_t safe_in;
    uint32_t safe_out;
    uint32_t uncertain;
    uint32_t s2_safe_in;
    uint32_t s2_safe_out;
    uint32_t s2_uncertain;
    uint32_t duplicate_candidates = 0;
    uint32_t deduplicated_candidates = 0;
    uint32_t unique_fetch_candidates = 0;
    uint32_t candidate_budget_seen = 0;
    uint32_t candidate_budget_selected = 0;
    uint32_t candidate_budget_dropped = 0;
    uint32_t num_candidates_buffered = 0;
    uint32_t num_candidates_reranked = 0;
    uint32_t num_safein_payload_prefetched = 0;
    uint32_t num_remaining_payload_fetches = 0;
    uint32_t false_safeout;       // GT IDs missing from predicted results
    uint32_t false_safein_upper;  // safe_in - hits_in_topk (upper bound)
};

// ============================================================================
// Aggregated round metrics
// ============================================================================

struct RoundMetrics {
    double recall_at[3] = {};          // recall@1, @5, @10
    double recall_at_k = 0;
    bool recall_available = true;
    double avg_query_ms = 0;
    double p50 = 0, p95 = 0, p99 = 0;
    double avg_io_wait = 0;
    double avg_cpu = 0;
    double avg_coarse_select = 0;
    double avg_coarse_score = 0;
    double avg_coarse_topn = 0;
    double avg_coarse_routing_mode = 0;
    double avg_coarse_super_count = 0;
    double avg_coarse_super_probes = 0;
    double avg_coarse_child_candidates_scored = 0;
    double avg_coarse_candidate_budget = 0;
    double avg_coarse_exact_fallback = 0;
    double avg_coarse_exact_overlap = 0;
    double avg_coarse_hierarchy_build_ms = 0;
    double avg_probe = 0;
    double avg_probe_prepare = 0;
    double avg_probe_prepare_rotation = 0;
    double avg_probe_prepare_subtract = 0;
    double avg_probe_prepare_normalize = 0;
    double avg_probe_prepare_quantize = 0;
    double avg_probe_prepare_lut_build = 0;
    double avg_probe_prepare_quant_lut = 0;
    double avg_probe_stage1 = 0;
    double avg_probe_stage1_estimate = 0;
    double avg_probe_stage1_mask = 0;
    double avg_probe_stage1_iterate = 0;
    double avg_probe_stage1_classify_only = 0;
    double avg_probe_stage2 = 0;
    double avg_probe_stage2_collect = 0;
    double avg_probe_stage2_kernel = 0;
    double avg_probe_stage2_scatter = 0;
    double avg_probe_stage2_kernel_sign_flip = 0;
    double avg_probe_stage2_kernel_abs_fma = 0;
    double avg_probe_stage2_kernel_tail = 0;
    double avg_probe_stage2_kernel_reduce = 0;
    double avg_probe_stage2_decode = 0;
    double avg_stage1_fused_blocks = 0;
    double avg_stage1_fused_safeout_lanes = 0;
    double avg_stage1_fused_safein_lanes = 0;
    double avg_stage2_masked_kernel_calls = 0;
    double avg_stage2_lanes_requested = 0;
    double avg_stage2_lanes_skipped = 0;
    double avg_stage2_lanes_total_valid = 0;
    double avg_stage2_lane_density = 0;
    double avg_stage2_decode_blocks = 0;
    double avg_stage2_decode_input_bytes = 0;
    double avg_stage2_decode_output_bytes = 0;
    double avg_probe_classify = 0;
    double avg_probe_submit = 0;
    double avg_probe_submit_prepare_vec_only = 0;
    double avg_probe_submit_prepare_all = 0;
    double avg_probe_submit_emit = 0;
    double avg_probe_submit_vec_only_emit = 0;
    double avg_probe_submit_pending_slot_alloc = 0;
    double avg_probe_submit_prep_read = 0;
    double avg_rerank_cpu = 0;
    double avg_safein_payload_prefetch = 0;
    double avg_candidate_collect = 0;
    double avg_pool_vector_read = 0;
    double avg_rerank_compute = 0;
    double avg_rerank_vec_alloc = 0;
    double avg_rerank_vec_copy = 0;
    double avg_remaining_payload_fetch = 0;
    double avg_uring_prep = 0;
    double avg_uring_submit = 0;
    double avg_fetch_missing = 0;
    double avg_submit_calls = 0;
    double avg_submit_window_flushes = 0;
    double avg_submit_window_tail_flushes = 0;
    double avg_submit_window_requests = 0;
    double avg_vec_only_read_requests = 0;
    double avg_all_read_requests = 0;
    double avg_payload_read_requests = 0;
    double avg_fixed_vec_buffer_hits = 0;
    double avg_fixed_vec_buffer_misses = 0;
    double avg_probed_clusters = 0;
    double avg_probed = 0;
    double avg_safe_in = 0;
    double avg_safe_out = 0;
    double avg_uncertain = 0;
    double avg_s2_safe_in = 0;
    double avg_s2_safe_out = 0;
    double avg_s2_uncertain = 0;
    double avg_duplicate_candidates = 0;
    double avg_deduplicated_candidates = 0;
    double avg_unique_fetch_candidates = 0;
    double avg_candidate_budget_seen = 0;
    double avg_candidate_budget_selected = 0;
    double avg_candidate_budget_dropped = 0;
    double avg_candidate_batches_per_cluster = 0;
    double avg_safeout_frontier_estimates_buffered_per_cluster = 0;
    double avg_safeout_frontier_estimates_merged_per_cluster = 0;
    double avg_safeout_frontier_updates_per_cluster = 0;
    double avg_stage2_block_lookups = 0;
    double avg_stage2_block_reuses = 0;
    double avg_safeout_frontier_buffer_ms = 0;
    double avg_safeout_frontier_merge_ms = 0;
    double avg_safeout_frontier_online_ms = 0;
    double avg_dynamic_safein_clusters = 0;
    double avg_dynamic_safein_active_clusters = 0;
    double avg_dynamic_safein_disabled_clusters = 0;
    double avg_dynamic_safein_threshold = 0;
    double avg_dynamic_safein_final_frontier = 0;
    double avg_dynamic_safein_deferred_candidates = 0;
    double avg_dynamic_safein_deferred_flushes = 0;
    double avg_dynamic_safein_deferred_safein = 0;
    double avg_safein_prefetch_candidates = 0;
    double avg_safein_prefetch_true_topk = 0;
    double avg_safein_prefetch_false = 0;
    double safein_prefetch_false_rate = 0;
    double safein_prefetch_topk_coverage = 0;
    double avg_candidates_buffered = 0;
    double avg_candidates_reranked = 0;
    double avg_safein_payload_prefetched = 0;
    double avg_remaining_payload_fetches = 0;
    double avg_false_safeout = 0;
    double avg_false_safein_upper = 0;
    uint64_t total_final_safein = 0;   // absolute count: S1 SafeIn + S2 SafeIn
    double overlap_ratio = 0;
    double preload_time_ms = 0;
    double preload_bytes = 0;
    double resident_file_size_bytes = 0;
    double resident_file_buffer_bytes = 0;
    double resident_code_storage_bytes = 0;
    double resident_decoded_address_bytes = 0;
    double resident_raw_address_bytes = 0;
    double resident_parsed_address_duplicate_bytes = 0;
    double resident_preload_batch_size = 0;
    double resident_cluster_mem_bytes = 0;
    double resident_parallel_view_build_ms = 0;
    double resident_parallel_view_bytes = 0;
    double index_total_bytes = 0;
    double index_cluster_clu_bytes = 0;
    double index_data_dat_bytes = 0;
    double index_rotation_bytes = 0;
    double index_rotated_centroids_bytes = 0;
};

// ============================================================================
// Run one query round and compute metrics
// ============================================================================

static std::pair<std::vector<QueryResult>, RoundMetrics> RunQueryRound(
    const char* label,
    IvfIndex& index, AsyncReader& cluster_reader, AsyncReader* data_reader,
    const SearchConfig& search_cfg,
    const float* queries, uint32_t Q, Dim dim,
    const std::vector<int64_t>& qry_ids_data,
    const std::vector<std::vector<int64_t>>& gt_topk,
    const std::vector<std::vector<float>>& gt_dists,
    bool recall_available) {
    (void)gt_dists;

    Log("\n[%s] Querying %u vectors...\n", label, Q);

    if (!index.segment().resident_preload_enabled()) {
        auto preload_status = index.segment().PreloadAllClusters();
        if (!preload_status.ok()) {
            std::fprintf(stderr,
                         "Failed to preload .clu before benchmark round: %s\n",
                         preload_status.ToString().c_str());
            std::abort();
        }
    }

    if (search_cfg.enable_two_level_coarse_routing) {
        index.SetTwoLevelCoarseRouting(search_cfg.enable_two_level_coarse_routing,
                                       search_cfg.two_level_coarse_threshold,
                                       search_cfg.two_level_coarse_super_count,
                                       search_cfg.two_level_coarse_super_factor,
                                       search_cfg.two_level_coarse_budget_factor,
                                       search_cfg.enable_two_level_coarse_exact_overlap);
        (void)index.PrepareTwoLevelCoarseRouting(search_cfg.nprobe);
    }
    std::unique_ptr<OverlapScheduler> scheduler;
    if (data_reader != nullptr) {
        scheduler = std::make_unique<OverlapScheduler>(
            index, cluster_reader, *data_reader, search_cfg);
    } else {
        scheduler = std::make_unique<OverlapScheduler>(
            index, cluster_reader, search_cfg);
    }

    std::vector<QueryResult> qresults(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) {
        const float* qvec = queries + static_cast<size_t>(qi) * dim;
        auto results = scheduler->Search(qvec);

        QueryResult& qr = qresults[qi];
        qr.query_id = qry_ids_data[qi];
        qr.query_time_ms = results.stats().total_time_ms;
        qr.io_wait_ms = results.stats().io_wait_time_ms;
        qr.coarse_select_ms = results.stats().coarse_select_ms;
        qr.coarse_score_ms = results.stats().coarse_score_ms;
        qr.coarse_topn_ms = results.stats().coarse_topn_ms;
        qr.coarse_routing_mode = results.stats().coarse_routing_mode;
        qr.coarse_super_count = results.stats().coarse_super_count;
        qr.coarse_super_probes = results.stats().coarse_super_probes;
        qr.coarse_child_candidates_scored =
            results.stats().coarse_child_candidates_scored;
        qr.coarse_candidate_budget = results.stats().coarse_candidate_budget;
        qr.coarse_exact_fallback = results.stats().coarse_exact_fallback;
        qr.coarse_exact_overlap = results.stats().coarse_exact_overlap;
        qr.coarse_hierarchy_build_ms = results.stats().coarse_hierarchy_build_ms;
        qr.probe_time_ms = results.stats().probe_time_ms;
        qr.probe_prepare_ms = results.stats().probe_prepare_ms;
        qr.probe_prepare_rotation_ms = results.stats().probe_prepare_rotation_ms;
        qr.probe_prepare_subtract_ms = results.stats().probe_prepare_subtract_ms;
        qr.probe_prepare_normalize_ms = results.stats().probe_prepare_normalize_ms;
        qr.probe_prepare_quantize_ms = results.stats().probe_prepare_quantize_ms;
        qr.probe_prepare_lut_build_ms = results.stats().probe_prepare_lut_build_ms;
        qr.probe_prepare_quant_lut_ms = results.stats().probe_prepare_quant_lut_ms;
        qr.probe_stage1_ms = results.stats().probe_stage1_ms;
        qr.probe_stage1_estimate_ms = results.stats().probe_stage1_estimate_ms;
        qr.probe_stage1_mask_ms = results.stats().probe_stage1_mask_ms;
        qr.probe_stage1_iterate_ms = results.stats().probe_stage1_iterate_ms;
        qr.probe_stage1_classify_only_ms = results.stats().probe_stage1_classify_only_ms;
        qr.probe_stage2_ms = results.stats().probe_stage2_ms;
        qr.probe_stage2_collect_ms = results.stats().probe_stage2_collect_ms;
        qr.probe_stage2_kernel_ms = results.stats().probe_stage2_kernel_ms;
        qr.probe_stage2_scatter_ms = results.stats().probe_stage2_scatter_ms;
        qr.probe_stage2_kernel_sign_flip_ms = results.stats().probe_stage2_kernel_sign_flip_ms;
        qr.probe_stage2_kernel_abs_fma_ms = results.stats().probe_stage2_kernel_abs_fma_ms;
        qr.probe_stage2_kernel_tail_ms = results.stats().probe_stage2_kernel_tail_ms;
        qr.probe_stage2_kernel_reduce_ms = results.stats().probe_stage2_kernel_reduce_ms;
        qr.probe_stage2_decode_ms = results.stats().probe_stage2_decode_ms;
        qr.stage1_fused_blocks = results.stats().stage1_fused_blocks;
        qr.stage1_fused_safeout_lanes = results.stats().stage1_fused_safeout_lanes;
        qr.stage1_fused_safein_lanes = results.stats().stage1_fused_safein_lanes;
        qr.stage2_masked_kernel_calls = results.stats().stage2_masked_kernel_calls;
        qr.stage2_lanes_requested = results.stats().stage2_lanes_requested;
        qr.stage2_lanes_skipped = results.stats().stage2_lanes_skipped;
        qr.stage2_lanes_total_valid = results.stats().stage2_lanes_total_valid;
        qr.stage2_decode_blocks = results.stats().stage2_decode_blocks;
        qr.stage2_decode_input_bytes = results.stats().stage2_decode_input_bytes;
        qr.stage2_decode_output_bytes = results.stats().stage2_decode_output_bytes;
        qr.probe_classify_ms = results.stats().probe_classify_ms;
        qr.probe_submit_ms = results.stats().probe_submit_ms;
        qr.probe_submit_prepare_vec_only_ms =
            results.stats().probe_submit_prepare_vec_only_ms;
        qr.probe_submit_prepare_all_ms =
            results.stats().probe_submit_prepare_all_ms;
        qr.probe_submit_emit_ms = results.stats().probe_submit_emit_ms;
        qr.probe_submit_vec_only_emit_ms =
            results.stats().probe_submit_vec_only_emit_ms;
        qr.probe_submit_pending_slot_alloc_ms =
            results.stats().probe_submit_pending_slot_alloc_ms;
        qr.probe_submit_prep_read_ms =
            results.stats().probe_submit_prep_read_ms;
        qr.rerank_cpu_ms = results.stats().rerank_cpu_ms;
        qr.safein_payload_prefetch_ms = results.stats().safein_payload_prefetch_ms;
        qr.candidate_collect_ms = results.stats().candidate_collect_ms;
        qr.pool_vector_read_ms = results.stats().pool_vector_read_ms;
        qr.rerank_compute_ms = results.stats().rerank_compute_ms;
        qr.rerank_vec_alloc_ms = results.stats().rerank_vec_alloc_ms;
        qr.rerank_vec_copy_ms = results.stats().rerank_vec_copy_ms;
        qr.remaining_payload_fetch_ms = results.stats().remaining_payload_fetch_ms;
        qr.uring_prep_ms = results.stats().uring_prep_ms;
        qr.uring_submit_ms = results.stats().uring_submit_ms;
        qr.fetch_missing_ms = results.stats().fetch_missing_ms;
        qr.submit_calls = results.stats().total_submit_calls;
        qr.submit_window_flushes = results.stats().total_submit_window_flushes;
        qr.submit_window_tail_flushes =
            results.stats().total_submit_window_tail_flushes;
        qr.vec_only_read_requests = results.stats().vec_only_read_requests;
        qr.all_read_requests = results.stats().all_read_requests;
        qr.payload_read_requests = results.stats().payload_read_requests;
        qr.fixed_vec_buffer_hits = results.stats().fixed_vec_buffer_hits;
        qr.fixed_vec_buffer_misses = results.stats().fixed_vec_buffer_misses;
        qr.submit_window_requests =
            static_cast<double>(results.stats().total_submit_window_requests);
        qr.safeout_frontier_buffer_ms = results.stats().safeout_frontier_buffer_ms;
        qr.safeout_frontier_merge_ms = results.stats().safeout_frontier_merge_ms;
        qr.safeout_frontier_online_ms =
            qr.safeout_frontier_buffer_ms + qr.safeout_frontier_merge_ms;
        qr.dynamic_safein_clusters = results.stats().dynamic_safein_clusters;
        qr.dynamic_safein_active_clusters =
            results.stats().dynamic_safein_active_clusters;
        qr.dynamic_safein_disabled_clusters =
            results.stats().dynamic_safein_disabled_clusters;
        qr.dynamic_safein_threshold_avg =
            results.stats().dynamic_safein_threshold_samples > 0
                ? results.stats().dynamic_safein_threshold_sum /
                      results.stats().dynamic_safein_threshold_samples
                : 0.0;
        qr.dynamic_safein_final_threshold =
            results.stats().dynamic_safein_final_threshold;
        qr.dynamic_safein_final_frontier =
            results.stats().dynamic_safein_final_frontier;
        qr.dynamic_safein_deferred_candidates =
            results.stats().dynamic_safein_deferred_candidates;
        qr.dynamic_safein_deferred_flushes =
            results.stats().dynamic_safein_deferred_flushes;
        qr.dynamic_safein_deferred_safein =
            results.stats().dynamic_safein_deferred_safein;
        qr.safein_prefetch_candidates =
            results.stats().safein_prefetch_candidates;
        qr.safein_prefetch_true_topk =
            results.stats().safein_prefetch_true_topk;
        qr.safein_prefetch_false = results.stats().safein_prefetch_false;
        qr.safein_prefetch_unknown = results.stats().safein_prefetch_unknown;
        qr.stage2_block_lookups = static_cast<double>(results.stats().total_stage2_block_lookups);
        qr.stage2_block_reuses = static_cast<double>(results.stats().total_stage2_block_reuses);
        const uint32_t probed_clusters_u32 = search_cfg.nprobe;
        qr.probed_clusters = probed_clusters_u32;
        const double probed_clusters = static_cast<double>(probed_clusters_u32);
        if (probed_clusters > 0.0) {
            qr.candidate_batches_per_cluster =
                static_cast<double>(results.stats().total_candidate_batches) /
                probed_clusters;
            qr.safeout_frontier_estimates_buffered_per_cluster =
                static_cast<double>(
                    results.stats().total_safeout_frontier_estimates_buffered) /
                probed_clusters;
            qr.safeout_frontier_estimates_merged_per_cluster =
                static_cast<double>(
                    results.stats().total_safeout_frontier_estimates_merged) /
                probed_clusters;
            qr.safeout_frontier_updates_per_cluster =
                static_cast<double>(results.stats().total_safeout_frontier_updates) /
                probed_clusters;
        }
        qr.total_probed = results.stats().total_probed;
        qr.safe_in = results.stats().total_safe_in;
        qr.safe_out = results.stats().total_safe_out;
        qr.uncertain = results.stats().total_uncertain;
        qr.s2_safe_in = results.stats().s2_safe_in;
        qr.s2_safe_out = results.stats().s2_safe_out;
        qr.s2_uncertain = results.stats().s2_uncertain;
        qr.duplicate_candidates = results.stats().duplicate_candidates;
        qr.deduplicated_candidates = results.stats().deduplicated_candidates;
        qr.unique_fetch_candidates = results.stats().unique_fetch_candidates;
        qr.candidate_budget_seen = results.stats().candidate_budget_seen;
        qr.candidate_budget_selected =
            results.stats().candidate_budget_selected;
        qr.candidate_budget_dropped =
            results.stats().candidate_budget_dropped;
        qr.num_candidates_buffered = results.stats().buffered_candidates;
        qr.num_candidates_reranked = results.stats().reranked_candidates;
        qr.num_safein_payload_prefetched = results.stats().total_safein_payload_prefetched;
        qr.num_remaining_payload_fetches = results.stats().total_payload_fetched;

        for (uint32_t j = 0; j < results.size(); ++j) {
            qr.predicted_dists.push_back(results[j].distance);
            if (!results[j].payload.empty() &&
                results[j].payload[0].dtype == DType::INT64) {
                qr.predicted_ids.push_back(results[j].payload[0].fixed.i64);
            }
        }

        // False SafeOut: GT IDs not found in predicted results
        if (recall_available && qi < gt_topk.size()) {
            uint32_t gt_k = std::min(search_cfg.top_k,
                static_cast<uint32_t>(gt_topk[qi].size()));
            std::unordered_set<int64_t> pred_set(
                qr.predicted_ids.begin(), qr.predicted_ids.end());
            uint32_t missing = 0;
            uint32_t hits = 0;
            for (uint32_t g = 0; g < gt_k; ++g) {
                if (pred_set.count(gt_topk[qi][g]) == 0) missing++;
                else hits++;
            }
            qr.false_safeout = missing;
            qr.false_safein_upper = (qr.safe_in > hits)
                ? (qr.safe_in - hits) : 0;
        }

        if ((qi + 1) % 100 == 0 || qi + 1 == Q) {
            Log("  %s progress: %u/%u\n", label, qi + 1, Q);
        }
    }
    Log("  %s: all %u queries complete.\n", label, Q);

    // Compute metrics
    RoundMetrics m;
    m.recall_available = recall_available;
    uint32_t recall_K[3] = {1, 5, 10};
    double recall_sum[3] = {0, 0, 0};
    if (recall_available) {
        for (uint32_t qi = 0; qi < Q; ++qi) {
            for (int r = 0; r < 3; ++r) {
                recall_sum[r] += ComputeRecallAtK(qresults[qi].predicted_ids,
                                                  gt_topk[qi], recall_K[r]);
            }
        }
        for (int r = 0; r < 3; ++r) m.recall_at[r] = recall_sum[r] / Q;
        double recall_k_sum = 0.0;
        for (uint32_t qi = 0; qi < Q; ++qi) {
            recall_k_sum += ComputeRecallAtK(qresults[qi].predicted_ids,
                                             gt_topk[qi], search_cfg.top_k);
        }
        m.recall_at_k = recall_k_sum / Q;
    }

    std::vector<double> query_times(Q);
    for (uint32_t qi = 0; qi < Q; ++qi) query_times[qi] = qresults[qi].query_time_ms;
    std::sort(query_times.begin(), query_times.end());
    m.avg_query_ms = std::accumulate(query_times.begin(), query_times.end(), 0.0) / Q;
    m.p50 = Percentile(query_times, 0.50);
    m.p95 = Percentile(query_times, 0.95);
    m.p99 = Percentile(query_times, 0.99);

    double sum_probed_clusters = 0, sum_probed = 0, sum_si = 0, sum_so = 0, sum_unc = 0;
    double sum_s2_si = 0, sum_s2_so = 0, sum_s2_unc = 0;
    double sum_dup = 0, sum_dedup = 0, sum_unique_fetch = 0;
    double sum_candidate_budget_seen = 0;
    double sum_candidate_budget_selected = 0;
    double sum_candidate_budget_dropped = 0;
    double sum_false_so = 0, sum_false_si = 0;
    double sum_io_wait = 0, sum_total = 0;
    double sum_coarse_select = 0, sum_coarse_score = 0, sum_coarse_topn = 0;
    double sum_coarse_routing_mode = 0;
    double sum_coarse_super_count = 0;
    double sum_coarse_super_probes = 0;
    double sum_coarse_child_candidates_scored = 0;
    double sum_coarse_candidate_budget = 0;
    double sum_coarse_exact_fallback = 0;
    double sum_coarse_exact_overlap = 0;
    double sum_coarse_hierarchy_build = 0;
    double sum_probe = 0, sum_probe_prepare = 0;
    double sum_probe_prepare_rotation = 0;
    double sum_probe_prepare_subtract = 0;
    double sum_probe_prepare_normalize = 0;
    double sum_probe_prepare_quantize = 0;
    double sum_probe_prepare_lut_build = 0;
    double sum_probe_prepare_quant_lut = 0;
    double sum_probe_stage1 = 0, sum_probe_stage1_estimate = 0;
    double sum_probe_stage1_mask = 0, sum_probe_stage1_iterate = 0;
    double sum_probe_stage1_classify_only = 0, sum_probe_stage2 = 0;
    double sum_probe_stage2_collect = 0;
    double sum_probe_stage2_kernel = 0;
    double sum_probe_stage2_scatter = 0;
    double sum_probe_stage2_kernel_sign_flip = 0;
    double sum_probe_stage2_kernel_abs_fma = 0;
    double sum_probe_stage2_kernel_tail = 0;
    double sum_probe_stage2_kernel_reduce = 0;
    double sum_probe_stage2_decode = 0;
    double sum_stage1_fused_blocks = 0;
    double sum_stage1_fused_safeout_lanes = 0;
    double sum_stage1_fused_safein_lanes = 0;
    double sum_stage2_masked_kernel_calls = 0;
    double sum_stage2_lanes_requested = 0;
    double sum_stage2_lanes_skipped = 0;
    double sum_stage2_lanes_total_valid = 0;
    double sum_stage2_decode_blocks = 0;
    double sum_stage2_decode_input_bytes = 0;
    double sum_stage2_decode_output_bytes = 0;
    double sum_probe_classify = 0, sum_probe_submit = 0;
    double sum_probe_submit_prepare_vec_only = 0;
    double sum_probe_submit_prepare_all = 0;
    double sum_probe_submit_emit = 0;
    double sum_probe_submit_vec_only_emit = 0;
    double sum_probe_submit_pending_slot_alloc = 0;
    double sum_probe_submit_prep_read = 0;
    double sum_rerank_cpu = 0;
    double sum_safein_payload_prefetch = 0, sum_candidate_collect = 0;
    double sum_pool_vector_read = 0, sum_rerank_compute = 0;
    double sum_rerank_vec_alloc = 0, sum_rerank_vec_copy = 0;
    double sum_remaining_payload_fetch = 0;
    double sum_uring_prep = 0, sum_uring_submit = 0;
    double sum_fetch_missing = 0;
    double sum_submit_calls = 0;
    double sum_submit_window_flushes = 0;
    double sum_submit_window_tail_flushes = 0;
    double sum_submit_window_requests = 0;
    double sum_vec_only_read_requests = 0;
    double sum_all_read_requests = 0;
    double sum_payload_read_requests = 0;
    double sum_fixed_vec_buffer_hits = 0;
    double sum_fixed_vec_buffer_misses = 0;
    double sum_candidate_batches_per_cluster = 0;
    double sum_safeout_frontier_estimates_buffered_per_cluster = 0;
    double sum_safeout_frontier_estimates_merged_per_cluster = 0;
    double sum_safeout_frontier_updates_per_cluster = 0;
    double sum_stage2_block_lookups = 0;
    double sum_stage2_block_reuses = 0;
    double sum_safeout_frontier_buffer = 0;
    double sum_safeout_frontier_merge = 0;
    double sum_safeout_frontier_online = 0;
    double sum_dynamic_safein_clusters = 0;
    double sum_dynamic_safein_active_clusters = 0;
    double sum_dynamic_safein_disabled_clusters = 0;
    double sum_dynamic_safein_threshold = 0;
    double sum_dynamic_safein_final_frontier = 0;
    double sum_dynamic_safein_deferred_candidates = 0;
    double sum_dynamic_safein_deferred_flushes = 0;
    double sum_dynamic_safein_deferred_safein = 0;
    double sum_safein_prefetch_candidates = 0;
    double sum_safein_prefetch_true_topk = 0;
    double sum_safein_prefetch_false = 0;
    double sum_candidates_buffered = 0, sum_candidates_reranked = 0;
    double sum_safein_payload_prefetched = 0, sum_remaining_payload_fetches = 0;
    for (uint32_t qi = 0; qi < Q; ++qi) {
        sum_probed_clusters += qresults[qi].probed_clusters;
        sum_probed += qresults[qi].total_probed;
        sum_si += qresults[qi].safe_in;
        sum_so += qresults[qi].safe_out;
        sum_unc += qresults[qi].uncertain;
        sum_s2_si += qresults[qi].s2_safe_in;
        sum_s2_so += qresults[qi].s2_safe_out;
        sum_s2_unc += qresults[qi].s2_uncertain;
        sum_dup += qresults[qi].duplicate_candidates;
        sum_dedup += qresults[qi].deduplicated_candidates;
        sum_unique_fetch += qresults[qi].unique_fetch_candidates;
        sum_candidate_budget_seen += qresults[qi].candidate_budget_seen;
        sum_candidate_budget_selected += qresults[qi].candidate_budget_selected;
        sum_candidate_budget_dropped += qresults[qi].candidate_budget_dropped;
        sum_false_so += qresults[qi].false_safeout;
        sum_false_si += qresults[qi].false_safein_upper;
        sum_io_wait += qresults[qi].io_wait_ms;
        sum_coarse_select += qresults[qi].coarse_select_ms;
        sum_coarse_score += qresults[qi].coarse_score_ms;
        sum_coarse_topn += qresults[qi].coarse_topn_ms;
        sum_coarse_routing_mode += qresults[qi].coarse_routing_mode;
        sum_coarse_super_count += qresults[qi].coarse_super_count;
        sum_coarse_super_probes += qresults[qi].coarse_super_probes;
        sum_coarse_child_candidates_scored +=
            qresults[qi].coarse_child_candidates_scored;
        sum_coarse_candidate_budget += qresults[qi].coarse_candidate_budget;
        sum_coarse_exact_fallback += qresults[qi].coarse_exact_fallback;
        sum_coarse_exact_overlap += qresults[qi].coarse_exact_overlap;
        sum_coarse_hierarchy_build += qresults[qi].coarse_hierarchy_build_ms;
        sum_probe += qresults[qi].probe_time_ms;
        sum_probe_prepare += qresults[qi].probe_prepare_ms;
        sum_probe_prepare_rotation += qresults[qi].probe_prepare_rotation_ms;
        sum_probe_prepare_subtract += qresults[qi].probe_prepare_subtract_ms;
        sum_probe_prepare_normalize += qresults[qi].probe_prepare_normalize_ms;
        sum_probe_prepare_quantize += qresults[qi].probe_prepare_quantize_ms;
        sum_probe_prepare_lut_build += qresults[qi].probe_prepare_lut_build_ms;
        sum_probe_prepare_quant_lut += qresults[qi].probe_prepare_quant_lut_ms;
        sum_probe_stage1 += qresults[qi].probe_stage1_ms;
        sum_probe_stage1_estimate += qresults[qi].probe_stage1_estimate_ms;
        sum_probe_stage1_mask += qresults[qi].probe_stage1_mask_ms;
        sum_probe_stage1_iterate += qresults[qi].probe_stage1_iterate_ms;
        sum_probe_stage1_classify_only += qresults[qi].probe_stage1_classify_only_ms;
        sum_probe_stage2 += qresults[qi].probe_stage2_ms;
        sum_probe_stage2_collect += qresults[qi].probe_stage2_collect_ms;
        sum_probe_stage2_kernel += qresults[qi].probe_stage2_kernel_ms;
        sum_probe_stage2_scatter += qresults[qi].probe_stage2_scatter_ms;
        sum_probe_stage2_kernel_sign_flip += qresults[qi].probe_stage2_kernel_sign_flip_ms;
        sum_probe_stage2_kernel_abs_fma += qresults[qi].probe_stage2_kernel_abs_fma_ms;
        sum_probe_stage2_kernel_tail += qresults[qi].probe_stage2_kernel_tail_ms;
        sum_probe_stage2_kernel_reduce += qresults[qi].probe_stage2_kernel_reduce_ms;
        sum_probe_stage2_decode += qresults[qi].probe_stage2_decode_ms;
        sum_stage1_fused_blocks += qresults[qi].stage1_fused_blocks;
        sum_stage1_fused_safeout_lanes += qresults[qi].stage1_fused_safeout_lanes;
        sum_stage1_fused_safein_lanes += qresults[qi].stage1_fused_safein_lanes;
        sum_stage2_masked_kernel_calls += qresults[qi].stage2_masked_kernel_calls;
        sum_stage2_lanes_requested += qresults[qi].stage2_lanes_requested;
        sum_stage2_lanes_skipped += qresults[qi].stage2_lanes_skipped;
        sum_stage2_lanes_total_valid += qresults[qi].stage2_lanes_total_valid;
        sum_stage2_decode_blocks += qresults[qi].stage2_decode_blocks;
        sum_stage2_decode_input_bytes += qresults[qi].stage2_decode_input_bytes;
        sum_stage2_decode_output_bytes += qresults[qi].stage2_decode_output_bytes;
        sum_probe_classify += qresults[qi].probe_classify_ms;
        sum_probe_submit += qresults[qi].probe_submit_ms;
        sum_probe_submit_prepare_vec_only +=
            qresults[qi].probe_submit_prepare_vec_only_ms;
        sum_probe_submit_prepare_all +=
            qresults[qi].probe_submit_prepare_all_ms;
        sum_probe_submit_emit += qresults[qi].probe_submit_emit_ms;
        sum_probe_submit_vec_only_emit +=
            qresults[qi].probe_submit_vec_only_emit_ms;
        sum_probe_submit_pending_slot_alloc +=
            qresults[qi].probe_submit_pending_slot_alloc_ms;
        sum_probe_submit_prep_read +=
            qresults[qi].probe_submit_prep_read_ms;
        sum_rerank_cpu += qresults[qi].rerank_cpu_ms;
        sum_safein_payload_prefetch += qresults[qi].safein_payload_prefetch_ms;
        sum_candidate_collect += qresults[qi].candidate_collect_ms;
        sum_pool_vector_read += qresults[qi].pool_vector_read_ms;
        sum_rerank_compute += qresults[qi].rerank_compute_ms;
        sum_rerank_vec_alloc += qresults[qi].rerank_vec_alloc_ms;
        sum_rerank_vec_copy += qresults[qi].rerank_vec_copy_ms;
        sum_remaining_payload_fetch += qresults[qi].remaining_payload_fetch_ms;
        sum_uring_prep += qresults[qi].uring_prep_ms;
        sum_uring_submit += qresults[qi].uring_submit_ms;
        sum_fetch_missing += qresults[qi].fetch_missing_ms;
        sum_submit_calls += qresults[qi].submit_calls;
        sum_submit_window_flushes += qresults[qi].submit_window_flushes;
        sum_submit_window_tail_flushes +=
            qresults[qi].submit_window_tail_flushes;
        sum_submit_window_requests += qresults[qi].submit_window_requests;
        sum_vec_only_read_requests += qresults[qi].vec_only_read_requests;
        sum_all_read_requests += qresults[qi].all_read_requests;
        sum_payload_read_requests += qresults[qi].payload_read_requests;
        sum_fixed_vec_buffer_hits += qresults[qi].fixed_vec_buffer_hits;
        sum_fixed_vec_buffer_misses += qresults[qi].fixed_vec_buffer_misses;
        sum_candidate_batches_per_cluster += qresults[qi].candidate_batches_per_cluster;
        sum_safeout_frontier_estimates_buffered_per_cluster +=
            qresults[qi].safeout_frontier_estimates_buffered_per_cluster;
        sum_safeout_frontier_estimates_merged_per_cluster +=
            qresults[qi].safeout_frontier_estimates_merged_per_cluster;
        sum_safeout_frontier_updates_per_cluster +=
            qresults[qi].safeout_frontier_updates_per_cluster;
        sum_stage2_block_lookups += qresults[qi].stage2_block_lookups;
        sum_stage2_block_reuses += qresults[qi].stage2_block_reuses;
        sum_safeout_frontier_buffer += qresults[qi].safeout_frontier_buffer_ms;
        sum_safeout_frontier_merge += qresults[qi].safeout_frontier_merge_ms;
        sum_safeout_frontier_online += qresults[qi].safeout_frontier_online_ms;
        sum_dynamic_safein_clusters += qresults[qi].dynamic_safein_clusters;
        sum_dynamic_safein_active_clusters +=
            qresults[qi].dynamic_safein_active_clusters;
        sum_dynamic_safein_disabled_clusters +=
            qresults[qi].dynamic_safein_disabled_clusters;
        sum_dynamic_safein_threshold += qresults[qi].dynamic_safein_threshold_avg;
        sum_dynamic_safein_final_frontier +=
            qresults[qi].dynamic_safein_final_frontier;
        sum_dynamic_safein_deferred_candidates +=
            qresults[qi].dynamic_safein_deferred_candidates;
        sum_dynamic_safein_deferred_flushes +=
            qresults[qi].dynamic_safein_deferred_flushes;
        sum_dynamic_safein_deferred_safein +=
            qresults[qi].dynamic_safein_deferred_safein;
        sum_safein_prefetch_candidates +=
            qresults[qi].safein_prefetch_candidates;
        sum_safein_prefetch_true_topk +=
            qresults[qi].safein_prefetch_true_topk;
        sum_safein_prefetch_false += qresults[qi].safein_prefetch_false;
        sum_candidates_buffered += qresults[qi].num_candidates_buffered;
        sum_candidates_reranked += qresults[qi].num_candidates_reranked;
        sum_safein_payload_prefetched += qresults[qi].num_safein_payload_prefetched;
        sum_remaining_payload_fetches += qresults[qi].num_remaining_payload_fetches;
        sum_total += qresults[qi].query_time_ms;
    }

    m.avg_io_wait = sum_io_wait / Q;
    m.avg_cpu = (sum_total - sum_io_wait) / Q;
    m.avg_coarse_select = sum_coarse_select / Q;
    m.avg_coarse_score = sum_coarse_score / Q;
    m.avg_coarse_topn = sum_coarse_topn / Q;
    m.avg_coarse_routing_mode = sum_coarse_routing_mode / Q;
    m.avg_coarse_super_count = sum_coarse_super_count / Q;
    m.avg_coarse_super_probes = sum_coarse_super_probes / Q;
    m.avg_coarse_child_candidates_scored =
        sum_coarse_child_candidates_scored / Q;
    m.avg_coarse_candidate_budget = sum_coarse_candidate_budget / Q;
    m.avg_coarse_exact_fallback = sum_coarse_exact_fallback / Q;
    m.avg_coarse_exact_overlap = sum_coarse_exact_overlap / Q;
    m.avg_coarse_hierarchy_build_ms = sum_coarse_hierarchy_build / Q;
    m.avg_probe = sum_probe / Q;
    m.avg_probe_prepare = sum_probe_prepare / Q;
    m.avg_probe_prepare_rotation = sum_probe_prepare_rotation / Q;
    m.avg_probe_prepare_subtract = sum_probe_prepare_subtract / Q;
    m.avg_probe_prepare_normalize = sum_probe_prepare_normalize / Q;
    m.avg_probe_prepare_quantize = sum_probe_prepare_quantize / Q;
    m.avg_probe_prepare_lut_build = sum_probe_prepare_lut_build / Q;
    m.avg_probe_prepare_quant_lut = sum_probe_prepare_quant_lut / Q;
    m.avg_probe_stage1 = sum_probe_stage1 / Q;
    m.avg_probe_stage1_estimate = sum_probe_stage1_estimate / Q;
    m.avg_probe_stage1_mask = sum_probe_stage1_mask / Q;
    m.avg_probe_stage1_iterate = sum_probe_stage1_iterate / Q;
    m.avg_probe_stage1_classify_only = sum_probe_stage1_classify_only / Q;
    m.avg_probe_stage2 = sum_probe_stage2 / Q;
    m.avg_probe_stage2_collect = sum_probe_stage2_collect / Q;
    m.avg_probe_stage2_kernel = sum_probe_stage2_kernel / Q;
    m.avg_probe_stage2_scatter = sum_probe_stage2_scatter / Q;
    m.avg_probe_stage2_kernel_sign_flip = sum_probe_stage2_kernel_sign_flip / Q;
    m.avg_probe_stage2_kernel_abs_fma = sum_probe_stage2_kernel_abs_fma / Q;
    m.avg_probe_stage2_kernel_tail = sum_probe_stage2_kernel_tail / Q;
    m.avg_probe_stage2_kernel_reduce = sum_probe_stage2_kernel_reduce / Q;
    m.avg_probe_stage2_decode = sum_probe_stage2_decode / Q;
    m.avg_stage1_fused_blocks = sum_stage1_fused_blocks / Q;
    m.avg_stage1_fused_safeout_lanes = sum_stage1_fused_safeout_lanes / Q;
    m.avg_stage1_fused_safein_lanes = sum_stage1_fused_safein_lanes / Q;
    m.avg_stage2_masked_kernel_calls = sum_stage2_masked_kernel_calls / Q;
    m.avg_stage2_lanes_requested = sum_stage2_lanes_requested / Q;
    m.avg_stage2_lanes_skipped = sum_stage2_lanes_skipped / Q;
    m.avg_stage2_lanes_total_valid = sum_stage2_lanes_total_valid / Q;
    m.avg_stage2_lane_density = sum_stage2_lanes_total_valid > 0.0
        ? sum_stage2_lanes_requested / sum_stage2_lanes_total_valid
        : 0.0;
    m.avg_stage2_decode_blocks = sum_stage2_decode_blocks / Q;
    m.avg_stage2_decode_input_bytes = sum_stage2_decode_input_bytes / Q;
    m.avg_stage2_decode_output_bytes = sum_stage2_decode_output_bytes / Q;
    m.avg_probe_classify = sum_probe_classify / Q;
    m.avg_probe_submit = sum_probe_submit / Q;
    m.avg_probe_submit_prepare_vec_only = sum_probe_submit_prepare_vec_only / Q;
    m.avg_probe_submit_prepare_all = sum_probe_submit_prepare_all / Q;
    m.avg_probe_submit_emit = sum_probe_submit_emit / Q;
    m.avg_probe_submit_vec_only_emit = sum_probe_submit_vec_only_emit / Q;
    m.avg_probe_submit_pending_slot_alloc =
        sum_probe_submit_pending_slot_alloc / Q;
    m.avg_probe_submit_prep_read = sum_probe_submit_prep_read / Q;
    m.avg_rerank_cpu = sum_rerank_cpu / Q;
    m.avg_safein_payload_prefetch = sum_safein_payload_prefetch / Q;
    m.avg_candidate_collect = sum_candidate_collect / Q;
    m.avg_pool_vector_read = sum_pool_vector_read / Q;
    m.avg_rerank_compute = sum_rerank_compute / Q;
    m.avg_rerank_vec_alloc = sum_rerank_vec_alloc / Q;
    m.avg_rerank_vec_copy = sum_rerank_vec_copy / Q;
    m.avg_remaining_payload_fetch = sum_remaining_payload_fetch / Q;
    m.avg_uring_prep = sum_uring_prep / Q;
    m.avg_uring_submit = sum_uring_submit / Q;
    m.avg_fetch_missing = sum_fetch_missing / Q;
    m.avg_submit_calls = sum_submit_calls / Q;
    m.avg_submit_window_flushes = sum_submit_window_flushes / Q;
    m.avg_submit_window_tail_flushes = sum_submit_window_tail_flushes / Q;
    m.avg_submit_window_requests = sum_submit_window_requests / Q;
    m.avg_vec_only_read_requests = sum_vec_only_read_requests / Q;
    m.avg_all_read_requests = sum_all_read_requests / Q;
    m.avg_payload_read_requests = sum_payload_read_requests / Q;
    m.avg_fixed_vec_buffer_hits = sum_fixed_vec_buffer_hits / Q;
    m.avg_fixed_vec_buffer_misses = sum_fixed_vec_buffer_misses / Q;
    m.avg_candidate_batches_per_cluster = sum_candidate_batches_per_cluster / Q;
    m.avg_safeout_frontier_estimates_buffered_per_cluster =
        sum_safeout_frontier_estimates_buffered_per_cluster / Q;
    m.avg_safeout_frontier_estimates_merged_per_cluster =
        sum_safeout_frontier_estimates_merged_per_cluster / Q;
    m.avg_safeout_frontier_updates_per_cluster =
        sum_safeout_frontier_updates_per_cluster / Q;
    m.avg_stage2_block_lookups = sum_stage2_block_lookups / Q;
    m.avg_stage2_block_reuses = sum_stage2_block_reuses / Q;
    m.avg_safeout_frontier_buffer_ms = sum_safeout_frontier_buffer / Q;
    m.avg_safeout_frontier_merge_ms = sum_safeout_frontier_merge / Q;
    m.avg_safeout_frontier_online_ms = sum_safeout_frontier_online / Q;
    m.avg_dynamic_safein_clusters = sum_dynamic_safein_clusters / Q;
    m.avg_dynamic_safein_active_clusters = sum_dynamic_safein_active_clusters / Q;
    m.avg_dynamic_safein_disabled_clusters =
        sum_dynamic_safein_disabled_clusters / Q;
    m.avg_dynamic_safein_threshold = sum_dynamic_safein_threshold / Q;
    m.avg_dynamic_safein_final_frontier = sum_dynamic_safein_final_frontier / Q;
    m.avg_dynamic_safein_deferred_candidates =
        sum_dynamic_safein_deferred_candidates / Q;
    m.avg_dynamic_safein_deferred_flushes =
        sum_dynamic_safein_deferred_flushes / Q;
    m.avg_dynamic_safein_deferred_safein =
        sum_dynamic_safein_deferred_safein / Q;
    m.avg_safein_prefetch_candidates = sum_safein_prefetch_candidates / Q;
    m.avg_safein_prefetch_true_topk = sum_safein_prefetch_true_topk / Q;
    m.avg_safein_prefetch_false = sum_safein_prefetch_false / Q;
    m.safein_prefetch_false_rate =
        sum_safein_prefetch_candidates > 0.0
            ? sum_safein_prefetch_false / sum_safein_prefetch_candidates
            : 0.0;
    m.safein_prefetch_topk_coverage =
        (Q > 0 && search_cfg.top_k > 0)
            ? sum_safein_prefetch_true_topk /
                  (static_cast<double>(Q) * static_cast<double>(search_cfg.top_k))
            : 0.0;
    m.avg_probed_clusters = sum_probed_clusters / Q;
    m.avg_probed = sum_probed / Q;
    m.avg_safe_in = sum_si / Q;
    m.avg_safe_out = sum_so / Q;
    m.avg_uncertain = sum_unc / Q;
    m.avg_s2_safe_in = sum_s2_si / Q;
    m.avg_s2_safe_out = sum_s2_so / Q;
    m.avg_s2_uncertain = sum_s2_unc / Q;
    m.avg_duplicate_candidates = sum_dup / Q;
    m.avg_deduplicated_candidates = sum_dedup / Q;
    m.avg_unique_fetch_candidates = sum_unique_fetch / Q;
    m.avg_candidate_budget_seen = sum_candidate_budget_seen / Q;
    m.avg_candidate_budget_selected = sum_candidate_budget_selected / Q;
    m.avg_candidate_budget_dropped = sum_candidate_budget_dropped / Q;
    m.avg_candidates_buffered = sum_candidates_buffered / Q;
    m.avg_candidates_reranked = sum_candidates_reranked / Q;
    m.avg_safein_payload_prefetched = sum_safein_payload_prefetched / Q;
    m.avg_remaining_payload_fetches = sum_remaining_payload_fetches / Q;
    m.avg_false_safeout = sum_false_so / Q;
    m.avg_false_safein_upper = sum_false_si / Q;
    m.total_final_safein = static_cast<uint64_t>(sum_si + sum_s2_si);
    m.overlap_ratio = (sum_total > 0) ? 1.0 - sum_io_wait / sum_total : 0.0;
    m.preload_time_ms = index.segment().resident_preload_time_ms();
    m.preload_bytes = static_cast<double>(index.segment().resident_preload_bytes());
    m.resident_file_size_bytes =
        static_cast<double>(index.segment().resident_file_size_bytes());
    m.resident_file_buffer_bytes =
        static_cast<double>(index.segment().resident_file_buffer_bytes());
    m.resident_code_storage_bytes =
        static_cast<double>(index.segment().resident_code_storage_bytes());
    m.resident_decoded_address_bytes =
        static_cast<double>(index.segment().resident_decoded_address_bytes());
    m.resident_raw_address_bytes =
        static_cast<double>(index.segment().resident_raw_address_bytes());
    m.resident_parsed_address_duplicate_bytes =
        static_cast<double>(index.segment().resident_parsed_address_duplicate_bytes());
    m.resident_preload_batch_size =
        static_cast<double>(index.segment().resident_preload_batch_size());
    m.resident_cluster_mem_bytes =
        static_cast<double>(index.segment().resident_cluster_mem_bytes());
    m.resident_parallel_view_build_ms =
        index.segment().resident_parallel_view_build_ms();
    m.resident_parallel_view_bytes =
        static_cast<double>(index.segment().resident_parallel_view_bytes());
    m.index_cluster_clu_bytes = FileSizeBytesOrZero(index.dir() + "/cluster.clu");
    m.index_data_dat_bytes = FileSizeBytesOrZero(index.dir() + "/data.dat");
    m.index_rotation_bytes = FileSizeBytesOrZero(index.dir() + "/rotation.bin");
    m.index_rotated_centroids_bytes =
        FileSizeBytesOrZero(index.dir() + "/rotated_centroids.bin");
    m.index_total_bytes = m.index_cluster_clu_bytes + m.index_data_dat_bytes +
        m.index_rotation_bytes + m.index_rotated_centroids_bytes +
        FileSizeBytesOrZero(index.dir() + "/centroids.bin") +
        FileSizeBytesOrZero(index.dir() + "/segment.meta") +
        FileSizeBytesOrZero(index.dir() + "/build_metadata.json");

    if (m.recall_available) {
        Log("  %s: recall@1=%.4f @5=%.4f @10=%.4f @k=%.4f (k=%u)\n",
            label, m.recall_at[0], m.recall_at[1], m.recall_at[2],
            m.recall_at_k, search_cfg.top_k);
    } else {
        Log("  %s: recall skipped (query-only mode)\n", label);
    }
    Log("  %s: avg=%.3f ms  p50=%.3f  p95=%.3f  p99=%.3f\n", label,
        m.avg_query_ms, m.p50, m.p95, m.p99);
    Log("  %s: coarse_select=%.3f ms  score=%.3f ms  topn=%.3f ms\n",
        label, m.avg_coarse_select, m.avg_coarse_score, m.avg_coarse_topn);
    Log("  %s: io_wait=%.3f ms  cpu=%.3f ms  probe=%.3f ms  rerank_cpu=%.3f ms\n", label,
        m.avg_io_wait, m.avg_cpu, m.avg_probe, m.avg_rerank_cpu);
    Log("  %s: probe_prepare=%.3f ms  stage1=%.3f ms  stage2=%.3f ms  classify=%.3f ms  submit=%.3f ms\n",
        label, m.avg_probe_prepare, m.avg_probe_stage1, m.avg_probe_stage2,
        m.avg_probe_classify, m.avg_probe_submit);
    Log("  %s: submit_prepare_vec_only=%.3f ms  submit_prepare_all=%.3f ms  submit_emit=%.3f ms\n",
        label, m.avg_probe_submit_prepare_vec_only,
        m.avg_probe_submit_prepare_all, m.avg_probe_submit_emit);
    Log("  %s: submit_vec_only_emit=%.3f ms  submit_slot_alloc=%.3f ms  submit_prep_read=%.3f ms\n",
        label, m.avg_probe_submit_vec_only_emit,
        m.avg_probe_submit_pending_slot_alloc, m.avg_probe_submit_prep_read);
    Log("  %s: vec_only_reads=%.1f  all_reads=%.1f  payload_reads=%.1f  fixed_buf_hit/miss=%.1f/%.1f\n",
        label, m.avg_vec_only_read_requests, m.avg_all_read_requests,
        m.avg_payload_read_requests, m.avg_fixed_vec_buffer_hits,
        m.avg_fixed_vec_buffer_misses);
    Log("  %s: stage1_estimate=%.3f ms  stage1_mask=%.3f ms  stage1_iterate=%.3f ms  stage1_classify=%.3f ms\n",
        label, m.avg_probe_stage1_estimate, m.avg_probe_stage1_mask,
        m.avg_probe_stage1_iterate, m.avg_probe_stage1_classify_only);
    Log("  %s: safein_payload_prefetch=%.3f ms\n",
        label, m.avg_safein_payload_prefetch);
    Log("  %s: candidate_collect=%.3f ms  pool_vector_read=%.3f ms  rerank_compute=%.3f ms  remaining_payload_fetch=%.3f ms\n",
        label, m.avg_candidate_collect, m.avg_pool_vector_read,
        m.avg_rerank_compute, m.avg_remaining_payload_fetch);
    Log("  %s: uring_prep=%.3f ms  uring_submit=%.3f ms  fetch_missing=%.3f ms\n",
        label, m.avg_uring_prep, m.avg_uring_submit, m.avg_fetch_missing);
    Log("  %s: submit_calls=%.1f\n", label, m.avg_submit_calls);
    Log("  %s: submit_window_flushes=%.1f  submit_window_tail_flushes=%.1f  submit_window_requests=%.1f\n",
        label, m.avg_submit_window_flushes,
        m.avg_submit_window_tail_flushes,
        m.avg_submit_window_requests);
    Log("  %s: avg_probed_clusters=%.1f\n", label, m.avg_probed_clusters);
    Log("  %s: candidate_batches_per_cluster=%.2f\n",
        label, m.avg_candidate_batches_per_cluster);
    Log("  %s: safeout_frontier_buffered_per_cluster=%.2f  safeout_frontier_merged_per_cluster=%.2f  safeout_frontier_updates_per_cluster=%.2f\n",
        label, m.avg_safeout_frontier_estimates_buffered_per_cluster,
        m.avg_safeout_frontier_estimates_merged_per_cluster,
        m.avg_safeout_frontier_updates_per_cluster);
    Log("  %s: stage2_block_lookups=%.1f  stage2_block_reuses=%.1f\n",
        label, m.avg_stage2_block_lookups, m.avg_stage2_block_reuses);
    Log("  %s: safeout_frontier_buffer=%.6f ms  safeout_frontier_merge=%.6f ms  safeout_frontier_online=%.6f ms\n",
        label, m.avg_safeout_frontier_buffer_ms,
        m.avg_safeout_frontier_merge_ms,
        m.avg_safeout_frontier_online_ms);
    if (search_cfg.dynamic_safein_mode != DynamicSafeInMode::Static) {
        Log("  %s: dynamic_safein active=%.1f/%0.1f disabled=%.1f threshold_avg=%.6f frontier_final=%.6f\n",
            label, m.avg_dynamic_safein_active_clusters,
            m.avg_dynamic_safein_clusters,
            m.avg_dynamic_safein_disabled_clusters,
            m.avg_dynamic_safein_threshold,
            m.avg_dynamic_safein_final_frontier);
        Log("  %s: dynamic_safein deferred candidates=%.1f flushes=%.1f safein=%.1f\n",
            label, m.avg_dynamic_safein_deferred_candidates,
            m.avg_dynamic_safein_deferred_flushes,
            m.avg_dynamic_safein_deferred_safein);
    }
    Log("  %s: safein_prefetch true/false/total=%.1f/%.1f/%.1f coverage=%.2f%% false_rate=%.2f%%\n",
        label, m.avg_safein_prefetch_true_topk,
        m.avg_safein_prefetch_false,
        m.avg_safein_prefetch_candidates,
        100.0 * m.safein_prefetch_topk_coverage,
        100.0 * m.safein_prefetch_false_rate);
    Log("  %s: safe_in=%.1f  safe_out=%.1f  uncertain=%.1f\n", label,
        m.avg_safe_in, m.avg_safe_out, m.avg_uncertain);
    Log("  %s: s2_safe_in=%.1f  s2_safe_out=%.1f  s2_uncertain=%.1f\n", label,
        m.avg_s2_safe_in, m.avg_s2_safe_out, m.avg_s2_uncertain);
    Log("  %s: duplicate_candidates=%.1f  deduplicated=%.1f  unique_fetch=%.1f\n",
        label, m.avg_duplicate_candidates, m.avg_deduplicated_candidates,
        m.avg_unique_fetch_candidates);
    Log("  %s: candidate_budget=%u  seen=%.1f  selected=%.1f  dropped=%.1f\n",
        label, search_cfg.non_safeout_candidate_budget,
        m.avg_candidate_budget_seen,
        m.avg_candidate_budget_selected,
        m.avg_candidate_budget_dropped);
    Log("  %s: buffered=%.1f  reranked=%.1f  safein_payload_prefetched=%.1f  remaining_payload_fetches=%.1f\n",
        label, m.avg_candidates_buffered, m.avg_candidates_reranked,
        m.avg_safein_payload_prefetched, m.avg_remaining_payload_fetches);
    Log("  %s: false_safeout=%.2f  false_safein_upper=%.1f  total_safein=%lu\n",
        label, m.avg_false_safeout, m.avg_false_safein_upper,
        static_cast<unsigned long>(m.total_final_safein));
    Log("  %s: overlap=%.4f\n", label, m.overlap_ratio);

    return {std::move(qresults), m};
}

static const char* ClusteringSourceName(index::ClusteringSource source) {
    switch (source) {
        case index::ClusteringSource::Auto:
            return "auto";
        case index::ClusteringSource::Precomputed:
            return "precomputed";
    }
    return "unknown";
}

static const char* CoarseBuilderName(index::CoarseBuilder builder) {
    switch (builder) {
        case index::CoarseBuilder::SuperKMeans:
            return "superkmeans";
        case index::CoarseBuilder::HierarchicalSuperKMeans:
            return "hierarchical_superkmeans";
        case index::CoarseBuilder::FaissKMeans:
            return "faiss_kmeans";
        case index::CoarseBuilder::Auto:
        default:
            return "auto";
    }
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char* argv[]) {
    std::string data_dir = GetStringArg(argc, argv, "--dataset",
                                         "/home/zcq/VDB/data/coco_1k");
    std::string output_base = GetStringArg(argc, argv, "--output",
                                            "/home/zcq/VDB/test/");
    int arg_nlist      = GetIntArg(argc, argv, "--nlist", 32);
    int arg_nprobe     = GetIntArg(argc, argv, "--nprobe", 32);
    int arg_topk       = GetIntArg(argc, argv, "--topk", 10);
    int arg_dynamic_safeout = GetIntArg(argc, argv, "--dynamic-safeout", 1);
    DynamicSafeInMode arg_dynamic_safein_mode = DynamicSafeInMode::Static;
    const std::string arg_dynamic_safein =
        GetStringArg(argc, argv, "--dynamic-safein", "static");
    if (!ParseDynamicSafeInModeArg(arg_dynamic_safein,
                                   &arg_dynamic_safein_mode)) {
        std::fprintf(stderr,
                     "Invalid --dynamic-safein: %s "
                     "(expected static, off, or frontier)\n",
                     arg_dynamic_safein.c_str());
        return 1;
    }
    if (RejectDeletedDynamicSafeInFlag(argc, argv)) {
        return 1;
    }
    if (RejectDeletedClusterLoadingFlag(argc, argv)) {
        return 1;
    }
    if (RejectDeletedLegacySearchModeFlag(argc, argv)) {
        return 1;
    }
    int arg_dynamic_safein_min_probes =
        GetIntArg(argc, argv, "--dynamic-safein-min-probes", 0);
    int arg_dynamic_safein_stable_probes =
        GetIntArg(argc, argv, "--dynamic-safein-stable-probes", 2);
    float arg_dynamic_safein_rel_tol =
        GetFloatArg(argc, argv, "--dynamic-safein-rel-tol", 0.005f);
    float arg_dynamic_safein_abs_tol =
        GetFloatArg(argc, argv, "--dynamic-safein-abs-tol", 0.0f);
    int arg_dynamic_safein_defer_initial_clusters =
        GetIntArg(argc, argv, "--dynamic-safein-defer-initial-clusters", 0);
    int arg_dynamic_safein_defer_until_ready =
        GetIntArg(argc, argv, "--dynamic-safein-defer-until-ready", 0);
    int arg_dynamic_safein_defer_max_candidates =
        GetIntArg(argc, argv, "--dynamic-safein-defer-max-candidates", 0);
    int arg_non_safeout_candidate_budget =
        GetIntArg(argc, argv, "--non-safeout-candidate-budget", 0);
    int arg_safein_all_threshold_bytes =
        GetIntArg(argc, argv, "--safein-all-threshold-bytes", 256 * 1024);
    int arg_bits       = GetIntArg(argc, argv, "--bits", 1);
    int arg_block_size = GetIntArg(argc, argv, "--block-size", 64);
    float arg_c_factor = GetFloatArg(argc, argv, "--c-factor", 5.75f);
    RaBitQConfig arg_rabitq_config;
    if (!ResolveRaBitQBuildConfig(argc, argv, arg_bits, arg_block_size,
                                  arg_c_factor, &arg_rabitq_config)) {
        return 1;
    }
    const std::string arg_query_mode =
        GetStringArg(argc, argv, "--rabitq-validation-mode",
                     GetStringArg(argc, argv, "--rabitq-query-mode", "auto"));
    RaBitQValidationMode arg_rabitq_validation_mode = RaBitQValidationMode::Auto;
    if (!ParseRaBitQValidationModeArg(arg_query_mode,
                                      &arg_rabitq_validation_mode)) {
        std::fprintf(stderr,
                     "Invalid --rabitq-validation-mode: %s "
                     "(expected auto, official_1_plus_n, or legacy_signed_magnitude)\n",
                     arg_query_mode.c_str());
        return 1;
    }
    int arg_max_iter   = GetIntArg(argc, argv, "--max-iter", 20);
    int arg_seed       = GetIntArg(argc, argv, "--seed", 42);
    int arg_page_size  = GetIntArg(argc, argv, "--page-size", 4096);
    int arg_p_for_dk   = GetIntArg(argc, argv, "--p-for-dk", 99);
    std::string arg_coarse_builder =
        GetStringArg(argc, argv, "--coarse-builder", "auto");
    const bool invoked_as_build_index =
        fs::path(argv[0]).filename() == "bench_build_index";
    bool arg_build_only = HasFlag(argc, argv, "--build-only") ||
                          invoked_as_build_index;
    int arg_epsilon_samples = GetIntArg(argc, argv, "--epsilon-samples", 100);
    auto epsilon_sampling_mode_or = ParseEpsilonSamplingModeArg(
        GetStringArg(argc, argv, "--epsilon-sampling-mode",
                     "legacy_per_cluster"));
    if (!epsilon_sampling_mode_or.ok()) {
        std::fprintf(stderr, "%s\n",
                     epsilon_sampling_mode_or.status().ToString().c_str());
        return 1;
    }
    EpsilonSamplingMode arg_epsilon_sampling_mode =
        epsilon_sampling_mode_or.value();
    float arg_epsilon_percentile = GetFloatArg(argc, argv, "--epsilon-percentile", 0.99f);
    int arg_io_queue_depth = GetIntArg(argc, argv, "--io-queue-depth", 64);
    int arg_fixed_vec_buffer_count =
        GetIntArg(argc, argv, "--fixed-vec-buffer-count", 0);
    int arg_cluster_submit_reserve =
        GetIntArg(argc, argv, "--cluster-submit-reserve", 8);
    std::string arg_submission_mode =
        GetStringArg(argc, argv, "--submission-mode", "shared");
    int arg_query_only = GetIntArg(argc, argv, "--query-only", 0);
    int arg_skip_gt = GetIntArg(argc, argv, "--skip-gt", 0);
    if (invoked_as_build_index) {
        arg_query_only = 1;
        arg_skip_gt = 1;
    }
    int arg_skip_false_stats = GetIntArg(argc, argv, "--skip-false-stats", 0);
    std::string arg_gt_file = GetStringArg(argc, argv, "--gt-file", "");
    std::string arg_payload_mode =
        GetStringArg(argc, argv, "--payload-mode", "metadata");
    std::string arg_payload_index =
        GetStringArg(argc, argv, "--payload-index", "");
    std::string arg_payload_data =
        GetStringArg(argc, argv, "--payload-data", "");
    int arg_fine_grained_timing =
        GetIntArg(argc, argv, "--fine-grained-timing", 0);
    int arg_hotpath_detailed_timing =
        GetIntArg(argc, argv, "--hotpath-detailed-timing", 0);
    int arg_submit_online = GetIntArg(argc, argv, "--submit-online", 0);
    float arg_submit_ema_alpha =
        GetFloatArg(argc, argv, "--submit-ema-alpha", 0.25f);
    int arg_address_decode_simd =
        GetIntArg(argc, argv, "--address-decode-simd", 1);
    int arg_rerank_batched_distance_simd =
        GetIntArg(argc, argv, "--rerank-batched-distance-simd", 1);
    int arg_coarse_select_simd =
        GetIntArg(argc, argv, "--coarse-select-simd", 1);
    int arg_coarse_select_phase2 =
        GetIntArg(argc, argv, "--coarse-select-phase2", 0);
    int arg_two_level_coarse_routing =
        GetIntArg(argc, argv, "--two-level-coarse-routing", 0);
    int arg_two_level_coarse_threshold =
        GetIntArg(argc, argv, "--two-level-coarse-threshold", 4096);
    int arg_two_level_coarse_super_count =
        GetIntArg(argc, argv, "--two-level-coarse-super-count", 0);
    int arg_two_level_coarse_super_factor =
        GetIntArg(argc, argv, "--two-level-coarse-super-factor", 0);
    int arg_two_level_coarse_budget_factor =
        GetIntArg(argc, argv, "--two-level-coarse-budget-factor", 8);
    int arg_two_level_coarse_exact_overlap =
        GetIntArg(argc, argv, "--two-level-coarse-exact-overlap", 0);
    int arg_stage2_block_first =
        GetIntArg(argc, argv, "--stage2-block-first", 1);
    int arg_stage2_batch_classify =
        GetIntArg(argc, argv, "--stage2-batch-classify", 1);
    bool arg_cold = HasFlag(argc, argv, "--cold");
    bool arg_direct_io = HasFlag(argc, argv, "--direct-io");
    bool arg_iopoll = HasFlag(argc, argv, "--iopoll");
    bool arg_sqpoll = HasFlag(argc, argv, "--sqpoll");
    const bool query_only_mode = (arg_query_only != 0);
    const bool skip_gt = query_only_mode || (arg_skip_gt != 0);
    if (arg_two_level_coarse_routing != 0 && skip_gt) {
        Log("WARNING: approximate coarse routing is running without real GT recall; do not use this run as final performance evidence.\n");
    }
    const bool external_gt_requested = !arg_gt_file.empty();

    if (arg_submission_mode != "shared" &&
        arg_submission_mode != "isolated") {
        std::fprintf(stderr,
                     "Error: unsupported --submission-mode=%s (expected 'shared' or 'isolated')\n",
                     arg_submission_mode.c_str());
        return 1;
    }
    if (arg_safein_all_threshold_bytes < 0) {
        std::fprintf(stderr,
                     "Invalid --safein-all-threshold-bytes: %d "
                     "(expected >= 0)\n",
                     arg_safein_all_threshold_bytes);
        return 1;
    }
    if (arg_non_safeout_candidate_budget < 0) {
        std::fprintf(stderr,
                     "Invalid --non-safeout-candidate-budget: %d "
                     "(expected >= 0)\n",
                     arg_non_safeout_candidate_budget);
        return 1;
    }
    if (arg_payload_mode != "metadata" &&
        arg_payload_mode != "audio-flatstor" &&
        arg_payload_mode != "audio_flatstor" &&
        arg_payload_mode != "raw-flatstor" &&
        arg_payload_mode != "raw_flatstor" &&
        arg_payload_mode != "flatstor" &&
        arg_payload_mode != "flatstor-payload") {
        std::fprintf(stderr,
                     "Invalid --payload-mode: %s "
                     "(expected metadata, raw-flatstor, or audio-flatstor)\n",
                     arg_payload_mode.c_str());
        return 1;
    }
    if (arg_payload_mode == "audio_flatstor") {
        arg_payload_mode = "audio-flatstor";
    } else if (arg_payload_mode == "raw_flatstor" ||
               arg_payload_mode == "flatstor" ||
               arg_payload_mode == "flatstor-payload") {
        arg_payload_mode = "raw-flatstor";
    }
    if (arg_coarse_builder != "auto" &&
        arg_coarse_builder != "superkmeans" &&
        arg_coarse_builder != "hierarchical_superkmeans" &&
        arg_coarse_builder != "faiss_kmeans") {
        std::fprintf(stderr,
                     "Invalid --coarse-builder: %s (expected auto, superkmeans, hierarchical_superkmeans, or faiss_kmeans)\n",
                     arg_coarse_builder.c_str());
        return 1;
    }
    if (external_gt_requested && skip_gt) {
        Log("  External GT provided but recall is disabled (--query-only/--skip-gt); GT will be loaded only if needed.\n");
    }
    if (arg_dynamic_safeout != 0 && arg_dynamic_safeout != 1) {
        std::fprintf(stderr,
                     "Invalid --dynamic-safeout: %d (expected 0 or 1)\n",
                     arg_dynamic_safeout);
        return 1;
    }

    // nprobe sweep: --nprobe-sweep 50,100,150,200 (mutually exclusive with --nprobe)
    std::string arg_nprobe_sweep_str = GetStringArg(argc, argv, "--nprobe-sweep", "");
    std::vector<int> nprobe_sweep_list;
    if (!arg_nprobe_sweep_str.empty()) {
        // Check mutual exclusion with explicit --nprobe
        for (int i = 1; i < argc - 1; ++i) {
            if (std::strcmp(argv[i], "--nprobe") == 0) {
                std::fprintf(stderr, "Error: --nprobe and --nprobe-sweep are mutually exclusive\n");
                return 1;
            }
        }
        // Parse comma-separated list
        std::istringstream ss(arg_nprobe_sweep_str);
        std::string token;
        while (std::getline(ss, token, ',')) {
            if (!token.empty()) {
                nprobe_sweep_list.push_back(std::stoi(token));
            }
        }
        if (nprobe_sweep_list.empty()) {
            std::fprintf(stderr, "Error: --nprobe-sweep requires at least one value\n");
            return 1;
        }
    }

    // Precomputed clustering (skip KMeans if both provided)
    std::string arg_centroids = GetStringArg(argc, argv, "--centroids", "");
    std::string arg_assignments = GetStringArg(argc, argv, "--assignments", "");
    std::string arg_save_centroids =
        GetStringArg(argc, argv, "--save-centroids", "");
    std::string arg_save_assignments =
        GetStringArg(argc, argv, "--save-assignments", "");

    // Pre-built index directory (skip Phase C build if specified)
    std::string arg_index_dir = GetStringArg(argc, argv, "--index-dir", "");
    if ((arg_payload_mode == "audio-flatstor" ||
         arg_payload_mode == "raw-flatstor") &&
        arg_index_dir.empty() &&
        (arg_payload_index.empty() || arg_payload_data.empty())) {
        std::fprintf(stderr,
                     "--payload-mode %s requires --payload-index and "
                     "--payload-data when building a new index\n",
                     arg_payload_mode.c_str());
        return 1;
    }

    float arg_override_eps_ip = GetFloatArg(argc, argv, "--override-eps-ip", -1.0f);
    float arg_override_d_k = GetFloatArg(argc, argv, "--override-d-k", -1.0f);
    int arg_calibration_samples =
        GetIntArg(argc, argv, "--calibration-samples", 1000);
    float arg_safein_dk_percentile =
        GetFloatArg(argc, argv, "--safein-dk-percentile", -1.0f);
    int arg_safein_dk_samples =
        GetIntArg(argc, argv, "--safein-dk-samples", 0);
    std::string arg_safein_dk_samples_output =
        GetStringArg(argc, argv, "--safein-dk-samples-output", "");
    std::string arg_safein_dk_samples_input =
        GetStringArg(argc, argv, "--safein-dk-samples-input", "");
    int arg_safein_dk_samples_only =
        GetIntArg(argc, argv, "--safein-dk-samples-only", 0);
    float arg_safein_epsilon_percentile =
        GetFloatArg(argc, argv, "--safein-epsilon-percentile", -1.0f);
    float arg_safein_epsilon_override =
        GetFloatArg(argc, argv, "--safein-epsilon-override", -1.0f);
    float arg_safeout_epsilon_percentile =
        GetFloatArg(argc, argv, "--safeout-epsilon-percentile", -1.0f);
    float arg_safeout_epsilon_override =
        GetFloatArg(argc, argv, "--safeout-epsilon-override", -1.0f);
    int arg_enable_stage1_safein =
        GetIntArg(argc, argv, "--enable-stage1-safein", 1);
    auto safein_dk_scope_or = ParseSafeInDkSearchScopeArg(
        GetStringArg(argc, argv, "--safein-dk-search-scope", "full"));
    if (!safein_dk_scope_or.ok()) {
        std::fprintf(stderr, "%s\n", safein_dk_scope_or.status().ToString().c_str());
        return 1;
    }
    SafeInDkSearchScope arg_safein_dk_search_scope = safein_dk_scope_or.value();
    int arg_safein_dk_nprobe =
        GetIntArg(argc, argv, "--safein-dk-nprobe", arg_nprobe);
    auto safein_dk_sampling_mode_or = ParseSafeInDkSamplingModeArg(
        GetStringArg(argc, argv, "--safein-dk-sampling-mode", "unique"));
    if (!safein_dk_sampling_mode_or.ok()) {
        std::fprintf(stderr, "%s\n",
                     safein_dk_sampling_mode_or.status().ToString().c_str());
        return 1;
    }
    SafeInDkSamplingMode arg_safein_dk_sampling_mode =
        safein_dk_sampling_mode_or.value();
    if (arg_enable_stage1_safein != 0 && arg_enable_stage1_safein != 1) {
        std::fprintf(stderr,
                     "Invalid --enable-stage1-safein: %d (expected 0 or 1)\n",
                     arg_enable_stage1_safein);
        return 1;
    }
    if (arg_safein_dk_samples_only != 0 && arg_safein_dk_samples_only != 1) {
        std::fprintf(stderr,
                     "Invalid --safein-dk-samples-only: %d (expected 0 or 1)\n",
                     arg_safein_dk_samples_only);
        return 1;
    }
    if (!arg_safein_dk_samples_input.empty() &&
        !arg_safein_dk_samples_output.empty()) {
        std::fprintf(stderr,
                     "Cannot use --safein-dk-samples-input and "
                     "--safein-dk-samples-output together\n");
        return 1;
    }
    if (arg_safein_dk_samples_only != 0 &&
        arg_safein_dk_samples_output.empty()) {
        std::fprintf(stderr,
                     "--safein-dk-samples-only requires "
                     "--safein-dk-samples-output\n");
        return 1;
    }

    std::string ds_name = DatasetName(data_dir);
    std::string ts = Timestamp();
    Log("=== VDB E2E Benchmark ===\n");
    Log("Dataset: %s\n", data_dir.c_str());
    Log("Calibration samples: %d\n", arg_calibration_samples);
    // ================================================================
    // Phase A: Load Data
    // ================================================================
    Log("\n[Phase A] Loading data...\n");

    auto img_emb_or = io::LoadNpyFloat32(data_dir + "/image_embeddings.npy");
    if (!img_emb_or.ok()) {
        std::fprintf(stderr, "Failed to load image_embeddings: %s\n",
                     img_emb_or.status().ToString().c_str());
        return 1;
    }
    auto& img_emb = img_emb_or.value();

    auto img_ids_or = io::LoadNpyInt64(data_dir + "/image_ids.npy");
    if (!img_ids_or.ok()) {
        std::fprintf(stderr, "Failed to load image_ids: %s\n",
                     img_ids_or.status().ToString().c_str());
        return 1;
    }
    auto& img_ids = img_ids_or.value();

    auto qry_emb_or = io::LoadNpyFloat32(data_dir + "/query_embeddings.npy");
    if (!qry_emb_or.ok()) {
        std::fprintf(stderr, "Failed to load query_embeddings: %s\n",
                     qry_emb_or.status().ToString().c_str());
        return 1;
    }
    auto& qry_emb = qry_emb_or.value();

    auto qry_ids_or = io::LoadNpyInt64(data_dir + "/query_ids.npy");
    if (!qry_ids_or.ok()) {
        std::fprintf(stderr, "Failed to load query_ids: %s\n",
                     qry_ids_or.status().ToString().c_str());
        return 1;
    }
    auto& qry_ids = qry_ids_or.value();

    uint32_t N = img_emb.rows;
    uint32_t Q_total = qry_emb.rows;
    Dim dim = static_cast<Dim>(img_emb.cols);

    int q_limit = GetIntArg(argc, argv, "--queries", 0);
    uint32_t Q = (q_limit > 0 && static_cast<uint32_t>(q_limit) < Q_total)
                     ? static_cast<uint32_t>(q_limit) : Q_total;
    uint32_t safein_dk_available_queries = Q;
    uint32_t safein_dk_requested_samples =
        (arg_safein_dk_samples > 0) ? static_cast<uint32_t>(arg_safein_dk_samples) : Q;
    uint32_t safein_dk_samples_count = 0;
    std::string effective_safein_dk_sampling_mode =
        (arg_safein_dk_sampling_mode == SafeInDkSamplingMode::WithReplacement)
            ? "with_replacement"
            : "unique";
    if (safein_dk_requested_samples > safein_dk_available_queries &&
        arg_safein_dk_sampling_mode == SafeInDkSamplingMode::Unique) {
        effective_safein_dk_sampling_mode = "with_replacement";
        Log("  SafeIn d_k sampling auto-switch: requested_samples=%u available_queries=%u -> with_replacement\n",
            safein_dk_requested_samples, safein_dk_available_queries);
    }

    Log("  Images: %u, Queries: %u/%u, Dim: %u\n", N, Q, Q_total, dim);

    const bool use_flatstor_payload =
        (arg_payload_mode == "audio-flatstor" ||
         arg_payload_mode == "raw-flatstor");
    FlatstorPayloadReader flatstor_payload_reader;

    // Load metadata
    std::unordered_map<int64_t, std::string> id_to_caption;
    Status s = Status::OK();
    if (!use_flatstor_payload) {
        s = io::ReadJsonlLines(data_dir + "/metadata.jsonl",
            [&](uint32_t, std::string_view line) {
                int64_t id = ParseImageId(line);
                if (id >= 0) {
                    id_to_caption[id] = ParseCaption(line);
                }
            });
        if (!s.ok()) {
            std::fprintf(stderr, "Failed to load metadata: %s\n",
                         s.ToString().c_str());
            return 1;
        }
        Log("  Metadata entries: %zu\n", id_to_caption.size());
    } else {
        Log("  Metadata load skipped for %s payload mode\n",
            arg_payload_mode.c_str());
    }

    if (use_flatstor_payload && arg_index_dir.empty()) {
        Log("  Loading FlatStor payload index: %s\n", arg_payload_index.c_str());
        s = flatstor_payload_reader.Open(arg_payload_index, arg_payload_data, N);
        if (!s.ok()) {
            std::fprintf(stderr, "Failed to open FlatStor payload store: %s\n",
                         s.ToString().c_str());
            return 1;
        }
        Log("  FlatStor payload store: rows=%u bytes=%llu data=%s\n",
            flatstor_payload_reader.rows,
            static_cast<unsigned long long>(
                flatstor_payload_reader.total_payload_bytes),
            arg_payload_data.c_str());
    }

    // ================================================================
    // Phase B: Brute-Force Ground Truth
    // ================================================================
    const uint32_t GT_K = static_cast<uint32_t>(arg_topk);
    std::vector<std::vector<int64_t>> gt_topk(Q);
    std::vector<std::vector<float>> gt_dists(Q);
    std::string gt_mode = skip_gt ? "skipped" : "computed";
    std::string gt_source = "";
    double brute_force_time_ms = 0.0;
    if (!skip_gt) {
        if (external_gt_requested) {
            Log("\n[Phase B] Loading external ground truth from %s (top-%u)...\n",
                arg_gt_file.c_str(), GT_K);
            auto gt_or = LoadExternalGroundTruth(arg_gt_file, Q, GT_K);
            if (!gt_or.ok()) {
                std::fprintf(stderr, "Failed to load external ground truth: %s\n",
                             gt_or.status().ToString().c_str());
                return 1;
            }
            gt_topk = std::move(gt_or.value());
            gt_mode = "external";
            gt_source = arg_gt_file;
        } else {
        Log("\n[Phase B] Computing brute-force ground truth (top-%u)...\n", GT_K);
        auto t_bf_start = std::chrono::steady_clock::now();
        for (uint32_t qi = 0; qi < Q; ++qi) {
            const float* qvec = qry_emb.data.data() + static_cast<size_t>(qi) * dim;

            std::vector<std::pair<float, int64_t>> dists(N);
            for (uint32_t j = 0; j < N; ++j) {
                const float* ivec = img_emb.data.data() + static_cast<size_t>(j) * dim;
                dists[j] = {L2Sqr(qvec, ivec, dim), img_ids.data[j]};
            }

            std::partial_sort(dists.begin(), dists.begin() + GT_K, dists.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

            gt_topk[qi].resize(GT_K);
            gt_dists[qi].resize(GT_K);
            for (uint32_t k = 0; k < GT_K; ++k) {
                gt_topk[qi][k] = dists[k].second;
                gt_dists[qi][k] = dists[k].first;
            }

            if ((qi + 1) % 100 == 0 || qi + 1 == Q) {
                double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t_bf_start).count();
                Log("  GT progress: %u/%u (%.0f ms)\n", qi + 1, Q, elapsed);
            }
        }
        brute_force_time_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_bf_start).count();
        Log("  Brute-force time: %.1f ms\n", brute_force_time_ms);
        gt_mode = "computed";
        gt_source = "brute_force";
        }
    } else {
        Log("\n[Phase B] Skipped brute-force ground truth (%s)\n",
            query_only_mode ? "--query-only enabled" : "--skip-gt enabled");
        if (external_gt_requested) {
            gt_mode = "external_unused";
            gt_source = arg_gt_file;
        }
    }

    // ================================================================
    // Phase C: Build Index (skipped when --index-dir is provided)
    // ================================================================
    std::string output_dir = output_base + "/" + ds_name + "_" + ts;
    std::string index_dir = ResolveBenchIndexDir(
        ds_name, output_dir, arg_nlist, arg_rabitq_config,
        arg_epsilon_percentile, arg_index_dir);
    const std::string index_source =
        arg_index_dir.empty() ? "rebuilt" : "reused";
    const std::string resolved_index_dir = NormalizePath(index_dir);
    double training_time_ms = 0.0;
    std::vector<uint32_t> built_assignments;

    if (arg_index_dir.empty()) {
        fs::create_directories(index_dir);
        Log("\n[Phase C] Building index -> %s\n", index_dir.c_str());

        IvfBuilderConfig cfg;
        cfg.nlist = static_cast<uint32_t>(arg_nlist);
        if (arg_coarse_builder == "superkmeans") {
            cfg.coarse_builder = index::CoarseBuilder::SuperKMeans;
        } else if (arg_coarse_builder == "hierarchical_superkmeans") {
            cfg.coarse_builder = index::CoarseBuilder::HierarchicalSuperKMeans;
        } else if (arg_coarse_builder == "faiss_kmeans") {
            cfg.coarse_builder = index::CoarseBuilder::FaissKMeans;
        } else {
            cfg.coarse_builder = index::CoarseBuilder::Auto;
        }
        cfg.max_iterations = static_cast<uint32_t>(arg_max_iter);
        cfg.seed = static_cast<uint64_t>(arg_seed);
        cfg.metric = "cosine";
        cfg.faiss_train_size = 100000;
        cfg.faiss_niter = static_cast<uint32_t>(arg_max_iter == 20 ? 0 : arg_max_iter);
        cfg.faiss_nredo = 1;
        cfg.rabitq = arg_rabitq_config;
        cfg.nprobe = static_cast<uint32_t>(std::max(1, arg_nprobe));
        cfg.calibration_samples =
            std::min(static_cast<uint32_t>(arg_calibration_samples), N);
        cfg.epsilon_samples = static_cast<uint32_t>(arg_epsilon_samples);
        cfg.epsilon_percentile = arg_epsilon_percentile;
        cfg.calibration_topk = GT_K;
        cfg.calibration_percentile = static_cast<float>(arg_p_for_dk) / 100.0f;
        cfg.page_size = static_cast<uint32_t>(arg_page_size);
        cfg.calibration_queries = qry_emb.data.data();
        cfg.num_calibration_queries = Q;
        cfg.centroids_path = arg_centroids;
        cfg.assignments_path = arg_assignments;
        cfg.save_centroids_path = arg_save_centroids;
        cfg.save_assignments_path = arg_save_assignments;
        if (use_flatstor_payload) {
            const char* payload_field =
                (arg_payload_mode == "audio-flatstor") ? "audio_payload"
                                                       : "raw_payload";
            cfg.payload_schemas = {
                {0, "id", DType::INT64, false},
                {1, payload_field, DType::BYTES, false},
            };
        } else {
            cfg.payload_schemas = {
                {0, "id",      DType::INT64,  false},
                {1, "caption", DType::STRING, false},
            };
        }

        PayloadFn payload_fn = [&](uint32_t idx) -> std::vector<Datum> {
            int64_t id = img_ids.data[idx];
            if (use_flatstor_payload) {
                return {Datum::Int64(id),
                        Datum::Bytes(flatstor_payload_reader.Read(idx))};
            }
            auto it = id_to_caption.find(id);
            std::string cap = (it != id_to_caption.end()) ? it->second : "";
            return {Datum::Int64(id), Datum::String(std::move(cap))};
        };

        IvfBuilder builder(cfg);
        builder.SetProgressCallback([](uint32_t cluster, uint32_t total) {
            if ((cluster + 1) % 8 == 0 || cluster + 1 == total) {
                Log("  Build progress: cluster %u/%u\n", cluster + 1, total);
            }
        });

        auto t_build_start = std::chrono::steady_clock::now();
        Log("  Starting Build...\n");
        s = builder.Build(img_emb.data.data(), N, dim, index_dir, payload_fn);
        training_time_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_build_start).count();

        if (!s.ok()) {
            std::fprintf(stderr, "Build failed: %s\n", s.ToString().c_str());
            return 1;
        }
        built_assignments = builder.assignments();
        Log("  Build time: %.1f ms\n", training_time_ms);
    } else {
        Log("\n[Phase C] Skipped (using pre-built index: %s)\n", index_dir.c_str());
    }

    if (arg_build_only) {
        Log("\n[Phase C.5/D] Skipped (--build-only enabled)\n");
        Log("  Index source: %s  resolved_index_dir=%s\n",
            index_source.c_str(), resolved_index_dir.c_str());
        return 0;
    }

    // ================================================================
    // Phase C.5: Open Index
    // ================================================================
    IvfIndex index;
    s = index.Open(index_dir, arg_direct_io);
    if (!s.ok()) {
        std::fprintf(stderr, "Open failed: %s\n", s.ToString().c_str());
        return 1;
    }
    if (!ValidateRaBitQMode(index.segment().rabitq_config(),
                            arg_rabitq_validation_mode,
                            "--rabitq-validation-mode")) {
        return 1;
    }
    const RaBitQConfig& loaded_rabitq_config = index.segment().rabitq_config();

    Log("  Index params: eps_ip=%.6f  d_k=%.6f\n",
        index.conann().epsilon(), index.conann().d_k());
    Log("  Index dims: logical=%u  effective=%u  padding=%s  rotation=%s\n",
        index.logical_dim(), index.effective_dim(),
        index.padding_mode().c_str(), index.rotation_mode().c_str());
    Log("  Index source: %s  resolved_index_dir=%s\n",
        index_source.c_str(), resolved_index_dir.c_str());
    if (arg_override_eps_ip >= 0.0f || arg_override_d_k >= 0.0f) {
        const float eps = (arg_override_eps_ip >= 0.0f)
            ? arg_override_eps_ip
            : index.conann().epsilon();
        const float dk = (arg_override_d_k >= 0.0f)
            ? arg_override_d_k
            : index.conann().d_k();
        index.OverrideConANN(eps, dk);
        Log("  Override params applied: eps_ip=%.6f  d_k=%.6f\n",
            index.conann().epsilon(), index.conann().d_k());
    }

    const float loaded_eps_ip = index.conann().epsilon();
    const float loaded_d_k = index.conann().d_k();
    const bool uses_static_safein_threshold =
        arg_dynamic_safein_mode == DynamicSafeInMode::Static;
    const bool explicit_safein_dk_requested =
        arg_safein_dk_percentile >= 0.0f ||
        !arg_safein_dk_samples_input.empty() ||
        !arg_safein_dk_samples_output.empty();
    float runtime_safein_d_k = 0.0f;
    bool runtime_safein_d_k_valid = false;
    auto ensure_runtime_safein_d_k = [&]() {
        if (!runtime_safein_d_k_valid) {
            runtime_safein_d_k = index.conann().safein_d_k();
            if (runtime_safein_d_k <= 0.0f) {
                runtime_safein_d_k = index.conann().d_k();
            }
            runtime_safein_d_k_valid = true;
        }
    };
    if (uses_static_safein_threshold || explicit_safein_dk_requested ||
        arg_safein_epsilon_percentile >= 0.0f ||
        arg_safeout_epsilon_percentile >= 0.0f) {
        ensure_runtime_safein_d_k();
    }
    float runtime_safein_epsilon = index.conann().epsilon();
    float runtime_safeout_epsilon = index.conann().epsilon();
    if (arg_safein_epsilon_override >= 0.0f) {
        runtime_safein_epsilon = arg_safein_epsilon_override;
    }
    if (arg_safeout_epsilon_override >= 0.0f) {
        runtime_safeout_epsilon = arg_safeout_epsilon_override;
    }
    EpsilonCalibrationStats safein_epsilon_stats;
    EpsilonCalibrationStats safeout_epsilon_stats;
    std::vector<float> safein_dk_sample_values;
    const bool has_safein_epsilon_override =
        arg_safein_epsilon_percentile >= 0.0f ||
        arg_safein_epsilon_override >= 0.0f;
    const bool has_safeout_epsilon_override =
        arg_safeout_epsilon_percentile >= 0.0f ||
        arg_safeout_epsilon_override >= 0.0f;
    const bool split_safein_safeout_eps =
        has_safein_epsilon_override ||
        has_safeout_epsilon_override;

    const bool skip_false_stats = (arg_skip_false_stats != 0);
    const bool needs_cluster_members =
        !skip_false_stats ||
        arg_safein_epsilon_percentile >= 0.0f ||
        arg_safeout_epsilon_percentile >= 0.0f;

    std::unordered_map<int64_t, uint32_t> image_id_to_row;
    if (needs_cluster_members) {
        image_id_to_row.reserve(static_cast<size_t>(N));
        for (uint32_t row = 0; row < N; ++row) {
            image_id_to_row[img_ids.data[row]] = row;
        }
    }

    std::vector<std::vector<uint32_t>> cluster_members_for_stats;
    bool have_cluster_members_for_stats = false;
    if (needs_cluster_members && !built_assignments.empty()) {
        BuildClusterMembers(built_assignments, index.nlist(),
                            &cluster_members_for_stats);
        have_cluster_members_for_stats = true;
    } else if (needs_cluster_members && !arg_assignments.empty()) {
        auto assignments_or = LoadAssignments(arg_assignments, N);
        if (!assignments_or.ok()) {
            std::fprintf(stderr, "Failed to load assignments: %s\n",
                         assignments_or.status().ToString().c_str());
            return 1;
        }
        BuildClusterMembers(assignments_or.value(), index.nlist(),
                            &cluster_members_for_stats);
        have_cluster_members_for_stats = true;
    } else if (needs_cluster_members && !arg_index_dir.empty() &&
        !image_id_to_row.empty()) {
        auto recover_status = RecoverClusterMembersFromIndex(
            index, image_id_to_row, &cluster_members_for_stats);
        if (!recover_status.ok()) {
            std::fprintf(stderr,
                         "Failed to recover cluster members from index payload: %s\n",
                         recover_status.ToString().c_str());
            return 1;
        }
        have_cluster_members_for_stats = true;
    }

    if (arg_safein_dk_percentile >= 0.0f ||
        !arg_safein_dk_samples_input.empty() ||
        !arg_safein_dk_samples_output.empty() ||
        arg_safein_epsilon_percentile >= 0.0f ||
        arg_safeout_epsilon_percentile >= 0.0f) {
        const bool need_calibration_codes =
            !arg_safein_dk_samples_output.empty() ||
            (arg_safein_dk_samples_input.empty() &&
             arg_safein_dk_percentile >= 0.0f) ||
            arg_safein_epsilon_percentile >= 0.0f ||
            arg_safeout_epsilon_percentile >= 0.0f;
        if (need_calibration_codes && !have_cluster_members_for_stats) {
            std::fprintf(stderr,
                         "SafeIn/SafeOut runtime calibration requires payload ids, "
                         "fresh assignments, or --assignments.\n");
            return 1;
        }
        std::vector<std::vector<rabitq::RaBitQCode>> all_codes;
        std::vector<uint32_t> calibration_cluster_subset;
        const std::vector<uint32_t>* calibration_cluster_subset_ptr = nullptr;
        if (need_calibration_codes &&
            arg_safein_dk_search_scope == SafeInDkSearchScope::NProbe) {
            const uint32_t dk_queries = std::min<uint32_t>(
                std::max<uint32_t>(safein_dk_requested_samples, 1u), Q);
            std::vector<uint32_t> sample_qids(dk_queries);
            std::iota(sample_qids.begin(), sample_qids.end(), 0u);
            std::unordered_set<uint32_t> cluster_set;
            cluster_set.reserve(static_cast<size_t>(dk_queries) *
                                static_cast<size_t>(std::max(1, arg_safein_dk_nprobe)));
            for (uint32_t qi : sample_qids) {
                const float* q = qry_emb.data.data() + static_cast<size_t>(qi) * dim;
                const auto probed = index.FindNearestClusters(
                    q, static_cast<uint32_t>(std::max(1, arg_safein_dk_nprobe)));
                for (ClusterID cid : probed) cluster_set.insert(cid);
            }
            calibration_cluster_subset.assign(cluster_set.begin(), cluster_set.end());
            std::sort(calibration_cluster_subset.begin(),
                      calibration_cluster_subset.end());
            calibration_cluster_subset_ptr = &calibration_cluster_subset;
            Log("  SafeIn calibration cluster subset: %zu clusters (scope=nprobe, nprobe=%d)\n",
                calibration_cluster_subset.size(), arg_safein_dk_nprobe);
        }
        if (need_calibration_codes) {
            EncodeAllCodes(img_emb.data, N, dim, cluster_members_for_stats,
                           index.centroids(), index.rotation(),
                           loaded_rabitq_config,
                           calibration_cluster_subset_ptr, &all_codes);
        }

        if (!arg_safein_dk_samples_input.empty()) {
            auto dk_or = io::LoadNpyFloat32(arg_safein_dk_samples_input);
            if (!dk_or.ok()) {
                std::fprintf(stderr, "Failed to load safein d_k samples from %s: %s\n",
                             arg_safein_dk_samples_input.c_str(),
                             dk_or.status().ToString().c_str());
                return 1;
            }
            safein_dk_sample_values = std::move(dk_or.value().data);
            safein_dk_samples_count =
                static_cast<uint32_t>(safein_dk_sample_values.size());
            if (safein_dk_samples_count == 0) {
                std::fprintf(stderr, "safein d_k samples input is empty: %s\n",
                             arg_safein_dk_samples_input.c_str());
                return 1;
            }
        } else if (!arg_safein_dk_samples_output.empty() ||
                   arg_safein_dk_percentile >= 0.0f) {
            const SafeInDkSamplingMode effective_mode =
                (effective_safein_dk_sampling_mode == "with_replacement")
                    ? SafeInDkSamplingMode::WithReplacement
                    : SafeInDkSamplingMode::Unique;
            safein_dk_sample_values = GenerateRabitqSafeInDkSamples(
                qry_emb.data.data(), Q, dim, GT_K, safein_dk_requested_samples,
                cluster_members_for_stats, all_codes, index.centroids(),
                index.rotation(), loaded_rabitq_config,
                static_cast<uint64_t>(arg_seed), effective_mode, &index,
                arg_safein_dk_search_scope,
                static_cast<uint32_t>(std::max(1, arg_safein_dk_nprobe)));
            safein_dk_samples_count =
                static_cast<uint32_t>(safein_dk_sample_values.size());
            if (safein_dk_samples_count == 0) {
                std::fprintf(stderr,
                             "SafeIn d_k sample generation produced no valid samples\n");
                return 1;
            }
            if (!arg_safein_dk_samples_output.empty()) {
                s = io::SaveNpyFloat32Vector(arg_safein_dk_samples_output,
                                             safein_dk_sample_values);
                if (!s.ok()) {
                    std::fprintf(stderr,
                                 "Failed to write safein d_k samples to %s: %s\n",
                                 arg_safein_dk_samples_output.c_str(),
                                 s.ToString().c_str());
                    return 1;
                }
                Log("  SafeIn d_k samples written: %s\n",
                    arg_safein_dk_samples_output.c_str());
            }
        }
        if (!safein_dk_sample_values.empty()) {
            const auto& samples = safein_dk_sample_values;
            safein_dk_samples_count = static_cast<uint32_t>(samples.size());
            Log("  SafeIn d_k samples: count=%u min=%.6f p50=%.6f p90=%.6f p95=%.6f p97=%.6f p98=%.6f p99=%.6f max=%.6f\n",
                safein_dk_samples_count,
                *std::min_element(samples.begin(), samples.end()),
                SelectPercentileForLog(samples, 0.50f),
                SelectPercentileForLog(samples, 0.90f),
                SelectPercentileForLog(samples, 0.95f),
                SelectPercentileForLog(samples, 0.97f),
                SelectPercentileForLog(samples, 0.98f),
                SelectPercentileForLog(samples, 0.99f),
                *std::max_element(samples.begin(), samples.end()));
        }
        if (arg_safein_dk_percentile >= 0.0f && !safein_dk_sample_values.empty()) {
            runtime_safein_d_k = SelectSafeInDkFromSamples(
                safein_dk_sample_values, arg_safein_dk_percentile);
            runtime_safein_d_k_valid = true;
            Log("  SafeIn d_k selected: percentile=%.4f value=%.6f\n",
                arg_safein_dk_percentile, runtime_safein_d_k);
        }
        if (arg_safein_dk_samples_only != 0) {
            Log("  SafeIn d_k samples-only mode complete. Exiting before query path.\n");
            return 0;
        }
        if (arg_safein_epsilon_percentile >= 0.0f) {
            ensure_runtime_safein_d_k();
            runtime_safein_epsilon = CalibrateSplitEpsilon(
                all_codes, cluster_members_for_stats, img_emb.data.data(),
                index.centroids(), index.rotation(), dim,
                static_cast<uint32_t>(arg_epsilon_samples),
                arg_safein_epsilon_percentile, static_cast<uint64_t>(arg_seed),
                runtime_safein_d_k, loaded_rabitq_config,
                calibration_cluster_subset_ptr, arg_epsilon_sampling_mode,
                &safein_epsilon_stats);
        } else if (arg_safein_epsilon_override >= 0.0f) {
            runtime_safein_epsilon = arg_safein_epsilon_override;
        }
        if (arg_safeout_epsilon_percentile >= 0.0f) {
            ensure_runtime_safein_d_k();
            runtime_safeout_epsilon = CalibrateSplitEpsilon(
                all_codes, cluster_members_for_stats, img_emb.data.data(),
                index.centroids(), index.rotation(), dim,
                static_cast<uint32_t>(arg_epsilon_samples),
                arg_safeout_epsilon_percentile, static_cast<uint64_t>(arg_seed),
                runtime_safein_d_k, loaded_rabitq_config,
                calibration_cluster_subset_ptr, arg_epsilon_sampling_mode,
                &safeout_epsilon_stats);
        }
        if (arg_safein_dk_percentile >= 0.0f) {
            ensure_runtime_safein_d_k();
            index.OverrideConANN(runtime_safeout_epsilon,
                                 index.conann().legacy_d_k(),
                                 runtime_safein_d_k, true);
            Log("  Runtime SafeIn/SafeOut overrides: safein_d_k=%.6f safein_eps=%.6f safeout_eps=%.6f scope=%s nprobe=%d\n",
                runtime_safein_d_k, runtime_safein_epsilon,
                runtime_safeout_epsilon,
                arg_safein_dk_search_scope == SafeInDkSearchScope::NProbe ? "nprobe" : "full",
                arg_safein_dk_nprobe);
        } else {
            Log("  Runtime SafeIn/SafeOut overrides: safein_d_k=(none) safein_eps=%.6f safeout_eps=%.6f scope=%s nprobe=%d\n",
                runtime_safein_epsilon, runtime_safeout_epsilon,
                arg_safein_dk_search_scope == SafeInDkSearchScope::NProbe ? "nprobe" : "full",
                arg_safein_dk_nprobe);
        }
    } else if (!runtime_safein_d_k_valid &&
               arg_dynamic_safein_mode == DynamicSafeInMode::Frontier) {
        Log("  Runtime SafeIn threshold: dynamic_frontier (no static safein_d_k loaded)\n");
    }

    if (!index.segment().resident_preload_enabled()) {
        Log("\n[Phase C.5] Prewarming resident/full-preload clusters...\n");
        auto preload_status = index.segment().PreloadAllClusters();
        if (!preload_status.ok()) {
            std::fprintf(stderr,
                         "Failed to preload resident clusters: %s\n",
                         preload_status.ToString().c_str());
            return 1;
        }
        Log("  Prewarm done: preload_time=%.3f ms  preload_bytes=%llu\n",
            index.segment().resident_preload_time_ms(),
            static_cast<unsigned long long>(
                index.segment().resident_preload_bytes()));
    }

    // ================================================================
    // Phase D: Query
    // ================================================================
    IoUringReader cluster_reader;
    auto init_status = cluster_reader.Init(
        static_cast<uint32_t>(arg_io_queue_depth), 4096, arg_iopoll, arg_sqpoll);
    if (!init_status.ok()) {
        std::fprintf(stderr, "IoUring init failed: %s\n", init_status.ToString().c_str());
        return 1;
    }

    std::unique_ptr<IoUringReader> data_reader;
    if (arg_submission_mode == "isolated") {
        data_reader = std::make_unique<IoUringReader>();
        auto data_status = data_reader->Init(
            static_cast<uint32_t>(arg_io_queue_depth), 4096, arg_iopoll, arg_sqpoll);
        if (!data_status.ok()) {
            std::fprintf(stderr, "Data IoUring init failed: %s\n",
                         data_status.ToString().c_str());
            return 1;
        }
    }

    // Register file descriptors for IOSQE_FIXED_FILE optimization
    if (arg_submission_mode == "isolated") {
        int clu_fd = index.segment().clu_fd();
        auto reg_status = cluster_reader.RegisterFiles(&clu_fd, 1);
        if (!reg_status.ok()) {
            Log("  Warning: RegisterFiles(cluster) failed: %s (continuing without)\n",
                reg_status.ToString().c_str());
        }
        int dat_fd = index.segment().data_reader().fd();
        auto data_reg_status = data_reader->RegisterFiles(&dat_fd, 1);
        if (!data_reg_status.ok()) {
            Log("  Warning: RegisterFiles(data) failed: %s (continuing without)\n",
                data_reg_status.ToString().c_str());
        }
    } else {
        int fds[2] = {index.segment().clu_fd(),
                      index.segment().data_reader().fd()};
        auto reg_status = cluster_reader.RegisterFiles(fds, 2);
        if (!reg_status.ok()) {
            Log("  Warning: RegisterFiles failed: %s (continuing without)\n",
                reg_status.ToString().c_str());
        }
    }

    // Prepare query IDs as a flat vector for RunQueryRound
    std::vector<int64_t> qry_ids_vec(qry_ids.data.begin(),
                                      qry_ids.data.begin() + Q);

    // Base search config (shared across single run and sweep)
    SearchConfig search_cfg;
    search_cfg.top_k = GT_K;
    search_cfg.nprobe = static_cast<uint32_t>(arg_nprobe);
    search_cfg.enable_dynamic_safeout = (arg_dynamic_safeout != 0);
    search_cfg.dynamic_safein_mode = arg_dynamic_safein_mode;
    search_cfg.dynamic_safein_min_probes =
        static_cast<uint32_t>(std::max(arg_dynamic_safein_min_probes, 0));
    search_cfg.dynamic_safein_stable_probes =
        static_cast<uint32_t>(std::max(arg_dynamic_safein_stable_probes, 1));
    search_cfg.dynamic_safein_rel_tol = arg_dynamic_safein_rel_tol;
    search_cfg.dynamic_safein_abs_tol = arg_dynamic_safein_abs_tol;
    search_cfg.dynamic_safein_defer_initial_clusters =
        static_cast<uint32_t>(std::max(arg_dynamic_safein_defer_initial_clusters, 0));
    search_cfg.dynamic_safein_defer_until_ready =
        (arg_dynamic_safein_defer_until_ready != 0);
    search_cfg.dynamic_safein_defer_max_candidates =
        static_cast<uint32_t>(std::max(arg_dynamic_safein_defer_max_candidates, 0));
    search_cfg.non_safeout_candidate_budget =
        static_cast<uint32_t>(arg_non_safeout_candidate_budget);
    search_cfg.safein_all_threshold =
        static_cast<uint32_t>(arg_safein_all_threshold_bytes);
    search_cfg.io_queue_depth = static_cast<uint32_t>(arg_io_queue_depth);
    search_cfg.fixed_vec_buffer_count =
        static_cast<uint32_t>(std::max(arg_fixed_vec_buffer_count, 0));
    search_cfg.cluster_submit_reserve = static_cast<uint32_t>(
        std::max(arg_cluster_submit_reserve, 1));
    search_cfg.use_sqpoll = arg_sqpoll;
    search_cfg.submission_mode =
        (arg_submission_mode == "isolated")
            ? SubmissionMode::Isolated
            : SubmissionMode::Shared;
    search_cfg.enable_fine_grained_timing = (arg_fine_grained_timing != 0);
    search_cfg.enable_hotpath_detailed_timing =
        (arg_hotpath_detailed_timing != 0);
    search_cfg.submit_batch_size = static_cast<uint32_t>(
        GetIntArg(argc, argv, "--submit-batch", 32));
    search_cfg.enable_online_submit_tuning = (arg_submit_online != 0);
    search_cfg.submit_ema_alpha = arg_submit_ema_alpha;
    search_cfg.enable_address_decode_simd = (arg_address_decode_simd != 0);
    search_cfg.enable_rerank_batched_distance_simd =
        (arg_rerank_batched_distance_simd != 0);
    search_cfg.enable_coarse_select_simd = (arg_coarse_select_simd != 0);
    search_cfg.enable_coarse_select_phase2 = (arg_coarse_select_phase2 != 0);
    search_cfg.enable_two_level_coarse_routing =
        (arg_two_level_coarse_routing != 0);
    search_cfg.two_level_coarse_threshold =
        static_cast<uint32_t>(std::max(1, arg_two_level_coarse_threshold));
    search_cfg.two_level_coarse_super_count =
        static_cast<uint32_t>(std::max(0, arg_two_level_coarse_super_count));
    search_cfg.two_level_coarse_super_factor =
        static_cast<uint32_t>(std::max(0, arg_two_level_coarse_super_factor));
    search_cfg.two_level_coarse_budget_factor =
        static_cast<uint32_t>(std::max(1, arg_two_level_coarse_budget_factor));
    search_cfg.enable_two_level_coarse_exact_overlap =
        (arg_two_level_coarse_exact_overlap != 0);
    search_cfg.enable_stage1_safein = (arg_enable_stage1_safein != 0);
    search_cfg.enable_stage2_collect_block_first = (arg_stage2_block_first != 0);
    search_cfg.enable_stage2_scatter_batch_classify = (arg_stage2_batch_classify != 0);
    if (has_safein_epsilon_override) {
        search_cfg.safein_epsilon_override = runtime_safein_epsilon;
    }
    if (has_safeout_epsilon_override) {
        search_cfg.safeout_epsilon_override = runtime_safeout_epsilon;
    }
    if (split_safein_safeout_eps) {
        search_cfg.enable_stage2_scatter_batch_classify = false;
    }
    if (have_cluster_members_for_stats) {
        search_cfg.false_stats_cluster_members = &cluster_members_for_stats;
    }

    if (!index.segment().resident_preload_enabled()) {
        auto preload_status = index.segment().PreloadAllClusters();
        if (!preload_status.ok()) {
            std::fprintf(stderr,
                         "Failed to preload resident clusters before benchmark: %s\n",
                         preload_status.ToString().c_str());
            return 1;
        }
    }

    // ================================================================
    // nprobe sweep mode (tasks 3.2–3.4)
    // ================================================================
    if (!nprobe_sweep_list.empty()) {
        Log("\n[Sweep] nprobe sweep: %s\n", arg_nprobe_sweep_str.c_str());

        // Prepare CSV
        fs::create_directories(output_dir);
        std::string sweep_csv_path = output_dir + "/nprobe_sweep.csv";
        bool write_header = !fs::exists(sweep_csv_path);
        std::ofstream sweep_csv(sweep_csv_path, std::ios::app);
        if (write_header) {
            sweep_csv << "nprobe,recall@1,recall@10,avg_ms,p50_ms,p99_ms,"
                         "avg_probe_ms,avg_io_wait_ms,avg_uring_submit_ms,"
                         "avg_submit_calls,avg_safe_out_rate,io_queue_depth,"
                         "sqpoll_enabled,cluster_submit_reserve,submission_mode,"
                         "clustering_source,avg_duplicate_candidates,avg_deduplicated_candidates,"
	                         "avg_unique_fetch_candidates,index_source,resolved_index_dir,"
	                         "rabitq_format_key,rabitq_estimator_mode,rabitq_total_bits,"
	                         "rabitq_ex_bits,rabitq_exdata_layout,"
	                         "rabitq_effective_exdata_layout,"
                         "loaded_eps_ip,loaded_d_k,preload_time_ms,preload_bytes,"
                         "resident_preload_batch_size,resident_file_size_bytes,"
                         "resident_file_buffer_bytes,resident_code_storage_bytes,"
                         "resident_decoded_address_bytes,resident_raw_address_bytes,"
                         "resident_parsed_address_duplicate_bytes,resident_cluster_mem_bytes,"
                         "logical_dim,effective_dim,padding_mode,rotation_mode,"
                         "avg_prepare_rotation_ms,avg_prepare_quant_lut_ms,index_total_bytes\n";
        }

        // Warmup query count for sweep (100 or all if Q < 100)
        uint32_t sw_warmup = std::min(100u, Q);
        uint32_t sw_measure = Q;

        for (int np : nprobe_sweep_list) {
            Log("\n[Sweep] nprobe=%d ...\n", np);
            search_cfg.nprobe = static_cast<uint32_t>(np);

            // Warmup round (discard results)
            {
                auto [wq, wm] = RunQueryRound(
                    "SWEEP-WARMUP", index, cluster_reader, data_reader.get(), search_cfg,
                    qry_emb.data.data(), sw_warmup, dim, qry_ids_vec, gt_topk, gt_dists,
                    !skip_gt);
                (void)wq; (void)wm;
            }

            // Measurement round
            auto [sq, sm] = RunQueryRound(
                "SWEEP-MEASURE", index, cluster_reader, data_reader.get(), search_cfg,
                qry_emb.data.data(), sw_measure, dim, qry_ids_vec, gt_topk, gt_dists,
                !skip_gt);

            double safe_out_rate = (sm.avg_probed > 0)
                ? sm.avg_safe_out / sm.avg_probed * 100.0 : 0.0;

            Log("[sweep] nprobe=%d  recall@10=%.4f  avg=%.3fms  probe=%.3fms  safe_out_rate=%.1f%%\n",
                np, sm.recall_at[2], sm.avg_query_ms, sm.avg_probe, safe_out_rate);

            sweep_csv << np << "," << sm.recall_at[0] << "," << sm.recall_at[2] << ","
                      << sm.avg_query_ms << "," << sm.p50 << "," << sm.p99 << ","
                      << sm.avg_probe << "," << sm.avg_io_wait << ","
                      << sm.avg_uring_submit << "," << sm.avg_submit_calls << ","
                      << safe_out_rate << "," << search_cfg.io_queue_depth << ","
                      << (cluster_reader.sqpoll_enabled() ? 1 : 0) << ","
                      << search_cfg.cluster_submit_reserve << ","
                      << arg_submission_mode << ","
                      << ClusteringSourceName(index.clustering_source()) << ","
                      << sm.avg_duplicate_candidates << ","
                      << sm.avg_deduplicated_candidates << ","
                      << sm.avg_unique_fetch_candidates << ","
                      << index_source << ","
                      << "\"" << resolved_index_dir << "\"" << ","
                      << "\"" << RaBitQFormatKey(index.segment().rabitq_config()) << "\","
                      << "\"" << std::string(RaBitQEstimatorModeName(
                             index.segment().rabitq_config().estimator_mode)) << "\","
                      << static_cast<uint32_t>(
                             index.segment().rabitq_config().effective_total_bits()) << ","
	                      << static_cast<uint32_t>(
	                             index.segment().rabitq_config().stage2_payload_bits()) << ","
	                      << "\"" << std::string(RaBitQExDataLayoutName(
	                             index.segment().rabitq_config().exdata_layout)) << "\","
	                      << "\"" << std::string(RaBitQExDataLayoutName(
	                             index.segment().rabitq_config().effective_exdata_layout())) << "\","
	                      << index.conann().epsilon() << ","
                      << index.conann().d_k() << ","
                      << sm.preload_time_ms << ","
                      << sm.preload_bytes << ","
                      << sm.resident_preload_batch_size << ","
                      << sm.resident_file_size_bytes << ","
                      << sm.resident_file_buffer_bytes << ","
                      << sm.resident_code_storage_bytes << ","
                      << sm.resident_decoded_address_bytes << ","
                      << sm.resident_raw_address_bytes << ","
                      << sm.resident_parsed_address_duplicate_bytes << ","
                      << sm.resident_cluster_mem_bytes << ","
                      << index.logical_dim() << ","
                      << index.effective_dim() << ","
                      << "\"" << index.padding_mode() << "\"" << ","
                      << "\"" << index.rotation_mode() << "\"" << ","
                      << sm.avg_probe_prepare_rotation << ","
                      << sm.avg_probe_prepare_quant_lut << ","
                      << sm.index_total_bytes << "\n";
            sweep_csv.flush();
        }

        Log("\n[Sweep] Results written to %s\n", sweep_csv_path.c_str());
        return 0;
    }

    // Single round: fixed nprobe + dynamic SafeOut
    auto [qresults, metrics] = RunQueryRound(
        arg_cold ? "HOT" : "FIXED", index, cluster_reader, data_reader.get(), search_cfg,
        qry_emb.data.data(), Q, dim, qry_ids_vec, gt_topk, gt_dists, !skip_gt);

    // ================================================================
    // Phase D2: Cold-read round (optional)
    // ================================================================
    RoundMetrics cold_metrics{};
    if (arg_cold) {
        int clu_fd = index.segment().clu_fd();
        int dat_fd = index.segment().data_reader().fd();

        off_t clu_size = lseek(clu_fd, 0, SEEK_END);
        off_t dat_size = lseek(dat_fd, 0, SEEK_END);

        Log("\n[Cold] Evicting page cache via posix_fadvise(FADV_DONTNEED)...\n");
        Log("  .clu fd=%d size=%ld bytes\n", clu_fd, static_cast<long>(clu_size));
        Log("  .dat fd=%d size=%ld bytes\n", dat_fd, static_cast<long>(dat_size));

        int r1 = posix_fadvise(clu_fd, 0, clu_size, POSIX_FADV_DONTNEED);
        int r2 = posix_fadvise(dat_fd, 0, dat_size, POSIX_FADV_DONTNEED);
        if (r1 != 0 || r2 != 0) {
            Log("  WARNING: posix_fadvise returned non-zero (clu=%d, dat=%d)\n",
                r1, r2);
        } else {
            Log("  Page cache eviction done.\n");
        }

        auto [cold_qresults, cm] = RunQueryRound(
            "COLD", index, cluster_reader, data_reader.get(), search_cfg,
            qry_emb.data.data(), Q, dim, qry_ids_vec, gt_topk, gt_dists, !skip_gt);
        cold_metrics = cm;

        Log("\n╔══════════════════════════════════════════════════════════╗\n");
        Log("║              HOT vs COLD Read Comparison                ║\n");
        Log("╠══════════════════════════════════════════════════════════╣\n");
        Log("║  Metric              │     HOT     │     COLD    │ Δ   ║\n");
        Log("╟──────────────────────┼─────────────┼─────────────┼─────╢\n");
        Log("║  avg_query (ms)      │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.avg_query_ms, cold_metrics.avg_query_ms,
            (cold_metrics.avg_query_ms / metrics.avg_query_ms - 1.0) * 100);
        Log("║  avg_io_wait (ms)    │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.avg_io_wait, cold_metrics.avg_io_wait,
            cold_metrics.avg_io_wait > 0.001
                ? (cold_metrics.avg_io_wait / std::max(metrics.avg_io_wait, 0.001) - 1.0) * 100
                : 0.0);
        Log("║  avg_probe (ms)      │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.avg_probe, cold_metrics.avg_probe,
            (cold_metrics.avg_probe / std::max(metrics.avg_probe, 0.001) - 1.0) * 100);
        Log("║  avg_rerank_cpu (ms) │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.avg_rerank_cpu, cold_metrics.avg_rerank_cpu,
            (cold_metrics.avg_rerank_cpu / std::max(metrics.avg_rerank_cpu, 0.001) - 1.0) * 100);
        Log("║  p99 (ms)            │ %11.3f │ %11.3f │%+.0f%%║\n",
            metrics.p99, cold_metrics.p99,
            (cold_metrics.p99 / std::max(metrics.p99, 0.001) - 1.0) * 100);
        Log("║  overlap_ratio       │ %11.4f │ %11.4f │     ║\n",
            metrics.overlap_ratio, cold_metrics.overlap_ratio);
        Log("║  recall@10           │ %11.4f │ %11.4f │     ║\n",
            metrics.recall_at[2], cold_metrics.recall_at[2]);
        Log("╚══════════════════════════════════════════════════════════╝\n");
    }

    // ================================================================
    // Phase E: Summary Output
    // ================================================================
    Log("\n=== Summary ===\n");
    if (metrics.recall_available) {
        Log("  recall@1=%.4f  recall@5=%.4f  recall@10=%.4f\n",
            metrics.recall_at[0], metrics.recall_at[1], metrics.recall_at[2]);
    } else {
        Log("  recall skipped (query-only mode)\n");
    }
    Log("  avg_query=%.3f ms  p50=%.3f  p95=%.3f  p99=%.3f\n",
        metrics.avg_query_ms, metrics.p50, metrics.p95, metrics.p99);
    Log("  build_time=%.1f ms  brute_force=%.1f ms\n",
        training_time_ms, brute_force_time_ms);
    Log("  benchmark_mode=%s  gt_mode=%s\n",
        query_only_mode ? "query_only" : "full_e2e",
        gt_mode.c_str());
    Log("  index_source=%s  resolved_index_dir=%s\n",
        index_source.c_str(), resolved_index_dir.c_str());
    Log("  exrabitq_storage_version=%u  exrabitq_storage_format=%s\n",
        index.segment().cluster_reader().file_version(),
        ExRaBitQStorageFormatName(index.segment().cluster_reader().file_version()));
    Log("  rabitq_estimator_mode=%s  rabitq_total_bits=%u  rabitq_ex_bits=%u\n",
        std::string(RaBitQEstimatorModeName(
            index.segment().rabitq_config().estimator_mode)).c_str(),
        static_cast<uint32_t>(index.segment().rabitq_config().effective_total_bits()),
        static_cast<uint32_t>(index.segment().rabitq_config().stage2_payload_bits()));
    Log("  rabitq_exdata_layout=%s  rabitq_effective_exdata_layout=%s\n",
        std::string(RaBitQExDataLayoutName(
            index.segment().rabitq_config().exdata_layout)).c_str(),
        std::string(RaBitQExDataLayoutName(
            index.segment().rabitq_config().effective_exdata_layout())).c_str());
    Log("  rabitq_format_key=%s  rabitq_validation_mode=%s\n",
        RaBitQFormatKey(index.segment().rabitq_config()).c_str(),
        RaBitQValidationModeName(arg_rabitq_validation_mode));
    Log("  exrabitq_stage2_magnitude_packed=%s\n",
        (index.segment().cluster_reader().file_version() >= 12 &&
         index.segment().rabitq_config().has_stage2_payload())
            ? "true"
            : "false");
    Log("  loaded_eps_ip=%.6f  loaded_d_k=%.6f\n",
        index.conann().epsilon(), index.conann().d_k());
    Log("  clustering_source=%s\n",
        ClusteringSourceName(index.clustering_source()));
    Log("  io_wait=%.3f ms  cpu=%.3f ms  coarse_select=%.3f ms  score=%.3f ms  topn=%.3f ms  probe=%.3f ms\n",
        metrics.avg_io_wait, metrics.avg_cpu, metrics.avg_coarse_select,
        metrics.avg_coarse_score, metrics.avg_coarse_topn, metrics.avg_probe);
    Log("  avg_probed_clusters=%.1f\n", metrics.avg_probed_clusters);
    Log("  dynamic_safeout_enabled=%d\n",
        search_cfg.enable_dynamic_safeout ? 1 : 0);
    Log("  fine_grained_timing=%d\n", search_cfg.enable_fine_grained_timing ? 1 : 0);
    Log("  hotpath_detailed_timing=%d\n",
        search_cfg.enable_hotpath_detailed_timing ? 1 : 0);
    Log("  fixed_vec_buffer_count=%u\n", search_cfg.fixed_vec_buffer_count);
    if (!search_cfg.enable_fine_grained_timing) {
        Log("  timing_mode=low_overhead_coarse_split (stage1/stage2 are workload-weighted classify splits)\n");
    }
    Log("  probe_prepare=%.3f ms  probe_stage1=%.3f ms  probe_stage2=%.3f ms  probe_classify=%.3f ms  probe_submit=%.3f ms\n",
        metrics.avg_probe_prepare, metrics.avg_probe_stage1, metrics.avg_probe_stage2,
        metrics.avg_probe_classify, metrics.avg_probe_submit);
    Log("  prepare_subtract=%.3f ms  prepare_normalize=%.3f ms  prepare_quantize=%.3f ms  prepare_lut_build=%.3f ms\n",
        metrics.avg_probe_prepare_subtract, metrics.avg_probe_prepare_normalize,
        metrics.avg_probe_prepare_quantize, metrics.avg_probe_prepare_lut_build);
    Log("  stage1_estimate=%.3f ms  stage1_mask=%.3f ms  stage1_iterate=%.3f ms  stage1_classify=%.3f ms\n",
        metrics.avg_probe_stage1_estimate, metrics.avg_probe_stage1_mask,
        metrics.avg_probe_stage1_iterate, metrics.avg_probe_stage1_classify_only);
    Log("  stage1_fused_blocks=%.1f  fused_safeout_lanes=%.1f  fused_safein_lanes=%.1f\n",
        metrics.avg_stage1_fused_blocks,
        metrics.avg_stage1_fused_safeout_lanes,
        metrics.avg_stage1_fused_safein_lanes);
    Log("  stage2_collect=%.3f ms  stage2_kernel=%.3f ms  stage2_scatter=%.3f ms\n",
        metrics.avg_probe_stage2_collect, metrics.avg_probe_stage2_kernel,
        metrics.avg_probe_stage2_scatter);
    Log("  stage2_decode=%.3f ms  decode_blocks=%.1f  decode_in_bytes=%.1f  decode_out_bytes=%.1f\n",
        metrics.avg_probe_stage2_decode,
        metrics.avg_stage2_decode_blocks,
        metrics.avg_stage2_decode_input_bytes,
        metrics.avg_stage2_decode_output_bytes);
    Log("  stage2_kernel_sign_flip=%.3f ms  stage2_kernel_abs_fma=%.3f ms  stage2_kernel_tail=%.3f ms  stage2_kernel_reduce=%.3f ms\n",
        metrics.avg_probe_stage2_kernel_sign_flip,
        metrics.avg_probe_stage2_kernel_abs_fma,
        metrics.avg_probe_stage2_kernel_tail,
        metrics.avg_probe_stage2_kernel_reduce);
    Log("  stage2_masked_calls=%.1f  lanes_requested=%.1f  lanes_skipped=%.1f  lanes_total_valid=%.1f  lane_density=%.4f\n",
        metrics.avg_stage2_masked_kernel_calls,
        metrics.avg_stage2_lanes_requested,
        metrics.avg_stage2_lanes_skipped,
        metrics.avg_stage2_lanes_total_valid,
        metrics.avg_stage2_lane_density);
    Log("  cluster_mode=resident_full_preload  preload_time=%.3f ms  preload_bytes=%.0f  resident_mem=%.0f  parallel_view_build=%.3f ms  parallel_view_bytes=%.0f\n",
        metrics.preload_time_ms,
        metrics.preload_bytes,
        metrics.resident_cluster_mem_bytes,
        metrics.resident_parallel_view_build_ms,
        metrics.resident_parallel_view_bytes);
    Log("  safe_in=%.1f  safe_out=%.1f  uncertain=%.1f\n",
        metrics.avg_safe_in, metrics.avg_safe_out, metrics.avg_uncertain);
    Log("  s2_safe_in=%.1f  s2_safe_out=%.1f  s2_uncertain=%.1f\n",
        metrics.avg_s2_safe_in, metrics.avg_s2_safe_out, metrics.avg_s2_uncertain);
    Log("  duplicate_candidates=%.1f  deduplicated=%.1f  unique_fetch=%.1f\n",
        metrics.avg_duplicate_candidates,
        metrics.avg_deduplicated_candidates,
        metrics.avg_unique_fetch_candidates);
    Log("  non_safeout_candidate_budget=%u  budget_seen=%.1f  selected=%.1f  dropped=%.1f\n",
        search_cfg.non_safeout_candidate_budget,
        metrics.avg_candidate_budget_seen,
        metrics.avg_candidate_budget_selected,
        metrics.avg_candidate_budget_dropped);
    Log("  false_safeout=%.2f  false_safein_upper=%.1f  total_safein=%lu\n",
        metrics.avg_false_safeout, metrics.avg_false_safein_upper,
        static_cast<unsigned long>(metrics.total_final_safein));
    Log("  overlap=%.4f\n", metrics.overlap_ratio);

    // ================================================================
    // Phase F: Output JSON
    // ================================================================
    fs::create_directories(output_dir);  // ensure output_dir exists (needed when --index-dir is used)
    Log("\n[Phase F] Writing results to %s\n", output_dir.c_str());

    // config.json
    {
        std::ofstream f(output_dir + "/config.json");
        f << "{\n";
        f << "  " << JStr("dataset", ds_name) << ",\n";
        f << "  " << JStr("dataset_path", data_dir) << ",\n";
        f << "  " << JStr("timestamp", ts) << ",\n";
        f << "  " << JStr("benchmark_mode", query_only_mode ? "query_only" : "full_e2e") << ",\n";
        f << "  " << JStr("gt_mode", gt_mode) << ",\n";
        f << "  " << JStr("gt_source", gt_source) << ",\n";
        f << "  " << JStr("gt_file", arg_gt_file) << ",\n";
        f << "  " << JStr("index_source", index_source) << ",\n";
        f << "  " << JStr("resolved_index_dir", resolved_index_dir) << ",\n";
        f << "  " << JInt("exrabitq_storage_version",
                          index.segment().cluster_reader().file_version()) << ",\n";
        f << "  " << JStr("exrabitq_storage_format",
                          ExRaBitQStorageFormatName(
                              index.segment().cluster_reader().file_version())) << ",\n";
        f << "  " << JBool("exrabitq_stage2_magnitude_packed",
                            index.segment().cluster_reader().file_version() >= 12 &&
                            index.segment().rabitq_config().has_stage2_payload()) << ",\n";
        f << "  " << JStr("rabitq_estimator_mode",
                          std::string(RaBitQEstimatorModeName(
                              index.segment().rabitq_config().estimator_mode))) << ",\n";
        f << "  " << JInt("rabitq_total_bits",
                          index.segment().rabitq_config().effective_total_bits()) << ",\n";
        f << "  " << JInt("rabitq_ex_bits",
                          index.segment().rabitq_config().stage2_payload_bits()) << ",\n";
        f << "  " << JStr("rabitq_exdata_layout",
                          std::string(RaBitQExDataLayoutName(
                              index.segment().rabitq_config().exdata_layout))) << ",\n";
        f << "  " << JStr("rabitq_effective_exdata_layout",
                          std::string(RaBitQExDataLayoutName(
                              index.segment().rabitq_config().effective_exdata_layout()))) << ",\n";
        f << "  " << JStr("rabitq_format_key",
                          RaBitQFormatKey(index.segment().rabitq_config())) << ",\n";
        f << "  " << JStr("rabitq_validation_mode",
                          RaBitQValidationModeName(arg_rabitq_validation_mode)) << ",\n";
        f << "  " << JInt("num_images", N) << ",\n";
        f << "  " << JInt("num_queries", Q) << ",\n";
        f << "  " << JInt("dimension", dim) << ",\n";
        f << "  " << JInt("logical_dimension", index.logical_dim()) << ",\n";
        f << "  " << JInt("effective_dimension", index.effective_dim()) << ",\n";
        f << "  " << JStr("padding_mode", index.padding_mode()) << ",\n";
        f << "  " << JStr("rotation_mode", index.rotation_mode()) << ",\n";
        f << "  \"build_config\": {\n";
        f << "    " << JInt("nlist", arg_nlist) << ",\n";
        f << "    " << JInt("max_iterations", arg_max_iter) << ",\n";
        f << "    " << JInt("seed", arg_seed) << ",\n";
        f << "    " << JInt("rabitq_bits",
                            arg_rabitq_config.effective_total_bits()) << ",\n";
        f << "    " << JInt("rabitq_total_bits",
                            arg_rabitq_config.effective_total_bits()) << ",\n";
        f << "    " << JInt("rabitq_ex_bits",
                            arg_rabitq_config.stage2_payload_bits()) << ",\n";
        f << "    " << JStr("rabitq_estimator_mode",
                            std::string(RaBitQEstimatorModeName(
                                arg_rabitq_config.estimator_mode))) << ",\n";
        f << "    " << JStr("rabitq_exdata_layout",
                            std::string(RaBitQExDataLayoutName(
                                arg_rabitq_config.exdata_layout))) << ",\n";
        f << "    " << JStr("rabitq_effective_exdata_layout",
                            std::string(RaBitQExDataLayoutName(
                                arg_rabitq_config.effective_exdata_layout()))) << ",\n";
        f << "    " << JStr("rabitq_format_key",
                            RaBitQFormatKey(arg_rabitq_config)) << ",\n";
        f << "    " << JInt("rabitq_block_size", arg_block_size) << ",\n";
        f << "    " << JNum("rabitq_c_factor", arg_c_factor) << ",\n";
        f << "    " << JInt("page_size", arg_page_size) << ",\n";
        f << "    " << JInt("epsilon_samples", arg_epsilon_samples) << ",\n";
        f << "    " << JStr("epsilon_sampling_mode",
                             EpsilonSamplingModeName(arg_epsilon_sampling_mode)) << ",\n";
        f << "    " << JNum("epsilon_percentile", arg_epsilon_percentile) << ",\n";
        f << "    " << JStr("coarse_builder", arg_coarse_builder) << ",\n";
        f << "    " << JStr("requested_metric", index.requested_metric()) << ",\n";
        f << "    " << JStr("effective_metric", index.effective_metric()) << ",\n";
        f << "    " << JInt("faiss_train_size", 100000) << ",\n";
        f << "    " << JInt("faiss_niter", arg_max_iter == 20 ? 10 : arg_max_iter) << ",\n";
        f << "    " << JInt("faiss_nredo", 1) << ",\n";
        f << "    " << JStr("faiss_backend", "cpu") << ",\n";
        f << "    " << JStr("requested_index_dir", arg_index_dir) << ",\n";
        f << "    " << JStr("payload_mode", arg_payload_mode) << ",\n";
        f << "    " << JStr("payload_index", arg_payload_index) << ",\n";
        f << "    " << JStr("payload_data", arg_payload_data) << ",\n";
        f << "    " << JNum("payload_source_bytes",
                            static_cast<double>(
                                flatstor_payload_reader.total_payload_bytes)) << ",\n";
        f << "    " << JStr("resolved_index_dir", resolved_index_dir) << "\n";
        f << "  },\n";
        f << "  \"search_config\": {\n";
        f << "    " << JInt("top_k", search_cfg.top_k) << ",\n";
        f << "    " << JInt("nprobe", search_cfg.nprobe) << ",\n";
        f << "    " << JBool("dynamic_safeout_enabled",
                             search_cfg.enable_dynamic_safeout) << ",\n";
        f << "    " << JStr("dynamic_safein_mode",
                            DynamicSafeInModeName(search_cfg.dynamic_safein_mode)) << ",\n";
        f << "    " << JInt("dynamic_safein_min_probes",
                            search_cfg.dynamic_safein_min_probes) << ",\n";
        f << "    " << JInt("dynamic_safein_stable_probes",
                            search_cfg.dynamic_safein_stable_probes) << ",\n";
        f << "    " << JNum("dynamic_safein_rel_tol",
                            search_cfg.dynamic_safein_rel_tol) << ",\n";
        f << "    " << JNum("dynamic_safein_abs_tol",
                            search_cfg.dynamic_safein_abs_tol) << ",\n";
        f << "    " << JInt("dynamic_safein_defer_initial_clusters",
                            search_cfg.dynamic_safein_defer_initial_clusters) << ",\n";
        f << "    " << JBool("dynamic_safein_defer_until_ready",
                             search_cfg.dynamic_safein_defer_until_ready) << ",\n";
        f << "    " << JInt("dynamic_safein_defer_max_candidates",
                            search_cfg.dynamic_safein_defer_max_candidates) << ",\n";
        f << "    " << JInt("non_safeout_candidate_budget",
                            search_cfg.non_safeout_candidate_budget) << ",\n";
        f << "    " << JInt("safein_all_threshold_bytes",
                            search_cfg.safein_all_threshold) << ",\n";
        f << "    " << JInt("io_queue_depth", search_cfg.io_queue_depth) << ",\n";
        f << "    " << JInt("fixed_vec_buffer_count", search_cfg.fixed_vec_buffer_count) << ",\n";
        f << "    " << JInt("cluster_submit_reserve", search_cfg.cluster_submit_reserve) << ",\n";
        f << "    " << JBool("iopoll_requested", arg_iopoll) << ",\n";
        f << "    " << JBool("sqpoll_requested", arg_sqpoll) << ",\n";
        f << "    " << JBool("sqpoll_effective", cluster_reader.sqpoll_enabled()) << ",\n";
        f << "    " << JStr("submission_mode", arg_submission_mode) << ",\n";
        f << "    " << JStr("cluster_loading", "resident_full_preload") << ",\n";
        f << "    " << JBool("enable_fine_grained_timing", search_cfg.enable_fine_grained_timing) << ",\n";
        f << "    " << JBool("enable_hotpath_detailed_timing", search_cfg.enable_hotpath_detailed_timing) << ",\n";
        f << "    " << JBool("enable_address_decode_simd", search_cfg.enable_address_decode_simd) << ",\n";
        f << "    " << JBool("enable_rerank_batched_distance_simd", search_cfg.enable_rerank_batched_distance_simd) << ",\n";
        f << "    " << JBool("enable_coarse_select_simd", search_cfg.enable_coarse_select_simd) << ",\n";
        f << "    " << JBool("enable_coarse_select_phase2", search_cfg.enable_coarse_select_phase2) << ",\n";
        f << "    " << JBool("enable_two_level_coarse_routing", search_cfg.enable_two_level_coarse_routing) << ",\n";
        f << "    " << JInt("two_level_coarse_threshold", search_cfg.two_level_coarse_threshold) << ",\n";
        f << "    " << JInt("two_level_coarse_super_count", search_cfg.two_level_coarse_super_count) << ",\n";
        f << "    " << JInt("two_level_coarse_super_factor", search_cfg.two_level_coarse_super_factor) << ",\n";
        f << "    " << JInt("two_level_coarse_budget_factor", search_cfg.two_level_coarse_budget_factor) << ",\n";
        f << "    " << JBool("enable_two_level_coarse_exact_overlap", search_cfg.enable_two_level_coarse_exact_overlap) << ",\n";
        f << "    " << JBool("enable_stage1_safein", search_cfg.enable_stage1_safein) << ",\n";
        f << "    " << JBool("enable_stage2_scatter_batch_classify",
                             search_cfg.enable_stage2_scatter_batch_classify) << ",\n";
        f << "    " << JBool("enable_online_submit_tuning", search_cfg.enable_online_submit_tuning) << ",\n";
        f << "    " << JNum("submit_ema_alpha", search_cfg.submit_ema_alpha) << ",\n";
        f << "    " << JNum("safein_epsilon_override", search_cfg.safein_epsilon_override) << ",\n";
        f << "    " << JNum("safeout_epsilon_override", search_cfg.safeout_epsilon_override) << ",\n";
        f << "    " << JStr("timing_mode",
                             search_cfg.enable_hotpath_detailed_timing
                                 ? "hotpath_detailed_diagnostic"
                                 : (search_cfg.enable_fine_grained_timing
                                        ? "fine_grained_diagnostic"
                                        : "low_overhead_coarse_split")) << "\n";
        f << "  },\n";
        f << "  \"runtime_index\": {\n";
        f << "    " << JNum("loaded_eps_ip", loaded_eps_ip) << ",\n";
        f << "    " << JNum("loaded_d_k", loaded_d_k) << ",\n";
        f << "    " << JNum("runtime_eps_ip", index.conann().epsilon()) << ",\n";
        f << "    " << JNum("runtime_d_k", index.conann().d_k()) << ",\n";
        f << "    \"runtime_safein_d_k\": ";
        if (runtime_safein_d_k_valid) {
            f << std::fixed << std::setprecision(4) << runtime_safein_d_k;
        } else {
            f << "null";
        }
        f << ",\n";
        f << "    " << JNum("runtime_safein_epsilon", runtime_safein_epsilon) << ",\n";
        f << "    " << JNum("runtime_safeout_epsilon", runtime_safeout_epsilon) << ",\n";
        f << "    " << JNum("safein_dk_percentile", arg_safein_dk_percentile) << ",\n";
        f << "    " << JInt("safein_dk_samples", arg_safein_dk_samples) << ",\n";
        f << "    " << JStr("safein_dk_samples_input", arg_safein_dk_samples_input) << ",\n";
        f << "    " << JStr("safein_dk_samples_output", arg_safein_dk_samples_output) << ",\n";
        f << "    " << JInt("safein_dk_samples_count", safein_dk_samples_count) << ",\n";
        f << "    " << JStr("safein_dk_sampling_mode", effective_safein_dk_sampling_mode) << ",\n";
        f << "    " << JInt("safein_dk_available_queries", safein_dk_available_queries) << ",\n";
        f << "    " << JInt("safein_dk_requested_samples", safein_dk_requested_samples) << ",\n";
        f << "    " << JStr("safein_dk_search_scope",
                             arg_safein_dk_search_scope == SafeInDkSearchScope::NProbe
                                 ? "nprobe"
                                 : "full") << ",\n";
        f << "    " << JInt("safein_dk_nprobe", arg_safein_dk_nprobe) << ",\n";
        f << "    " << JNum("safein_epsilon_percentile", arg_safein_epsilon_percentile) << ",\n";
        f << "    " << JNum("safein_epsilon_override_arg", arg_safein_epsilon_override) << ",\n";
        f << "    " << JNum("safeout_epsilon_percentile", arg_safeout_epsilon_percentile) << ",\n";
        f << "    " << JStr("epsilon_sampling_mode",
                             EpsilonSamplingModeName(arg_epsilon_sampling_mode)) << ",\n";
        f << "    " << JInt("epsilon_requested_samples", arg_epsilon_samples) << ",\n";
        f << "    " << JInt("safein_epsilon_valid_error_count",
                             safein_epsilon_stats.valid_error_count) << ",\n";
        f << "    " << JInt("safein_epsilon_attempted_pairs",
                             safein_epsilon_stats.attempted_pairs) << ",\n";
        f << "    " << JInt("safeout_epsilon_valid_error_count",
                             safeout_epsilon_stats.valid_error_count) << ",\n";
        f << "    " << JInt("safeout_epsilon_attempted_pairs",
                             safeout_epsilon_stats.attempted_pairs) << ",\n";
        f << "    " << JStr("coarse_builder", CoarseBuilderName(index.coarse_builder())) << ",\n";
        f << "    " << JStr("requested_metric", index.requested_metric()) << ",\n";
        f << "    " << JStr("effective_metric", index.effective_metric()) << ",\n";
        f << "    " << JStr("faiss_backend", "cpu") << ",\n";
        f << "    " << JStr("clustering_source", ClusteringSourceName(index.clustering_source())) << "\n";
        f << "  }\n";
        f << "}\n";
    }

    // results.json
    {
        std::ofstream f(output_dir + "/results.json");
        f << "{\n";

        // metrics
        f << "  \"metrics\": {\n";
        f << "    " << JNum("training_time_ms", training_time_ms) << ",\n";
        f << "    " << JNum("brute_force_time_ms", brute_force_time_ms) << ",\n";
        f << "    " << JStr("index_source", index_source) << ",\n";
        f << "    " << JStr("resolved_index_dir", resolved_index_dir) << ",\n";
        f << "    " << JStr("payload_mode", arg_payload_mode) << ",\n";
        f << "    " << JNum("payload_source_bytes",
                            static_cast<double>(
                                flatstor_payload_reader.total_payload_bytes)) << ",\n";
        f << "    " << JInt("exrabitq_storage_version",
                            index.segment().cluster_reader().file_version()) << ",\n";
        f << "    " << JStr("exrabitq_storage_format",
                            ExRaBitQStorageFormatName(
                                index.segment().cluster_reader().file_version())) << ",\n";
        f << "    " << JBool("exrabitq_stage2_magnitude_packed",
                              index.segment().cluster_reader().file_version() >= 12 &&
                              index.segment().rabitq_config().has_stage2_payload()) << ",\n";
        f << "    " << JStr("rabitq_estimator_mode",
                            std::string(RaBitQEstimatorModeName(
                                index.segment().rabitq_config().estimator_mode))) << ",\n";
        f << "    " << JInt("rabitq_total_bits",
                            index.segment().rabitq_config().effective_total_bits()) << ",\n";
        f << "    " << JInt("rabitq_ex_bits",
                            index.segment().rabitq_config().stage2_payload_bits()) << ",\n";
        f << "    " << JStr("rabitq_format_key",
                            RaBitQFormatKey(index.segment().rabitq_config())) << ",\n";
        f << "    " << JStr("rabitq_validation_mode",
                            RaBitQValidationModeName(arg_rabitq_validation_mode)) << ",\n";
        f << "    " << JNum("loaded_eps_ip", loaded_eps_ip) << ",\n";
        f << "    " << JNum("loaded_d_k", loaded_d_k) << ",\n";
        f << "    " << JNum("runtime_eps_ip", index.conann().epsilon()) << ",\n";
        f << "    " << JNum("runtime_d_k", index.conann().d_k()) << ",\n";
        f << "    \"runtime_safein_d_k\": ";
        if (runtime_safein_d_k_valid) {
            f << std::fixed << std::setprecision(4) << runtime_safein_d_k;
        } else {
            f << "null";
        }
        f << ",\n";
        f << "    " << JNum("runtime_safein_epsilon", runtime_safein_epsilon) << ",\n";
        f << "    " << JNum("runtime_safeout_epsilon", runtime_safeout_epsilon) << ",\n";
        f << "    " << JNum("safein_dk_percentile", arg_safein_dk_percentile) << ",\n";
        f << "    " << JInt("safein_dk_samples", arg_safein_dk_samples) << ",\n";
        f << "    " << JStr("safein_dk_samples_input", arg_safein_dk_samples_input) << ",\n";
        f << "    " << JStr("safein_dk_samples_output", arg_safein_dk_samples_output) << ",\n";
        f << "    " << JInt("safein_dk_samples_count", safein_dk_samples_count) << ",\n";
        f << "    " << JStr("safein_dk_sampling_mode", effective_safein_dk_sampling_mode) << ",\n";
        f << "    " << JInt("safein_dk_available_queries", safein_dk_available_queries) << ",\n";
        f << "    " << JInt("safein_dk_requested_samples", safein_dk_requested_samples) << ",\n";
        f << "    " << JStr("safein_dk_search_scope",
                            arg_safein_dk_search_scope == SafeInDkSearchScope::NProbe
                                ? "nprobe"
                                : "full") << ",\n";
        f << "    " << JInt("safein_dk_nprobe", arg_safein_dk_nprobe) << ",\n";
        f << "    " << JNum("safein_epsilon_percentile", arg_safein_epsilon_percentile) << ",\n";
        f << "    " << JNum("safein_epsilon_override_arg", arg_safein_epsilon_override) << ",\n";
        f << "    " << JNum("safeout_epsilon_percentile", arg_safeout_epsilon_percentile) << ",\n";
        f << "    " << JStr("epsilon_sampling_mode",
                            EpsilonSamplingModeName(arg_epsilon_sampling_mode)) << ",\n";
        f << "    " << JInt("epsilon_requested_samples", arg_epsilon_samples) << ",\n";
        f << "    " << JInt("safein_epsilon_valid_error_count",
                            safein_epsilon_stats.valid_error_count) << ",\n";
        f << "    " << JInt("safein_epsilon_attempted_pairs",
                            safein_epsilon_stats.attempted_pairs) << ",\n";
        f << "    " << JInt("safeout_epsilon_valid_error_count",
                            safeout_epsilon_stats.valid_error_count) << ",\n";
        f << "    " << JInt("safeout_epsilon_attempted_pairs",
                            safeout_epsilon_stats.attempted_pairs) << ",\n";
        f << "    " << JBool("recall_available", metrics.recall_available) << ",\n";
        f << "    " << JInt("logical_dimension", index.logical_dim()) << ",\n";
        f << "    " << JInt("effective_dimension", index.effective_dim()) << ",\n";
        f << "    " << JStr("padding_mode", index.padding_mode()) << ",\n";
        f << "    " << JStr("rotation_mode", index.rotation_mode()) << ",\n";
        f << "    " << JStr("gt_mode", gt_mode) << ",\n";
        f << "    " << JStr("gt_source", gt_source) << ",\n";
        f << "    " << JStr("gt_file", arg_gt_file) << ",\n";
        f << "    " << JNum("recall_at_1", metrics.recall_at[0]) << ",\n";
        f << "    " << JNum("recall_at_5", metrics.recall_at[1]) << ",\n";
        f << "    " << JNum("recall_at_10", metrics.recall_at[2]) << ",\n";
        f << "    " << JNum("recall_at_k", metrics.recall_at_k) << ",\n";
        f << "    " << JNum("avg_query_time_ms", metrics.avg_query_ms) << ",\n";
        f << "    " << JNum("p50_query_time_ms", metrics.p50) << ",\n";
        f << "    " << JNum("p95_query_time_ms", metrics.p95) << ",\n";
        f << "    " << JNum("p99_query_time_ms", metrics.p99) << ",\n";
        f << "    " << JNum("preload_time_ms", metrics.preload_time_ms) << ",\n";
        f << "    " << JNum("preload_bytes", metrics.preload_bytes) << ",\n";
        f << "    " << JStr("resident_preload_mode", index.segment().resident_preload_mode()) << ",\n";
        f << "    " << JNum("resident_preload_batch_size", metrics.resident_preload_batch_size) << ",\n";
        f << "    " << JNum("resident_file_size_bytes", metrics.resident_file_size_bytes) << ",\n";
        f << "    " << JNum("resident_file_buffer_bytes", metrics.resident_file_buffer_bytes) << ",\n";
        f << "    " << JNum("resident_code_storage_bytes", metrics.resident_code_storage_bytes) << ",\n";
        f << "    " << JNum("resident_decoded_address_bytes", metrics.resident_decoded_address_bytes) << ",\n";
        f << "    " << JNum("resident_raw_address_bytes", metrics.resident_raw_address_bytes) << ",\n";
        f << "    " << JNum("resident_parsed_address_duplicate_bytes", metrics.resident_parsed_address_duplicate_bytes) << ",\n";
        f << "    " << JNum("resident_cluster_mem_bytes", metrics.resident_cluster_mem_bytes) << ",\n";
        f << "    " << JNum("resident_parallel_view_build_ms", metrics.resident_parallel_view_build_ms) << ",\n";
        f << "    " << JNum("resident_parallel_view_bytes", metrics.resident_parallel_view_bytes) << ",\n";
        f << "    " << JNum("index_total_bytes", metrics.index_total_bytes) << ",\n";
        f << "    " << JNum("index_cluster_clu_bytes", metrics.index_cluster_clu_bytes) << ",\n";
        f << "    " << JNum("index_data_dat_bytes", metrics.index_data_dat_bytes) << ",\n";
        f << "    " << JNum("index_rotation_bytes", metrics.index_rotation_bytes) << ",\n";
        f << "    " << JNum("index_rotated_centroids_bytes", metrics.index_rotated_centroids_bytes) << ",\n";
        f << "    " << JInt("num_queries", Q) << "\n";
        f << "  },\n";

        // pipeline_stats
        f << "  \"pipeline_stats\": {\n";
        f << "    " << JNum("avg_total_probed", metrics.avg_probed) << ",\n";
        f << "    " << JNum("avg_safe_in", metrics.avg_safe_in) << ",\n";
        f << "    " << JNum("avg_safe_out", metrics.avg_safe_out) << ",\n";
        f << "    " << JNum("avg_uncertain", metrics.avg_uncertain) << ",\n";
        f << "    " << JNum("avg_s2_safe_in", metrics.avg_s2_safe_in) << ",\n";
        f << "    " << JNum("avg_s2_safe_out", metrics.avg_s2_safe_out) << ",\n";
        f << "    " << JNum("avg_s2_uncertain", metrics.avg_s2_uncertain) << ",\n";
        f << "    " << JNum("avg_duplicate_candidates", metrics.avg_duplicate_candidates) << ",\n";
        f << "    " << JNum("avg_deduplicated_candidates", metrics.avg_deduplicated_candidates) << ",\n";
        f << "    " << JNum("avg_unique_fetch_candidates", metrics.avg_unique_fetch_candidates) << ",\n";
        f << "    " << JNum("avg_candidate_budget_seen", metrics.avg_candidate_budget_seen) << ",\n";
        f << "    " << JNum("avg_candidate_budget_selected", metrics.avg_candidate_budget_selected) << ",\n";
        f << "    " << JNum("avg_candidate_budget_dropped", metrics.avg_candidate_budget_dropped) << ",\n";
        f << "    " << JNum("avg_false_safeout", metrics.avg_false_safeout) << ",\n";
        f << "    " << JNum("avg_false_safein_upper", metrics.avg_false_safein_upper) << ",\n";
        f << "    " << JInt("total_final_safein", static_cast<int64_t>(metrics.total_final_safein)) << ",\n";
        f << "    " << JNum("avg_io_wait_ms", metrics.avg_io_wait) << ",\n";
        f << "    " << JNum("avg_cpu_time_ms", metrics.avg_cpu) << ",\n";
        f << "    " << JNum("avg_coarse_select_ms", metrics.avg_coarse_select) << ",\n";
        f << "    " << JNum("avg_coarse_score_ms", metrics.avg_coarse_score) << ",\n";
        f << "    " << JNum("avg_coarse_topn_ms", metrics.avg_coarse_topn) << ",\n";
        f << "    " << JNum("avg_coarse_routing_mode", metrics.avg_coarse_routing_mode) << ",\n";
        f << "    " << JNum("avg_coarse_super_count", metrics.avg_coarse_super_count) << ",\n";
        f << "    " << JNum("avg_coarse_super_probes", metrics.avg_coarse_super_probes) << ",\n";
        f << "    " << JNum("avg_coarse_child_candidates_scored", metrics.avg_coarse_child_candidates_scored) << ",\n";
        f << "    " << JNum("avg_coarse_candidate_budget", metrics.avg_coarse_candidate_budget) << ",\n";
        f << "    " << JNum("avg_coarse_exact_fallback", metrics.avg_coarse_exact_fallback) << ",\n";
        f << "    " << JNum("avg_coarse_exact_overlap", metrics.avg_coarse_exact_overlap) << ",\n";
        f << "    " << JNum("avg_coarse_hierarchy_build_ms", metrics.avg_coarse_hierarchy_build_ms) << ",\n";
        f << "    " << JNum("avg_probe_time_ms", metrics.avg_probe) << ",\n";
        f << "    " << JNum("avg_probe_prepare_ms", metrics.avg_probe_prepare) << ",\n";
        f << "    " << JNum("avg_probe_prepare_rotation_ms", metrics.avg_probe_prepare_rotation) << ",\n";
        f << "    " << JNum("avg_probe_prepare_subtract_ms", metrics.avg_probe_prepare_subtract) << ",\n";
        f << "    " << JNum("avg_probe_prepare_normalize_ms", metrics.avg_probe_prepare_normalize) << ",\n";
        f << "    " << JNum("avg_probe_prepare_quantize_ms", metrics.avg_probe_prepare_quantize) << ",\n";
        f << "    " << JNum("avg_probe_prepare_lut_build_ms", metrics.avg_probe_prepare_lut_build) << ",\n";
        f << "    " << JNum("avg_probe_prepare_quant_lut_ms", metrics.avg_probe_prepare_quant_lut) << ",\n";
        f << "    " << JNum("avg_probe_stage1_ms", metrics.avg_probe_stage1) << ",\n";
        f << "    " << JNum("avg_probe_stage1_estimate_ms", metrics.avg_probe_stage1_estimate) << ",\n";
        f << "    " << JNum("avg_probe_stage1_mask_ms", metrics.avg_probe_stage1_mask) << ",\n";
        f << "    " << JNum("avg_probe_stage1_iterate_ms", metrics.avg_probe_stage1_iterate) << ",\n";
        f << "    " << JNum("avg_probe_stage1_classify_only_ms", metrics.avg_probe_stage1_classify_only) << ",\n";
        f << "    " << JNum("avg_probe_stage2_ms", metrics.avg_probe_stage2) << ",\n";
        f << "    " << JNum("avg_probe_stage2_collect_ms", metrics.avg_probe_stage2_collect) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_ms", metrics.avg_probe_stage2_kernel) << ",\n";
        f << "    " << JNum("avg_probe_stage2_scatter_ms", metrics.avg_probe_stage2_scatter) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_sign_flip_ms", metrics.avg_probe_stage2_kernel_sign_flip) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_abs_fma_ms", metrics.avg_probe_stage2_kernel_abs_fma) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_tail_ms", metrics.avg_probe_stage2_kernel_tail) << ",\n";
        f << "    " << JNum("avg_probe_stage2_kernel_reduce_ms", metrics.avg_probe_stage2_kernel_reduce) << ",\n";
        f << "    " << JNum("avg_probe_stage2_decode_ms", metrics.avg_probe_stage2_decode) << ",\n";
        f << "    " << JNum("avg_stage1_fused_blocks", metrics.avg_stage1_fused_blocks) << ",\n";
        f << "    " << JNum("avg_stage1_fused_safeout_lanes", metrics.avg_stage1_fused_safeout_lanes) << ",\n";
        f << "    " << JNum("avg_stage1_fused_safein_lanes", metrics.avg_stage1_fused_safein_lanes) << ",\n";
        f << "    " << JNum("avg_stage2_masked_kernel_calls", metrics.avg_stage2_masked_kernel_calls) << ",\n";
        f << "    " << JNum("avg_stage2_lanes_requested", metrics.avg_stage2_lanes_requested) << ",\n";
        f << "    " << JNum("avg_stage2_lanes_skipped", metrics.avg_stage2_lanes_skipped) << ",\n";
        f << "    " << JNum("avg_stage2_lanes_total_valid", metrics.avg_stage2_lanes_total_valid) << ",\n";
        f << "    " << JNum("avg_stage2_lane_density", metrics.avg_stage2_lane_density) << ",\n";
        f << "    " << JNum("avg_stage2_decode_blocks", metrics.avg_stage2_decode_blocks) << ",\n";
        f << "    " << JNum("avg_stage2_decode_input_bytes", metrics.avg_stage2_decode_input_bytes) << ",\n";
        f << "    " << JNum("avg_stage2_decode_output_bytes", metrics.avg_stage2_decode_output_bytes) << ",\n";
        f << "    " << JNum("avg_probe_classify_ms", metrics.avg_probe_classify) << ",\n";
        f << "    " << JNum("avg_probe_submit_ms", metrics.avg_probe_submit) << ",\n";
        f << "    " << JNum("avg_probe_submit_prepare_vec_only_ms", metrics.avg_probe_submit_prepare_vec_only) << ",\n";
        f << "    " << JNum("avg_probe_submit_prepare_all_ms", metrics.avg_probe_submit_prepare_all) << ",\n";
        f << "    " << JNum("avg_probe_submit_emit_ms", metrics.avg_probe_submit_emit) << ",\n";
        f << "    " << JNum("avg_probe_submit_vec_only_emit_ms", metrics.avg_probe_submit_vec_only_emit) << ",\n";
        f << "    " << JNum("avg_probe_submit_pending_slot_alloc_ms", metrics.avg_probe_submit_pending_slot_alloc) << ",\n";
        f << "    " << JNum("avg_probe_submit_prep_read_ms", metrics.avg_probe_submit_prep_read) << ",\n";
        f << "    " << JNum("avg_rerank_cpu_ms", metrics.avg_rerank_cpu) << ",\n";
        f << "    " << JNum("avg_safein_payload_prefetch_ms", metrics.avg_safein_payload_prefetch) << ",\n";
        f << "    " << JNum("avg_candidate_collect_ms", metrics.avg_candidate_collect) << ",\n";
        f << "    " << JNum("avg_pool_vector_read_ms", metrics.avg_pool_vector_read) << ",\n";
        f << "    " << JNum("avg_rerank_compute_ms", metrics.avg_rerank_compute) << ",\n";
        f << "    " << JNum("avg_rerank_vec_alloc_ms", metrics.avg_rerank_vec_alloc) << ",\n";
        f << "    " << JNum("avg_rerank_vec_copy_ms", metrics.avg_rerank_vec_copy) << ",\n";
        f << "    " << JNum("avg_remaining_payload_fetch_ms", metrics.avg_remaining_payload_fetch) << ",\n";
        f << "    " << JNum("avg_uring_prep_ms", metrics.avg_uring_prep) << ",\n";
        f << "    " << JNum("avg_uring_submit_ms", metrics.avg_uring_submit) << ",\n";
        f << "    " << JNum("avg_fetch_missing_ms", metrics.avg_fetch_missing) << ",\n";
        f << "    " << JNum("avg_submit_calls", metrics.avg_submit_calls) << ",\n";
        f << "    " << JNum("avg_submit_window_flushes", metrics.avg_submit_window_flushes) << ",\n";
        f << "    " << JNum("avg_submit_window_tail_flushes", metrics.avg_submit_window_tail_flushes) << ",\n";
        f << "    " << JNum("avg_submit_window_requests", metrics.avg_submit_window_requests) << ",\n";
        f << "    " << JNum("avg_vec_only_read_requests", metrics.avg_vec_only_read_requests) << ",\n";
        f << "    " << JNum("avg_all_read_requests", metrics.avg_all_read_requests) << ",\n";
        f << "    " << JNum("avg_payload_read_requests", metrics.avg_payload_read_requests) << ",\n";
        f << "    " << JNum("avg_fixed_vec_buffer_hits", metrics.avg_fixed_vec_buffer_hits) << ",\n";
        f << "    " << JNum("avg_fixed_vec_buffer_misses", metrics.avg_fixed_vec_buffer_misses) << ",\n";
        f << "    " << JNum("avg_probed_clusters", metrics.avg_probed_clusters) << ",\n";
        f << "    " << JNum("avg_candidate_batches_per_cluster", metrics.avg_candidate_batches_per_cluster) << ",\n";
        f << "    " << JNum("avg_safeout_frontier_estimates_buffered_per_cluster", metrics.avg_safeout_frontier_estimates_buffered_per_cluster) << ",\n";
        f << "    " << JNum("avg_safeout_frontier_estimates_merged_per_cluster", metrics.avg_safeout_frontier_estimates_merged_per_cluster) << ",\n";
        f << "    " << JNum("avg_safeout_frontier_updates_per_cluster", metrics.avg_safeout_frontier_updates_per_cluster) << ",\n";
        f << "    " << JNum("avg_stage2_block_lookups", metrics.avg_stage2_block_lookups) << ",\n";
        f << "    " << JNum("avg_stage2_block_reuses", metrics.avg_stage2_block_reuses) << ",\n";
        f << "    " << JNum("avg_safeout_frontier_buffer_ms", metrics.avg_safeout_frontier_buffer_ms) << ",\n";
        f << "    " << JNum("avg_safeout_frontier_merge_ms", metrics.avg_safeout_frontier_merge_ms) << ",\n";
        f << "    " << JNum("avg_safeout_frontier_online_ms", metrics.avg_safeout_frontier_online_ms) << ",\n";
        f << "    " << JNum("avg_dynamic_safein_clusters", metrics.avg_dynamic_safein_clusters) << ",\n";
        f << "    " << JNum("avg_dynamic_safein_active_clusters", metrics.avg_dynamic_safein_active_clusters) << ",\n";
        f << "    " << JNum("avg_dynamic_safein_disabled_clusters", metrics.avg_dynamic_safein_disabled_clusters) << ",\n";
        f << "    " << JNum("avg_dynamic_safein_threshold", metrics.avg_dynamic_safein_threshold) << ",\n";
        f << "    " << JNum("avg_dynamic_safein_final_frontier", metrics.avg_dynamic_safein_final_frontier) << ",\n";
        f << "    " << JNum("avg_dynamic_safein_deferred_candidates", metrics.avg_dynamic_safein_deferred_candidates) << ",\n";
        f << "    " << JNum("avg_dynamic_safein_deferred_flushes", metrics.avg_dynamic_safein_deferred_flushes) << ",\n";
        f << "    " << JNum("avg_dynamic_safein_deferred_safein", metrics.avg_dynamic_safein_deferred_safein) << ",\n";
        f << "    " << JNum("avg_safein_prefetch_candidates", metrics.avg_safein_prefetch_candidates) << ",\n";
        f << "    " << JNum("avg_safein_prefetch_true_topk", metrics.avg_safein_prefetch_true_topk) << ",\n";
        f << "    " << JNum("avg_safein_prefetch_false", metrics.avg_safein_prefetch_false) << ",\n";
        f << "    " << JNum("safein_prefetch_false_rate", metrics.safein_prefetch_false_rate) << ",\n";
        f << "    " << JNum("safein_prefetch_topk_coverage", metrics.safein_prefetch_topk_coverage) << ",\n";
        f << "    " << JNum("avg_candidates_buffered", metrics.avg_candidates_buffered) << ",\n";
        f << "    " << JNum("avg_candidates_reranked", metrics.avg_candidates_reranked) << ",\n";
        f << "    " << JNum("avg_safein_payload_prefetched", metrics.avg_safein_payload_prefetched) << ",\n";
        f << "    " << JNum("avg_remaining_payload_fetches", metrics.avg_remaining_payload_fetches) << ",\n";
        f << "    " << JNum("overlap_ratio", metrics.overlap_ratio) << "\n";
        f << "  },\n";

        // per_query_sample
        uint32_t stride = std::max(Q / 50, 1u);
        f << "  \"per_query_sample\": [\n";
        bool first = true;
        for (uint32_t qi = 0; qi < Q; qi += stride) {
            if (!first) f << ",\n";
            first = false;

            const auto& qr = qresults[qi];
            double r10 = metrics.recall_available
                ? ComputeRecallAtK(qr.predicted_ids, gt_topk[qi], 10)
                : 0.0;
            bool hit1 = metrics.recall_available &&
                        !qr.predicted_ids.empty() && !gt_topk[qi].empty() &&
                        qr.predicted_ids[0] == gt_topk[qi][0];
            double cpu_ms = qr.query_time_ms - qr.io_wait_ms;

            f << "    {\n";
            f << "      " << JInt("query_id", qr.query_id) << ",\n";
            f << "      " << JArr64("gt_top10_ids", metrics.recall_available ? gt_topk[qi] : std::vector<int64_t>{}) << ",\n";
            f << "      " << JArr64("predicted_top10_ids", qr.predicted_ids) << ",\n";
            f << "      " << JArrF("predicted_top10_distances", qr.predicted_dists) << ",\n";
            f << "      " << JBool("hit_at_1", hit1) << ",\n";
            f << "      " << JNum("recall_at_10", r10) << ",\n";
            f << "      " << JNum("query_time_ms", qr.query_time_ms) << ",\n";
            f << "      " << JNum("io_wait_ms", qr.io_wait_ms) << ",\n";
            f << "      " << JNum("cpu_time_ms", cpu_ms) << ",\n";
            f << "      " << JNum("coarse_select_ms", qr.coarse_select_ms) << ",\n";
            f << "      " << JNum("coarse_score_ms", qr.coarse_score_ms) << ",\n";
            f << "      " << JNum("coarse_topn_ms", qr.coarse_topn_ms) << ",\n";
            f << "      " << JInt("coarse_routing_mode", qr.coarse_routing_mode) << ",\n";
            f << "      " << JInt("coarse_super_count", qr.coarse_super_count) << ",\n";
            f << "      " << JInt("coarse_super_probes", qr.coarse_super_probes) << ",\n";
            f << "      " << JInt("coarse_child_candidates_scored", qr.coarse_child_candidates_scored) << ",\n";
            f << "      " << JInt("coarse_candidate_budget", qr.coarse_candidate_budget) << ",\n";
            f << "      " << JInt("coarse_exact_fallback", qr.coarse_exact_fallback) << ",\n";
            f << "      " << JInt("coarse_exact_overlap", qr.coarse_exact_overlap) << ",\n";
            f << "      " << JNum("coarse_hierarchy_build_ms", qr.coarse_hierarchy_build_ms) << ",\n";
            f << "      " << JNum("probe_ms", qr.probe_time_ms) << ",\n";
            f << "      " << JNum("probe_prepare_ms", qr.probe_prepare_ms) << ",\n";
            f << "      " << JNum("probe_prepare_rotation_ms", qr.probe_prepare_rotation_ms) << ",\n";
            f << "      " << JNum("probe_prepare_subtract_ms", qr.probe_prepare_subtract_ms) << ",\n";
            f << "      " << JNum("probe_prepare_normalize_ms", qr.probe_prepare_normalize_ms) << ",\n";
            f << "      " << JNum("probe_prepare_quantize_ms", qr.probe_prepare_quantize_ms) << ",\n";
            f << "      " << JNum("probe_prepare_lut_build_ms", qr.probe_prepare_lut_build_ms) << ",\n";
            f << "      " << JNum("probe_prepare_quant_lut_ms", qr.probe_prepare_quant_lut_ms) << ",\n";
            f << "      " << JNum("probe_stage1_ms", qr.probe_stage1_ms) << ",\n";
            f << "      " << JNum("probe_stage1_estimate_ms", qr.probe_stage1_estimate_ms) << ",\n";
            f << "      " << JNum("probe_stage1_mask_ms", qr.probe_stage1_mask_ms) << ",\n";
            f << "      " << JNum("probe_stage1_iterate_ms", qr.probe_stage1_iterate_ms) << ",\n";
            f << "      " << JNum("probe_stage1_classify_only_ms", qr.probe_stage1_classify_only_ms) << ",\n";
            f << "      " << JNum("probe_stage2_ms", qr.probe_stage2_ms) << ",\n";
            f << "      " << JNum("probe_stage2_collect_ms", qr.probe_stage2_collect_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_ms", qr.probe_stage2_kernel_ms) << ",\n";
            f << "      " << JNum("probe_stage2_scatter_ms", qr.probe_stage2_scatter_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_sign_flip_ms", qr.probe_stage2_kernel_sign_flip_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_abs_fma_ms", qr.probe_stage2_kernel_abs_fma_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_tail_ms", qr.probe_stage2_kernel_tail_ms) << ",\n";
            f << "      " << JNum("probe_stage2_kernel_reduce_ms", qr.probe_stage2_kernel_reduce_ms) << ",\n";
            f << "      " << JNum("probe_stage2_decode_ms", qr.probe_stage2_decode_ms) << ",\n";
            f << "      " << JNum("stage1_fused_blocks", qr.stage1_fused_blocks) << ",\n";
            f << "      " << JNum("stage1_fused_safeout_lanes", qr.stage1_fused_safeout_lanes) << ",\n";
            f << "      " << JNum("stage1_fused_safein_lanes", qr.stage1_fused_safein_lanes) << ",\n";
            f << "      " << JNum("stage2_masked_kernel_calls", qr.stage2_masked_kernel_calls) << ",\n";
            f << "      " << JNum("stage2_lanes_requested", qr.stage2_lanes_requested) << ",\n";
            f << "      " << JNum("stage2_lanes_skipped", qr.stage2_lanes_skipped) << ",\n";
            f << "      " << JNum("stage2_lanes_total_valid", qr.stage2_lanes_total_valid) << ",\n";
            f << "      " << JNum("stage2_decode_blocks", qr.stage2_decode_blocks) << ",\n";
            f << "      " << JNum("stage2_decode_input_bytes", qr.stage2_decode_input_bytes) << ",\n";
            f << "      " << JNum("stage2_decode_output_bytes", qr.stage2_decode_output_bytes) << ",\n";
            f << "      " << JNum("probe_classify_ms", qr.probe_classify_ms) << ",\n";
            f << "      " << JNum("probe_submit_ms", qr.probe_submit_ms) << ",\n";
            f << "      " << JNum("probe_submit_prepare_vec_only_ms", qr.probe_submit_prepare_vec_only_ms) << ",\n";
            f << "      " << JNum("probe_submit_prepare_all_ms", qr.probe_submit_prepare_all_ms) << ",\n";
            f << "      " << JNum("probe_submit_emit_ms", qr.probe_submit_emit_ms) << ",\n";
            f << "      " << JNum("probe_submit_vec_only_emit_ms", qr.probe_submit_vec_only_emit_ms) << ",\n";
            f << "      " << JNum("probe_submit_pending_slot_alloc_ms", qr.probe_submit_pending_slot_alloc_ms) << ",\n";
            f << "      " << JNum("probe_submit_prep_read_ms", qr.probe_submit_prep_read_ms) << ",\n";
            f << "      " << JNum("safein_payload_prefetch_ms", qr.safein_payload_prefetch_ms) << ",\n";
            f << "      " << JNum("candidate_collect_ms", qr.candidate_collect_ms) << ",\n";
            f << "      " << JNum("pool_vector_read_ms", qr.pool_vector_read_ms) << ",\n";
            f << "      " << JNum("rerank_compute_ms", qr.rerank_compute_ms) << ",\n";
            f << "      " << JNum("rerank_vec_alloc_ms", qr.rerank_vec_alloc_ms) << ",\n";
            f << "      " << JNum("rerank_vec_copy_ms", qr.rerank_vec_copy_ms) << ",\n";
            f << "      " << JNum("remaining_payload_fetch_ms", qr.remaining_payload_fetch_ms) << ",\n";
            f << "      " << JInt("num_candidates_buffered", qr.num_candidates_buffered) << ",\n";
            f << "      " << JInt("num_candidates_reranked", qr.num_candidates_reranked) << ",\n";
            f << "      " << JInt("candidate_budget_seen", qr.candidate_budget_seen) << ",\n";
            f << "      " << JInt("candidate_budget_selected", qr.candidate_budget_selected) << ",\n";
            f << "      " << JInt("candidate_budget_dropped", qr.candidate_budget_dropped) << ",\n";
            f << "      " << JInt("num_safein_payload_prefetched", qr.num_safein_payload_prefetched) << ",\n";
            f << "      " << JInt("num_remaining_payload_fetches", qr.num_remaining_payload_fetches) << ",\n";
            f << "      " << JInt("dynamic_safein_deferred_candidates", qr.dynamic_safein_deferred_candidates) << ",\n";
            f << "      " << JInt("dynamic_safein_deferred_flushes", qr.dynamic_safein_deferred_flushes) << ",\n";
            f << "      " << JInt("dynamic_safein_deferred_safein", qr.dynamic_safein_deferred_safein) << ",\n";
            f << "      " << JInt("safein_prefetch_candidates", qr.safein_prefetch_candidates) << ",\n";
            f << "      " << JInt("safein_prefetch_true_topk", qr.safein_prefetch_true_topk) << ",\n";
            f << "      " << JInt("safein_prefetch_false", qr.safein_prefetch_false) << ",\n";
            f << "      " << JInt("safein_prefetch_unknown", qr.safein_prefetch_unknown) << ",\n";
            f << "      " << JInt("submit_calls", qr.submit_calls) << ",\n";
            f << "      " << JInt("submit_window_flushes", qr.submit_window_flushes) << ",\n";
            f << "      " << JInt("submit_window_tail_flushes", qr.submit_window_tail_flushes) << ",\n";
            f << "      " << JInt("vec_only_read_requests", qr.vec_only_read_requests) << ",\n";
            f << "      " << JInt("all_read_requests", qr.all_read_requests) << ",\n";
            f << "      " << JInt("payload_read_requests", qr.payload_read_requests) << ",\n";
            f << "      " << JInt("fixed_vec_buffer_hits", qr.fixed_vec_buffer_hits) << ",\n";
            f << "      " << JInt("fixed_vec_buffer_misses", qr.fixed_vec_buffer_misses) << ",\n";
            f << "      " << JNum("submit_window_requests", qr.submit_window_requests) << ",\n";
            f << "      " << JNum("safeout_frontier_buffer_ms", qr.safeout_frontier_buffer_ms) << ",\n";
            f << "      " << JNum("safeout_frontier_merge_ms", qr.safeout_frontier_merge_ms) << ",\n";
            f << "      " << JNum("safeout_frontier_online_ms", qr.safeout_frontier_online_ms) << ",\n";
            f << "      " << JNum("stage2_block_lookups", qr.stage2_block_lookups) << ",\n";
            f << "      " << JNum("stage2_block_reuses", qr.stage2_block_reuses) << ",\n";
            f << "      " << JInt("probed_clusters", qr.probed_clusters) << "\n";
            f << "    }";
        }
        f << "\n  ]\n";
        f << "}\n";
    }

    Log("  Output: %s\n", output_dir.c_str());

    return 0;
}
