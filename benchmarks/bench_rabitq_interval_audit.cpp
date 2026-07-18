/// Offline correctness auditor for precision-conditioned RaBitQ intervals.
///
/// The auditor intentionally consumes the resident `.clu` representation and
/// the same TileLaneBitMajor SIMD kernel used by ClusterProber.  It therefore
/// audits the deployed partial-ExData estimator instead of rebuilding an
/// independent low-bit index or using the legacy diagnostic encoder.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/math/distributions/beta.hpp>

#include "vdb/index/ivf_index.h"
#include "vdb/io/vecs_reader.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/simd/distance_l2.h"
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/storage/pack_codes.h"

namespace fs = std::filesystem;
using vdb::Dim;
using vdb::Status;
using vdb::index::IvfIndex;

namespace {

constexpr uint32_t kFastScanBatch = 32;
constexpr uint32_t kStage2Batch = 8;

struct Args {
  std::string index_dir;
  std::string base_path;
  std::string query_path;
  std::string cluster_members_cache;
  std::string outdir = "./rabitq_interval_audit";
  uint32_t nprobe = 64;
  uint32_t topk = 10;
  uint32_t calibration_queries = 500;
  uint32_t holdout_queries = 500;
  uint32_t query_offset = 0;
  uint64_t split_seed = 20260718;
  uint32_t split_fold = 0;
  double alpha = 0.01;
  uint32_t semantic_check_blocks = 32;
};

struct CandidateRow {
  float exact = 0.0f;
  float scale = 0.0f;
  float s1 = 0.0f;
  std::array<float, 2> s2{}; // total active bits {3, 4}
};

struct CalibrationAccumulator {
  std::vector<float> pair_abs;
  std::vector<float> query_abs;
  std::vector<float> query_lower;
  std::vector<float> query_upper;
};

struct Envelope {
  std::string name;
  double epsilon_lower = 0.0;
  double epsilon_upper = 0.0;
};

struct EvalSummary {
  uint64_t queries = 0;
  uint64_t candidates = 0;
  uint64_t lower_pair_violations = 0;
  uint64_t upper_pair_violations = 0;
  uint64_t lower_query_violations = 0;
  uint64_t upper_query_violations = 0;
  uint64_t any_query_violations = 0;
  uint64_t safeout_candidates = 0;
  uint64_t harmful_safeout = 0;
  uint64_t harmful_safeout_queries = 0;
  long double interval_width_sum = 0.0L;
};

struct SemanticCheck {
  uint64_t blocks = 0;
  uint64_t lanes = 0;
  double max_ip_abs_diff = 0.0;
  double max_distance_abs_diff = 0.0;
};

struct CalibrationProfile {
  double pair_p99 = 0.0;
  double symmetric_pcve = 0.0;
  double asymmetric_lower = 0.0;
  double asymmetric_upper = 0.0;
  uint32_t symmetric_rank = 0;
  uint32_t directional_rank = 0;
};

void Log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vprintf(fmt, ap);
  va_end(ap);
  std::fflush(stdout);
}

std::string GetArg(int argc, char **argv, const char *name,
                   const std::string &fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0)
      return argv[i + 1];
  }
  return fallback;
}

uint32_t GetUIntArg(int argc, char **argv, const char *name,
                    uint32_t fallback) {
  const std::string value = GetArg(argc, argv, name, "");
  if (value.empty())
    return fallback;
  return static_cast<uint32_t>(std::stoul(value));
}

uint64_t GetUInt64Arg(int argc, char **argv, const char *name,
                      uint64_t fallback) {
  const std::string value = GetArg(argc, argv, name, "");
  if (value.empty())
    return fallback;
  return std::stoull(value);
}

double GetDoubleArg(int argc, char **argv, const char *name, double fallback) {
  const std::string value = GetArg(argc, argv, name, "");
  return value.empty() ? fallback : std::stod(value);
}

bool ParseArgs(int argc, char **argv, Args *args) {
  args->index_dir = GetArg(argc, argv, "--index-dir", "");
  args->base_path = GetArg(argc, argv, "--base", "");
  args->query_path = GetArg(argc, argv, "--query", "");
  args->cluster_members_cache =
      GetArg(argc, argv, "--cluster-members-cache", "");
  args->outdir = GetArg(argc, argv, "--outdir", args->outdir);
  args->nprobe = GetUIntArg(argc, argv, "--nprobe", args->nprobe);
  args->topk = GetUIntArg(argc, argv, "--topk", args->topk);
  args->calibration_queries = GetUIntArg(argc, argv, "--calibration-queries",
                                         args->calibration_queries);
  args->holdout_queries =
      GetUIntArg(argc, argv, "--holdout-queries", args->holdout_queries);
  args->query_offset =
      GetUIntArg(argc, argv, "--query-offset", args->query_offset);
  args->split_seed = GetUInt64Arg(argc, argv, "--split-seed", args->split_seed);
  args->split_fold = GetUIntArg(argc, argv, "--split-fold", args->split_fold);
  args->alpha = GetDoubleArg(argc, argv, "--alpha", args->alpha);
  args->semantic_check_blocks = GetUIntArg(
      argc, argv, "--semantic-check-blocks", args->semantic_check_blocks);
  return !args->index_dir.empty() && !args->base_path.empty() &&
         !args->query_path.empty() && !args->cluster_members_cache.empty() &&
         args->nprobe > 0 && args->topk > 0 && args->calibration_queries > 0 &&
         args->holdout_queries > 0 && args->alpha > 0.0 && args->alpha < 1.0;
}

Status
LoadClusterMembersCache(const std::string &path, uint32_t nlist,
                        std::vector<std::vector<uint32_t>> *cluster_members) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return Status::IOError("Failed to open cluster member cache: " + path);
  }
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t file_nlist = 0;
  in.read(reinterpret_cast<char *>(&magic), sizeof(magic));
  in.read(reinterpret_cast<char *>(&version), sizeof(version));
  in.read(reinterpret_cast<char *>(&file_nlist), sizeof(file_nlist));
  if (!in || magic != 0x4d434752u || version != 1 || file_nlist != nlist) {
    return Status::Corruption("Invalid RGCM header or nlist mismatch: " + path);
  }
  cluster_members->assign(nlist, {});
  for (uint32_t cid = 0; cid < nlist; ++cid) {
    uint32_t count = 0;
    in.read(reinterpret_cast<char *>(&count), sizeof(count));
    if (!in)
      return Status::Corruption("Truncated RGCM count table");
    auto &rows = (*cluster_members)[cid];
    rows.resize(count);
    if (count != 0) {
      in.read(reinterpret_cast<char *>(rows.data()),
              static_cast<std::streamsize>(count * sizeof(uint32_t)));
      if (!in)
        return Status::Corruption("Truncated RGCM row table");
    }
  }
  return Status::OK();
}

double Quantile(std::vector<float> *values, double probability) {
  if (values->empty())
    return 0.0;
  probability = std::clamp(probability, 0.0, 1.0);
  const size_t rank = static_cast<size_t>(
      std::max<double>(1.0, std::ceil(probability * values->size())));
  const size_t index = std::min(rank, values->size()) - 1;
  std::nth_element(values->begin(), values->begin() + index, values->end());
  return (*values)[index];
}

double FiniteSampleUpper(std::vector<float> *values, double alpha,
                         uint32_t *rank_out) {
  if (values->empty()) {
    *rank_out = 0;
    return 0.0;
  }
  const uint32_t n = static_cast<uint32_t>(values->size());
  const uint32_t rank = std::min<uint32_t>(
      n, static_cast<uint32_t>(
             std::ceil((static_cast<double>(n) + 1.0) * (1.0 - alpha))));
  *rank_out = rank;
  std::nth_element(values->begin(), values->begin() + (rank - 1),
                   values->end());
  return (*values)[rank - 1];
}

double ClopperPearsonUpper(uint64_t violations, uint64_t trials,
                           double confidence = 0.95) {
  if (trials == 0 || violations >= trials)
    return 1.0;
  boost::math::beta_distribution<double> distribution(
      static_cast<double>(violations + 1),
      static_cast<double>(trials - violations));
  return boost::math::quantile(distribution, confidence);
}

bool ValidateClusterMemberOrder(uint32_t cid,
                                const vdb::query::ParsedCluster &pc,
                                const std::vector<uint32_t> &members,
                                const vdb::io::NpyArrayFloat &base,
                                const float *centroid, Dim dim,
                                std::string *error) {
  if (members.size() != pc.num_records) {
    *error = "cluster " + std::to_string(cid) +
             " member count mismatch: cache=" + std::to_string(members.size()) +
             " clu=" + std::to_string(pc.num_records);
    return false;
  }
  if (members.empty())
    return true;
  const std::array<uint32_t, 3> samples = {
      0u, static_cast<uint32_t>(members.size() / 2),
      static_cast<uint32_t>(members.size() - 1)};
  for (uint32_t local : samples) {
    const uint32_t row = members[local];
    if (row >= base.rows) {
      *error = "cluster cache row out of range";
      return false;
    }
    const float *vec = base.data.data() + static_cast<size_t>(row) * dim;
    const float exact_norm = std::sqrt(vdb::simd::L2Sqr(vec, centroid, dim));
    const float stored_norm =
        pc.norm_oc(local / kFastScanBatch, local % kFastScanBatch);
    const float tolerance = 2e-4f * std::max(1.0f, exact_norm);
    if (std::abs(exact_norm - stored_norm) > tolerance) {
      *error = "cluster " + std::to_string(cid) +
               " member order/norm mismatch at local=" + std::to_string(local) +
               " row=" + std::to_string(row) +
               " exact_norm=" + std::to_string(exact_norm) +
               " stored_norm=" + std::to_string(stored_norm);
      return false;
    }
  }
  return true;
}

class QueryComputer {
public:
  QueryComputer(IvfIndex *index, const vdb::io::NpyArrayFloat *base,
                const std::vector<std::vector<uint32_t>> *cluster_members,
                uint32_t nprobe, uint32_t semantic_check_limit)
      : index_(*index), base_(*base), cluster_members_(*cluster_members),
        nprobe_(nprobe), semantic_check_limit_(semantic_check_limit),
        estimator_(index->dim(),
                   index->segment().rabitq_config().active_code_bits()),
        validated_clusters_(index->nlist(), false) {}

  Status Compute(const float *query, std::vector<CandidateRow> *rows) {
    rows->clear();
    const Dim dim = index_.dim();
    const auto probed = index_.FindNearestClusters(query, nprobe_);
    for (uint32_t cid : probed) {
      const vdb::query::ParsedCluster *pc =
          index_.segment().GetResidentParsedCluster(cid);
      if (pc == nullptr) {
        return Status::Corruption("Missing resident ParsedCluster");
      }
      if (!pc->uses_official_1_plus_n() || pc->rabitq_ex_bits != 3 ||
          pc->rabitq_exdata_layout !=
              vdb::RaBitQExDataLayout::kTileLaneBitMajor) {
        return Status::InvalidArgument(
            "Auditor requires official stored-4-bit TileLaneBitMajor index");
      }
      const auto &members = cluster_members_[cid];
      if (!validated_clusters_[cid]) {
        std::string error;
        if (!ValidateClusterMemberOrder(cid, *pc, members, base_,
                                        index_.centroid(cid), dim, &error)) {
          return Status::Corruption(error);
        }
        validated_clusters_[cid] = true;
        ++validated_cluster_count_;
      }

      vdb::rabitq::PreparedQuery pq;
      vdb::rabitq::ClusterPreparedScratch scratch;
      estimator_.PrepareQueryInto(query, index_.centroid(cid),
                                  index_.rotation(), &pq, &scratch);
      vdb::rabitq::PreparedClusterQueryView view;
      view.prepared = &pq;
      view.scratch = &scratch;
      view.safein_margin_factor = 0.0f;
      view.safeout_margin_factor = 0.0f;

      const uint32_t cluster_begin = static_cast<uint32_t>(rows->size());
      rows->resize(rows->size() + pc->num_records);
      std::vector<float> ip_x0(pc->num_records, 0.0f);
      const uint32_t packed_size = vdb::storage::FastScanPackedSize(dim);
      for (uint32_t block = 0; block < pc->num_fastscan_blocks; ++block) {
        const uint32_t local_begin = block * kFastScanBatch;
        const uint32_t count =
            std::min<uint32_t>(kFastScanBatch, pc->num_records - local_begin);
        const uint8_t *block_ptr =
            pc->fastscan_blocks +
            static_cast<size_t>(block) * pc->fastscan_block_size;
        const float *norms =
            reinterpret_cast<const float *>(block_ptr + packed_size);
        alignas(64) float distances[kFastScanBatch] = {};
        const auto eval = estimator_.EvaluateStage1FastScan(
            view, block_ptr, norms, count,
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(), false, distances);
        for (uint32_t lane = 0; lane < count; ++lane) {
          (*rows)[cluster_begin + local_begin + lane].s1 = distances[lane];
          ip_x0[local_begin + lane] = eval.ip_x0_qr[lane];
        }
      }

      for (uint32_t block = 0; block < pc->exrabitq_num_batch_blocks; ++block) {
        const auto block_view = pc->exrabitq_batch_block_view(block);
        if (block_view.abs_blocks == nullptr ||
            block_view.official_factor_adds == nullptr ||
            block_view.official_factor_rescales == nullptr) {
          return Status::Corruption("Missing official Stage2 block data");
        }
        const uint32_t local_begin = block * kStage2Batch;
        const uint32_t mask = block_view.valid_count >= 32
                                  ? 0xffffffffu
                                  : ((1u << block_view.valid_count) - 1u);
        for (uint32_t bit_index = 0; bit_index < 2; ++bit_index) {
          const uint8_t active_ex_bits =
              static_cast<uint8_t>(bit_index == 0 ? 2 : 3);
          alignas(64) float ip_ex[kStage2Batch] = {};
          vdb::simd::IPOfficialRaBitQBatchCompactTileLaneBitMajorMasked(
              pq.rotated.data(), block_view.abs_blocks,
              block_view.loaded_magnitude_bits, mask, block_view.valid_count,
              dim, pc->exrabitq_dim_block, ip_ex, nullptr, active_ex_bits);

          for (uint32_t lane = 0; lane < block_view.valid_count; ++lane) {
            const uint32_t local = local_begin + lane;
            if (local >= pc->num_records) {
              return Status::Corruption("Stage2 local index overflow");
            }
            const float normalized_ip =
                vdb::simd::OfficialRaBitQCombineNormalizedIP(
                    ip_x0[local], ip_ex[lane], pq.sum_q, pc->rabitq_ex_bits);
            (*rows)[cluster_begin + local].s2[bit_index] =
                vdb::simd::OfficialRaBitQEstimateDistance(
                    pq.norm_qc_sq, block_view.official_factor_adds[lane],
                    block_view.official_factor_rescales[lane],
                    pq.norm_qc * normalized_ip);
          }

          if (semantic_[bit_index].blocks < semantic_check_limit_) {
            Status semantic_status = CheckScalarBlock(
                pq, *pc, block_view, ip_x0.data() + local_begin, ip_ex,
                active_ex_bits, &semantic_[bit_index]);
            if (!semantic_status.ok())
              return semantic_status;
          }
        }
      }

      for (uint32_t local = 0; local < pc->num_records; ++local) {
        const uint32_t row = members[local];
        CandidateRow &candidate = (*rows)[cluster_begin + local];
        const float *vec = base_.data.data() + static_cast<size_t>(row) * dim;
        candidate.exact = vdb::simd::L2Sqr(query, vec, dim);
        const float norm_oc =
            pc->norm_oc(local / kFastScanBatch, local % kFastScanBatch);
        candidate.scale = 2.0f * pq.norm_qc * norm_oc;
      }
    }
    return Status::OK();
  }

  const std::array<SemanticCheck, 2> &semantic() const { return semantic_; }
  uint64_t validated_cluster_count() const { return validated_cluster_count_; }

private:
  Status CheckScalarBlock(
      const vdb::rabitq::PreparedQuery &pq, const vdb::query::ParsedCluster &pc,
      const vdb::query::ParsedCluster::ExRaBitQBatchBlockView &block_view,
      const float *ip_x0, const float *simd_ip, uint8_t active_ex_bits,
      SemanticCheck *result) const {
    const uint8_t active_mask =
        static_cast<uint8_t>((1u << active_ex_bits) - 1u);
    std::vector<uint8_t> decoded(pc.dim);
    for (uint32_t lane = 0; lane < block_view.valid_count; ++lane) {
      if (!vdb::simd::ExRaBitQUnpackOfficialTileLaneBitMajor(
              block_view.abs_blocks, block_view.valid_count, lane, pc.dim,
              block_view.loaded_magnitude_bits, decoded.data())) {
        return Status::Corruption("Failed to decode TileLaneBitMajor block");
      }
      double scalar_ip = 0.0;
      for (uint32_t d = 0; d < pc.dim; ++d) {
        scalar_ip += static_cast<double>(pq.rotated[d]) *
                     static_cast<double>(decoded[d] & active_mask);
      }
      const double ip_diff = std::abs(scalar_ip - simd_ip[lane]);
      result->max_ip_abs_diff = std::max(result->max_ip_abs_diff, ip_diff);
      const float scalar_normalized =
          vdb::simd::OfficialRaBitQCombineNormalizedIP(
              ip_x0[lane], static_cast<float>(scalar_ip), pq.sum_q,
              pc.rabitq_ex_bits);
      const float simd_normalized =
          vdb::simd::OfficialRaBitQCombineNormalizedIP(
              ip_x0[lane], simd_ip[lane], pq.sum_q, pc.rabitq_ex_bits);
      const float scalar_distance = vdb::simd::OfficialRaBitQEstimateDistance(
          pq.norm_qc_sq, block_view.official_factor_adds[lane],
          block_view.official_factor_rescales[lane],
          pq.norm_qc * scalar_normalized);
      const float simd_distance = vdb::simd::OfficialRaBitQEstimateDistance(
          pq.norm_qc_sq, block_view.official_factor_adds[lane],
          block_view.official_factor_rescales[lane],
          pq.norm_qc * simd_normalized);
      result->max_distance_abs_diff = std::max(
          result->max_distance_abs_diff,
          static_cast<double>(std::abs(scalar_distance - simd_distance)));
      ++result->lanes;
    }
    ++result->blocks;
    if (result->max_ip_abs_diff > 2e-3 ||
        result->max_distance_abs_diff > 2e-3) {
      return Status::Corruption(
          "Partial-bit SIMD/scalar semantic check exceeded tolerance");
    }
    return Status::OK();
  }

  IvfIndex &index_;
  const vdb::io::NpyArrayFloat &base_;
  const std::vector<std::vector<uint32_t>> &cluster_members_;
  uint32_t nprobe_;
  uint32_t semantic_check_limit_;
  vdb::rabitq::RaBitQEstimator estimator_;
  std::vector<bool> validated_clusters_;
  uint64_t validated_cluster_count_ = 0;
  std::array<SemanticCheck, 2> semantic_{};
};

void AddCalibrationQuery(const std::vector<CandidateRow> &rows,
                         int estimator_id,
                         CalibrationAccumulator *accumulator) {
  float max_abs = 0.0f;
  float max_lower = 0.0f;
  float max_upper = 0.0f;
  for (const CandidateRow &row : rows) {
    const float estimate = estimator_id < 0 ? row.s1 : row.s2[estimator_id];
    const float denom = std::max(1e-20f, row.scale);
    const float signed_error = (estimate - row.exact) / denom;
    const float abs_error = std::abs(signed_error);
    accumulator->pair_abs.push_back(abs_error);
    max_abs = std::max(max_abs, abs_error);
    max_lower = std::max(max_lower, std::max(0.0f, signed_error));
    max_upper = std::max(max_upper, std::max(0.0f, -signed_error));
  }
  accumulator->query_abs.push_back(max_abs);
  accumulator->query_lower.push_back(max_lower);
  accumulator->query_upper.push_back(max_upper);
}

CalibrationProfile BuildProfile(CalibrationAccumulator *accumulator,
                                double symmetric_alpha,
                                double directional_alpha) {
  CalibrationProfile profile;
  profile.pair_p99 = Quantile(&accumulator->pair_abs, 0.99);
  profile.symmetric_pcve = FiniteSampleUpper(
      &accumulator->query_abs, symmetric_alpha, &profile.symmetric_rank);
  uint32_t lower_rank = 0;
  uint32_t upper_rank = 0;
  profile.asymmetric_lower = FiniteSampleUpper(&accumulator->query_lower,
                                               directional_alpha, &lower_rank);
  profile.asymmetric_upper = FiniteSampleUpper(&accumulator->query_upper,
                                               directional_alpha, &upper_rank);
  profile.directional_rank = std::max(lower_rank, upper_rank);
  return profile;
}

float KthSmallest(std::vector<float> *values, uint32_t topk) {
  const uint32_t k =
      std::min<uint32_t>(topk, static_cast<uint32_t>(values->size()));
  if (k == 0)
    return std::numeric_limits<float>::infinity();
  std::nth_element(values->begin(), values->begin() + (k - 1), values->end());
  return (*values)[k - 1];
}

void EvaluateQuery(const std::vector<CandidateRow> &rows, int estimator_id,
                   const Envelope &envelope, uint32_t topk,
                   EvalSummary *summary) {
  ++summary->queries;
  summary->candidates += rows.size();
  bool lower_query_violation = false;
  bool upper_query_violation = false;
  std::vector<float> upper;
  std::vector<float> exact;
  upper.reserve(rows.size());
  exact.reserve(rows.size());
  for (const CandidateRow &row : rows) {
    const float estimate = estimator_id < 0 ? row.s1 : row.s2[estimator_id];
    const float lower = estimate - row.scale * envelope.epsilon_lower;
    const float upper_value = estimate + row.scale * envelope.epsilon_upper;
    const float tolerance = 2e-5f * std::max(1.0f, row.exact);
    if (lower > row.exact + tolerance) {
      ++summary->lower_pair_violations;
      lower_query_violation = true;
    }
    if (upper_value + tolerance < row.exact) {
      ++summary->upper_pair_violations;
      upper_query_violation = true;
    }
    upper.push_back(upper_value);
    exact.push_back(row.exact);
    summary->interval_width_sum +=
        static_cast<long double>(row.scale) *
        (envelope.epsilon_lower + envelope.epsilon_upper);
  }
  summary->lower_query_violations += lower_query_violation ? 1 : 0;
  summary->upper_query_violations += upper_query_violation ? 1 : 0;
  summary->any_query_violations +=
      (lower_query_violation || upper_query_violation) ? 1 : 0;

  const float upper_frontier = KthSmallest(&upper, topk);
  const float exact_frontier = KthSmallest(&exact, topk);
  bool harmful_query = false;
  for (const CandidateRow &row : rows) {
    const float estimate = estimator_id < 0 ? row.s1 : row.s2[estimator_id];
    const float lower = estimate - row.scale * envelope.epsilon_lower;
    if (lower > upper_frontier) {
      ++summary->safeout_candidates;
      const float tolerance = 2e-5f * std::max(1.0f, exact_frontier);
      if (row.exact <= exact_frontier + tolerance) {
        ++summary->harmful_safeout;
        harmful_query = true;
      }
    }
  }
  summary->harmful_safeout_queries += harmful_query ? 1 : 0;
}

std::vector<Envelope> StageEnvelopes(const CalibrationProfile &profile,
                                     double current_epsilon) {
  return {
      {"current", current_epsilon, current_epsilon},
      {"pair_p99", profile.pair_p99, profile.pair_p99},
      {"symmetric_pcve", profile.symmetric_pcve, profile.symmetric_pcve},
      {"a_pcve", profile.asymmetric_lower, profile.asymmetric_upper},
  };
}

std::vector<Envelope> Stage2Envelopes(const CalibrationProfile &profile,
                                      const CalibrationProfile &full_profile,
                                      double current_epsilon) {
  return {
      {"current_inherited_full_B", current_epsilon, current_epsilon},
      {"shared_full_bit_pcve", full_profile.symmetric_pcve,
       full_profile.symmetric_pcve},
      {"pair_p99_per_bit", profile.pair_p99, profile.pair_p99},
      {"symmetric_pcve", profile.symmetric_pcve, profile.symmetric_pcve},
      {"a_pcve", profile.asymmetric_lower, profile.asymmetric_upper},
  };
}

void WriteEnvelopeJson(std::ofstream &out, const Envelope &envelope,
                       const EvalSummary &summary, bool comma) {
  const double candidates =
      static_cast<double>(std::max<uint64_t>(1, summary.candidates));
  const double queries =
      static_cast<double>(std::max<uint64_t>(1, summary.queries));
  const uint64_t verify = summary.candidates - summary.safeout_candidates;
  out << "        {\n";
  out << "          \"method\": \"" << envelope.name << "\",\n";
  out << "          \"queries\": " << summary.queries << ",\n";
  out << "          \"candidates\": " << summary.candidates << ",\n";
  out << "          \"epsilon_lower\": " << envelope.epsilon_lower << ",\n";
  out << "          \"epsilon_upper\": " << envelope.epsilon_upper << ",\n";
  out << "          \"lower_pair_violations\": "
      << summary.lower_pair_violations << ",\n";
  out << "          \"upper_pair_violations\": "
      << summary.upper_pair_violations << ",\n";
  out << "          \"lower_query_violations\": "
      << summary.lower_query_violations << ",\n";
  out << "          \"upper_query_violations\": "
      << summary.upper_query_violations << ",\n";
  out << "          \"any_query_violations\": " << summary.any_query_violations
      << ",\n";
  out << "          \"query_violation_rate\": "
      << static_cast<double>(summary.any_query_violations) / queries << ",\n";
  out << "          \"query_violation_cp95_upper\": "
      << ClopperPearsonUpper(summary.any_query_violations, summary.queries)
      << ",\n";
  out << "          \"safeout_candidates\": " << summary.safeout_candidates
      << ",\n";
  out << "          \"verification_candidates\": " << verify << ",\n";
  out << "          \"verification_fraction\": "
      << static_cast<double>(verify) / candidates << ",\n";
  out << "          \"harmful_safeout\": " << summary.harmful_safeout << ",\n";
  out << "          \"harmful_safeout_queries\": "
      << summary.harmful_safeout_queries << ",\n";
  out << "          \"mean_interval_width\": "
      << static_cast<double>(summary.interval_width_sum / candidates) << "\n";
  out << "        }" << (comma ? "," : "") << "\n";
}

void WriteResults(const Args &args, IvfIndex &index, double elapsed_seconds,
                  uint64_t validated_clusters,
                  const std::array<SemanticCheck, 2> &semantic,
                  const CalibrationProfile &s1_profile,
                  const std::array<CalibrationProfile, 2> &s2_profile,
                  const std::vector<Envelope> &s1_envelopes,
                  const std::vector<EvalSummary> &s1_results,
                  const std::array<std::vector<Envelope>, 2> &s2_envelopes,
                  const std::array<std::vector<EvalSummary>, 2> &s2_results) {
  fs::create_directories(args.outdir);
  std::ofstream out(args.outdir + "/summary.json");
  out << std::setprecision(10);
  out << "{\n";
  out << "  \"index_dir\": \"" << args.index_dir << "\",\n";
  out << "  \"base\": \"" << args.base_path << "\",\n";
  out << "  \"query\": \"" << args.query_path << "\",\n";
  out << "  \"nprobe\": " << args.nprobe << ",\n";
  out << "  \"topk\": " << args.topk << ",\n";
  out << "  \"calibration_queries\": " << args.calibration_queries << ",\n";
  out << "  \"holdout_queries\": " << args.holdout_queries << ",\n";
  out << "  \"split_seed\": " << args.split_seed << ",\n";
  out << "  \"split_fold\": " << args.split_fold << ",\n";
  out << "  \"alpha_total\": " << args.alpha << ",\n";
  out << "  \"alpha_symmetric_per_stage\": " << args.alpha / 2.0 << ",\n";
  out << "  \"alpha_directional_component\": " << args.alpha / 4.0 << ",\n";
  out << "  \"stored_total_bits\": "
      << static_cast<uint32_t>(
             index.segment().rabitq_config().effective_total_bits())
      << ",\n";
  out << "  \"stored_ex_bits\": "
      << static_cast<uint32_t>(
             index.segment().rabitq_config().stage2_payload_bits())
      << ",\n";
  out << "  \"current_epsilon\": " << index.conann().epsilon() << ",\n";
  out << "  \"validated_cluster_count\": " << validated_clusters << ",\n";
  out << "  \"elapsed_seconds\": " << elapsed_seconds << ",\n";
  out << "  \"gate0_semantic_check\": {\n";
  for (uint32_t i = 0; i < 2; ++i) {
    out << "    \"b" << (i == 0 ? 3 : 4) << "\": {\n";
    out << "      \"blocks\": " << semantic[i].blocks << ",\n";
    out << "      \"lanes\": " << semantic[i].lanes << ",\n";
    out << "      \"max_ip_abs_diff\": " << semantic[i].max_ip_abs_diff
        << ",\n";
    out << "      \"max_distance_abs_diff\": "
        << semantic[i].max_distance_abs_diff << ",\n";
    out << "      \"passed\": "
        << ((semantic[i].max_ip_abs_diff <= 2e-3 &&
             semantic[i].max_distance_abs_diff <= 2e-3)
                ? "true"
                : "false")
        << "\n";
    out << "    }" << (i == 0 ? "," : "") << "\n";
  }
  out << "  },\n";
  out << "  \"profiles\": {\n";
  auto write_profile = [&](const char *name, const CalibrationProfile &p,
                           bool comma) {
    out << "    \"" << name << "\": {\n";
    out << "      \"pair_p99\": " << p.pair_p99 << ",\n";
    out << "      \"symmetric_pcve\": " << p.symmetric_pcve << ",\n";
    out << "      \"asymmetric_lower\": " << p.asymmetric_lower << ",\n";
    out << "      \"asymmetric_upper\": " << p.asymmetric_upper << ",\n";
    out << "      \"symmetric_rank\": " << p.symmetric_rank << ",\n";
    out << "      \"directional_rank\": " << p.directional_rank << "\n";
    out << "    }" << (comma ? "," : "") << "\n";
  };
  write_profile("stage1", s1_profile, true);
  write_profile("stage2_b3", s2_profile[0], true);
  write_profile("stage2_b4", s2_profile[1], false);
  out << "  },\n";
  out << "  \"heldout\": {\n";
  out << "    \"stage1\": [\n";
  for (size_t i = 0; i < s1_envelopes.size(); ++i) {
    WriteEnvelopeJson(out, s1_envelopes[i], s1_results[i],
                      i + 1 < s1_envelopes.size());
  }
  out << "    ],\n";
  for (uint32_t bit_index = 0; bit_index < 2; ++bit_index) {
    out << "    \"stage2_b" << (bit_index == 0 ? 3 : 4) << "\": [\n";
    for (size_t i = 0; i < s2_envelopes[bit_index].size(); ++i) {
      WriteEnvelopeJson(out, s2_envelopes[bit_index][i],
                        s2_results[bit_index][i],
                        i + 1 < s2_envelopes[bit_index].size());
    }
    out << "    ]" << (bit_index == 0 ? "," : "") << "\n";
  }
  out << "  }\n";
  out << "}\n";
}

} // namespace

int main(int argc, char **argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::fprintf(
        stderr,
        "Usage: bench_rabitq_interval_audit --index-dir DIR --base FILE "
        "--query FILE --cluster-members-cache FILE [--outdir DIR] "
        "[--nprobe 64] [--topk 10] [--calibration-queries 500] "
        "[--holdout-queries 500] [--query-offset 0] [--alpha 0.01]\n");
    return 2;
  }

  fs::create_directories(args.outdir);
  const auto start = std::chrono::steady_clock::now();
  IvfIndex index;
  Status status = index.Open(args.index_dir, false);
  if (!status.ok()) {
    std::fprintf(stderr, "Failed to open index: %s\n",
                 status.ToString().c_str());
    return 1;
  }
  const auto &config = index.segment().rabitq_config();
  if (!config.uses_official_1_plus_n() || config.effective_total_bits() != 4 ||
      config.stage2_payload_bits() != 3) {
    std::fprintf(stderr, "Expected official stored-4-bit (1+3) index\n");
    return 1;
  }
  Log("Preloading resident code regions (3 ExData bits)\n");
  status = index.segment().PreloadAllClusters(3);
  if (!status.ok()) {
    std::fprintf(stderr, "Failed to preload clusters: %s\n",
                 status.ToString().c_str());
    return 1;
  }

  Log("Loading base vectors: %s\n", args.base_path.c_str());
  auto base_or = vdb::io::LoadVectors(args.base_path);
  if (!base_or.ok()) {
    std::fprintf(stderr, "Failed to load base: %s\n",
                 base_or.status().ToString().c_str());
    return 1;
  }
  Log("Loading queries: %s\n", args.query_path.c_str());
  auto query_or = vdb::io::LoadVectors(args.query_path);
  if (!query_or.ok()) {
    std::fprintf(stderr, "Failed to load queries: %s\n",
                 query_or.status().ToString().c_str());
    return 1;
  }
  const auto &base = base_or.value();
  const auto &queries = query_or.value();
  const uint64_t requested_end = static_cast<uint64_t>(args.query_offset) +
                                 args.calibration_queries +
                                 args.holdout_queries;
  if (base.cols != index.dim() || queries.cols != index.dim() ||
      requested_end > queries.rows) {
    std::fprintf(stderr,
                 "Shape/query-range mismatch: base=(%u,%u) query=(%u,%u) "
                 "index_dim=%u requested_end=%llu\n",
                 base.rows, base.cols, queries.rows, queries.cols, index.dim(),
                 static_cast<unsigned long long>(requested_end));
    return 1;
  }

  std::vector<std::vector<uint32_t>> cluster_members;
  status = LoadClusterMembersCache(args.cluster_members_cache, index.nlist(),
                                   &cluster_members);
  if (!status.ok()) {
    std::fprintf(stderr, "Failed to load cluster members: %s\n",
                 status.ToString().c_str());
    return 1;
  }

  QueryComputer computer(&index, &base, &cluster_members, args.nprobe,
                         args.semantic_check_blocks);
  std::vector<uint32_t> query_ids(args.calibration_queries +
                                  args.holdout_queries);
  std::iota(query_ids.begin(), query_ids.end(), args.query_offset);
  std::mt19937_64 split_rng(args.split_seed);
  std::shuffle(query_ids.begin(), query_ids.end(), split_rng);
  if (args.split_fold > 1) {
    std::fprintf(stderr, "--split-fold must be 0 or 1\n");
    return 2;
  }
  if (args.split_fold == 1) {
    if (args.calibration_queries != args.holdout_queries) {
      std::fprintf(stderr,
                   "--split-fold 1 requires equal calibration/holdout sizes\n");
      return 2;
    }
    std::rotate(query_ids.begin(), query_ids.begin() + args.calibration_queries,
                query_ids.end());
  }
  CalibrationAccumulator s1_calibration;
  std::array<CalibrationAccumulator, 2> s2_calibration;
  const size_t reserve_pairs =
      static_cast<size_t>(args.calibration_queries) * args.nprobe * 512u;
  s1_calibration.pair_abs.reserve(reserve_pairs);
  for (auto &accumulator : s2_calibration) {
    accumulator.pair_abs.reserve(reserve_pairs);
  }
  std::vector<CandidateRow> rows;
  Log("Calibration: %u real queries, nprobe=%u\n", args.calibration_queries,
      args.nprobe);
  for (uint32_t local_q = 0; local_q < args.calibration_queries; ++local_q) {
    const uint32_t query_id = query_ids[local_q];
    const float *query =
        queries.data.data() + static_cast<size_t>(query_id) * index.dim();
    status = computer.Compute(query, &rows);
    if (!status.ok()) {
      std::fprintf(stderr, "Calibration query %u failed: %s\n", query_id,
                   status.ToString().c_str());
      return 1;
    }
    AddCalibrationQuery(rows, -1, &s1_calibration);
    AddCalibrationQuery(rows, 0, &s2_calibration[0]);
    AddCalibrationQuery(rows, 1, &s2_calibration[1]);
    if ((local_q + 1) % 25 == 0 || local_q + 1 == args.calibration_queries) {
      Log("  calibrated %u/%u queries (last candidates=%zu)\n", local_q + 1,
          args.calibration_queries, rows.size());
    }
  }

  const double symmetric_alpha = args.alpha / 2.0;   // S1 or S2 score
  const double directional_alpha = args.alpha / 4.0; // S1/S2 x L/U
  CalibrationProfile s1_profile =
      BuildProfile(&s1_calibration, symmetric_alpha, directional_alpha);
  std::array<CalibrationProfile, 2> s2_profile = {
      BuildProfile(&s2_calibration[0], symmetric_alpha, directional_alpha),
      BuildProfile(&s2_calibration[1], symmetric_alpha, directional_alpha)};

  const double current_epsilon = index.conann().epsilon();
  const std::vector<Envelope> s1_envelopes =
      StageEnvelopes(s1_profile, current_epsilon);
  std::array<std::vector<Envelope>, 2> s2_envelopes = {
      Stage2Envelopes(s2_profile[0], s2_profile[1], current_epsilon / 8.0),
      Stage2Envelopes(s2_profile[1], s2_profile[1], current_epsilon / 8.0)};
  std::vector<EvalSummary> s1_results(s1_envelopes.size());
  std::array<std::vector<EvalSummary>, 2> s2_results = {
      std::vector<EvalSummary>(s2_envelopes[0].size()),
      std::vector<EvalSummary>(s2_envelopes[1].size())};

  Log("Holdout: %u disjoint queries\n", args.holdout_queries);
  for (uint32_t local_q = 0; local_q < args.holdout_queries; ++local_q) {
    const uint32_t query_id = query_ids[args.calibration_queries + local_q];
    const float *query =
        queries.data.data() + static_cast<size_t>(query_id) * index.dim();
    status = computer.Compute(query, &rows);
    if (!status.ok()) {
      std::fprintf(stderr, "Holdout query %u failed: %s\n", query_id,
                   status.ToString().c_str());
      return 1;
    }
    for (size_t i = 0; i < s1_envelopes.size(); ++i) {
      EvaluateQuery(rows, -1, s1_envelopes[i], args.topk, &s1_results[i]);
    }
    for (uint32_t bit_index = 0; bit_index < 2; ++bit_index) {
      for (size_t i = 0; i < s2_envelopes[bit_index].size(); ++i) {
        EvaluateQuery(rows, static_cast<int>(bit_index),
                      s2_envelopes[bit_index][i], args.topk,
                      &s2_results[bit_index][i]);
      }
    }
    if ((local_q + 1) % 25 == 0 || local_q + 1 == args.holdout_queries) {
      Log("  audited %u/%u queries (last candidates=%zu)\n", local_q + 1,
          args.holdout_queries, rows.size());
    }
  }

  const double elapsed_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  WriteResults(args, index, elapsed_seconds, computer.validated_cluster_count(),
               computer.semantic(), s1_profile, s2_profile, s1_envelopes,
               s1_results, s2_envelopes, s2_results);
  Log("Completed in %.2f s; wrote %s/summary.json\n", elapsed_seconds,
      args.outdir.c_str());
  return 0;
}
