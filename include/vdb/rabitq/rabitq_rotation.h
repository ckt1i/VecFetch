#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"

namespace vdb {
namespace rabitq {

enum class RotationKind : uint8_t {
    RandomMatrix = 0,
    Hadamard = 1,
    BlockedHadamardPermuted = 2,
};

// ============================================================================
// RotationMatrix — stores a L×L orthogonal matrix P for RaBitQ
// ============================================================================

/// Random orthogonal rotation matrix used in RaBitQ encoding/querying.
///
/// In RaBitQ, the codebook is defined as C' = P × C where C is the set of
/// unit hypercube vertices {±1/√L}^L. The rotation P is the "codebook" —
/// it only needs to be stored once per index (not per vector).
///
/// **Encoding:**   o' = P^T × ō  (rotate the normalized residual into
///                                 the canonical basis where sign-quantization
///                                 approximates the nearest codebook vertex)
///
/// **Querying:**   q' = P^T × q̄  (same rotation applied to query)
///
/// Since P is orthogonal, P^{-1} = P^T, so both directions use the same
/// matrix — just transposed.
///
/// Storage: row-major float[dim * dim].
///   P[i][j] = data_[i * dim + j]
///
/// Two construction methods:
///   1. **RandomOrthogonal** (default): generate a random matrix and
///      orthogonalize via Gram-Schmidt QR decomposition. O(L²) per apply.
///   2. **Hadamard** (optional): use a scaled Walsh-Hadamard matrix times a
///      random diagonal ±1 matrix. O(L log L) per apply via fast transform.
///
class RotationMatrix {
 public:
    /// Default-construct an empty rotation (dim=0).
    /// Required by StatusOr&lt;RotationMatrix&gt;; not useful on its own.
    RotationMatrix() : dim_(0), kind_(RotationKind::RandomMatrix),
                       use_fast_hadamard_(false) {}

    /// Construct an uninitialized rotation for dimension `dim`.
    /// Call GenerateRandom() or GenerateHadamard() to populate.
    explicit RotationMatrix(Dim dim);

    /// Construct from existing matrix data (row-major, dim×dim).
    /// Takes ownership of the data via move.
    RotationMatrix(Dim dim, std::vector<float> data);

    ~RotationMatrix() = default;

    // Move-only (matrix can be large)
    RotationMatrix(RotationMatrix&& other) noexcept;
    RotationMatrix& operator=(RotationMatrix&& other) noexcept;
    VDB_DISALLOW_COPY(RotationMatrix);

    /// Generate a random orthogonal matrix via QR decomposition of a
    /// random Gaussian matrix.
    ///
    /// Algorithm:
    ///   1. Fill L×L matrix with i.i.d. N(0,1) entries.
    ///   2. Perform modified Gram-Schmidt to obtain orthonormal columns.
    ///   3. Store result as row-major P where each row is an orthonormal vector.
    ///
    /// @param seed  Random seed for reproducibility (0 = use random_device)
    void GenerateRandom(uint64_t seed = 0);

    /// Generate a Hadamard-diagonal rotation: P = (1/√L) H_L × D
    /// where H_L is the Walsh-Hadamard matrix and D is a random diagonal
    /// matrix with ±1 entries.
    ///
    /// **Requirement:** dim must be a power of 2.
    /// If dim is not a power of 2, returns false and leaves the matrix unchanged.
    ///
    /// When use_fast_transform=true, the Apply/ApplyTranspose methods will
    /// use the O(L log L) fast Walsh-Hadamard transform instead of the
    /// O(L²) matrix-vector multiply.
    ///
    /// @param seed              Random seed (0 = use random_device)
    /// @param use_fast_transform  If true, Apply uses FWHT instead of matmul.
    ///                            The full matrix is still stored for debugging.
    /// @return true on success, false if dim is not power-of-2
    bool GenerateHadamard(uint64_t seed = 0, bool use_fast_transform = true);

    /// Generate a blocked Hadamard rotation for non-power-of-two dimensions.
    ///
    /// The generated rotation is deterministic for a fixed `(dim, seed)` and
    /// consists of:
    ///   1. A global permutation of dimensions.
    ///   2. A greedy descending power-of-two block partition.
    ///   3. Per-block Hadamard-diagonal transforms.
    ///
    /// The dense matrix is still materialized in `data_` for compatibility,
    /// while Apply/ApplyInverse can use the blocked fast path.
    bool GenerateBlockedHadamardPermuted(uint64_t seed = 0,
                                         bool use_fast_transform = true);

    /// Apply the rotation: out = P^T × in (forward rotation for encoding).
    ///
    /// This computes P^{-1} × in = P^T × in since P is orthogonal.
    /// If the matrix was generated with Hadamard fast transform enabled,
    /// this uses O(L log L) FWHT; otherwise O(L²) matmul.
    ///
    /// @param in   Input vector (length = dim)
    /// @param out  Output vector (length = dim, may NOT alias `in`)
    void Apply(const float* VDB_RESTRICT in, float* VDB_RESTRICT out) const;

    /// Apply the forward rotation: out = P × in (inverse of Apply).
    ///
    /// Used to transform from the rotated space back to the original space.
    /// Rarely needed in the hot path (only for debugging / reconstruction).
    ///
    /// @param in   Input vector in rotated space (length = dim)
    /// @param out  Output vector in original space (length = dim)
    void ApplyInverse(const float* VDB_RESTRICT in, float* VDB_RESTRICT out) const;

    /// Dimension of the rotation matrix.
    Dim dim() const { return dim_; }

    /// Raw matrix data access (row-major, dim × dim).
    const float* data() const { return data_.data(); }
    float*       data()       { return data_.data(); }

    /// Whether this rotation uses the fast Hadamard transform path.
    bool is_fast_hadamard() const { return use_fast_hadamard_; }
    RotationKind kind() const { return kind_; }
    bool is_blocked_hadamard_permuted() const {
        return kind_ == RotationKind::BlockedHadamardPermuted;
    }
    uint64_t seed() const { return seed_; }

    /// Random diagonal signs for Hadamard mode (length = dim).
    /// Valid after GenerateHadamard() and GenerateBlockedHadamardPermuted().
    const std::vector<int8_t>& diagonal_signs() const { return diag_signs_; }
    const std::vector<uint32_t>& block_sizes() const { return block_sizes_; }
    const std::vector<uint32_t>& permutation() const { return permutation_; }
    const std::vector<uint32_t>& inverse_permutation() const {
        return inverse_permutation_;
    }

    /// Save the rotation matrix to a binary file.
    ///
    /// File format: [dim:uint32][data: dim*dim floats][flags:uint8][diag_signs: dim*int8 (if flags&1)]
    /// flags bit 0 = use_fast_hadamard_. When flags&1, dim int8 diag_signs follow.
    ///
    /// @param path  Output file path
    /// @return      Status
    Status Save(const std::string& path) const;

    /// Load a rotation matrix from a binary file.
    ///
    /// @param path  Input file path
    /// @param dim   Expected dimensionality (must match the saved matrix)
    /// @return      Loaded RotationMatrix, or error status
    static StatusOr<RotationMatrix> Load(const std::string& path, Dim dim);

 private:
    Dim dim_;
    std::vector<float> data_;        // Row-major P[dim × dim]
    RotationKind kind_ = RotationKind::RandomMatrix;
    bool use_fast_hadamard_ = false; // Whether Apply() uses FWHT
    std::vector<int8_t> diag_signs_; // ±1 diagonal for Hadamard mode
    std::vector<uint32_t> block_sizes_;
    std::vector<uint32_t> permutation_;
    std::vector<uint32_t> inverse_permutation_;
    uint64_t seed_ = 0;
};

}  // namespace rabitq
}  // namespace vdb
