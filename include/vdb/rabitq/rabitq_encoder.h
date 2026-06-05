#pragma once

#include <cstdint>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"
#include "vdb/rabitq/rabitq_rotation.h"

namespace vdb {
namespace rabitq {

// ============================================================================
// RaBitQ Encoded Vector — output of the encoder
// ============================================================================

/// A single vector encoded by RaBitQ M-bit quantization.
///
/// Contains:
///   - `code`:  Packed quantization codes in **bit-plane layout**.
///              For M-bit quantization, `code` has M planes of ceil(dim/64)
///              uint64_t words each:
///                plane 0 = MSB (= sign bits, equivalent to 1-bit code)
///                plane 1 = next bit
///                ...
///                plane M-1 = LSB
///              Total size: M * ceil(dim/64) words.
///              For M=1, this is identical to the original 1-bit layout.
///   - `norm`:  ‖o - c‖₂  (the L2 norm of the residual before normalization).
///              Needed in the distance formula to reconstruct the scale.
///   - `sum_x`: Popcount of the MSB plane (plane 0). For M=1 this is the
///              popcount of the entire code. Used in Stage 1 fast path.
///   - `bits`:  Number of quantization bits (M). 1, 2, or 4.
///
struct RaBitQCode {
    std::vector<uint64_t> code;   // M * ceil(dim/64) words, bit-plane layout
    float norm;                    // ‖o - c‖₂
    uint32_t sum_x;                // popcount(MSB plane)
    uint8_t bits = 1;              // M: quantization bits

    // ExRaBitQ fields (populated when bits > 1)
    std::vector<uint8_t> ex_code;  // per-dimension code_abs values [0, 2^M-1]
    std::vector<uint8_t> ex_sign_packed;  // packed sign bits, little-endian by dimension
    float xipnorm = 0.0f;                 // 1 / Σ(code_abs[i]+0.5)*|o'[i]|
    bool ex_code_sign_folded = false;     // official 1+n stores sign in ex_code.
    float ex_factor_add = 0.0f;           // official f_add_ex.
    float ex_factor_rescale = 0.0f;       // official f_rescale_ex, excluding query norm.
};

// ============================================================================
// RaBitQEncoder — encodes float vectors into M-bit RaBitQ codes
// ============================================================================

/// Encoder for RaBitQ M-bit quantization (M=1, 2, or 4).
///
/// The encoding pipeline for each database vector o:
///
///   1. **Center:**     r = o - c           (subtract cluster centroid)
///   2. **Norm:**       ‖r‖ = ‖o - c‖₂     (store this for distance formula)
///   3. **Normalize:**  ō = r / ‖r‖        (unit vector)
///   4. **Rotate:**     ō' = P^T × ō       (rotate into canonical basis)
///   5. **Quantize:**   bin[i] = uniform_quantize(ō'[i], 2^M levels)
///   6. **Pack:**       bit-plane layout into uint64_t[M * ceil(dim/64)]
///
/// For M=1, step 5 degenerates to sign quantization (bin = (ō'[i] >= 0) ? 1 : 0)
/// and the output is identical to the original 1-bit encoder.
///
/// The rotation matrix P is shared across all vectors in the index.
/// The centroid c is per-cluster (pass nullptr for flat index / testing).
///
class RaBitQEncoder {
 public:
    /// Construct an encoder for vectors of dimension `dim` using rotation `P`.
    ///
    /// For M-bit encoding (bits > 1), a global scaling factor `t_const` is
    /// precomputed once by sampling 100 random Gaussian vectors and averaging
    /// their optimal rescale factor. This replaces per-vector lattice search
    /// and gives 10-50× faster encoding with negligible precision loss.
    /// See: rabitqlib/quantization/rabitq_impl.hpp:380 (faster_quantize_ex)
    ///
    /// @param dim          Vector dimensionality
    /// @param rotation     Shared rotation matrix (must outlive the encoder).
    ///                     The encoder does NOT take ownership.
    /// @param bits         Quantization bits: 1, 2, or 4 (default 1)
    /// @param t_const_seed RNG seed for t_const precomputation (default 42).
    ///                     Use the same seed for reproducible results.
    RaBitQEncoder(Dim dim, const RotationMatrix& rotation, uint8_t bits = 1,
                  uint64_t t_const_seed = 42);
    RaBitQEncoder(Dim dim, const RotationMatrix& rotation,
                  const RaBitQConfig& config,
                  uint64_t t_const_seed = 42);

    ~RaBitQEncoder() = default;
    VDB_DISALLOW_COPY_AND_MOVE(RaBitQEncoder);

    /// Encode a single vector (fast path: uses precomputed t_const for bits>1).
    ///
    /// @param vec       Raw float vector (length = dim)
    /// @param centroid  Cluster centroid (length = dim). Pass nullptr to use
    ///                  the zero vector (flat index / testing).
    /// @return          Encoded RaBitQ code with norm and popcount.
    RaBitQCode Encode(const float* vec, const float* centroid = nullptr) const;

    /// Encode a single vector using the slow lattice-search path (bits>1).
    ///
    /// Runs per-vector best_rescale_factor priority-queue search to find
    /// the optimal t for quantization. Used for correctness validation
    /// against Encode(). For bits=1 the behavior is identical to Encode().
    ///
    /// @param vec       Raw float vector (length = dim)
    /// @param centroid  Cluster centroid (length = dim). Pass nullptr for zero.
    /// @return          Encoded RaBitQ code with norm and popcount.
    RaBitQCode EncodeSlow(const float* vec, const float* centroid = nullptr) const;

    /// Encode a batch of vectors.
    ///
    /// @param vecs      Row-major array of n vectors (n × dim floats)
    /// @param n         Number of vectors to encode
    /// @param centroid  Shared cluster centroid (or nullptr for zero)
    /// @return          Vector of encoded codes (size = n)
    std::vector<RaBitQCode> EncodeBatch(const float* vecs, uint32_t n,
                                         const float* centroid = nullptr) const;

    /// Total uint64_t words per code (M * words_per_plane).
    uint32_t num_code_words() const { return total_words_; }

    /// Number of uint64_t words per bit-plane (= ceil(dim/64)).
    uint32_t words_per_plane() const { return words_per_plane_; }

    /// Quantization bits (M).
    uint8_t bits() const { return bits_; }

    /// Vector dimensionality.
    Dim dim() const { return dim_; }

 private:
    Dim dim_;
    uint8_t bits_;                    // M: quantization bits (1, 2, or 4)
    uint8_t total_bits_ = 1;
    uint8_t ex_bits_ = 0;
    bool official_1_plus_n_ = false;
    uint32_t words_per_plane_;        // ceil(dim / 64)
    uint32_t total_words_;            // bits_ * words_per_plane_
    const RotationMatrix& rotation_;
    // Fast-path fields (bits > 1 only)
    int max_code_ = 0;               // (1 << bits_) - 1; max allowed code value
    double t_const_ = 0.0;          // Precomputed global rescale factor for fast quantization
};

}  // namespace rabitq
}  // namespace vdb
