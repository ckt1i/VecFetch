#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_encoder.h"

namespace vdb {
namespace bench {

enum class SafeInDkSamplingMode {
    Unique,
    WithReplacement,
};

enum class EpsilonSamplingMode {
    LegacyPerCluster,
    GlobalPair,
};

struct EpsilonCalibrationStats {
    EpsilonSamplingMode sampling_mode = EpsilonSamplingMode::LegacyPerCluster;
    uint32_t requested_samples = 0;
    uint32_t valid_error_count = 0;
    uint32_t attempted_pairs = 0;
};

StatusOr<std::vector<uint32_t>> LoadAssignments(const std::string& path,
                                                uint32_t expected_rows);

void BuildClusterMembers(const std::vector<uint32_t>& assignments,
                         uint32_t nlist,
                         std::vector<std::vector<uint32_t>>* cluster_members);

bool ExtractPayloadId(const std::vector<Datum>& payload, int64_t* id);

Status RecoverClusterMembersFromIndex(
    index::IvfIndex& index,
    const std::unordered_map<int64_t, uint32_t>& image_id_to_row,
    std::vector<std::vector<uint32_t>>* cluster_members);

void EncodeAllCodes(const std::vector<float>& base_data,
                    uint32_t n,
                    Dim dim,
                    const std::vector<std::vector<uint32_t>>& cluster_members,
                    const std::vector<float>& centroids,
                    const rabitq::RotationMatrix& rotation,
                    uint8_t bits,
                    const std::vector<uint32_t>* cluster_subset,
                    std::vector<std::vector<rabitq::RaBitQCode>>* all_codes);

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
    const index::IvfIndex* index = nullptr,
    index::SafeInDkSearchScope search_scope =
        index::SafeInDkSearchScope::FullDatabase,
    uint32_t nprobe = 0);

float SelectSafeInDkFromSamples(const std::vector<float>& samples,
                                float percentile);

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
    const index::IvfIndex* index = nullptr,
    index::SafeInDkSearchScope search_scope =
        index::SafeInDkSearchScope::FullDatabase,
    uint32_t nprobe = 0);

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
    EpsilonSamplingMode sampling_mode =
        EpsilonSamplingMode::LegacyPerCluster,
    EpsilonCalibrationStats* stats = nullptr);

const char* EpsilonSamplingModeName(EpsilonSamplingMode mode);

StatusOr<EpsilonSamplingMode> ParseEpsilonSamplingModeArg(
    const std::string& value);

StatusOr<index::SafeInDkSearchScope> ParseSafeInDkSearchScopeArg(
    const std::string& value);

StatusOr<SafeInDkSamplingMode> ParseSafeInDkSamplingModeArg(
    const std::string& value);

}  // namespace bench
}  // namespace vdb
