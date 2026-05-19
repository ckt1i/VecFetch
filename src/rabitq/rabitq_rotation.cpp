#include "vdb/rabitq/rabitq_rotation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <numeric>

#include "vdb/simd/hadamard.h"

namespace vdb {
namespace rabitq {

// ============================================================================
// Construction / Move
// ============================================================================

RotationMatrix::RotationMatrix(Dim dim)
    : dim_(dim), data_(static_cast<size_t>(dim) * dim, 0.0f) {}

RotationMatrix::RotationMatrix(Dim dim, std::vector<float> data)
    : dim_(dim), data_(std::move(data)) {}

RotationMatrix::RotationMatrix(RotationMatrix&& other) noexcept
    : dim_(other.dim_),
      data_(std::move(other.data_)),
      kind_(other.kind_),
      use_fast_hadamard_(other.use_fast_hadamard_),
      diag_signs_(std::move(other.diag_signs_)),
      block_sizes_(std::move(other.block_sizes_)),
      permutation_(std::move(other.permutation_)),
      inverse_permutation_(std::move(other.inverse_permutation_)),
      seed_(other.seed_) {
    other.dim_ = 0;
    other.kind_ = RotationKind::RandomMatrix;
    other.use_fast_hadamard_ = false;
    other.seed_ = 0;
}

RotationMatrix& RotationMatrix::operator=(RotationMatrix&& other) noexcept {
    if (this != &other) {
        dim_ = other.dim_;
        data_ = std::move(other.data_);
        kind_ = other.kind_;
        use_fast_hadamard_ = other.use_fast_hadamard_;
        diag_signs_ = std::move(other.diag_signs_);
        block_sizes_ = std::move(other.block_sizes_);
        permutation_ = std::move(other.permutation_);
        inverse_permutation_ = std::move(other.inverse_permutation_);
        seed_ = other.seed_;
        other.dim_ = 0;
        other.kind_ = RotationKind::RandomMatrix;
        other.use_fast_hadamard_ = false;
        other.seed_ = 0;
    }
    return *this;
}

// ============================================================================
// GenerateRandom — QR decomposition of random Gaussian matrix
// ============================================================================

void RotationMatrix::GenerateRandom(uint64_t seed) {
    kind_ = RotationKind::RandomMatrix;
    use_fast_hadamard_ = false;
    seed_ = seed;
    diag_signs_.clear();
    block_sizes_.clear();
    permutation_.clear();
    inverse_permutation_.clear();

    const size_t L = dim_;
    const size_t n = L * L;

    // Step 1: Fill with i.i.d. N(0,1) values
    std::mt19937_64 rng;
    if (seed == 0) {
        std::random_device rd;
        rng.seed(rd());
    } else {
        rng.seed(seed);
    }
    std::normal_distribution<float> dist(0.0f, 1.0f);

    data_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        data_[i] = dist(rng);
    }

    // Step 2: Modified Gram-Schmidt QR decomposition (column-oriented)
    // We treat the matrix as column-major for Gram-Schmidt, then
    // the result Q is stored row-major (Q[i][j] = data_[i*L + j]).
    //
    // Actually, we'll work with columns stored contiguously in a temp buffer.
    // Column j of the matrix is data_[0*L+j], data_[1*L+j], ..., data_[(L-1)*L+j]
    //
    // For efficiency, work on rows as columns (transpose interpretation):
    // Treat data_ as L column vectors of length L stored row-major.
    // Row i = column vector i.
    // After Gram-Schmidt on rows, the rows will be orthonormal.

    // Gram-Schmidt on rows: make row i orthogonal to rows 0..i-1
    for (size_t i = 0; i < L; ++i) {
        float* row_i = data_.data() + i * L;

        // Subtract projections onto previous rows
        for (size_t j = 0; j < i; ++j) {
            const float* row_j = data_.data() + j * L;
            // dot(row_i, row_j)
            float dot = 0.0f;
            for (size_t k = 0; k < L; ++k) {
                dot += row_i[k] * row_j[k];
            }
            // row_i -= dot * row_j
            for (size_t k = 0; k < L; ++k) {
                row_i[k] -= dot * row_j[k];
            }
        }

        // Normalize row_i
        float norm = 0.0f;
        for (size_t k = 0; k < L; ++k) {
            norm += row_i[k] * row_i[k];
        }
        norm = std::sqrt(norm);
        if (norm > 1e-10f) {
            float inv_norm = 1.0f / norm;
            for (size_t k = 0; k < L; ++k) {
                row_i[k] *= inv_norm;
            }
        }
    }
}

// ============================================================================
// GenerateHadamard — Walsh-Hadamard × random diagonal
// ============================================================================

namespace {

/// Check if n is a power of 2.
bool IsPowerOf2(uint32_t n) {
    return n > 0 && (n & (n - 1)) == 0;
}

/// In-place Fast Walsh-Hadamard Transform (unnormalized).
/// After this, vec[i] contains the transform coefficient.
/// The transform is its own inverse (up to scaling by 1/n).
///
/// Delegates to simd::FWHT_AVX512 which has an AVX-512 fast path for the
/// levels with len >= 16, and falls back to a scalar implementation for the
/// early levels (len < 16).
void FWHT_InPlace(float* vec, uint32_t n) {
    simd::FWHT_AVX512(vec, n);
}

std::mt19937_64 MakeRng(uint64_t seed) {
    std::mt19937_64 rng;
    if (seed == 0) {
        std::random_device rd;
        rng.seed(rd());
    } else {
        rng.seed(seed);
    }
    return rng;
}

std::vector<uint32_t> GreedyPowerOf2Blocks(uint32_t dim) {
    std::vector<uint32_t> blocks;
    uint32_t remaining = dim;
    while (remaining > 0) {
        uint32_t block = 1u;
        while ((block << 1u) > block && (block << 1u) <= remaining) {
            block <<= 1u;
        }
        blocks.push_back(block);
        remaining -= block;
    }
    return blocks;
}

void ApplyHadamardDiagonalBlock(const int8_t* signs,
                                uint32_t block_size,
                                float* values) {
    FWHT_InPlace(values, block_size);
    const float scale = 1.0f / std::sqrt(static_cast<float>(block_size));
    for (uint32_t i = 0; i < block_size; ++i) {
        values[i] *= scale * static_cast<float>(signs[i]);
    }
}

void ApplyHadamardDiagonalBlockInverse(const int8_t* signs,
                                       uint32_t block_size,
                                       float* values) {
    for (uint32_t i = 0; i < block_size; ++i) {
        values[i] *= static_cast<float>(signs[i]);
    }
    FWHT_InPlace(values, block_size);
    const float scale = 1.0f / std::sqrt(static_cast<float>(block_size));
    for (uint32_t i = 0; i < block_size; ++i) {
        values[i] *= scale;
    }
}

void ApplyBlockedFast(const std::vector<uint32_t>& permutation,
                      const std::vector<uint32_t>& block_sizes,
                      const std::vector<int8_t>& diag_signs,
                      const float* in,
                      float* out) {
    const size_t L = permutation.size();
    for (size_t i = 0; i < L; ++i) {
        out[i] = in[permutation[i]];
    }
    size_t offset = 0;
    for (uint32_t block_size : block_sizes) {
        ApplyHadamardDiagonalBlock(
            diag_signs.data() + offset, block_size, out + offset);
        offset += block_size;
    }
}

void ApplyBlockedFastInverse(const std::vector<uint32_t>& permutation,
                             const std::vector<uint32_t>& block_sizes,
                             const std::vector<int8_t>& diag_signs,
                             const float* in,
                             float* out) {
    const size_t L = permutation.size();
    std::vector<float> tmp(L, 0.0f);
    std::memcpy(tmp.data(), in, L * sizeof(float));
    size_t offset = 0;
    for (uint32_t block_size : block_sizes) {
        ApplyHadamardDiagonalBlockInverse(
            diag_signs.data() + offset, block_size, tmp.data() + offset);
        offset += block_size;
    }
    for (size_t i = 0; i < L; ++i) {
        out[permutation[i]] = tmp[i];
    }
}

}  // namespace

bool RotationMatrix::GenerateHadamard(uint64_t seed, bool use_fast_transform) {
    if (!IsPowerOf2(dim_)) {
        return false;
    }

    const size_t L = dim_;
    kind_ = RotationKind::Hadamard;
    seed_ = seed;
    block_sizes_.clear();
    permutation_.clear();
    inverse_permutation_.clear();

    // Generate random diagonal signs
    std::mt19937_64 rng = MakeRng(seed);

    diag_signs_.resize(L);
    std::uniform_int_distribution<int> coin(0, 1);
    for (size_t i = 0; i < L; ++i) {
        diag_signs_[i] = coin(rng) ? +1 : -1;
    }

    // Build the full matrix P = (1/√L) H × D
    // H is the Walsh-Hadamard matrix (H[i][j] = (-1)^popcount(i&j))
    // D is the diagonal matrix with diag_signs_
    //
    // P[i][j] = (1/√L) × H[i][j] × D[j][j]
    //         = (1/√L) × (-1)^popcount(i&j) × diag_signs_[j]

    const float scale = 1.0f / std::sqrt(static_cast<float>(L));
    data_.resize(L * L);

    for (size_t i = 0; i < L; ++i) {
        for (size_t j = 0; j < L; ++j) {
            // H[i][j] = (-1)^popcount(i & j)
            int pc = __builtin_popcount(static_cast<uint32_t>(i & j));
            float h_val = (pc & 1) ? -1.0f : 1.0f;
            data_[i * L + j] = scale * h_val * static_cast<float>(diag_signs_[j]);
        }
    }

    use_fast_hadamard_ = use_fast_transform;
    return true;
}

bool RotationMatrix::GenerateBlockedHadamardPermuted(uint64_t seed,
                                                     bool use_fast_transform) {
    if (dim_ == 0) {
        return false;
    }

    kind_ = RotationKind::BlockedHadamardPermuted;
    use_fast_hadamard_ = use_fast_transform;
    seed_ = seed;
    block_sizes_ = GreedyPowerOf2Blocks(dim_);
    permutation_.resize(dim_);
    inverse_permutation_.resize(dim_);
    std::iota(permutation_.begin(), permutation_.end(), 0u);

    std::mt19937_64 rng = MakeRng(seed);
    std::shuffle(permutation_.begin(), permutation_.end(), rng);
    for (size_t i = 0; i < permutation_.size(); ++i) {
        inverse_permutation_[permutation_[i]] = static_cast<uint32_t>(i);
    }

    diag_signs_.resize(dim_);
    std::uniform_int_distribution<int> coin(0, 1);
    for (size_t i = 0; i < diag_signs_.size(); ++i) {
        diag_signs_[i] = coin(rng) ? +1 : -1;
    }

    data_.assign(static_cast<size_t>(dim_) * dim_, 0.0f);
    std::vector<float> basis(dim_, 0.0f);
    std::vector<float> row(dim_, 0.0f);
    for (size_t i = 0; i < dim_; ++i) {
        std::fill(basis.begin(), basis.end(), 0.0f);
        basis[i] = 1.0f;
        ApplyBlockedFast(permutation_, block_sizes_, diag_signs_,
                         basis.data(), row.data());
        std::memcpy(data_.data() + i * dim_, row.data(), dim_ * sizeof(float));
    }
    return true;
}

// ============================================================================
// Apply — P^T × in (encoding/query rotation)
// ============================================================================

void RotationMatrix::Apply(const float* VDB_RESTRICT in,
                           float* VDB_RESTRICT out) const {
    const size_t L = dim_;

    if (kind_ == RotationKind::BlockedHadamardPermuted && use_fast_hadamard_) {
        ApplyBlockedFast(permutation_, block_sizes_, diag_signs_, in, out);
    } else if (use_fast_hadamard_) {
        // Fast path: O(L log L)
        // P^T = D^T H^T / √L = D H / √L  (since H is symmetric, D is diagonal)
        // So P^T × in = (1/√L) D (H × in)
        //
        // Step 1: copy in → out
        std::memcpy(out, in, L * sizeof(float));

        // Step 2: Apply FWHT in-place
        FWHT_InPlace(out, static_cast<uint32_t>(L));

        // Step 3: Scale by 1/√L and multiply by D
        const float scale = 1.0f / std::sqrt(static_cast<float>(L));
        for (size_t i = 0; i < L; ++i) {
            out[i] *= scale * static_cast<float>(diag_signs_[i]);
        }
    } else {
        // General path: O(L²) matrix-vector multiply
        // out = P^T × in → out[j] = Σ_i P[i][j] × in[i] = Σ_i data_[i*L+j] × in[i]
        // Equivalent to: out[j] = column j of P dotted with in
        //
        // For better cache behavior, iterate over rows of P^T = columns of P:
        // out[j] = Σ_i P[i][j] * in[i]
        // This is a column-access pattern on P. We can reformulate:
        // out = P^T × in, where P^T[j][i] = P[i][j]
        //
        // We iterate: for each output element j, sum over i.
        // But for better cache usage, accumulate: for each row i of P, scatter.
        std::memset(out, 0, L * sizeof(float));
        for (size_t i = 0; i < L; ++i) {
            const float in_i = in[i];
            const float* row = data_.data() + i * L;
            for (size_t j = 0; j < L; ++j) {
                out[j] += row[j] * in_i;
            }
        }
    }
}

// ============================================================================
// ApplyInverse — P × in (inverse rotation)
// ============================================================================

void RotationMatrix::ApplyInverse(const float* VDB_RESTRICT in,
                                  float* VDB_RESTRICT out) const {
    const size_t L = dim_;

    if (kind_ == RotationKind::BlockedHadamardPermuted && use_fast_hadamard_) {
        ApplyBlockedFastInverse(permutation_, block_sizes_, diag_signs_, in, out);
    } else if (use_fast_hadamard_) {
        // P = (1/√L) H × D
        // P × in = (1/√L) H × (D × in)
        //
        // Step 1: Apply D to in → out
        for (size_t i = 0; i < L; ++i) {
            out[i] = in[i] * static_cast<float>(diag_signs_[i]);
        }

        // Step 2: Apply FWHT in-place
        FWHT_InPlace(out, static_cast<uint32_t>(L));

        // Step 3: Scale by 1/√L
        const float scale = 1.0f / std::sqrt(static_cast<float>(L));
        for (size_t i = 0; i < L; ++i) {
            out[i] *= scale;
        }
    } else {
        // General path: out = P × in → out[i] = Σ_j P[i][j] × in[j]
        // = row i of P dotted with in (row-access: cache-friendly)
        for (size_t i = 0; i < L; ++i) {
            const float* row = data_.data() + i * L;
            float sum = 0.0f;
            for (size_t j = 0; j < L; ++j) {
                sum += row[j] * in[j];
            }
            out[i] = sum;
        }
    }
}

// ============================================================================
// Save / Load — persist rotation matrix to binary file
// ============================================================================

Status RotationMatrix::Save(const std::string& path) const {
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) {
        return Status::IOError("Failed to open file for writing: " + path);
    }

    // Write dim
    uint32_t d = dim_;
    ofs.write(reinterpret_cast<const char*>(&d), sizeof(d));

    // Write matrix data (dim * dim floats)
    const size_t data_bytes = static_cast<size_t>(dim_) * dim_ * sizeof(float);
    ofs.write(reinterpret_cast<const char*>(data_.data()), data_bytes);

    // Write flags: bit0 = use_fast_hadamard_, bit1 = blocked-hadamard-permuted
    uint8_t flags = use_fast_hadamard_ ? 1u : 0u;
    if (kind_ == RotationKind::BlockedHadamardPermuted) {
        flags |= 2u;
    }
    ofs.write(reinterpret_cast<const char*>(&flags), sizeof(flags));

    // When Hadamard mode, write diag_signs (dim × int8)
    if (use_fast_hadamard_) {
        ofs.write(reinterpret_cast<const char*>(diag_signs_.data()),
                  static_cast<std::streamsize>(diag_signs_.size()) * sizeof(int8_t));
    }

    if (flags & 2u) {
        const uint64_t persisted_seed = seed_;
        const uint32_t num_blocks = static_cast<uint32_t>(block_sizes_.size());
        ofs.write(reinterpret_cast<const char*>(&persisted_seed), sizeof(persisted_seed));
        ofs.write(reinterpret_cast<const char*>(&num_blocks), sizeof(num_blocks));
        ofs.write(reinterpret_cast<const char*>(block_sizes_.data()),
                  static_cast<std::streamsize>(num_blocks) * sizeof(uint32_t));
        ofs.write(reinterpret_cast<const char*>(permutation_.data()),
                  static_cast<std::streamsize>(permutation_.size()) * sizeof(uint32_t));
    }

    if (!ofs.good()) {
        return Status::IOError("Failed to write rotation matrix to: " + path);
    }
    return Status::OK();
}

StatusOr<RotationMatrix> RotationMatrix::Load(const std::string& path, Dim dim) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return Status::IOError("Failed to open rotation file: " + path);
    }

    // Read dim
    uint32_t file_dim = 0;
    ifs.read(reinterpret_cast<char*>(&file_dim), sizeof(file_dim));
    if (!ifs.good()) {
        return Status::Corruption("Failed to read dim from rotation file");
    }
    if (file_dim != dim) {
        return Status::InvalidArgument(
            "Rotation matrix dimension mismatch: expected " +
            std::to_string(dim) + ", got " + std::to_string(file_dim));
    }

    // Read matrix data
    const size_t n = static_cast<size_t>(dim) * dim;
    std::vector<float> data(n);
    ifs.read(reinterpret_cast<char*>(data.data()), n * sizeof(float));
    if (!ifs.good()) {
        return Status::Corruption("Failed to read rotation matrix data");
    }

    // Read flags: bit 0 = use_fast_hadamard_
    uint8_t flags = 0;
    ifs.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    if (!ifs.good()) {
        return Status::Corruption("Failed to read flags from rotation file");
    }

    RotationMatrix result(dim, std::move(data));

    if (flags & 1u) {
        // Hadamard mode: read diag_signs (dim × int8)
        std::vector<int8_t> diag_signs(dim);
        ifs.read(reinterpret_cast<char*>(diag_signs.data()),
                 static_cast<std::streamsize>(dim) * sizeof(int8_t));
        if (!ifs.good()) {
            return Status::Corruption("Failed to read diag_signs from rotation file");
        }
        result.use_fast_hadamard_ = true;
        result.diag_signs_ = std::move(diag_signs);
    } else {
        result.use_fast_hadamard_ = false;
    }

    result.kind_ = (flags & 2u) ? RotationKind::BlockedHadamardPermuted
                                : ((flags & 1u) ? RotationKind::Hadamard
                                                : RotationKind::RandomMatrix);

    if (flags & 2u) {
        uint64_t seed = 0;
        uint32_t num_blocks = 0;
        ifs.read(reinterpret_cast<char*>(&seed), sizeof(seed));
        ifs.read(reinterpret_cast<char*>(&num_blocks), sizeof(num_blocks));
        if (!ifs.good()) {
            return Status::Corruption("Failed to read blocked hadamard metadata");
        }
        std::vector<uint32_t> block_sizes(num_blocks);
        std::vector<uint32_t> permutation(dim);
        ifs.read(reinterpret_cast<char*>(block_sizes.data()),
                 static_cast<std::streamsize>(num_blocks) * sizeof(uint32_t));
        ifs.read(reinterpret_cast<char*>(permutation.data()),
                 static_cast<std::streamsize>(dim) * sizeof(uint32_t));
        if (!ifs.good()) {
            return Status::Corruption("Failed to read blocked hadamard payload");
        }
        uint32_t total = 0;
        for (uint32_t block : block_sizes) {
            if (!IsPowerOf2(block)) {
                return Status::Corruption("Blocked hadamard block is not power-of-2");
            }
            total += block;
        }
        if (total != dim) {
            return Status::Corruption("Blocked hadamard blocks do not cover dimension");
        }
        std::vector<uint32_t> inverse(dim, 0);
        std::vector<uint8_t> seen(dim, 0);
        for (uint32_t i = 0; i < dim; ++i) {
            if (permutation[i] >= dim || seen[permutation[i]] != 0) {
                return Status::Corruption("Blocked hadamard permutation is invalid");
            }
            seen[permutation[i]] = 1;
            inverse[permutation[i]] = i;
        }
        result.seed_ = seed;
        result.block_sizes_ = std::move(block_sizes);
        result.permutation_ = std::move(permutation);
        result.inverse_permutation_ = std::move(inverse);
    } else {
        result.seed_ = 0;
    }

    return result;
}

}  // namespace rabitq
}  // namespace vdb
