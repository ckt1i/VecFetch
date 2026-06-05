#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/simd/popcount.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <queue>
#include <random>
#include <vector>

namespace vdb {
namespace rabitq {

namespace {

// ============================================================================
// kTightStart — per-ex_bits tight search start bounds
//
// Mirrors rabitqlib/quantization/rabitq_impl.hpp:263 (kTightStart).
// Used in BestRescaleFactorSlow to prune the lattice search start point;
// also consumed by ComputeTConst's inner 100-sample calls.
// ============================================================================
static constexpr double kTightStart[9] = {
    0.0,   // ex_bits=0 (unused)
    0.15,  // ex_bits=1
    0.20,  // ex_bits=2
    0.52,  // ex_bits=3
    0.59,  // ex_bits=4
    0.71,  // ex_bits=5
    0.75,  // ex_bits=6
    0.77,  // ex_bits=7
    0.81,  // ex_bits=8
};

// ============================================================================
// BestRescaleFactorSlow — per-vector optimal t via lattice search
//
// Mirrors rabitqlib/quantization/rabitq_impl.hpp:276 (best_rescale_factor).
// Used internally by ComputeTConst (100 samples during construction) and by
// EncodeSlow (full per-vector search path, for correctness validation).
// ============================================================================
static double BestRescaleFactorSlow(const float* abs_rot, uint32_t dim,
                                     uint32_t ex_bits) {
    constexpr double kEps = 1e-5;
    constexpr int kNEnum = 10;
    const int max_code = static_cast<int>((1u << ex_bits) - 1);

    double max_o = *std::max_element(abs_rot, abs_rot + dim);
    if (max_o < kEps) max_o = kEps;

    double t_end = static_cast<double>(max_code + kNEnum) / max_o;
    double t_start = t_end * kTightStart[ex_bits];

    std::vector<int> cur_code(dim);
    double sqr_denom = static_cast<double>(dim) * 0.25;
    double numer = 0.0;

    for (uint32_t i = 0; i < dim; ++i) {
        int c = static_cast<int>(t_start * abs_rot[i] + kEps);
        if (c > max_code) c = max_code;
        cur_code[i] = c;
        sqr_denom += static_cast<double>(c) * c + c;
        numer += (c + 0.5) * abs_rot[i];
    }

    using PQEntry = std::pair<double, uint32_t>;
    std::priority_queue<PQEntry, std::vector<PQEntry>, std::greater<PQEntry>> pq;
    for (uint32_t i = 0; i < dim; ++i) {
        if (abs_rot[i] > kEps && cur_code[i] < max_code) {
            pq.emplace(static_cast<double>(cur_code[i] + 1) / abs_rot[i], i);
        }
    }

    double max_ip = 0.0;
    double best_t = t_start;

    while (!pq.empty()) {
        auto [cur_t, idx] = pq.top();
        pq.pop();

        cur_code[idx]++;
        int updated = cur_code[idx];
        sqr_denom += 2.0 * updated;
        numer += abs_rot[idx];

        double cur_ip = numer / std::sqrt(sqr_denom);
        if (cur_ip > max_ip) {
            max_ip = cur_ip;
            best_t = cur_t;
        }

        if (updated < max_code) {
            double next_t = static_cast<double>(updated + 1) / abs_rot[idx];
            if (next_t < t_end) {
                pq.emplace(next_t, idx);
            }
        }
    }

    return best_t;
}

// ============================================================================
// ComputeTConst — precompute global rescale factor for fast quantization
//
// Mirrors rabitqlib/quantization/rabitq_impl.hpp:363 (get_const_scaling_factors).
// Samples kSamples random unit Gaussian vectors, runs BestRescaleFactorSlow
// on each, and returns the mean. Called once in the constructor for bits > 1.
// ============================================================================
static double ComputeTConst(uint32_t dim, uint32_t ex_bits, uint64_t seed) {
    constexpr int kSamples = 100;
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> gauss(0.0, 1.0);

    std::vector<float> v(dim);
    double sum_t = 0.0;

    for (int s = 0; s < kSamples; ++s) {
        // Sample random Gaussian vector
        double norm_sq = 0.0;
        for (uint32_t i = 0; i < dim; ++i) {
            double x = gauss(rng);
            v[i] = static_cast<float>(x);
            norm_sq += x * x;
        }
        // Normalize and take abs
        double inv_norm = 1.0 / std::sqrt(norm_sq);
        for (uint32_t i = 0; i < dim; ++i) {
            v[i] = static_cast<float>(std::abs(v[i] * inv_norm));
        }
        sum_t += BestRescaleFactorSlow(v.data(), dim, ex_bits);
    }

    return sum_t / kSamples;
}

// ============================================================================
// FastQuantizeEx — O(L) quantization using precomputed t_const
//
// Mirrors rabitqlib/quantization/rabitq_impl.hpp:380 (faster_quantize_ex).
// Replaces per-vector lattice search in Encode() for bits > 1.
// Returns xipnorm = 1 / Σ (code[i]+0.5) * abs_rot[i].
// ============================================================================
static float FastQuantizeEx(const float* abs_rot, uint32_t dim, int max_code,
                             double t_const, uint8_t* out_ex_code) {
    constexpr double kEps = 1e-5;
    double ipnorm = 0.0;
    for (uint32_t i = 0; i < dim; ++i) {
        int c = static_cast<int>(t_const * abs_rot[i] + kEps);
        if (c > max_code) c = max_code;
        out_ex_code[i] = static_cast<uint8_t>(c);
        ipnorm += (c + 0.5) * abs_rot[i];
    }
    return (ipnorm > 1e-30) ? static_cast<float>(1.0 / ipnorm) : 1.0f;
}

static void PackExSignBits(const float* rotated,
                           uint32_t dim,
                           std::vector<uint8_t>& out_packed_sign) {
    out_packed_sign.assign((dim + 7) / 8, 0);
    for (uint32_t i = 0; i < dim; ++i) {
        if (rotated[i] >= 0.0f) {
            out_packed_sign[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
    }
}

}  // namespace

// ============================================================================
// Construction
// ============================================================================

RaBitQEncoder::RaBitQEncoder(Dim dim, const RotationMatrix& rotation,
                             uint8_t bits, uint64_t t_const_seed)
    : dim_(dim),
      bits_(bits),
      total_bits_(bits),
      ex_bits_(bits > 1 ? bits : 0),
      official_1_plus_n_(false),
      words_per_plane_((dim + 63) / 64),
      total_words_(static_cast<uint32_t>(bits) * ((dim + 63) / 64)),
      rotation_(rotation) {
    if (bits_ > 1) {
        max_code_ = static_cast<int>((1u << bits_) - 1);
        t_const_ = ComputeTConst(dim_, bits_, t_const_seed);
    }
}

RaBitQEncoder::RaBitQEncoder(Dim dim, const RotationMatrix& rotation,
                             const RaBitQConfig& config,
                             uint64_t t_const_seed)
    : RaBitQEncoder(dim, rotation, config.active_code_bits(), t_const_seed) {
    official_1_plus_n_ = config.uses_official_1_plus_n();
    total_bits_ = config.effective_total_bits();
    ex_bits_ = config.stage2_payload_bits();
    if (official_1_plus_n_ && ex_bits_ > 0 && bits_ == 1) {
        max_code_ = static_cast<int>((1u << ex_bits_) - 1u);
        t_const_ = ComputeTConst(dim_, ex_bits_, t_const_seed);
    }
}

// ============================================================================
// Encode — single vector
// ============================================================================

RaBitQCode RaBitQEncoder::Encode(const float* vec,
                                  const float* centroid) const {
    const size_t L = dim_;
    const uint8_t M = bits_;
    const uint32_t wpp = words_per_plane_;

    // Temporary buffers for the pipeline
    std::vector<float> residual(L);
    std::vector<float> rotated(L);

    // Step 1: Center — r = o - c
    if (centroid != nullptr) {
        for (size_t i = 0; i < L; ++i) {
            residual[i] = vec[i] - centroid[i];
        }
    } else {
        std::memcpy(residual.data(), vec, L * sizeof(float));
    }

    // Step 2: Compute norm ‖r‖₂
    float norm_sq = 0.0f;
    for (size_t i = 0; i < L; ++i) {
        norm_sq += residual[i] * residual[i];
    }
    float norm = std::sqrt(norm_sq);

    // Step 3: Normalize — ō = r / ‖r‖
    // Guard against zero-norm vectors (map to zero vector → all bits 0 after sign)
    if (norm > 1e-30f) {
        float inv_norm = 1.0f / norm;
        for (size_t i = 0; i < L; ++i) {
            residual[i] *= inv_norm;
        }
    }

    // Step 4: Rotate — ō' = P^T × ō
    rotation_.Apply(residual.data(), rotated.data());

    // Step 5 & 6: M-bit quantize and pack into bit-plane layout
    RaBitQCode result;
    result.code.resize(total_words_, 0ULL);
    result.norm = norm;
    result.bits = M;

    const uint32_t num_levels = 1u << M;  // 2^M
    const int max_code = static_cast<int>(num_levels - 1);

    if (M == 1) {
        // 1-bit: sign quantization (original path, unchanged)
        for (size_t i = 0; i < L; ++i) {
            if (rotated[i] >= 0.0f) {
                size_t word_idx = i / 64;
                size_t bit_idx  = i % 64;
                result.code[word_idx] |= (1ULL << bit_idx);
            }
        }
        if (official_1_plus_n_ && ex_bits_ > 0) {
            std::vector<float> abs_rot(L);
            for (size_t i = 0; i < L; ++i) {
                abs_rot[i] = std::abs(rotated[i]);
            }

            result.ex_code.resize(L);
            result.xipnorm = FastQuantizeEx(abs_rot.data(), static_cast<uint32_t>(L),
                                            max_code_, t_const_, result.ex_code.data());
            for (size_t i = 0; i < L; ++i) {
                if (rotated[i] < 0.0f) {
                    result.ex_code[i] =
                        static_cast<uint8_t>(max_code_ - result.ex_code[i]);
                }
            }
            result.ex_code_sign_folded = true;
            result.ex_sign_packed.clear();
            result.ex_factor_add = norm * norm;
            result.ex_factor_rescale = -2.0f * norm * result.xipnorm;
        }
    } else {
        // M-bit: sign-based bit-plane (for Stage 1) + fast_quantize ex_code (for Stage 2)

        // --- Bit-plane: same as original uniform quantization for Stage 1 ---
        const float scale = static_cast<float>(num_levels) * 0.5f;
        for (size_t i = 0; i < L; ++i) {
            float v = rotated[i];
            int bin = static_cast<int>(std::floor((v + 1.0f) * scale));
            bin = std::clamp(bin, 0, max_code);

            size_t word_idx = i / 64;
            size_t bit_idx  = i % 64;
            for (uint8_t p = 0; p < M; ++p) {
                int bit = (bin >> (M - 1 - p)) & 1;
                if (bit) {
                    result.code[static_cast<size_t>(p) * wpp + word_idx] |=
                        (1ULL << bit_idx);
                }
            }
        }

        // --- ExRaBitQ: fast_quantize for Stage 2 ---
        // Uses precomputed t_const_ (set in constructor) to skip per-vector
        // lattice search. O(L) path; see FastQuantizeEx / rabitq_impl.hpp:380.
        std::vector<float> abs_rot(L);
        for (size_t i = 0; i < L; ++i) {
            abs_rot[i] = std::abs(rotated[i]);
        }

        result.ex_code.resize(L);
        result.xipnorm = FastQuantizeEx(abs_rot.data(), static_cast<uint32_t>(L),
                                        max_code_, t_const_,
                                        result.ex_code.data());
        if (official_1_plus_n_) {
            for (size_t i = 0; i < L; ++i) {
                if (rotated[i] < 0.0f) {
                    result.ex_code[i] =
                        static_cast<uint8_t>(max_code_ - result.ex_code[i]);
                }
            }
            result.ex_code_sign_folded = true;
            result.ex_sign_packed.clear();
            result.ex_factor_add = norm * norm;
            result.ex_factor_rescale = -2.0f * norm * result.xipnorm;
        } else {
            PackExSignBits(rotated.data(), static_cast<uint32_t>(L),
                           result.ex_sign_packed);
        }
    }

    // Precompute sum_x = popcount(MSB plane only)
    result.sum_x = simd::PopcountTotal(result.code.data(), wpp);

    return result;
}

// ============================================================================
// EncodeSlow — original per-vector lattice search (reference / validation)
// ============================================================================

RaBitQCode RaBitQEncoder::EncodeSlow(const float* vec,
                                      const float* centroid) const {
    const size_t L = dim_;
    const uint8_t M = bits_;
    const uint32_t wpp = words_per_plane_;

    std::vector<float> residual(L);
    std::vector<float> rotated(L);

    // Steps 1-4 identical to Encode()
    if (centroid != nullptr) {
        for (size_t i = 0; i < L; ++i) residual[i] = vec[i] - centroid[i];
    } else {
        std::memcpy(residual.data(), vec, L * sizeof(float));
    }

    float norm_sq = 0.0f;
    for (size_t i = 0; i < L; ++i) norm_sq += residual[i] * residual[i];
    float norm = std::sqrt(norm_sq);

    if (norm > 1e-30f) {
        float inv_norm = 1.0f / norm;
        for (size_t i = 0; i < L; ++i) residual[i] *= inv_norm;
    }

    rotation_.Apply(residual.data(), rotated.data());

    RaBitQCode result;
    result.code.resize(total_words_, 0ULL);
    result.norm = norm;
    result.bits = M;

    const uint32_t num_levels = 1u << M;
    const int max_code = static_cast<int>(num_levels - 1);

    if (M == 1) {
        for (size_t i = 0; i < L; ++i) {
            if (rotated[i] >= 0.0f) {
                result.code[i / 64] |= (1ULL << (i % 64));
            }
        }
        if (official_1_plus_n_ && ex_bits_ > 0) {
            constexpr double kEps = 1e-5;
            std::vector<float> abs_rot(L);
            for (size_t i = 0; i < L; ++i) abs_rot[i] = std::abs(rotated[i]);

            double best_t = BestRescaleFactorSlow(abs_rot.data(),
                                                   static_cast<uint32_t>(L),
                                                   static_cast<uint32_t>(ex_bits_));

            result.ex_code.resize(L);
            double final_numer = 0.0;
            for (size_t i = 0; i < L; ++i) {
                int ca = static_cast<int>(best_t * abs_rot[i] + kEps);
                if (ca > max_code_) ca = max_code_;
                result.ex_code[i] = static_cast<uint8_t>(ca);
                final_numer += (ca + 0.5) * abs_rot[i];
            }
            result.xipnorm = (final_numer > 1e-30)
                            ? static_cast<float>(1.0 / final_numer) : 1.0f;
            for (size_t i = 0; i < L; ++i) {
                if (rotated[i] < 0.0f) {
                    result.ex_code[i] =
                        static_cast<uint8_t>(max_code_ - result.ex_code[i]);
                }
            }
            result.ex_code_sign_folded = true;
            result.ex_sign_packed.clear();
            result.ex_factor_add = norm * norm;
            result.ex_factor_rescale = -2.0f * norm * result.xipnorm;
        }
    } else {
        // Bit-plane (Stage 1) — same as Encode()
        const float scale = static_cast<float>(num_levels) * 0.5f;
        for (size_t i = 0; i < L; ++i) {
            float v = rotated[i];
            int bin = static_cast<int>(std::floor((v + 1.0f) * scale));
            bin = std::clamp(bin, 0, max_code);
            size_t word_idx = i / 64;
            size_t bit_idx  = i % 64;
            for (uint8_t p = 0; p < M; ++p) {
                if ((bin >> (M - 1 - p)) & 1) {
                    result.code[static_cast<size_t>(p) * wpp + word_idx] |=
                        (1ULL << bit_idx);
                }
            }
        }

        // ExRaBitQ Stage 2 — original per-vector lattice search
        constexpr double kEps = 1e-5;

        std::vector<float> abs_rot(L);
        for (size_t i = 0; i < L; ++i) abs_rot[i] = std::abs(rotated[i]);

        double best_t = BestRescaleFactorSlow(abs_rot.data(),
                                               static_cast<uint32_t>(L),
                                               static_cast<uint32_t>(M));

        result.ex_code.resize(L);
        double final_numer = 0.0;
        for (size_t i = 0; i < L; ++i) {
            int ca = static_cast<int>(best_t * abs_rot[i] + kEps);
            if (ca > max_code) ca = max_code;
            result.ex_code[i] = static_cast<uint8_t>(ca);
            final_numer += (ca + 0.5) * abs_rot[i];
        }
        result.xipnorm = (final_numer > 1e-30)
                        ? static_cast<float>(1.0 / final_numer) : 1.0f;
        if (official_1_plus_n_) {
            for (size_t i = 0; i < L; ++i) {
                if (rotated[i] < 0.0f) {
                    result.ex_code[i] =
                        static_cast<uint8_t>(max_code - result.ex_code[i]);
                }
            }
            result.ex_code_sign_folded = true;
            result.ex_sign_packed.clear();
            result.ex_factor_add = norm * norm;
            result.ex_factor_rescale = -2.0f * norm * result.xipnorm;
        } else {
            PackExSignBits(rotated.data(), static_cast<uint32_t>(L),
                           result.ex_sign_packed);
        }
    }

    result.sum_x = simd::PopcountTotal(result.code.data(), wpp);
    return result;
}

// ============================================================================
// EncodeBatch — multiple vectors
// ============================================================================

std::vector<RaBitQCode> RaBitQEncoder::EncodeBatch(
    const float* vecs, uint32_t n, const float* centroid) const {
    std::vector<RaBitQCode> results;
    results.reserve(n);

    for (uint32_t i = 0; i < n; ++i) {
        results.push_back(Encode(vecs + static_cast<size_t>(i) * dim_, centroid));
    }

    return results;
}

}  // namespace rabitq
}  // namespace vdb
