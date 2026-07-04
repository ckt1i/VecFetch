#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "rabitq_bench_calibration.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/npy_reader.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"
#include "vdb/query/search_results.h"
#include "vdb/storage/cluster_store.h"

namespace fs = std::filesystem;

using vdb::ClusterID;
using vdb::DType;
using vdb::Dim;
using vdb::Status;
using vdb::StatusOr;
using vdb::index::IvfIndex;
using vdb::query::DynamicSafeInMode;
using vdb::query::IoUringReader;
using vdb::query::OverlapScheduler;
using vdb::query::SeparateRecordMap;
using vdb::query::SeparateRecordLocation;
using vdb::query::SearchConfig;
using vdb::query::SearchResults;
using vdb::query::SubmissionMode;
using vdb::bench::BuildClusterMembers;
using vdb::bench::CalibrateSplitEpsilon;
using vdb::bench::EncodeAllCodes;
using vdb::bench::EpsilonCalibrationStats;
using vdb::bench::EpsilonSamplingMode;
using vdb::bench::EpsilonSamplingModeName;
using vdb::bench::LoadAssignments;
using vdb::bench::ParseEpsilonSamplingModeArg;
using vdb::bench::RecoverClusterMembersFromIndex;

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static bool HasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

static std::string GetStringArg(int argc, char** argv, const char* key,
                                const std::string& def = "") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return def;
}

static int GetIntArg(int argc, char** argv, const char* key, int def = 0) {
    std::string v = GetStringArg(argc, argv, key, "");
    if (v.empty()) return def;
    return std::atoi(v.c_str());
}

static int64_t GetInt64Arg(int argc, char** argv, const char* key,
                           int64_t def = 0) {
    std::string v = GetStringArg(argc, argv, key, "");
    if (v.empty()) return def;
    return std::atoll(v.c_str());
}

static double GetDoubleArg(int argc, char** argv, const char* key,
                           double def = 0.0) {
    std::string v = GetStringArg(argc, argv, key, "");
    if (v.empty()) return def;
    return std::atof(v.c_str());
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

static std::string JStr(const std::string& k, const std::string& v) {
    return "\"" + k + "\": \"" + JsonEscape(v) + "\"";
}

static std::string JNum(const std::string& k, double v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return "\"" + k + "\": " + buf;
}

static std::string JInt(const std::string& k, int64_t v) {
    return "\"" + k + "\": " + std::to_string(v);
}

static std::string JBool(const std::string& k, bool v) {
    return "\"" + k + "\": " + (v ? "true" : "false");
}

static bool ReadFloatCache(const std::string& path, float* out) {
    if (path.empty() || out == nullptr || !fs::exists(path)) return false;
    std::ifstream f(path);
    if (!f.is_open()) return false;
    float value = -1.0f;
    f >> value;
    if (!f.good() && !f.eof()) return false;
    if (value < 0.0f) return false;
    *out = value;
    return true;
}

static bool WriteFloatCache(const std::string& path, float value) {
    if (path.empty()) return true;
    const fs::path p(path);
    if (!p.parent_path().empty()) {
        fs::create_directories(p.parent_path());
    }
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << value << "\n";
    return f.good();
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

static bool ValidateRaBitQMode(const vdb::RaBitQConfig& config,
                               RaBitQValidationMode requested,
                               const char* flag_name) {
    if (requested == RaBitQValidationMode::Auto) return true;
    const bool official = config.uses_official_1_plus_n();
    if (requested == RaBitQValidationMode::Official && !official) {
        std::fprintf(stderr,
                     "%s=official_1_plus_n requested, but index is %s\n",
                     flag_name,
                     std::string(vdb::RaBitQEstimatorModeName(
                         config.estimator_mode)).c_str());
        return false;
    }
    if (requested == RaBitQValidationMode::Legacy && official) {
        std::fprintf(stderr,
                     "%s=legacy_signed_magnitude requested, but index is %s\n",
                     flag_name,
                     std::string(vdb::RaBitQEstimatorModeName(
                         config.estimator_mode)).c_str());
        return false;
    }
    return true;
}

struct SeparateStoreMapHeader {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint64_t count;
    uint32_t vec_bytes;
    uint32_t reserved;
};

struct SeparateStoreMapRecord {
    uint64_t combined_offset;
    uint64_t row_id;
    uint64_t payload_offset;
    uint32_t payload_bytes;
    uint32_t reserved;
};

static_assert(sizeof(SeparateStoreMapHeader) == 32,
              "Unexpected SeparateStoreMapHeader layout");
static_assert(sizeof(SeparateStoreMapRecord) == 32,
              "Unexpected SeparateStoreMapRecord layout");

struct SeparateStoreMapData {
    SeparateRecordMap records;
    uint64_t record_count = 0;
    uint32_t vec_bytes = 0;
};

static StatusOr<SeparateStoreMapData> LoadSeparateStoreMap(
    const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Status::IOError("Failed to open separate-store map: " + path);
    }

    SeparateStoreMapHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f.good()) {
        return Status::IOError("Failed to read separate-store map header: " +
                               path);
    }
    const char expected_magic[8] = {'N', 'C', 'M', 'B', 'M', 'A', 'P', '1'};
    if (std::memcmp(hdr.magic, expected_magic, sizeof(expected_magic)) != 0) {
        return Status::InvalidArgument(
            "Invalid separate-store map magic: " + path);
    }
    if (hdr.version != 1 ||
        hdr.record_size != sizeof(SeparateStoreMapRecord)) {
        return Status::InvalidArgument(
            "Unsupported separate-store map version/record size: " + path);
    }

    SeparateStoreMapData out;
    out.record_count = hdr.count;
    out.vec_bytes = hdr.vec_bytes;
    out.records.reserve(static_cast<size_t>(hdr.count));
    for (uint64_t i = 0; i < hdr.count; ++i) {
        SeparateStoreMapRecord rec{};
        f.read(reinterpret_cast<char*>(&rec), sizeof(rec));
        if (!f.good()) {
            return Status::IOError("Truncated separate-store map: " + path);
        }
        SeparateRecordLocation loc;
        loc.row_id = rec.row_id;
        loc.payload_offset = rec.payload_offset;
        loc.payload_bytes = rec.payload_bytes;
        out.records.emplace(rec.combined_offset, loc);
    }
    return out;
}

struct ProcRss {
    int64_t rss_kib = 0;
    int64_t hwm_kib = 0;
};

static ProcRss ReadProcRss() {
    std::ifstream f("/proc/self/status");
    ProcRss out;
    std::string key;
    while (f >> key) {
        if (key == "VmRSS:") {
            f >> out.rss_kib;
        } else if (key == "VmHWM:") {
            f >> out.hwm_kib;
        }
        std::string rest;
        std::getline(f, rest);
    }
    return out;
}

struct SmapsRollupStats {
    int64_t anon_huge_pages_kib = 0;
    int64_t file_pmd_mapped_kib = 0;
};

static SmapsRollupStats ReadSmapsRollupStats() {
    std::ifstream f("/proc/self/smaps_rollup");
    SmapsRollupStats out;
    std::string key;
    while (f >> key) {
        if (key == "AnonHugePages:") {
            f >> out.anon_huge_pages_kib;
        } else if (key == "FilePmdMapped:") {
            f >> out.file_pmd_mapped_kib;
        }
        std::string rest;
        std::getline(f, rest);
    }
    return out;
}

struct NpyHeader {
    std::string descr;
    bool fortran_order = false;
    std::vector<uint64_t> shape;
    uint64_t data_offset = 0;
};

static uint16_t ReadLe16(const char* p) {
    return static_cast<uint16_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint16_t>(static_cast<unsigned char>(p[1])) << 8);
}

static uint32_t ReadLe32(const char* p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

static std::string ExtractQuotedValue(const std::string& h,
                                      const std::string& key) {
    const size_t key_pos = h.find(key);
    if (key_pos == std::string::npos) return "";
    size_t colon = h.find(':', key_pos);
    if (colon == std::string::npos) return "";
    size_t quote = h.find_first_of("'\"", colon + 1);
    if (quote == std::string::npos) return "";
    const char q = h[quote];
    size_t end = h.find(q, quote + 1);
    if (end == std::string::npos) return "";
    return h.substr(quote + 1, end - quote - 1);
}

static StatusOr<NpyHeader> ParseNpyHeader(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Status::IOError("Failed to open npy file: " + path);
    }

    char magic[6];
    f.read(magic, sizeof(magic));
    if (!f.good() || std::memcmp(magic, "\x93NUMPY", 6) != 0) {
        return Status::InvalidArgument("Invalid npy magic: " + path);
    }
    char version[2];
    f.read(version, sizeof(version));
    if (!f.good()) {
        return Status::IOError("Failed to read npy version: " + path);
    }

    uint32_t header_len = 0;
    if (version[0] == 1) {
        char len_buf[2];
        f.read(len_buf, sizeof(len_buf));
        if (!f.good()) {
            return Status::IOError("Failed to read npy v1 header length: " + path);
        }
        header_len = ReadLe16(len_buf);
    } else if (version[0] == 2 || version[0] == 3) {
        char len_buf[4];
        f.read(len_buf, sizeof(len_buf));
        if (!f.good()) {
            return Status::IOError("Failed to read npy v2/v3 header length: " + path);
        }
        header_len = ReadLe32(len_buf);
    } else {
        return Status::InvalidArgument("Unsupported npy version in: " + path);
    }

    std::string header(header_len, '\0');
    f.read(header.data(), static_cast<std::streamsize>(header_len));
    if (!f.good()) {
        return Status::IOError("Failed to read npy header: " + path);
    }

    NpyHeader out;
    out.descr = ExtractQuotedValue(header, "descr");
    out.fortran_order = (header.find("True") != std::string::npos &&
                         header.find("fortran_order") != std::string::npos);
    const size_t shape_key = header.find("shape");
    const size_t lparen = header.find('(', shape_key);
    const size_t rparen = header.find(')', lparen);
    if (shape_key == std::string::npos || lparen == std::string::npos ||
        rparen == std::string::npos || rparen <= lparen) {
        return Status::InvalidArgument("Failed to parse npy shape: " + path);
    }
    std::string shape = header.substr(lparen + 1, rparen - lparen - 1);
    std::stringstream ss(shape);
    while (ss.good()) {
        while (ss.peek() == ' ' || ss.peek() == ',') ss.get();
        if (!std::isdigit(ss.peek())) {
            ss.get();
            continue;
        }
        uint64_t dim = 0;
        ss >> dim;
        out.shape.push_back(dim);
        while (ss.peek() == ' ' || ss.peek() == ',') ss.get();
    }
    if (out.shape.empty()) {
        return Status::InvalidArgument("Empty npy shape: " + path);
    }
    out.data_offset = static_cast<uint64_t>(f.tellg());
    return out;
}

static bool LoadClusterMembersCache(
    const std::string& path,
    uint32_t nlist,
    std::vector<std::vector<uint32_t>>* cluster_members) {
    if (path.empty() || cluster_members == nullptr || !fs::exists(path)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) return false;

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t file_nlist = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&file_nlist), sizeof(file_nlist));
    if (!in || magic != 0x4d434752u || version != 1 || file_nlist != nlist) {
        return false;
    }

    std::vector<std::vector<uint32_t>> loaded(nlist);
    for (uint32_t cid = 0; cid < nlist; ++cid) {
        uint32_t count = 0;
        in.read(reinterpret_cast<char*>(&count), sizeof(count));
        if (!in) return false;
        loaded[cid].resize(count);
        if (count > 0) {
            in.read(reinterpret_cast<char*>(loaded[cid].data()),
                    static_cast<std::streamsize>(sizeof(uint32_t) * count));
            if (!in) return false;
        }
    }
    *cluster_members = std::move(loaded);
    return true;
}

static bool SaveClusterMembersCache(
    const std::string& path,
    const std::vector<std::vector<uint32_t>>& cluster_members) {
    if (path.empty()) return true;
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    const uint32_t magic = 0x4d434752u;  // "RGCM" little-endian
    const uint32_t version = 1;
    const uint32_t nlist = static_cast<uint32_t>(cluster_members.size());
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&nlist), sizeof(nlist));
    for (const auto& members : cluster_members) {
        const uint32_t count = static_cast<uint32_t>(members.size());
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        if (count > 0) {
            out.write(reinterpret_cast<const char*>(members.data()),
                      static_cast<std::streamsize>(sizeof(uint32_t) * count));
        }
    }
    return static_cast<bool>(out);
}

struct FloatRows {
    std::vector<float> data;
    uint32_t rows = 0;
    uint32_t cols = 0;
};

static StatusOr<FloatRows> LoadNpyFloat32Rows(const std::string& path,
                                              uint64_t row_offset,
                                              uint64_t row_count) {
    auto hdr_or = ParseNpyHeader(path);
    if (!hdr_or.ok()) return hdr_or.status();
    const auto& hdr = hdr_or.value();
    if (hdr.descr != "<f4" && hdr.descr != "=f4") {
        return Status::InvalidArgument("Expected float32 query npy: " + path);
    }
    if (hdr.fortran_order) {
        return Status::InvalidArgument("Fortran-order npy unsupported: " + path);
    }
    const uint64_t rows = hdr.shape[0];
    const uint64_t cols = hdr.shape.size() >= 2 ? hdr.shape[1] : 1;
    if (row_offset > rows) {
        return Status::InvalidArgument("query offset exceeds rows: " + path);
    }
    if (row_count == 0 || row_offset + row_count > rows) {
        row_count = rows - row_offset;
    }
    if (row_count > static_cast<uint64_t>(UINT32_MAX) ||
        cols > static_cast<uint64_t>(UINT32_MAX)) {
        return Status::InvalidArgument("query slice too large: " + path);
    }

    FloatRows out;
    out.rows = static_cast<uint32_t>(row_count);
    out.cols = static_cast<uint32_t>(cols);
    out.data.resize(static_cast<size_t>(row_count) * static_cast<size_t>(cols));

    std::ifstream f(path, std::ios::binary);
    const uint64_t byte_offset =
        hdr.data_offset + row_offset * cols * sizeof(float);
    f.seekg(static_cast<std::streamoff>(byte_offset));
    f.read(reinterpret_cast<char*>(out.data.data()),
           static_cast<std::streamsize>(out.data.size() * sizeof(float)));
    if (!f.good()) {
        return Status::IOError("Failed to read query rows from: " + path);
    }
    return out;
}

static StatusOr<std::vector<int64_t>> LoadNpyInt64VectorRows(
    const std::string& path, uint64_t row_offset, uint64_t row_count) {
    auto hdr_or = ParseNpyHeader(path);
    if (!hdr_or.ok()) return hdr_or.status();
    const auto& hdr = hdr_or.value();
    if (hdr.descr != "<i8" && hdr.descr != "=i8") {
        return Status::InvalidArgument("Expected int64 npy: " + path);
    }
    if (hdr.fortran_order || hdr.shape.size() != 1) {
        return Status::InvalidArgument("Expected 1-D C-order int64 npy: " + path);
    }
    const uint64_t rows = hdr.shape[0];
    if (row_offset > rows) {
        return Status::InvalidArgument("id offset exceeds rows: " + path);
    }
    if (row_count == 0 || row_offset + row_count > rows) {
        row_count = rows - row_offset;
    }
    std::vector<int64_t> out(row_count);
    std::ifstream f(path, std::ios::binary);
    const uint64_t byte_offset = hdr.data_offset + row_offset * sizeof(int64_t);
    f.seekg(static_cast<std::streamoff>(byte_offset));
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(out.size() * sizeof(int64_t)));
    if (!f.good()) {
        return Status::IOError("Failed to read int64 rows from: " + path);
    }
    return out;
}

static StatusOr<std::vector<std::vector<int64_t>>> LoadNpyGroundTruthRows(
    const std::string& path, uint64_t row_offset, uint64_t row_count,
    uint32_t topk) {
    auto hdr_or = ParseNpyHeader(path);
    if (!hdr_or.ok()) return hdr_or.status();
    const auto& hdr = hdr_or.value();
    const bool is_i64 = (hdr.descr == "<i8" || hdr.descr == "=i8");
    const bool is_i32 = (hdr.descr == "<i4" || hdr.descr == "=i4");
    if (!is_i64 && !is_i32) {
        return Status::InvalidArgument("Expected int32/int64 GT npy: " + path);
    }
    if (hdr.fortran_order || hdr.shape.size() < 2) {
        return Status::InvalidArgument("Expected 2-D C-order GT npy: " + path);
    }
    const uint64_t rows = hdr.shape[0];
    const uint64_t cols = hdr.shape[1];
    if (cols < topk) {
        return Status::InvalidArgument("GT top-k smaller than requested topk: " + path);
    }
    if (row_offset > rows) {
        return Status::InvalidArgument("GT offset exceeds rows: " + path);
    }
    if (row_count == 0 || row_offset + row_count > rows) {
        row_count = rows - row_offset;
    }

    std::vector<std::vector<int64_t>> out(row_count, std::vector<int64_t>(topk));
    std::ifstream f(path, std::ios::binary);
    const uint64_t elem_bytes = is_i64 ? sizeof(int64_t) : sizeof(int32_t);
    std::vector<char> row_buf(static_cast<size_t>(cols) * elem_bytes);
    for (uint64_t r = 0; r < row_count; ++r) {
        const uint64_t byte_offset =
            hdr.data_offset + (row_offset + r) * cols * elem_bytes;
        f.seekg(static_cast<std::streamoff>(byte_offset));
        f.read(row_buf.data(), static_cast<std::streamsize>(row_buf.size()));
        if (!f.good()) {
            return Status::IOError("Failed to read GT row from: " + path);
        }
        for (uint32_t k = 0; k < topk; ++k) {
            if (is_i64) {
                const auto* p = reinterpret_cast<const int64_t*>(row_buf.data());
                out[r][k] = p[k];
            } else {
                const auto* p = reinterpret_cast<const int32_t*>(row_buf.data());
                out[r][k] = static_cast<int64_t>(p[k]);
            }
        }
    }
    return out;
}

static double Percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double idx = p * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, values.size() - 1);
    const double frac = idx - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

static double ComputeRecallAtK(const std::vector<int64_t>& predicted,
                               const std::vector<int64_t>& gt,
                               uint32_t k) {
    if (gt.empty()) return 0.0;
    const uint32_t pk = std::min(k, static_cast<uint32_t>(predicted.size()));
    const uint32_t gk = std::min(k, static_cast<uint32_t>(gt.size()));
    std::unordered_set<int64_t> gt_set(gt.begin(), gt.begin() + gk);
    uint32_t hits = 0;
    for (uint32_t i = 0; i < pk; ++i) {
        if (gt_set.count(predicted[i])) ++hits;
    }
    return static_cast<double>(hits) / static_cast<double>(gk);
}

static const char* ExRaBitQStorageFormatName(uint32_t version) {
    if (version >= 14) return "official_1_plus_n_direct_compact_exdata";
    if (version >= 13) return "official_1_plus_n_packed_exdata";
    if (version >= 12) return "compact_blocked_packed_magnitude";
    if (version >= 11) return "compact_blocked";
    if (version >= 10) return "packed_sign";
    return "legacy_byte_sign";
}

struct QueryMetrics {
    std::vector<double> latencies_ms;
    double recall1_sum = 0.0;
    double recall5_sum = 0.0;
    double recall10_sum = 0.0;
    double recallk_sum = 0.0;
    bool recall_available = false;
    uint64_t total_probed = 0;
    uint64_t total_safe_in = 0;
    uint64_t total_safe_out = 0;
    uint64_t total_uncertain = 0;
    uint64_t s2_safe_in = 0;
    uint64_t s2_safe_out = 0;
    uint64_t s2_uncertain = 0;
    uint64_t candidates_reranked = 0;
    uint64_t vec_only_reads = 0;
    uint64_t all_reads = 0;
    uint64_t payload_reads = 0;
    uint64_t vec_only_read_bytes = 0;
    uint64_t all_read_bytes = 0;
    uint64_t payload_read_bytes = 0;
    uint64_t budgeted_prefetch_considered = 0;
    uint64_t budgeted_prefetch_scheduled = 0;
    uint64_t budgeted_prefetch_duplicates = 0;
    uint64_t budgeted_prefetch_skipped_limit = 0;
    uint64_t budgeted_prefetch_completed = 0;
    uint64_t budgeted_prefetch_cache_hits = 0;
    uint64_t budgeted_prefetch_inflight_uses = 0;
    uint64_t budgeted_prefetch_used = 0;
    uint64_t budgeted_prefetch_wasted = 0;
    uint64_t budgeted_prefetch_read_bytes = 0;
    uint64_t safein_prefetch_considered = 0;
    uint64_t safein_prefetch_candidates = 0;
    uint64_t safein_prefetch_true_topk = 0;
    uint64_t safein_prefetch_false = 0;
    uint64_t safein_prefetch_unknown = 0;
    uint64_t safein_prefetch_skipped_count_limit = 0;
    uint64_t safein_prefetch_skipped_byte_limit = 0;
    uint64_t safein_prefetch_scheduled_bytes = 0;
    uint64_t dynamic_safein_clusters = 0;
    uint64_t dynamic_safein_active_clusters = 0;
    uint64_t dynamic_safein_disabled_clusters = 0;
    uint64_t dynamic_safein_ready_transitions = 0;
    uint64_t dynamic_safein_deferred_candidates = 0;
    uint64_t dynamic_safein_deferred_flushes = 0;
    uint64_t dynamic_safein_deferred_safein = 0;
    uint64_t candidate_budget_seen = 0;
    uint64_t candidate_budget_selected = 0;
    uint64_t candidate_budget_dropped = 0;
    uint64_t unique_fetch_candidates = 0;
    uint64_t total_io_submitted = 0;
    uint64_t total_submit_calls = 0;
    uint64_t total_submit_window_flushes = 0;
    uint64_t total_submit_window_requests = 0;
    uint64_t separate_store_lookup_misses = 0;
    uint64_t stage2_lanes_requested = 0;
    uint64_t stage2_decode_blocks = 0;
    uint64_t stage2_decode_input_bytes = 0;
    uint64_t stage2_decode_output_bytes = 0;
    uint64_t stage2_active_ex_bits_sum = 0;
    uint64_t stage2_stored_ex_bits_sum = 0;
    double probe_ms = 0.0;
    double coarse_select_ms = 0.0;
    double coarse_score_ms = 0.0;
    double coarse_topn_ms = 0.0;
    uint64_t coarse_routing_mode = 0;
    uint64_t coarse_super_count = 0;
    uint64_t coarse_super_probes = 0;
    uint64_t coarse_child_candidates_scored = 0;
    uint64_t coarse_candidate_budget = 0;
    uint64_t coarse_exact_fallback = 0;
    uint64_t coarse_exact_overlap = 0;
    double coarse_hierarchy_build_ms = 0.0;
    double stage1_ms = 0.0;
    double stage2_ms = 0.0;
    double stage2_decode_ms = 0.0;
    double submit_ms = 0.0;
    double rerank_compute_ms = 0.0;
    double fetch_missing_ms = 0.0;
    double io_wait_ms = 0.0;
    double final_drain_ms = 0.0;
    double execute_buffered_ms = 0.0;
    double collector_finalize_ms = 0.0;
    double assemble_results_ms = 0.0;
    double search_unaccounted_ms = 0.0;
};

static void Accumulate(QueryMetrics& m, const SearchResults& results,
                       const std::vector<int64_t>* gt, uint32_t topk) {
    const auto& st = results.stats();
    m.latencies_ms.push_back(st.total_time_ms);
    m.total_probed += st.total_probed;
    m.total_safe_in += st.total_safe_in;
    m.total_safe_out += st.total_safe_out;
    m.total_uncertain += st.total_uncertain;
    m.s2_safe_in += st.s2_safe_in;
    m.s2_safe_out += st.s2_safe_out;
    m.s2_uncertain += st.s2_uncertain;
    m.candidates_reranked += st.reranked_candidates;
    m.vec_only_reads += st.vec_only_read_requests;
    m.all_reads += st.all_read_requests;
    m.payload_reads += st.payload_read_requests;
    m.vec_only_read_bytes += st.vec_only_read_bytes;
    m.all_read_bytes += st.all_read_bytes;
    m.payload_read_bytes += st.payload_read_bytes;
    m.budgeted_prefetch_considered += st.budgeted_prefetch_considered;
    m.budgeted_prefetch_scheduled += st.budgeted_prefetch_scheduled;
    m.budgeted_prefetch_duplicates += st.budgeted_prefetch_duplicates;
    m.budgeted_prefetch_skipped_limit += st.budgeted_prefetch_skipped_limit;
    m.budgeted_prefetch_completed += st.budgeted_prefetch_completed;
    m.budgeted_prefetch_cache_hits += st.budgeted_prefetch_cache_hits;
    m.budgeted_prefetch_inflight_uses += st.budgeted_prefetch_inflight_uses;
    m.budgeted_prefetch_used += st.budgeted_prefetch_used;
    m.budgeted_prefetch_wasted += st.budgeted_prefetch_wasted;
    m.budgeted_prefetch_read_bytes += st.budgeted_prefetch_read_bytes;
    m.safein_prefetch_considered += st.safein_prefetch_considered;
    m.safein_prefetch_candidates += st.safein_prefetch_candidates;
    m.safein_prefetch_true_topk += st.safein_prefetch_true_topk;
    m.safein_prefetch_false += st.safein_prefetch_false;
    m.safein_prefetch_unknown += st.safein_prefetch_unknown;
    m.safein_prefetch_skipped_count_limit +=
        st.safein_prefetch_skipped_count_limit;
    m.safein_prefetch_skipped_byte_limit +=
        st.safein_prefetch_skipped_byte_limit;
    m.safein_prefetch_scheduled_bytes += st.safein_prefetch_scheduled_bytes;
    m.dynamic_safein_clusters += st.dynamic_safein_clusters;
    m.dynamic_safein_active_clusters += st.dynamic_safein_active_clusters;
    m.dynamic_safein_disabled_clusters += st.dynamic_safein_disabled_clusters;
    m.dynamic_safein_ready_transitions += st.dynamic_safein_ready_transitions;
    m.dynamic_safein_deferred_candidates += st.dynamic_safein_deferred_candidates;
    m.dynamic_safein_deferred_flushes += st.dynamic_safein_deferred_flushes;
    m.dynamic_safein_deferred_safein += st.dynamic_safein_deferred_safein;
    m.candidate_budget_seen += st.candidate_budget_seen;
    m.candidate_budget_selected += st.candidate_budget_selected;
    m.candidate_budget_dropped += st.candidate_budget_dropped;
    m.unique_fetch_candidates += st.unique_fetch_candidates;
    m.total_io_submitted += st.total_io_submitted;
    m.total_submit_calls += st.total_submit_calls;
    m.total_submit_window_flushes += st.total_submit_window_flushes;
    m.total_submit_window_requests += st.total_submit_window_requests;
    m.separate_store_lookup_misses += st.separate_store_lookup_misses;
    m.stage2_lanes_requested += st.stage2_lanes_requested;
    m.stage2_decode_blocks += st.stage2_decode_blocks;
    m.stage2_decode_input_bytes += st.stage2_decode_input_bytes;
    m.stage2_decode_output_bytes += st.stage2_decode_output_bytes;
    m.stage2_active_ex_bits_sum += st.stage2_active_ex_bits_sum;
    m.stage2_stored_ex_bits_sum += st.stage2_stored_ex_bits_sum;
    m.probe_ms += st.probe_time_ms;
    m.coarse_select_ms += st.coarse_select_ms;
    m.coarse_score_ms += st.coarse_score_ms;
    m.coarse_topn_ms += st.coarse_topn_ms;
    m.coarse_routing_mode += st.coarse_routing_mode;
    m.coarse_super_count += st.coarse_super_count;
    m.coarse_super_probes += st.coarse_super_probes;
    m.coarse_child_candidates_scored += st.coarse_child_candidates_scored;
    m.coarse_candidate_budget += st.coarse_candidate_budget;
    m.coarse_exact_fallback += st.coarse_exact_fallback;
    m.coarse_exact_overlap += st.coarse_exact_overlap;
    m.coarse_hierarchy_build_ms += st.coarse_hierarchy_build_ms;
    m.stage1_ms += st.probe_stage1_ms;
    m.stage2_ms += st.probe_stage2_ms;
    m.stage2_decode_ms += st.probe_stage2_decode_ms;
    m.submit_ms += st.probe_submit_ms;
    m.rerank_compute_ms += st.rerank_compute_ms;
    m.fetch_missing_ms += st.fetch_missing_ms;
    m.io_wait_ms += st.io_wait_time_ms;
    m.final_drain_ms += st.final_drain_ms;
    m.execute_buffered_ms += st.execute_buffered_ms;
    m.collector_finalize_ms += st.collector_finalize_ms;
    m.assemble_results_ms += st.assemble_results_ms;
    m.search_unaccounted_ms += st.search_unaccounted_ms;

    if (gt != nullptr) {
        std::vector<int64_t> predicted;
        predicted.reserve(results.size());
        for (uint32_t i = 0; i < results.size(); ++i) {
            const auto& payload = results[i].payload;
            if (!payload.empty() && payload[0].dtype == DType::INT64) {
                predicted.push_back(payload[0].fixed.i64);
            }
        }
        m.recall_available = true;
        m.recall1_sum += ComputeRecallAtK(predicted, *gt, 1);
        m.recall5_sum += ComputeRecallAtK(predicted, *gt, std::min<uint32_t>(5, topk));
        m.recall10_sum += ComputeRecallAtK(predicted, *gt, std::min<uint32_t>(10, topk));
        m.recallk_sum += ComputeRecallAtK(predicted, *gt, topk);
    }
}

static int Usage() {
    std::fprintf(stderr,
        "Usage: bench_e2e --index-dir DIR --query-file queries.npy [options]\n"
        "\n"
        "Online query benchmark. It does not load image_embeddings.npy or metadata.jsonl.\n"
        "\n"
        "Required:\n"
        "  --index-dir DIR             Existing index containing cluster.clu and data.dat\n"
        "  --query-file FILE.npy       Float32 query matrix\n"
        "  --dataset DIR               Dataset dir for runtime epsilon calibration\n"
        "  --assignments FILE.ivecs    Cluster assignments for calibration\n"
        "\n"
        "Common options:\n"
        "  --output DIR                Output dir (default: ./bench_online_query_out)\n"
        "  --query-offset N            First query row (default: 0)\n"
        "  --query-count N             Number of query rows (default: all remaining)\n"
        "  --queries N                 Alias for --query-count\n"
        "  --query-ids FILE.npy        Optional int64 query ids slice\n"
        "  --gt-file FILE.npy          Optional int32/int64 GT matrix slice\n"
        "  --gt-offset N               First GT row (default: query-offset)\n"
        "  --topk N                    Top-k (default: 10)\n"
        "  --nprobe N                  Probed clusters (default: 64)\n"
        "  --separate-store-dir DIR    No Combine store with vector.dat/payload.dat/address_map.bin\n"
        "  --dynamic-safein static|frontier (default: static)\n"
        "  --safein-as-vec-only 0|1 Keep SafeIn labels but force SafeIn VEC_ALL prefetches to VEC_ONLY (default: 0)\n"
        "  --safein-all-threshold-bytes N Max single record bytes for SafeIn VEC_ALL (default: 262144)\n"
        "  --safein-prefetch-max-count N Query-level SafeIn VEC_ALL count cap (default: 0 = unlimited)\n"
        "  --safein-prefetch-max-bytes N Query-level SafeIn VEC_ALL byte cap (default: 0 = unlimited)\n"
        "  --safein-prefetch-emit-quantum N Emit at most N SafeIn full reads before VEC_ONLY in each flush (default: 0 = legacy all-first)\n"
        "  --false-stats-cluster-members-cache FILE Cache cluster members for SafeIn truth metrics\n"
        "  --skip-false-stats 0|1     Disable SafeIn truth metrics (default: 0)\n"
        "  --dynamic-safein-defer-initial-clusters N Defer SafeIn prefetch for first N clusters (default: 0)\n"
        "  --dynamic-safein-defer-until-ready 0|1 Defer SafeIn prefetch until frontier is stable (default: 0)\n"
        "  --dynamic-safein-defer-max-candidates N Deferred candidate cap (default: 0 = unlimited)\n"
        "  --dynamic-safeout 0|1       Dynamic SafeOut (default: 1)\n"
        "  --rabitq-validation-mode auto|official_1_plus_n|legacy_signed_magnitude\n"
        "  --rabitq-active-bits N      Query-time RaBitQ total bits; default uses stored bits\n"
        "  --rabitq-resident-bits N    Resident-loaded RaBitQ total bits; default follows active bits when set\n"
        "  --safeout-epsilon-percentile P Runtime SafeOut epsilon percentile\n"
        "  --epsilon-samples N         Calibration samples per cluster (default: 100)\n"
        "  --epsilon-sampling-mode legacy_per_cluster|global_pair (default: legacy_per_cluster)\n"
        "  --safeout-epsilon-cache FILE Cache calibrated runtime SafeOut epsilon\n"
        "  --non-safeout-candidate-budget N (default: 0)\n"
        "  --budgeted-prefetch-limit N Speculative raw-vector prefetch cap for budgeted queries (default: 0)\n"
        "  --fixed-vec-buffer-count N  Fixed vector buffers (default: 0)\n"
        "  --serial-data-drain 0|1    Drain data I/O after each cluster (default: 0)\n"
        "  --two-level-coarse-routing 0|1 Enable two-level coarse routing (default: 0)\n"
        "  --two-level-coarse-budget-factor N Candidate budget = nprobe*N (default: 8)\n"
        "  --two-level-coarse-budget-cap N Cap two-level child candidates (default: 0 = no cap)\n"
        "  --fine-grained-timing 0|1   Enable detailed stage timing (default: 0)\n"
        "  --hotpath-detailed-timing 0|1 (default: 0)\n"
        "  --direct-io                 Open index files with direct I/O\n");
    return 2;
}

int main(int argc, char** argv) {
    if (HasFlag(argc, argv, "--help") || HasFlag(argc, argv, "-h")) {
        return Usage();
    }

    const auto t_process_start = std::chrono::steady_clock::now();
    const ProcRss rss_start = ReadProcRss();

    const std::string index_dir = GetStringArg(argc, argv, "--index-dir", "");
    const std::string query_file = GetStringArg(argc, argv, "--query-file", "");
    if (index_dir.empty() || query_file.empty()) {
        return Usage();
    }

    const std::string output_dir =
        GetStringArg(argc, argv, "--output", "bench_online_query_out");
    const std::string dataset_dir = GetStringArg(argc, argv, "--dataset", "");
    const std::string assignments_file =
        GetStringArg(argc, argv, "--assignments", "");
    const std::string false_stats_cluster_members_cache =
        GetStringArg(argc, argv, "--false-stats-cluster-members-cache", "");
    const bool skip_false_stats =
        GetIntArg(argc, argv, "--skip-false-stats", 0) != 0;
    const std::string separate_store_dir =
        GetStringArg(argc, argv, "--separate-store-dir", "");
    const double safeout_epsilon_percentile =
        GetDoubleArg(argc, argv, "--safeout-epsilon-percentile",
                     GetDoubleArg(argc, argv, "--epsilon-percentile", -1.0));
    const int epsilon_samples =
        std::max(1, GetIntArg(argc, argv, "--epsilon-samples", 100));
    const std::string epsilon_sampling_mode_arg =
        GetStringArg(argc, argv, "--epsilon-sampling-mode",
                     "legacy_per_cluster");
    auto epsilon_sampling_mode_or =
        ParseEpsilonSamplingModeArg(epsilon_sampling_mode_arg);
    if (!epsilon_sampling_mode_or.ok()) {
        std::fprintf(stderr, "Invalid --epsilon-sampling-mode: %s\n",
                     epsilon_sampling_mode_or.status().ToString().c_str());
        return 1;
    }
    const EpsilonSamplingMode epsilon_sampling_mode =
        epsilon_sampling_mode_or.value();
    const int seed = GetIntArg(argc, argv, "--seed", 42);
    const std::string safeout_epsilon_cache =
        GetStringArg(argc, argv, "--safeout-epsilon-cache", "");
    if (HasFlag(argc, argv, "--safeout-epsilon-override")) {
        std::fprintf(stderr,
                     "--safeout-epsilon-override is disabled for online runner; "
                     "use --safeout-epsilon-percentile so the runner owns the "
                     "runtime SafeOut epsilon selection.\n");
        return 1;
    }
    const uint64_t query_offset =
        static_cast<uint64_t>(std::max(0, GetIntArg(argc, argv, "--query-offset", 0)));
    int query_count_arg = GetIntArg(argc, argv, "--query-count", -1);
    if (query_count_arg < 0) {
        query_count_arg = GetIntArg(argc, argv, "--queries", 0);
    }
    const uint64_t query_count =
        query_count_arg > 0 ? static_cast<uint64_t>(query_count_arg) : 0;
    const uint32_t topk = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--topk", 10)));
    const uint32_t nprobe = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--nprobe", 64)));
    const int active_bits_arg = GetIntArg(argc, argv, "--rabitq-active-bits", -1);
    const int resident_bits_arg =
        GetIntArg(argc, argv, "--rabitq-resident-bits", -1);
    const int active_ex_bits_compat_arg =
        GetIntArg(argc, argv, "--rabitq-active-ex-bits", -1);
    int active_ex_bits_arg = active_ex_bits_compat_arg;
    if (active_bits_arg >= 0) {
        if (active_bits_arg < 1) {
            std::fprintf(stderr, "--rabitq-active-bits must be >= 1\n");
            return 1;
        }
        const int converted = active_bits_arg - 1;
        if (active_ex_bits_compat_arg >= 0 &&
            active_ex_bits_compat_arg != converted) {
            std::fprintf(stderr,
                         "--rabitq-active-bits conflicts with the legacy "
                         "precision alias\n");
            return 1;
        }
        active_ex_bits_arg = converted;
    }
    int resident_ex_bits_arg = -1;
    if (resident_bits_arg >= 0) {
        if (resident_bits_arg < 2) {
            std::fprintf(stderr,
                         "--rabitq-resident-bits must be >= 2 for selective "
                         "resident preload\n");
            return 1;
        }
        resident_ex_bits_arg = resident_bits_arg - 1;
    } else if (active_bits_arg >= 0) {
        resident_ex_bits_arg = active_ex_bits_arg;
    }
    if (resident_ex_bits_arg >= 0 && active_ex_bits_arg < 0) {
        active_ex_bits_arg = resident_ex_bits_arg;
    }
    if (resident_ex_bits_arg >= 0 && active_ex_bits_arg >= 0 &&
        resident_ex_bits_arg < active_ex_bits_arg) {
        std::fprintf(stderr,
                     "--rabitq-resident-bits cannot be lower than "
                     "--rabitq-active-bits\n");
        return 1;
    }
    const int io_queue_depth = std::max(1, GetIntArg(argc, argv, "--io-queue-depth", 64));
    const bool iopoll = GetIntArg(argc, argv, "--iopoll", 0) != 0;
    const bool sqpoll = GetIntArg(argc, argv, "--sqpoll", 0) != 0;
    const bool direct_io = HasFlag(argc, argv, "--direct-io");
    const std::string submission_mode =
        GetStringArg(argc, argv, "--submission-mode", "shared");
    const std::string dynamic_safein =
        GetStringArg(argc, argv, "--dynamic-safein", "static");
    const std::string rabitq_mode =
        GetStringArg(argc, argv, "--rabitq-validation-mode",
                     GetStringArg(argc, argv, "--rabitq-query-mode", "auto"));
    RaBitQValidationMode rabitq_validation_mode = RaBitQValidationMode::Auto;
    if (!ParseRaBitQValidationModeArg(rabitq_mode, &rabitq_validation_mode)) {
        std::fprintf(stderr,
                     "Invalid --rabitq-validation-mode: %s "
                     "(expected auto, official_1_plus_n, or legacy_signed_magnitude)\n",
                     rabitq_mode.c_str());
        return 1;
    }

    Log("=== VDB Online Query Benchmark ===\n");
    Log("Index: %s\n", index_dir.c_str());
    Log("Query file: %s offset=%llu count=%llu\n", query_file.c_str(),
        static_cast<unsigned long long>(query_offset),
        static_cast<unsigned long long>(query_count));

    auto queries_or = LoadNpyFloat32Rows(query_file, query_offset, query_count);
    if (!queries_or.ok()) {
        std::fprintf(stderr, "Failed to load query slice: %s\n",
                     queries_or.status().ToString().c_str());
        return 1;
    }
    FloatRows queries = std::move(queries_or.value());
    const ProcRss rss_after_query_load = ReadProcRss();

    std::vector<int64_t> query_ids(queries.rows);
    std::iota(query_ids.begin(), query_ids.end(),
              static_cast<int64_t>(query_offset));
    const std::string query_ids_file = GetStringArg(argc, argv, "--query-ids", "");
    if (!query_ids_file.empty()) {
        auto ids_or = LoadNpyInt64VectorRows(query_ids_file, query_offset, queries.rows);
        if (!ids_or.ok()) {
            std::fprintf(stderr, "Failed to load query id slice: %s\n",
                         ids_or.status().ToString().c_str());
            return 1;
        }
        query_ids = std::move(ids_or.value());
    }

    std::vector<std::vector<int64_t>> gt;
    const std::string gt_file = GetStringArg(argc, argv, "--gt-file", "");
    const uint64_t gt_offset = static_cast<uint64_t>(
        std::max(0, GetIntArg(argc, argv, "--gt-offset",
                              static_cast<int>(query_offset))));
    if (!gt_file.empty()) {
        auto gt_or = LoadNpyGroundTruthRows(gt_file, gt_offset, queries.rows, topk);
        if (!gt_or.ok()) {
            std::fprintf(stderr, "Failed to load GT slice: %s\n",
                         gt_or.status().ToString().c_str());
            return 1;
        }
        gt = std::move(gt_or.value());
    }
    const ProcRss rss_after_gt_load = ReadProcRss();

    IvfIndex index;
    Status s = index.Open(index_dir, direct_io);
    if (!s.ok()) {
        std::fprintf(stderr, "Open index failed: %s\n", s.ToString().c_str());
        return 1;
    }
    if (!ValidateRaBitQMode(index.segment().rabitq_config(),
                            rabitq_validation_mode,
                            "--rabitq-validation-mode")) {
        return 1;
    }

    std::unordered_map<int64_t, uint32_t> false_stats_image_id_to_row;
    std::vector<std::vector<uint32_t>> false_stats_cluster_members;
    bool have_false_stats_cluster_members = false;
    if (!skip_false_stats && !gt.empty()) {
        if (dataset_dir.empty()) {
            std::fprintf(stderr,
                         "SafeIn false-stats require --dataset so image_ids.npy can be loaded\n");
            return 1;
        }
        const std::string image_ids_path =
            (fs::path(dataset_dir) / "image_ids.npy").string();
        auto ids_or = vdb::io::LoadNpyInt64(image_ids_path);
        if (!ids_or.ok()) {
            std::fprintf(stderr,
                         "Failed to load image ids for SafeIn false-stats %s: %s\n",
                         image_ids_path.c_str(),
                         ids_or.status().ToString().c_str());
            return 1;
        }
        const auto& ids = ids_or.value();
        false_stats_image_id_to_row.reserve(ids.count);
        for (uint32_t row = 0; row < ids.count; ++row) {
            false_stats_image_id_to_row[ids.data[row]] = row;
        }

        if (LoadClusterMembersCache(false_stats_cluster_members_cache,
                                    index.nlist(),
                                    &false_stats_cluster_members)) {
            have_false_stats_cluster_members = true;
            Log("  SafeIn false-stats cluster members loaded: %s\n",
                false_stats_cluster_members_cache.c_str());
        } else if (!assignments_file.empty() && fs::exists(assignments_file)) {
            auto assignments_or = LoadAssignments(assignments_file, ids.count);
            if (!assignments_or.ok()) {
                std::fprintf(stderr, "Failed to load assignments %s: %s\n",
                             assignments_file.c_str(),
                             assignments_or.status().ToString().c_str());
                return 1;
            }
            BuildClusterMembers(assignments_or.value(), index.nlist(),
                                &false_stats_cluster_members);
            have_false_stats_cluster_members = true;
        } else {
            auto recover_status = RecoverClusterMembersFromIndex(
                index, false_stats_image_id_to_row, &false_stats_cluster_members);
            if (!recover_status.ok()) {
                std::fprintf(stderr,
                             "Failed to recover SafeIn false-stats cluster members: %s\n",
                             recover_status.ToString().c_str());
                return 1;
            }
            have_false_stats_cluster_members = true;
        }
        if (have_false_stats_cluster_members &&
            !false_stats_cluster_members_cache.empty() &&
            !fs::exists(false_stats_cluster_members_cache)) {
            if (!SaveClusterMembersCache(false_stats_cluster_members_cache,
                                         false_stats_cluster_members)) {
                std::fprintf(stderr,
                             "Failed to save SafeIn false-stats cluster cache: %s\n",
                             false_stats_cluster_members_cache.c_str());
                return 1;
            }
            Log("  SafeIn false-stats cluster members cached: %s\n",
                false_stats_cluster_members_cache.c_str());
        }
    }
    if (queries.cols != index.logical_dim()) {
        std::fprintf(stderr,
                     "Query dim mismatch: query cols=%u index logical_dim=%u\n",
                     queries.cols, index.logical_dim());
        return 1;
    }

    SeparateStoreMapData separate_store;
    int separate_vector_fd = -1;
    int separate_payload_fd = -1;
    if (!separate_store_dir.empty()) {
        const std::string map_path = separate_store_dir + "/address_map.bin";
        auto map_or = LoadSeparateStoreMap(map_path);
        if (!map_or.ok()) {
            std::fprintf(stderr, "Failed to load separate-store map: %s\n",
                         map_or.status().ToString().c_str());
            return 1;
        }
        separate_store = std::move(map_or.value());
        const uint32_t expected_vec_bytes = index.logical_dim() * sizeof(float);
        if (separate_store.vec_bytes != expected_vec_bytes) {
            std::fprintf(stderr,
                         "Separate-store vector size mismatch: map=%u expected=%u\n",
                         separate_store.vec_bytes, expected_vec_bytes);
            return 1;
        }
        const std::string vector_path = separate_store_dir + "/vector.dat";
        const std::string payload_path = separate_store_dir + "/payload.dat";
        separate_vector_fd = ::open(vector_path.c_str(), O_RDONLY);
        if (separate_vector_fd < 0) {
            std::perror(("open " + vector_path).c_str());
            return 1;
        }
        separate_payload_fd = ::open(payload_path.c_str(), O_RDONLY);
        if (separate_payload_fd < 0) {
            std::perror(("open " + payload_path).c_str());
            ::close(separate_vector_fd);
            return 1;
        }
        Log("Separate store: %s records=%llu vec_bytes=%u\n",
            separate_store_dir.c_str(),
            static_cast<unsigned long long>(separate_store.record_count),
            separate_store.vec_bytes);
    }
    const ProcRss rss_after_index_open = ReadProcRss();

    const auto& preload_rabitq_cfg = index.segment().rabitq_config();
    const uint8_t stored_ex_bits = preload_rabitq_cfg.stage2_payload_bits();
    const vdb::RaBitQExDataLayout active_layout =
        preload_rabitq_cfg.effective_exdata_layout();
    const bool active_supported =
        vdb::RaBitQExDataLayoutSupportsActiveExBits(active_layout);
    auto validate_ex_bits_arg = [&](const char* arg_name, int ex_bits) -> bool {
        if (ex_bits < 0) return true;
        if (ex_bits > static_cast<int>(stored_ex_bits)) {
            std::fprintf(stderr,
                         "%s=%d exceeds stored bits=%u\n",
                         arg_name,
                         ex_bits + 1,
                         static_cast<uint32_t>(stored_ex_bits + 1u));
            return false;
        }
        if (ex_bits > 0 &&
            ex_bits < static_cast<int>(stored_ex_bits) &&
            !active_supported) {
            std::fprintf(stderr,
                         "%s=%d is lower than stored bits=%u, but layout=%s "
                         "does not support partial active computation\n",
                         arg_name,
                         ex_bits + 1,
                         static_cast<uint32_t>(stored_ex_bits + 1u),
                         std::string(vdb::RaBitQExDataLayoutName(active_layout)).c_str());
            return false;
        }
        return true;
    };
    if (!validate_ex_bits_arg("--rabitq-active-bits", active_ex_bits_arg) ||
        !validate_ex_bits_arg("--rabitq-resident-bits", resident_ex_bits_arg)) {
        if (separate_payload_fd >= 0) ::close(separate_payload_fd);
        if (separate_vector_fd >= 0) ::close(separate_vector_fd);
        return 1;
    }

    float runtime_safeout_epsilon = index.conann().epsilon();
    const float loaded_safeout_epsilon = index.conann().epsilon();
    bool runtime_safeout_epsilon_overridden = false;
    bool runtime_safeout_epsilon_cache_hit = false;
    EpsilonCalibrationStats safeout_epsilon_stats;
    safeout_epsilon_stats.requested_samples =
        static_cast<uint32_t>(epsilon_samples);
    safeout_epsilon_stats.sampling_mode = epsilon_sampling_mode;
    if (safeout_epsilon_percentile >= 0.0) {
        if (ReadFloatCache(safeout_epsilon_cache, &runtime_safeout_epsilon)) {
            runtime_safeout_epsilon_overridden = true;
            runtime_safeout_epsilon_cache_hit = true;
            Log("  Runtime SafeOut epsilon cache hit: %s value=%.6f\n",
                safeout_epsilon_cache.c_str(), runtime_safeout_epsilon);
        } else {
            if (dataset_dir.empty()) {
                std::fprintf(stderr,
                             "--safeout-epsilon-percentile requires --dataset "
                             "when --safeout-epsilon-cache is absent or cold\n");
                return 1;
            }
            const std::string image_embeddings_path =
                (fs::path(dataset_dir) / "image_embeddings.npy").string();
            Log("  Runtime SafeOut epsilon calibration: dataset=%s assignments=%s percentile=%.4f samples=%d mode=%s\n",
                dataset_dir.c_str(), assignments_file.c_str(),
                safeout_epsilon_percentile, epsilon_samples,
                EpsilonSamplingModeName(epsilon_sampling_mode));

            auto base_or = vdb::io::LoadNpyFloat32(image_embeddings_path);
            if (!base_or.ok()) {
                std::fprintf(stderr, "Failed to load calibration vectors %s: %s\n",
                             image_embeddings_path.c_str(),
                             base_or.status().ToString().c_str());
                return 1;
            }
            auto base = std::move(base_or.value());
            if (base.cols != index.logical_dim()) {
                std::fprintf(stderr,
                             "Calibration vector dim mismatch: base cols=%u "
                             "index logical_dim=%u\n",
                             base.cols, index.logical_dim());
                return 1;
            }
            std::vector<std::vector<uint32_t>> cluster_members;
            if (!assignments_file.empty() && fs::exists(assignments_file)) {
                auto assignments_or = LoadAssignments(assignments_file, base.rows);
                if (!assignments_or.ok()) {
                    std::fprintf(stderr, "Failed to load assignments %s: %s\n",
                                 assignments_file.c_str(),
                                 assignments_or.status().ToString().c_str());
                    return 1;
                }
                BuildClusterMembers(assignments_or.value(), index.nlist(),
                                    &cluster_members);
            } else {
                const std::string image_ids_path =
                    (fs::path(dataset_dir) / "image_ids.npy").string();
                auto ids_or = vdb::io::LoadNpyInt64(image_ids_path);
                if (!ids_or.ok()) {
                    std::fprintf(stderr,
                                 "Failed to load image ids for cluster recovery %s: %s\n",
                                 image_ids_path.c_str(),
                                 ids_or.status().ToString().c_str());
                    return 1;
                }
                const auto& ids = ids_or.value();
                if (ids.count != base.rows) {
                    std::fprintf(stderr,
                                 "image_ids rows mismatch: ids=%u base=%u\n",
                                 ids.count, base.rows);
                    return 1;
                }
                std::unordered_map<int64_t, uint32_t> image_id_to_row;
                image_id_to_row.reserve(ids.count);
                for (uint32_t row = 0; row < ids.count; ++row) {
                    image_id_to_row[ids.data[row]] = row;
                }
                Status recover_status = RecoverClusterMembersFromIndex(
                    index, image_id_to_row, &cluster_members);
                if (!recover_status.ok()) {
                    std::fprintf(stderr,
                                 "Failed to recover cluster members from index: %s\n",
                                 recover_status.ToString().c_str());
                    return 1;
                }
            }
            std::vector<std::vector<vdb::rabitq::RaBitQCode>> all_codes;
            EncodeAllCodes(base.data, base.rows, base.cols, cluster_members,
                           index.centroids(), index.rotation(),
                           index.segment().rabitq_config(), nullptr,
                           &all_codes);

            float runtime_safein_d_k = index.conann().safein_d_k();
            if (runtime_safein_d_k <= 0.0f) {
                runtime_safein_d_k = index.conann().d_k();
            }
            runtime_safeout_epsilon = CalibrateSplitEpsilon(
                all_codes, cluster_members, base.data.data(), index.centroids(),
                index.rotation(), base.cols,
                static_cast<uint32_t>(epsilon_samples),
                static_cast<float>(safeout_epsilon_percentile),
                static_cast<uint64_t>(seed), runtime_safein_d_k,
                index.segment().rabitq_config(), nullptr, epsilon_sampling_mode,
                &safeout_epsilon_stats);
            runtime_safeout_epsilon_overridden = true;
            if (!WriteFloatCache(safeout_epsilon_cache,
                                 runtime_safeout_epsilon)) {
                std::fprintf(stderr,
                             "Failed to write SafeOut epsilon cache: %s\n",
                             safeout_epsilon_cache.c_str());
                return 1;
            }
            if (!safeout_epsilon_cache.empty()) {
                Log("  Runtime SafeOut epsilon cache written: %s value=%.6f\n",
                    safeout_epsilon_cache.c_str(), runtime_safeout_epsilon);
            }
        }
        index.OverrideConANN(runtime_safeout_epsilon,
                             index.conann().legacy_d_k(),
                             index.conann().safein_d_k() > 0.0f
                                 ? index.conann().safein_d_k()
                                 : index.conann().d_k(),
                             true);
        Log("  Runtime SafeOut epsilon override from percentile: safeout_eps=%.6f loaded_eps=%.6f percentile=%.4f cache_hit=%d\n",
            runtime_safeout_epsilon, loaded_safeout_epsilon,
            safeout_epsilon_percentile,
            runtime_safeout_epsilon_cache_hit ? 1 : 0);
    }

    auto t_preload_start = std::chrono::steady_clock::now();
    s = index.segment().PreloadAllClusters(
        resident_ex_bits_arg >= 0
            ? static_cast<uint8_t>(resident_ex_bits_arg)
            : 0);
    if (!s.ok()) {
        std::fprintf(stderr, "Preload failed: %s\n", s.ToString().c_str());
        return 1;
    }
    const double preload_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_preload_start).count();
    const ProcRss rss_after_preload = ReadProcRss();
    const SmapsRollupStats smaps_after_preload = ReadSmapsRollupStats();

    IoUringReader cluster_reader;
    auto init_status = cluster_reader.Init(
        static_cast<uint32_t>(io_queue_depth), 4096, iopoll, sqpoll);
    if (!init_status.ok()) {
        std::fprintf(stderr, "IoUring init failed: %s\n",
                     init_status.ToString().c_str());
        return 1;
    }

    std::unique_ptr<IoUringReader> data_reader;
    if (submission_mode == "isolated") {
        data_reader = std::make_unique<IoUringReader>();
        auto data_status = data_reader->Init(
            static_cast<uint32_t>(io_queue_depth), 4096, iopoll, sqpoll);
        if (!data_status.ok()) {
            std::fprintf(stderr, "Data IoUring init failed: %s\n",
                         data_status.ToString().c_str());
            return 1;
        }
        int clu_fd = index.segment().clu_fd();
        (void)cluster_reader.RegisterFiles(&clu_fd, 1);
        int dat_fd = index.segment().data_reader().fd();
        (void)data_reader->RegisterFiles(&dat_fd, 1);
    } else if (submission_mode == "shared") {
        int fds[2] = {index.segment().clu_fd(), index.segment().data_reader().fd()};
        (void)cluster_reader.RegisterFiles(fds, 2);
    } else {
        std::fprintf(stderr, "Invalid --submission-mode: %s\n",
                     submission_mode.c_str());
        return 1;
    }

    SearchConfig cfg;
    cfg.top_k = topk;
    cfg.nprobe = nprobe;
    cfg.io_queue_depth = static_cast<uint32_t>(io_queue_depth);
    cfg.use_sqpoll = sqpoll;
    cfg.submission_mode = submission_mode == "isolated"
        ? SubmissionMode::Isolated
        : SubmissionMode::Shared;
    cfg.enable_dynamic_safeout =
        GetIntArg(argc, argv, "--dynamic-safeout", 1) != 0;
    cfg.dynamic_safein_mode = dynamic_safein == "frontier"
        ? DynamicSafeInMode::Frontier
        : DynamicSafeInMode::Static;
    cfg.dynamic_safein_min_probes = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--dynamic-safein-min-probes", 0)));
    cfg.dynamic_safein_stable_probes = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--dynamic-safein-stable-probes", 2)));
    cfg.dynamic_safein_rel_tol =
        static_cast<float>(GetDoubleArg(argc, argv, "--dynamic-safein-rel-tol", 0.005));
    cfg.dynamic_safein_abs_tol =
        static_cast<float>(GetDoubleArg(argc, argv, "--dynamic-safein-abs-tol", 0.0));
    cfg.dynamic_safein_defer_initial_clusters = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--dynamic-safein-defer-initial-clusters", 0)));
    cfg.dynamic_safein_defer_until_ready =
        GetIntArg(argc, argv, "--dynamic-safein-defer-until-ready", 0) != 0;
    cfg.dynamic_safein_defer_max_candidates = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--dynamic-safein-defer-max-candidates", 0)));
    cfg.safein_as_vec_only =
        GetIntArg(argc, argv, "--safein-as-vec-only", 0) != 0;
    cfg.safein_all_threshold = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--safein-all-threshold-bytes", 256 * 1024)));
    cfg.safein_prefetch_max_count = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--safein-prefetch-max-count", 0)));
    cfg.safein_prefetch_max_bytes = static_cast<uint64_t>(
        std::max<int64_t>(0, GetInt64Arg(argc, argv, "--safein-prefetch-max-bytes", 0)));
    cfg.safein_prefetch_emit_quantum = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--safein-prefetch-emit-quantum", 0)));
    cfg.non_safeout_candidate_budget = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--non-safeout-candidate-budget", 0)));
    cfg.budgeted_prefetch_limit = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--budgeted-prefetch-limit", 0)));
    cfg.fixed_vec_buffer_count = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--fixed-vec-buffer-count", 0)));
    cfg.cluster_submit_reserve = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--cluster-submit-reserve", 8)));
    cfg.submit_batch_size = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--submit-batch", 32)));
    cfg.serial_data_drains =
        GetIntArg(argc, argv, "--serial-data-drain", 0) != 0;
    cfg.enable_fine_grained_timing =
        GetIntArg(argc, argv, "--fine-grained-timing", 0) != 0;
    cfg.enable_hotpath_detailed_timing =
        GetIntArg(argc, argv, "--hotpath-detailed-timing", 0) != 0;
    cfg.enable_address_decode_simd =
        GetIntArg(argc, argv, "--address-decode-simd", 1) != 0;
    cfg.enable_rerank_batched_distance_simd =
        GetIntArg(argc, argv, "--rerank-batched-distance-simd", 1) != 0;
    cfg.enable_coarse_select_simd =
        GetIntArg(argc, argv, "--coarse-select-simd", 1) != 0;
    cfg.enable_two_level_coarse_routing =
        GetIntArg(argc, argv, "--two-level-coarse-routing", 0) != 0;
    cfg.two_level_coarse_threshold = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--two-level-coarse-threshold", 4096)));
    cfg.two_level_coarse_super_count = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--two-level-coarse-super-count", 0)));
    cfg.two_level_coarse_super_factor = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--two-level-coarse-super-factor", 0)));
    cfg.two_level_coarse_budget_factor = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--two-level-coarse-budget-factor", 8)));
    cfg.two_level_coarse_budget_cap = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--two-level-coarse-budget-cap", 0)));
    cfg.enable_two_level_coarse_exact_overlap =
        GetIntArg(argc, argv, "--two-level-coarse-exact-overlap", 0) != 0;
    cfg.enable_stage1_safein =
        GetIntArg(argc, argv, "--enable-stage1-safein", 1) != 0;
    cfg.enable_stage2_collect_block_first =
        GetIntArg(argc, argv, "--stage2-block-first", 1) != 0;
    cfg.enable_stage2_scatter_batch_classify =
        GetIntArg(argc, argv, "--stage2-batch-classify", 1) != 0;
    if (active_ex_bits_arg >= 0) {
        cfg.rabitq_active_ex_bits_set = true;
        cfg.rabitq_active_ex_bits = static_cast<uint8_t>(active_ex_bits_arg);
    }
    const double safein_eps_override =
        GetDoubleArg(argc, argv, "--safein-epsilon-override", -1.0);
    cfg.safein_epsilon_override = static_cast<float>(safein_eps_override);
    if (runtime_safeout_epsilon_overridden) {
        cfg.safeout_epsilon_override = runtime_safeout_epsilon;
    }
    if (!separate_store_dir.empty()) {
        cfg.separate_record_store.enabled = true;
        cfg.separate_record_store.vector_fd = separate_vector_fd;
        cfg.separate_record_store.payload_fd = separate_payload_fd;
        cfg.separate_record_store.address_map = &separate_store.records;
    }
    if (have_false_stats_cluster_members) {
        cfg.false_stats_cluster_members = &false_stats_cluster_members;
    }

    double two_level_coarse_warmup_ms = 0.0;
    if (cfg.enable_two_level_coarse_routing) {
        index.SetTwoLevelCoarseRouting(cfg.enable_two_level_coarse_routing,
                                       cfg.two_level_coarse_threshold,
                                       cfg.two_level_coarse_super_count,
                                       cfg.two_level_coarse_super_factor,
                                       cfg.two_level_coarse_budget_factor,
                                       cfg.enable_two_level_coarse_exact_overlap,
                                       cfg.two_level_coarse_budget_cap);
        const auto warmup_start = std::chrono::steady_clock::now();
        const bool prepared = index.PrepareTwoLevelCoarseRouting(cfg.nprobe);
        two_level_coarse_warmup_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - warmup_start).count();
        Log("  two-level coarse warmup: %s (%.3f ms)\n",
            prepared ? "prepared" : "skipped", two_level_coarse_warmup_ms);
    }

    std::unique_ptr<OverlapScheduler> scheduler;
    if (data_reader) {
        scheduler = std::make_unique<OverlapScheduler>(
            index, cluster_reader, *data_reader, cfg);
    } else {
        scheduler = std::make_unique<OverlapScheduler>(index, cluster_reader, cfg);
    }

    std::vector<std::unordered_set<uint32_t>> false_stats_truth_sets;
    if (have_false_stats_cluster_members && !false_stats_image_id_to_row.empty() &&
        !gt.empty()) {
        false_stats_truth_sets.resize(queries.rows);
        for (uint32_t qi = 0; qi < queries.rows && qi < gt.size(); ++qi) {
            false_stats_truth_sets[qi].reserve(gt[qi].size());
            for (int64_t id : gt[qi]) {
                auto it = false_stats_image_id_to_row.find(id);
                if (it != false_stats_image_id_to_row.end()) {
                    false_stats_truth_sets[qi].insert(it->second);
                }
            }
        }
    }

    QueryMetrics metrics;
    ProcRss peak_during_query = ReadProcRss();
    Log("\n[Query] Running %u queries...\n", queries.rows);
    for (uint32_t qi = 0; qi < queries.rows; ++qi) {
        const float* q = queries.data.data() + static_cast<size_t>(qi) * queries.cols;
        if (!false_stats_truth_sets.empty() && qi < false_stats_truth_sets.size()) {
            scheduler->SetFalseStatsTrueTopKRows(&false_stats_truth_sets[qi]);
        } else {
            scheduler->SetFalseStatsTrueTopKRows(nullptr);
        }
        SearchResults results = scheduler->Search(q);
        const std::vector<int64_t>* gt_row =
            (!gt.empty() && qi < gt.size()) ? &gt[qi] : nullptr;
        Accumulate(metrics, results, gt_row, topk);
        const ProcRss sample = ReadProcRss();
        if (sample.rss_kib > peak_during_query.rss_kib) {
            peak_during_query.rss_kib = sample.rss_kib;
        }
        if (sample.hwm_kib > peak_during_query.hwm_kib) {
            peak_during_query.hwm_kib = sample.hwm_kib;
        }
        if ((qi + 1) % 100 == 0 || qi + 1 == queries.rows) {
            Log("  progress: %u/%u\n", qi + 1, queries.rows);
        }
    }
    const ProcRss rss_after_queries = ReadProcRss();
    const SmapsRollupStats smaps_after_queries = ReadSmapsRollupStats();
    const double process_wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t_process_start).count();

    const uint32_t Q = queries.rows;
    const double inv_q = Q > 0 ? 1.0 / static_cast<double>(Q) : 0.0;
    const double avg_latency =
        std::accumulate(metrics.latencies_ms.begin(), metrics.latencies_ms.end(), 0.0) *
        inv_q;
    const double p50 = Percentile(metrics.latencies_ms, 0.50);
    const double p95 = Percentile(metrics.latencies_ms, 0.95);
    const double p99 = Percentile(metrics.latencies_ms, 0.99);
    const double recall1 = metrics.recall_available ? metrics.recall1_sum * inv_q : 0.0;
    const double recall5 = metrics.recall_available ? metrics.recall5_sum * inv_q : 0.0;
    const double recall10 = metrics.recall_available ? metrics.recall10_sum * inv_q : 0.0;
    const double recallk = metrics.recall_available ? metrics.recallk_sum * inv_q : 0.0;

    Log("\n=== Summary ===\n");
    Log("  avg=%.4f ms p50=%.4f p95=%.4f p99=%.4f\n",
        avg_latency, p50, p95, p99);
    if (metrics.recall_available) {
        Log("  recall@1=%.4f recall@5=%.4f recall@10=%.4f recall@k=%.4f\n",
            recall1, recall5, recall10, recallk);
    }
    Log("  rss_after_query_load=%lld KiB rss_after_preload=%lld KiB peak_query=%lld KiB\n",
        static_cast<long long>(rss_after_query_load.rss_kib),
        static_cast<long long>(rss_after_preload.rss_kib),
        static_cast<long long>(peak_during_query.rss_kib));

    fs::create_directories(output_dir);
    const std::string json_path = output_dir + "/results.json";
    std::ofstream f(json_path);
    if (!f.is_open()) {
        std::fprintf(stderr, "Failed to open output: %s\n", json_path.c_str());
        return 1;
    }

    const uint32_t clu_version = index.segment().cluster_reader().file_version();
    const bool packed_stage2 =
        (clu_version >= 12 && index.segment().rabitq_config().has_stage2_payload());
    const auto& result_rabitq_cfg = index.segment().rabitq_config();
    const uint8_t active_ex_bits =
        cfg.rabitq_active_ex_bits_set ? cfg.rabitq_active_ex_bits : stored_ex_bits;
    const uint8_t resident_loaded_ex_bits =
        index.segment().resident_loaded_ex_bits();
    const uint8_t stored_total_bits =
        static_cast<uint8_t>(stored_ex_bits + 1u);
    const uint8_t active_total_bits =
        static_cast<uint8_t>(active_ex_bits + 1u);
    const uint8_t resident_total_bits =
        static_cast<uint8_t>(resident_loaded_ex_bits + 1u);
    const vdb::RaBitQExDataLayout result_layout =
        result_rabitq_cfg.effective_exdata_layout();
    const bool layout_honors_active_ex_bits =
        active_ex_bits == 0 || active_ex_bits == stored_ex_bits ||
        vdb::RaBitQExDataLayoutSupportsActiveExBits(result_layout);
    const std::string active_bits_mode =
        active_ex_bits == 0
            ? "base_bit_only"
            : (active_ex_bits == stored_ex_bits
                   ? "stored_bits"
                   : "partial_stored_code");

    f << "{\n";
    f << "  \"metrics\": {\n";
    f << "    " << JStr("benchmark_mode", "online_query") << ",\n";
    f << "    " << JStr("index_dir", index_dir) << ",\n";
    f << "    " << JStr("record_layout",
                        separate_store_dir.empty() ? "combined"
                                                   : "no_combine_flatstor") << ",\n";
    f << "    " << JStr("separate_store_dir", separate_store_dir) << ",\n";
    f << "    " << JInt("separate_store_records",
                        static_cast<int64_t>(separate_store.record_count)) << ",\n";
    f << "    " << JInt("separate_store_vec_bytes",
                        static_cast<int64_t>(separate_store.vec_bytes)) << ",\n";
    f << "    " << JStr("query_file", query_file) << ",\n";
    f << "    " << JInt("query_offset", static_cast<int64_t>(query_offset)) << ",\n";
    f << "    " << JInt("num_queries", Q) << ",\n";
    f << "    " << JInt("query_dim", queries.cols) << ",\n";
    f << "    " << JInt("topk", topk) << ",\n";
    f << "    " << JInt("nprobe", nprobe) << ",\n";
    f << "    " << JInt("io_queue_depth", cfg.io_queue_depth) << ",\n";
    f << "    " << JInt("fixed_vec_buffer_count",
                        cfg.fixed_vec_buffer_count) << ",\n";
    f << "    " << JInt("cluster_submit_reserve",
                        cfg.cluster_submit_reserve) << ",\n";
    f << "    " << JInt("submit_batch_size", cfg.submit_batch_size) << ",\n";
    f << "    " << JStr("submission_mode", submission_mode) << ",\n";
    f << "    " << JBool("serial_data_drains",
                         cfg.serial_data_drains) << ",\n";
    f << "    " << JBool("enable_two_level_coarse_routing",
                         cfg.enable_two_level_coarse_routing) << ",\n";
    f << "    " << JInt("two_level_coarse_threshold",
                        cfg.two_level_coarse_threshold) << ",\n";
    f << "    " << JInt("two_level_coarse_super_count",
                        cfg.two_level_coarse_super_count) << ",\n";
    f << "    " << JInt("two_level_coarse_super_factor",
                        cfg.two_level_coarse_super_factor) << ",\n";
    f << "    " << JInt("two_level_coarse_budget_factor",
                        cfg.two_level_coarse_budget_factor) << ",\n";
    f << "    " << JInt("two_level_coarse_budget_cap",
                        cfg.two_level_coarse_budget_cap) << ",\n";
    f << "    " << JBool("enable_two_level_coarse_exact_overlap",
                         cfg.enable_two_level_coarse_exact_overlap) << ",\n";
    f << "    " << JBool("recall_available", metrics.recall_available) << ",\n";
    f << "    " << JNum("recall_at_1", recall1) << ",\n";
    f << "    " << JNum("recall_at_5", recall5) << ",\n";
    f << "    " << JNum("recall_at_10", recall10) << ",\n";
    f << "    " << JNum("recall_at_k", recallk) << ",\n";
    f << "    " << JNum("avg_query_time_ms", avg_latency) << ",\n";
    f << "    " << JNum("p50_ms", p50) << ",\n";
    f << "    " << JNum("p95_ms", p95) << ",\n";
    f << "    " << JNum("p99_ms", p99) << ",\n";
    f << "    " << JNum("process_wall_ms", process_wall_ms) << ",\n";
    f << "    " << JNum("preload_wall_ms", preload_wall_ms) << ",\n";
    f << "    " << JNum("two_level_coarse_warmup_ms", two_level_coarse_warmup_ms) << ",\n";
    f << "    " << JStr("resident_preload_mode", index.segment().resident_preload_mode()) << ",\n";
    f << "    " << JInt("resident_preload_batch_size", static_cast<int64_t>(index.segment().resident_preload_batch_size())) << ",\n";
    f << "    " << JInt("preload_bytes", static_cast<int64_t>(index.segment().resident_preload_bytes())) << ",\n";
    f << "    " << JInt("resident_file_size_bytes", static_cast<int64_t>(index.segment().resident_file_size_bytes())) << ",\n";
    f << "    " << JInt("resident_file_buffer_bytes", static_cast<int64_t>(index.segment().resident_file_buffer_bytes())) << ",\n";
    f << "    " << JInt("resident_code_storage_bytes", static_cast<int64_t>(index.segment().resident_code_storage_bytes())) << ",\n";
    f << "    " << JInt("resident_decoded_address_bytes", static_cast<int64_t>(index.segment().resident_decoded_address_bytes())) << ",\n";
    f << "    " << JInt("resident_raw_address_bytes", static_cast<int64_t>(index.segment().resident_raw_address_bytes())) << ",\n";
    f << "    " << JInt("resident_parsed_address_duplicate_bytes", static_cast<int64_t>(index.segment().resident_parsed_address_duplicate_bytes())) << ",\n";
    f << "    " << JInt("resident_cluster_mem_bytes", static_cast<int64_t>(index.segment().resident_cluster_mem_bytes())) << ",\n";
    f << "    " << JInt("resident_parallel_view_bytes", static_cast<int64_t>(index.segment().resident_parallel_view_bytes())) << ",\n";
    f << "    " << JInt("smaps_after_preload_anon_huge_pages_kib", smaps_after_preload.anon_huge_pages_kib) << ",\n";
    f << "    " << JInt("smaps_after_preload_file_pmd_mapped_kib", smaps_after_preload.file_pmd_mapped_kib) << ",\n";
    f << "    " << JInt("smaps_anon_huge_pages_kib", smaps_after_queries.anon_huge_pages_kib) << ",\n";
    f << "    " << JInt("smaps_file_pmd_mapped_kib", smaps_after_queries.file_pmd_mapped_kib) << ",\n";
    f << "    " << JInt("exrabitq_storage_version", clu_version) << ",\n";
    f << "    " << JStr("exrabitq_storage_format", ExRaBitQStorageFormatName(clu_version)) << ",\n";
    f << "    " << JBool("rabitq_variable_precision_packed", packed_stage2) << ",\n";
    f << "    " << JStr("rabitq_estimator_mode",
                        std::string(RaBitQEstimatorModeName(
                            result_rabitq_cfg.estimator_mode))) << ",\n";
    f << "    " << JInt("rabitq_total_bits",
                        result_rabitq_cfg.effective_total_bits()) << ",\n";
    f << "    " << JInt("rabitq_stored_bits",
                        static_cast<int64_t>(stored_total_bits)) << ",\n";
    f << "    " << JInt("rabitq_active_bits",
                        static_cast<int64_t>(active_total_bits)) << ",\n";
    f << "    " << JInt("rabitq_resident_loaded_bits",
                        static_cast<int64_t>(resident_total_bits)) << ",\n";
    f << "    " << JBool("rabitq_layout_honors_active_bits",
                         layout_honors_active_ex_bits) << ",\n";
    f << "    " << JStr("rabitq_active_bits_mode",
                        active_bits_mode) << ",\n";
    f << "    " << JStr("rabitq_code_layout",
                        std::string(vdb::RaBitQExDataLayoutName(
                            result_rabitq_cfg.exdata_layout))) << ",\n";
    f << "    " << JStr("rabitq_effective_code_layout",
                        std::string(vdb::RaBitQExDataLayoutName(
                            result_layout))) << ",\n";
    f << "    " << JStr("rabitq_format_key",
		                        RaBitQFormatKey(result_rabitq_cfg)) << ",\n";
    f << "    " << JStr("dynamic_safein_mode",
                        cfg.dynamic_safein_mode == DynamicSafeInMode::Frontier
                            ? "frontier"
                            : "static") << ",\n";
    f << "    " << JInt("dynamic_safein_min_probes",
                        cfg.dynamic_safein_min_probes) << ",\n";
    f << "    " << JInt("dynamic_safein_stable_probes",
                        cfg.dynamic_safein_stable_probes) << ",\n";
    f << "    " << JNum("dynamic_safein_rel_tol",
                        cfg.dynamic_safein_rel_tol) << ",\n";
    f << "    " << JNum("dynamic_safein_abs_tol",
                        cfg.dynamic_safein_abs_tol) << ",\n";
    f << "    " << JInt("dynamic_safein_defer_initial_clusters",
                        cfg.dynamic_safein_defer_initial_clusters) << ",\n";
    f << "    " << JBool("dynamic_safein_defer_until_ready",
                         cfg.dynamic_safein_defer_until_ready) << ",\n";
    f << "    " << JInt("dynamic_safein_defer_max_candidates",
                        cfg.dynamic_safein_defer_max_candidates) << ",\n";
    f << "    " << JInt("non_safeout_candidate_budget",
                        cfg.non_safeout_candidate_budget) << ",\n";
    f << "    " << JBool("safein_as_vec_only",
                         cfg.safein_as_vec_only) << ",\n";
    f << "    " << JInt("safein_all_threshold_bytes",
                        cfg.safein_all_threshold) << ",\n";
    f << "    " << JInt("safein_prefetch_max_count",
                        cfg.safein_prefetch_max_count) << ",\n";
    f << "    " << JInt("safein_prefetch_max_bytes",
                        static_cast<int64_t>(cfg.safein_prefetch_max_bytes)) << ",\n";
    f << "    " << JInt("safein_prefetch_emit_quantum",
                        cfg.safein_prefetch_emit_quantum) << ",\n";
    f << "    " << JInt("budgeted_prefetch_limit",
                        cfg.budgeted_prefetch_limit) << ",\n";
    f << "    " << JNum("loaded_safeout_epsilon",
                        loaded_safeout_epsilon) << ",\n";
    f << "    " << JNum("runtime_safeout_epsilon",
                        runtime_safeout_epsilon) << ",\n";
    f << "    " << JNum("safeout_epsilon_percentile",
                        safeout_epsilon_percentile) << ",\n";
    f << "    " << JInt("epsilon_requested_samples",
                        epsilon_samples) << ",\n";
    f << "    " << JStr("epsilon_sampling_mode",
                        EpsilonSamplingModeName(epsilon_sampling_mode)) << ",\n";
    f << "    " << JStr("safeout_epsilon_cache",
                        safeout_epsilon_cache) << ",\n";
    f << "    " << JBool("safeout_epsilon_cache_hit",
                         runtime_safeout_epsilon_cache_hit) << ",\n";
    f << "    " << JBool("safeout_epsilon_overridden",
                         runtime_safeout_epsilon_overridden) << ",\n";
    f << "    " << JInt("safeout_epsilon_valid_error_count",
                        safeout_epsilon_stats.valid_error_count) << ",\n";
    f << "    " << JInt("safeout_epsilon_attempted_pairs",
                        safeout_epsilon_stats.attempted_pairs) << ",\n";
    f << "    " << JStr("rabitq_validation_mode",
                        RaBitQValidationModeName(rabitq_validation_mode)) << "\n";
    f << "  },\n";
    f << "  \"pipeline_stats\": {\n";
    f << "    " << JNum("avg_total_probed", metrics.total_probed * inv_q) << ",\n";
    f << "    " << JNum("avg_safe_in", metrics.total_safe_in * inv_q) << ",\n";
    f << "    " << JNum("avg_safe_out", metrics.total_safe_out * inv_q) << ",\n";
    f << "    " << JNum("avg_uncertain", metrics.total_uncertain * inv_q) << ",\n";
    f << "    " << JNum("avg_s2_safe_in", metrics.s2_safe_in * inv_q) << ",\n";
    f << "    " << JNum("avg_s2_safe_out", metrics.s2_safe_out * inv_q) << ",\n";
    f << "    " << JNum("avg_s2_uncertain", metrics.s2_uncertain * inv_q) << ",\n";
    f << "    " << JNum("avg_candidates_reranked", metrics.candidates_reranked * inv_q) << ",\n";
    f << "    " << JNum("avg_vec_only_read_requests", metrics.vec_only_reads * inv_q) << ",\n";
    f << "    " << JNum("avg_all_read_requests", metrics.all_reads * inv_q) << ",\n";
    f << "    " << JNum("avg_payload_read_requests", metrics.payload_reads * inv_q) << ",\n";
    f << "    " << JNum("avg_vec_only_read_bytes", metrics.vec_only_read_bytes * inv_q) << ",\n";
    f << "    " << JNum("avg_all_read_bytes", metrics.all_read_bytes * inv_q) << ",\n";
    f << "    " << JNum("avg_payload_read_bytes", metrics.payload_read_bytes * inv_q) << ",\n";
    f << "    " << JNum("avg_total_read_bytes",
                        (metrics.vec_only_read_bytes + metrics.all_read_bytes +
                         metrics.payload_read_bytes) * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_considered",
                        metrics.budgeted_prefetch_considered * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_scheduled",
                        metrics.budgeted_prefetch_scheduled * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_duplicates",
                        metrics.budgeted_prefetch_duplicates * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_skipped_limit",
                        metrics.budgeted_prefetch_skipped_limit * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_completed",
                        metrics.budgeted_prefetch_completed * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_cache_hits",
                        metrics.budgeted_prefetch_cache_hits * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_inflight_uses",
                        metrics.budgeted_prefetch_inflight_uses * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_used",
                        metrics.budgeted_prefetch_used * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_wasted",
                        metrics.budgeted_prefetch_wasted * inv_q) << ",\n";
    f << "    " << JNum("avg_budgeted_prefetch_read_bytes",
                        metrics.budgeted_prefetch_read_bytes * inv_q) << ",\n";
    f << "    " << JNum("avg_safein_prefetch_considered",
                        metrics.safein_prefetch_considered * inv_q) << ",\n";
    f << "    " << JNum("avg_safein_prefetch_candidates",
                        metrics.safein_prefetch_candidates * inv_q) << ",\n";
    f << "    " << JNum("avg_safein_prefetch_true_topk",
                        metrics.safein_prefetch_true_topk * inv_q) << ",\n";
    f << "    " << JNum("avg_safein_prefetch_false",
                        metrics.safein_prefetch_false * inv_q) << ",\n";
    f << "    " << JNum("avg_safein_prefetch_unknown",
                        metrics.safein_prefetch_unknown * inv_q) << ",\n";
    f << "    " << JNum("safein_prefetch_false_rate",
                        metrics.safein_prefetch_candidates > 0
                            ? static_cast<double>(metrics.safein_prefetch_false) /
                                  static_cast<double>(metrics.safein_prefetch_candidates)
                            : 0.0) << ",\n";
    f << "    " << JNum("safein_prefetch_topk_coverage",
                        (metrics.recall_available && topk > 0)
                            ? static_cast<double>(metrics.safein_prefetch_true_topk) /
                                  static_cast<double>(Q * topk)
                            : 0.0) << ",\n";
    f << "    " << JNum("avg_safein_prefetch_skipped_count_limit",
                        metrics.safein_prefetch_skipped_count_limit * inv_q) << ",\n";
    f << "    " << JNum("avg_safein_prefetch_skipped_byte_limit",
                        metrics.safein_prefetch_skipped_byte_limit * inv_q) << ",\n";
    f << "    " << JNum("avg_safein_prefetch_scheduled_bytes",
                        metrics.safein_prefetch_scheduled_bytes * inv_q) << ",\n";
    f << "    " << JNum("avg_dynamic_safein_clusters",
                        metrics.dynamic_safein_clusters * inv_q) << ",\n";
    f << "    " << JNum("avg_dynamic_safein_active_clusters",
                        metrics.dynamic_safein_active_clusters * inv_q) << ",\n";
    f << "    " << JNum("avg_dynamic_safein_disabled_clusters",
                        metrics.dynamic_safein_disabled_clusters * inv_q) << ",\n";
    f << "    " << JNum("avg_dynamic_safein_ready_transitions",
                        metrics.dynamic_safein_ready_transitions * inv_q) << ",\n";
    f << "    " << JNum("avg_dynamic_safein_deferred_candidates",
                        metrics.dynamic_safein_deferred_candidates * inv_q) << ",\n";
    f << "    " << JNum("avg_dynamic_safein_deferred_flushes",
                        metrics.dynamic_safein_deferred_flushes * inv_q) << ",\n";
    f << "    " << JNum("avg_dynamic_safein_deferred_safein",
                        metrics.dynamic_safein_deferred_safein * inv_q) << ",\n";
    f << "    " << JNum("avg_candidate_budget_seen",
                        metrics.candidate_budget_seen * inv_q) << ",\n";
    f << "    " << JNum("avg_candidate_budget_selected",
                        metrics.candidate_budget_selected * inv_q) << ",\n";
    f << "    " << JNum("avg_candidate_budget_dropped",
                        metrics.candidate_budget_dropped * inv_q) << ",\n";
    f << "    " << JNum("avg_unique_fetch_candidates",
                        metrics.unique_fetch_candidates * inv_q) << ",\n";
    f << "    " << JNum("avg_total_io_submitted",
                        metrics.total_io_submitted * inv_q) << ",\n";
    f << "    " << JNum("avg_total_submit_calls",
                        metrics.total_submit_calls * inv_q) << ",\n";
    f << "    " << JNum("avg_total_submit_window_flushes",
                        metrics.total_submit_window_flushes * inv_q) << ",\n";
    f << "    " << JNum("avg_total_submit_window_requests",
                        metrics.total_submit_window_requests * inv_q) << ",\n";
    f << "    " << JNum("avg_separate_store_lookup_misses",
                        metrics.separate_store_lookup_misses * inv_q) << ",\n";
    f << "    " << JNum("avg_rabitq_active_bits_per_lane",
                        metrics.stage2_lanes_requested > 0
                            ? static_cast<double>(metrics.stage2_active_ex_bits_sum) /
                                      static_cast<double>(metrics.stage2_lanes_requested) +
                                  1.0
                            : 0.0) << ",\n";
    f << "    " << JNum("avg_rabitq_bitplane_lane_work",
                        metrics.stage2_active_ex_bits_sum * inv_q) << ",\n";
    f << "    " << JNum("avg_rabitq_stored_bits_per_lane",
                        metrics.stage2_lanes_requested > 0
                            ? static_cast<double>(metrics.stage2_stored_ex_bits_sum) /
                                      static_cast<double>(metrics.stage2_lanes_requested) +
                                  1.0
                            : 0.0) << ",\n";
    f << "    " << JNum("avg_probe_ms", metrics.probe_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_select_ms", metrics.coarse_select_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_score_ms", metrics.coarse_score_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_topn_ms", metrics.coarse_topn_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_routing_mode", metrics.coarse_routing_mode * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_super_count", metrics.coarse_super_count * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_super_probes", metrics.coarse_super_probes * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_child_candidates_scored", metrics.coarse_child_candidates_scored * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_candidate_budget", metrics.coarse_candidate_budget * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_exact_fallback", metrics.coarse_exact_fallback * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_exact_overlap", metrics.coarse_exact_overlap * inv_q) << ",\n";
    f << "    " << JNum("avg_coarse_hierarchy_build_ms", metrics.coarse_hierarchy_build_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_stage1_ms", metrics.stage1_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_stage2_ms", metrics.stage2_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_stage2_decode_ms", metrics.stage2_decode_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_probe_submit_ms", metrics.submit_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_rerank_compute_ms", metrics.rerank_compute_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_fetch_missing_ms", metrics.fetch_missing_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_io_wait_ms", metrics.io_wait_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_final_drain_ms", metrics.final_drain_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_execute_buffered_ms", metrics.execute_buffered_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_collector_finalize_ms", metrics.collector_finalize_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_assemble_results_ms", metrics.assemble_results_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_search_unaccounted_ms", metrics.search_unaccounted_ms * inv_q) << ",\n";
    f << "    " << JNum("avg_stage2_lanes_requested", metrics.stage2_lanes_requested * inv_q) << ",\n";
    f << "    " << JNum("avg_stage2_decode_blocks", metrics.stage2_decode_blocks * inv_q) << ",\n";
    f << "    " << JNum("avg_stage2_decode_input_bytes", metrics.stage2_decode_input_bytes * inv_q) << ",\n";
    f << "    " << JNum("avg_stage2_decode_output_bytes", metrics.stage2_decode_output_bytes * inv_q) << "\n";
    f << "  },\n";
    f << "  \"rss_profile\": {\n";
    f << "    " << JInt("process_start_rss_kib", rss_start.rss_kib) << ",\n";
    f << "    " << JInt("process_start_hwm_kib", rss_start.hwm_kib) << ",\n";
    f << "    " << JInt("after_query_load_rss_kib", rss_after_query_load.rss_kib) << ",\n";
    f << "    " << JInt("after_gt_load_rss_kib", rss_after_gt_load.rss_kib) << ",\n";
    f << "    " << JInt("after_index_open_rss_kib", rss_after_index_open.rss_kib) << ",\n";
    f << "    " << JInt("after_preload_rss_kib", rss_after_preload.rss_kib) << ",\n";
    f << "    " << JInt("peak_during_queries_rss_kib", peak_during_query.rss_kib) << ",\n";
    f << "    " << JInt("after_queries_rss_kib", rss_after_queries.rss_kib) << ",\n";
    f << "    " << JInt("after_queries_hwm_kib", rss_after_queries.hwm_kib) << ",\n";
    f << "    " << JInt("online_index_resident_delta_kib", rss_after_preload.rss_kib - rss_after_query_load.rss_kib) << ",\n";
    f << "    " << JInt("query_peak_delta_kib", peak_during_query.rss_kib - rss_after_preload.rss_kib) << "\n";
    f << "  }\n";
    f << "}\n";
    f.close();

    Log("  Output: %s\n", json_path.c_str());
    if (separate_payload_fd >= 0) ::close(separate_payload_fd);
    if (separate_vector_fd >= 0) ::close(separate_vector_fd);
    return 0;
}
