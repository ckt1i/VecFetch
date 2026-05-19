#include <gtest/gtest.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <vector>

#include "vdb/rabitq/rabitq_rotation.h"

using vdb::rabitq::RotationMatrix;
using vdb::Dim;

namespace {

/// Check that a matrix is orthogonal: P P^T ≈ I
void AssertOrthogonal(const RotationMatrix& P, float tol = 1e-5f) {
    Dim L = P.dim();
    const float* data = P.data();
    for (Dim i = 0; i < L; ++i) {
        for (Dim j = 0; j < L; ++j) {
            // dot(row_i, row_j)
            float dot = 0.0f;
            for (Dim k = 0; k < L; ++k) {
                dot += data[i * L + k] * data[j * L + k];
            }
            float expected = (i == j) ? 1.0f : 0.0f;
            EXPECT_NEAR(dot, expected, tol)
                << "P P^T [" << i << "][" << j << "]";
        }
    }
}

/// Check that Apply then ApplyInverse recovers the original vector.
void AssertRoundTrip(const RotationMatrix& P, const float* vec, float tol = 1e-4f) {
    Dim L = P.dim();
    std::vector<float> rotated(L), recovered(L);
    P.Apply(vec, rotated.data());
    P.ApplyInverse(rotated.data(), recovered.data());
    for (Dim i = 0; i < L; ++i) {
        EXPECT_NEAR(recovered[i], vec[i], tol) << "dim " << i;
    }
}

/// Check that rotation preserves vector norm (‖Px‖ = ‖x‖).
void AssertNormPreserved(const RotationMatrix& P, const float* vec, float tol = 1e-4f) {
    Dim L = P.dim();
    std::vector<float> rotated(L);
    P.Apply(vec, rotated.data());

    float norm_orig = 0.0f, norm_rot = 0.0f;
    for (Dim i = 0; i < L; ++i) {
        norm_orig += vec[i] * vec[i];
        norm_rot  += rotated[i] * rotated[i];
    }
    EXPECT_NEAR(std::sqrt(norm_rot), std::sqrt(norm_orig), tol);
}

}  // namespace

// ===========================================================================
// Random orthogonal matrix generation
// ===========================================================================

TEST(RotationMatrixTest, RandomOrthogonal_Small) {
    RotationMatrix P(4);
    P.GenerateRandom(42);
    AssertOrthogonal(P);
}

TEST(RotationMatrixTest, RandomOrthogonal_Medium) {
    RotationMatrix P(32);
    P.GenerateRandom(123);
    AssertOrthogonal(P);
}

TEST(RotationMatrixTest, RandomOrthogonal_Dim64) {
    RotationMatrix P(64);
    P.GenerateRandom(999);
    AssertOrthogonal(P, 1e-4f);
}

TEST(RotationMatrixTest, RandomOrthogonal_Dim128) {
    RotationMatrix P(128);
    P.GenerateRandom(7);
    AssertOrthogonal(P, 1e-3f);
}

TEST(RotationMatrixTest, DeterministicWithSeed) {
    RotationMatrix P1(16), P2(16);
    P1.GenerateRandom(42);
    P2.GenerateRandom(42);
    for (Dim i = 0; i < 16 * 16; ++i) {
        EXPECT_FLOAT_EQ(P1.data()[i], P2.data()[i]) << "index " << i;
    }
}

TEST(RotationMatrixTest, DifferentSeedsDiffer) {
    RotationMatrix P1(16), P2(16);
    P1.GenerateRandom(42);
    P2.GenerateRandom(43);
    bool any_differ = false;
    for (Dim i = 0; i < 16 * 16; ++i) {
        if (P1.data()[i] != P2.data()[i]) {
            any_differ = true;
            break;
        }
    }
    EXPECT_TRUE(any_differ);
}

// ===========================================================================
// Round-trip: Apply then ApplyInverse
// ===========================================================================

TEST(RotationMatrixTest, RoundTrip_Random) {
    RotationMatrix P(16);
    P.GenerateRandom(42);

    float vec[16];
    for (int i = 0; i < 16; ++i) vec[i] = static_cast<float>(i + 1);
    AssertRoundTrip(P, vec);
}

TEST(RotationMatrixTest, RoundTrip_UnitVector) {
    RotationMatrix P(8);
    P.GenerateRandom(77);

    float vec[8] = {0, 0, 0, 1, 0, 0, 0, 0};
    AssertRoundTrip(P, vec);
}

// ===========================================================================
// Norm preservation
// ===========================================================================

TEST(RotationMatrixTest, NormPreserved_Random) {
    RotationMatrix P(32);
    P.GenerateRandom(55);

    std::vector<float> vec(32);
    for (int i = 0; i < 32; ++i) vec[i] = static_cast<float>(i) * 0.1f;
    AssertNormPreserved(P, vec.data());
}

// ===========================================================================
// Hadamard mode
// ===========================================================================

TEST(RotationMatrixTest, Hadamard_PowerOf2_Orthogonal) {
    RotationMatrix P(16);
    EXPECT_TRUE(P.GenerateHadamard(42));
    AssertOrthogonal(P);
}

TEST(RotationMatrixTest, Hadamard_NonPowerOf2_Fails) {
    RotationMatrix P(12);
    EXPECT_FALSE(P.GenerateHadamard(42));
}

TEST(RotationMatrixTest, Hadamard_Dim64_Orthogonal) {
    RotationMatrix P(64);
    EXPECT_TRUE(P.GenerateHadamard(99));
    AssertOrthogonal(P, 1e-4f);
}

TEST(RotationMatrixTest, Hadamard_RoundTrip) {
    RotationMatrix P(16);
    P.GenerateHadamard(42, true);  // fast transform

    float vec[16];
    for (int i = 0; i < 16; ++i) vec[i] = static_cast<float>(i + 1);
    AssertRoundTrip(P, vec);
}

TEST(RotationMatrixTest, Hadamard_NormPreserved) {
    RotationMatrix P(32);
    P.GenerateHadamard(55, true);

    std::vector<float> vec(32);
    for (int i = 0; i < 32; ++i) vec[i] = static_cast<float>(i) * 0.1f;
    AssertNormPreserved(P, vec.data());
}

TEST(RotationMatrixTest, Hadamard_FastVsMatrix_SameResult) {
    // Both paths should produce the same rotation
    RotationMatrix P_fast(16), P_mat(16);
    P_fast.GenerateHadamard(42, true);    // fast transform
    P_mat.GenerateHadamard(42, false);    // matrix multiply

    float vec[16];
    for (int i = 0; i < 16; ++i) vec[i] = static_cast<float>(i);

    std::vector<float> out_fast(16), out_mat(16);
    P_fast.Apply(vec, out_fast.data());
    P_mat.Apply(vec, out_mat.data());

    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(out_fast[i], out_mat[i], 1e-4f) << "dim " << i;
    }
}

TEST(RotationMatrixTest, Hadamard_DeterministicWithSeed) {
    RotationMatrix P1(16), P2(16);
    P1.GenerateHadamard(42);
    P2.GenerateHadamard(42);
    for (Dim i = 0; i < 16 * 16; ++i) {
        EXPECT_FLOAT_EQ(P1.data()[i], P2.data()[i]);
    }
}

TEST(RotationMatrixTest, BlockedHadamardPermuted_Orthogonal) {
    RotationMatrix P(768);
    ASSERT_TRUE(P.GenerateBlockedHadamardPermuted(42, true));
    EXPECT_TRUE(P.is_fast_hadamard());
    EXPECT_TRUE(P.is_blocked_hadamard_permuted());
    EXPECT_EQ(P.block_sizes(), (std::vector<uint32_t>{512u, 256u}));
    AssertOrthogonal(P, 1e-4f);
}

TEST(RotationMatrixTest, BlockedHadamardPermuted_RoundTrip) {
    RotationMatrix P(768);
    ASSERT_TRUE(P.GenerateBlockedHadamardPermuted(7, true));
    std::vector<float> vec(768);
    for (int i = 0; i < 768; ++i) vec[i] = static_cast<float>(i % 17 - 8);
    AssertRoundTrip(P, vec.data(), 1e-3f);
}

TEST(RotationMatrixTest, BlockedHadamardPermuted_DeterministicMetadata) {
    RotationMatrix P1(768), P2(768);
    ASSERT_TRUE(P1.GenerateBlockedHadamardPermuted(123, true));
    ASSERT_TRUE(P2.GenerateBlockedHadamardPermuted(123, true));
    EXPECT_EQ(P1.block_sizes(), P2.block_sizes());
    EXPECT_EQ(P1.permutation(), P2.permutation());
    EXPECT_EQ(P1.inverse_permutation(), P2.inverse_permutation());
    EXPECT_EQ(P1.diagonal_signs(), P2.diagonal_signs());
    EXPECT_EQ(P1.seed(), P2.seed());
}

TEST(RotationMatrixTest, BlockedHadamardPermuted_FastVsDense) {
    RotationMatrix P_fast(768), P_dense(768);
    ASSERT_TRUE(P_fast.GenerateBlockedHadamardPermuted(42, true));
    ASSERT_TRUE(P_dense.GenerateBlockedHadamardPermuted(42, false));

    std::vector<float> vec(768), out_fast(768), out_dense(768);
    for (int i = 0; i < 768; ++i) vec[i] = std::sin(static_cast<float>(i) * 0.1f);
    P_fast.Apply(vec.data(), out_fast.data());
    P_dense.Apply(vec.data(), out_dense.data());
    for (int i = 0; i < 768; ++i) {
        EXPECT_NEAR(out_fast[i], out_dense[i], 1e-4f) << "dim " << i;
    }
}

TEST(RotationMatrixTest, BlockedHadamardPermuted_SaveLoadRoundTrip) {
    namespace fs = std::filesystem;
    const fs::path tmp_path =
        fs::temp_directory_path() / "blocked_hadamard_rotation_test.bin";
    RotationMatrix saved(768);
    ASSERT_TRUE(saved.GenerateBlockedHadamardPermuted(314, true));
    ASSERT_TRUE(saved.Save(tmp_path.string()).ok());

    auto loaded_or = RotationMatrix::Load(tmp_path.string(), 768);
    ASSERT_TRUE(loaded_or.ok()) << loaded_or.status().message();
    RotationMatrix loaded = std::move(loaded_or.value());

    EXPECT_TRUE(loaded.is_blocked_hadamard_permuted());
    EXPECT_TRUE(loaded.is_fast_hadamard());
    EXPECT_EQ(saved.seed(), loaded.seed());
    EXPECT_EQ(saved.block_sizes(), loaded.block_sizes());
    EXPECT_EQ(saved.permutation(), loaded.permutation());
    EXPECT_EQ(saved.inverse_permutation(), loaded.inverse_permutation());
    EXPECT_EQ(saved.diagonal_signs(), loaded.diagonal_signs());

    std::vector<float> vec(768), out_saved(768), out_loaded(768);
    for (int i = 0; i < 768; ++i) vec[i] = static_cast<float>((i % 13) - 6);
    saved.Apply(vec.data(), out_saved.data());
    loaded.Apply(vec.data(), out_loaded.data());
    for (int i = 0; i < 768; ++i) {
        EXPECT_NEAR(out_saved[i], out_loaded[i], 1e-5f) << "dim " << i;
    }
    fs::remove(tmp_path);
}

// ===========================================================================
// Move semantics
// ===========================================================================

TEST(RotationMatrixTest, MoveConstruct) {
    RotationMatrix P(8);
    P.GenerateRandom(42);
    const float* old_ptr = P.data();

    RotationMatrix P2(std::move(P));
    EXPECT_EQ(P2.dim(), 8u);
    EXPECT_EQ(P2.data(), old_ptr);
    EXPECT_EQ(P.dim(), 0u);
}

TEST(RotationMatrixTest, MoveAssign) {
    RotationMatrix P1(8), P2(4);
    P1.GenerateRandom(42);
    P2 = std::move(P1);
    EXPECT_EQ(P2.dim(), 8u);
    EXPECT_EQ(P1.dim(), 0u);
}

// ===========================================================================
// Edge case: dim=1
// ===========================================================================

TEST(RotationMatrixTest, Dim1_Random) {
    RotationMatrix P(1);
    P.GenerateRandom(42);
    // 1×1 orthogonal matrix is ±1
    float val = P.data()[0];
    EXPECT_NEAR(std::abs(val), 1.0f, 1e-6f);
}

TEST(RotationMatrixTest, Dim1_Hadamard) {
    RotationMatrix P(1);
    EXPECT_TRUE(P.GenerateHadamard(42));
    float val = P.data()[0];
    EXPECT_NEAR(std::abs(val), 1.0f, 1e-6f);
}
