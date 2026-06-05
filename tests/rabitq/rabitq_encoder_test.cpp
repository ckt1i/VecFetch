#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/popcount.h"

using vdb::rabitq::RotationMatrix;
using vdb::rabitq::RaBitQEncoder;
using vdb::rabitq::RaBitQCode;
using vdb::RaBitQConfig;
using vdb::RaBitQEstimatorMode;
using vdb::Dim;

namespace {

/// Generate a random float vector in [-1, 1]^dim
std::vector<float> RandomVec(Dim dim, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> v(dim);
    for (auto& x : v) x = dist(rng);
    return v;
}

/// Compute L2 norm of a vector
float VecNorm(const float* v, Dim d) {
    float s = 0.0f;
    for (Dim i = 0; i < d; ++i) s += v[i] * v[i];
    return std::sqrt(s);
}

}  // namespace

// ===========================================================================
// Basic encoding
// ===========================================================================

TEST(RaBitQEncoderTest, CodeSizeCorrect) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 100);
    auto code = encoder.Encode(vec.data());

    EXPECT_EQ(code.code.size(), static_cast<size_t>(encoder.num_code_words()));
    EXPECT_EQ(encoder.num_code_words(), 2u);  // 128/64 = 2
}

TEST(RaBitQEncoderTest, CodeSize_NonMultipleOf64) {
    Dim dim = 100;  // Not a multiple of 64
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    EXPECT_EQ(encoder.num_code_words(), 2u);  // ceil(100/64) = 2

    auto vec = RandomVec(dim, 100);
    auto code = encoder.Encode(vec.data());
    EXPECT_EQ(code.code.size(), 2u);
}

TEST(RaBitQEncoderTest, NormStoredCorrectly_NoCentroid) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 200);
    auto code = encoder.Encode(vec.data());

    // Norm should equal ‖vec‖ since centroid is zero
    float expected_norm = VecNorm(vec.data(), dim);
    EXPECT_NEAR(code.norm, expected_norm, 1e-5f);
}

TEST(RaBitQEncoderTest, NormStoredCorrectly_WithCentroid) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 200);
    auto centroid = RandomVec(dim, 300);
    auto code = encoder.Encode(vec.data(), centroid.data());

    // Norm should equal ‖vec - centroid‖
    std::vector<float> residual(dim);
    for (Dim i = 0; i < dim; ++i) residual[i] = vec[i] - centroid[i];
    float expected_norm = VecNorm(residual.data(), dim);
    EXPECT_NEAR(code.norm, expected_norm, 1e-5f);
}

TEST(RaBitQEncoderTest, PopcountConsistent) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 500);
    auto code = encoder.Encode(vec.data());

    // sum_x should match manual popcount
    uint32_t manual_popcount = vdb::simd::PopcountTotal(
        code.code.data(), static_cast<uint32_t>(code.code.size()));
    EXPECT_EQ(code.sum_x, manual_popcount);
}

TEST(RaBitQEncoderTest, BitsAreZeroOrOne) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 600);
    auto code = encoder.Encode(vec.data());

    // sum_x should be between 0 and dim (inclusive)
    EXPECT_GE(code.sum_x, 0u);
    EXPECT_LE(code.sum_x, dim);
}

TEST(RaBitQEncoderTest, SumXApproximatelyHalf) {
    // For random vectors after random rotation, sign bits should be
    // roughly 50/50. We check with a generous margin.
    Dim dim = 256;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    uint32_t total_sum_x = 0;
    const int N = 100;
    for (int i = 0; i < N; ++i) {
        auto vec = RandomVec(dim, 1000 + i);
        auto code = encoder.Encode(vec.data());
        total_sum_x += code.sum_x;
    }

    // Average sum_x should be around dim/2 = 128
    float avg = static_cast<float>(total_sum_x) / N;
    EXPECT_GT(avg, 90.0f);   // generous lower bound
    EXPECT_LT(avg, 166.0f);  // generous upper bound
}

// ===========================================================================
// Zero vector edge case
// ===========================================================================

TEST(RaBitQEncoderTest, ZeroVector) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    std::vector<float> zero(dim, 0.0f);
    auto code = encoder.Encode(zero.data());

    EXPECT_NEAR(code.norm, 0.0f, 1e-30f);
    // Code bits are implementation-defined for zero vector (all sign(0) >= 0 → 1)
}

// ===========================================================================
// BatchEncode
// ===========================================================================

TEST(RaBitQEncoderTest, BatchEncode_MatchesSingle) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    const uint32_t N = 10;
    std::vector<float> batch(N * dim);
    std::mt19937_64 rng(999);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& x : batch) x = dist(rng);

    auto batch_codes = encoder.EncodeBatch(batch.data(), N);
    EXPECT_EQ(batch_codes.size(), N);

    // Compare with individual encodes
    for (uint32_t i = 0; i < N; ++i) {
        auto single = encoder.Encode(batch.data() + i * dim);
        EXPECT_EQ(batch_codes[i].code, single.code) << "vector " << i;
        EXPECT_FLOAT_EQ(batch_codes[i].norm, single.norm) << "vector " << i;
        EXPECT_EQ(batch_codes[i].sum_x, single.sum_x) << "vector " << i;
    }
}

// ===========================================================================
// Determinism
// ===========================================================================

TEST(RaBitQEncoderTest, DeterministicEncoding) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 777);
    auto code1 = encoder.Encode(vec.data());
    auto code2 = encoder.Encode(vec.data());

    EXPECT_EQ(code1.code, code2.code);
    EXPECT_FLOAT_EQ(code1.norm, code2.norm);
    EXPECT_EQ(code1.sum_x, code2.sum_x);
}

// ===========================================================================
// High bits in tail words should be zero
// ===========================================================================

TEST(RaBitQEncoderTest, TailBitsZero) {
    Dim dim = 100;  // 100 dims → 2 words, but only 36 bits used in word 1
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 888);
    auto code = encoder.Encode(vec.data());

    // Word 1 should have bits 36..63 all zero
    uint64_t tail_mask = ~((1ULL << 36) - 1);  // mask for bits 36-63
    EXPECT_EQ(code.code[1] & tail_mask, 0u);
}

// ===========================================================================
// Hadamard rotation should also work
// ===========================================================================

TEST(RaBitQEncoderTest, HadamardRotation) {
    Dim dim = 64;
    RotationMatrix P(dim);
    ASSERT_TRUE(P.GenerateHadamard(42, true));
    RaBitQEncoder encoder(dim, P);

    auto vec = RandomVec(dim, 100);
    auto code = encoder.Encode(vec.data());

    EXPECT_EQ(code.code.size(), 1u);
    EXPECT_GT(code.norm, 0.0f);
    EXPECT_GE(code.sum_x, 0u);
    EXPECT_LE(code.sum_x, dim);
}

// ===========================================================================
// Multi-bit encoding tests
// ===========================================================================

TEST(RaBitQEncoderTest, CodeSizeCorrect_MultiBit) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    // bits=2: 2 planes × 2 words/plane = 4 total words
    RaBitQEncoder encoder2(dim, P, 2);
    EXPECT_EQ(encoder2.num_code_words(), 4u);
    EXPECT_EQ(encoder2.words_per_plane(), 2u);
    EXPECT_EQ(encoder2.bits(), 2);

    auto vec = RandomVec(dim, 100);
    auto code2 = encoder2.Encode(vec.data());
    EXPECT_EQ(code2.code.size(), 4u);
    EXPECT_EQ(code2.bits, 2);

    // bits=4: 4 planes × 2 words/plane = 8 total words
    RaBitQEncoder encoder4(dim, P, 4);
    EXPECT_EQ(encoder4.num_code_words(), 8u);
    EXPECT_EQ(encoder4.words_per_plane(), 2u);

    auto code4 = encoder4.Encode(vec.data());
    EXPECT_EQ(code4.code.size(), 8u);
    EXPECT_EQ(code4.bits, 4);
}

TEST(RaBitQEncoderTest, MSBPlane_MatchesSignBits) {
    // The MSB plane of M-bit encoding should be identical to 1-bit encoding
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder enc1(dim, P, 1);
    RaBitQEncoder enc2(dim, P, 2);
    RaBitQEncoder enc4(dim, P, 4);

    const int N = 20;
    for (int i = 0; i < N; ++i) {
        auto vec = RandomVec(dim, 500 + i);
        auto code1 = enc1.Encode(vec.data());
        auto code2 = enc2.Encode(vec.data());
        auto code4 = enc4.Encode(vec.data());

        uint32_t wpp = enc1.num_code_words();  // words_per_plane = 2

        // MSB plane of bits=2 should match bits=1 code
        for (uint32_t w = 0; w < wpp; ++w) {
            EXPECT_EQ(code2.code[w], code1.code[w])
                << "vec " << i << " word " << w << " bits=2 MSB != bits=1";
        }

        // MSB plane of bits=4 should match bits=1 code
        for (uint32_t w = 0; w < wpp; ++w) {
            EXPECT_EQ(code4.code[w], code1.code[w])
                << "vec " << i << " word " << w << " bits=4 MSB != bits=1";
        }

        // sum_x should be identical (all based on MSB plane)
        EXPECT_EQ(code2.sum_x, code1.sum_x) << "vec " << i;
        EXPECT_EQ(code4.sum_x, code1.sum_x) << "vec " << i;
    }
}

TEST(RaBitQEncoderTest, QuantizationRange_Bits2) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P, 2);
    uint32_t wpp = encoder.words_per_plane();

    auto vec = RandomVec(dim, 100);
    auto code = encoder.Encode(vec.data());

    // Extract each 2-bit value and verify in [0, 3]
    for (Dim i = 0; i < dim; ++i) {
        size_t word_idx = i / 64;
        size_t bit_idx  = i % 64;
        uint32_t msb = (code.code[0 * wpp + word_idx] >> bit_idx) & 1;
        uint32_t lsb = (code.code[1 * wpp + word_idx] >> bit_idx) & 1;
        uint32_t val = (msb << 1) | lsb;
        EXPECT_LE(val, 3u) << "dim " << i;
    }
}

TEST(RaBitQEncoderTest, QuantizationRange_Bits4) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P, 4);
    uint32_t wpp = encoder.words_per_plane();

    auto vec = RandomVec(dim, 100);
    auto code = encoder.Encode(vec.data());

    // Extract each 4-bit value and verify in [0, 15]
    for (Dim i = 0; i < dim; ++i) {
        size_t word_idx = i / 64;
        size_t bit_idx  = i % 64;
        uint32_t val = 0;
        for (uint8_t p = 0; p < 4; ++p) {
            uint32_t bit = (code.code[static_cast<size_t>(p) * wpp +
                            word_idx] >> bit_idx) & 1;
            val |= (bit << (3 - p));
        }
        EXPECT_LE(val, 15u) << "dim " << i;
    }
}

TEST(RaBitQEncoderTest, Bits1_BackwardCompat) {
    // Explicit bits=1 should produce identical output to default constructor
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder enc_default(dim, P);
    RaBitQEncoder enc_explicit(dim, P, 1);

    auto vec = RandomVec(dim, 100);
    auto code_default = enc_default.Encode(vec.data());
    auto code_explicit = enc_explicit.Encode(vec.data());

    EXPECT_EQ(code_default.code, code_explicit.code);
    EXPECT_FLOAT_EQ(code_default.norm, code_explicit.norm);
    EXPECT_EQ(code_default.sum_x, code_explicit.sum_x);
}

TEST(RaBitQEncoderTest, TailBitsZero_MultiBit) {
    Dim dim = 100;  // 100 dims → 2 words/plane, 36 bits used in tail word
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P, 2);
    uint32_t wpp = encoder.words_per_plane();

    auto vec = RandomVec(dim, 888);
    auto code = encoder.Encode(vec.data());

    uint64_t tail_mask = ~((1ULL << 36) - 1);  // mask for bits 36-63
    // Check tail bits zero for all planes
    for (uint8_t p = 0; p < 2; ++p) {
        EXPECT_EQ(code.code[static_cast<size_t>(p) * wpp + 1] & tail_mask, 0u)
            << "plane " << static_cast<int>(p);
    }
}

// ===========================================================================
// Fast path: t_const stability (Task 2.2)
// ===========================================================================

TEST(RaBitQEncoderTest, ComputeTConstStable_Deterministic) {
    // Same (dim, bits, seed) → same behavior across two encoder instances
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder enc1(dim, P, 4, /*t_const_seed=*/42);
    RaBitQEncoder enc2(dim, P, 4, /*t_const_seed=*/42);

    // Encode same vector; results must be bit-exact (deterministic t_const)
    auto vec = RandomVec(dim, 100);
    auto c1 = enc1.Encode(vec.data());
    auto c2 = enc2.Encode(vec.data());

    EXPECT_EQ(c1.ex_code, c2.ex_code);
    EXPECT_FLOAT_EQ(c1.xipnorm, c2.xipnorm);
}

TEST(RaBitQEncoderTest, ComputeTConstStable_DifferentSeeds) {
    // t_const should be within 5% across different seeds (statistical stability)
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    // We can't read t_const_ directly, but xipnorm from same vector
    // should be stable if t_const is stable.
    auto vec = RandomVec(dim, 999);

    float xipnorm_ref = 0.0f;
    {
        RaBitQEncoder enc(dim, P, 4, 42);
        xipnorm_ref = enc.Encode(vec.data()).xipnorm;
    }

    for (uint64_t seed : {0ull, 1ull, 100ull, 12345ull}) {
        RaBitQEncoder enc(dim, P, 4, seed);
        float xipnorm = enc.Encode(vec.data()).xipnorm;
        // Relative deviation < 5%
        float rel_diff = std::abs(xipnorm - xipnorm_ref) / std::abs(xipnorm_ref);
        EXPECT_LT(rel_diff, 0.05f)
            << "seed=" << seed << " xipnorm=" << xipnorm
            << " ref=" << xipnorm_ref;
    }
}

// ===========================================================================
// Fast vs Slow consistency (Task 5.2)
// ===========================================================================

TEST(RaBitQEncoderTest, FastVsSlowConsistency_Bits4) {
    // Fast path (Encode, global t_const) vs slow lattice search (EncodeSlow).
    //
    // The fast path uses a precomputed global t_const instead of per-vector
    // optimal t, so ex_code values will differ — this is by design and
    // expected. The official library validates this approach by end-to-end
    // recall, not per-code equivalence.
    //
    // What we check here:
    //   - MSB plane (1-bit sign) must be identical (same rotation, same sign)
    //   - norm and sum_x must be identical (norm = ||residual||, sum_x = popcount)
    //   - ex_code values must be in valid range [0, max_code]
    //   - xipnorm must be finite and positive
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder encoder(dim, P, 4, 42);
    const int max_code = (1 << 4) - 1;  // 15 for bits=4

    const int N = 100;
    uint32_t wpp = encoder.words_per_plane();

    for (int i = 0; i < N; ++i) {
        auto vec = RandomVec(dim, 200 + i);
        auto fast = encoder.Encode(vec.data());
        auto slow = encoder.EncodeSlow(vec.data());

        // MSB plane must be identical — same rotation, same sign bits
        for (uint32_t w = 0; w < wpp; ++w) {
            EXPECT_EQ(fast.code[w], slow.code[w])
                << "vec=" << i << " word=" << w << " MSB plane mismatch";
        }

        // Norm and sum_x are independent of t_const
        EXPECT_FLOAT_EQ(fast.norm, slow.norm) << "vec=" << i;
        EXPECT_EQ(fast.sum_x, slow.sum_x) << "vec=" << i;

        // ex_code values must be in valid range [0, max_code]
        ASSERT_EQ(static_cast<Dim>(fast.ex_code.size()), dim);
        for (Dim d = 0; d < dim; ++d) {
            EXPECT_LE(static_cast<int>(fast.ex_code[d]), max_code)
                << "vec=" << i << " dim=" << d;
        }

        // xipnorm must be finite and positive
        EXPECT_GT(fast.xipnorm, 0.0f) << "vec=" << i;
        EXPECT_TRUE(std::isfinite(fast.xipnorm)) << "vec=" << i;
    }
}

TEST(RaBitQEncoderTest, FastVsSlowConsistency_Bits2) {
    Dim dim = 128;
    RotationMatrix P(dim);
    P.GenerateRandom(42);
    RaBitQEncoder encoder(dim, P, 2, 42);
    const int max_code = (1 << 2) - 1;  // 3 for bits=2

    const int N = 100;
    uint32_t wpp = encoder.words_per_plane();

    for (int i = 0; i < N; ++i) {
        auto vec = RandomVec(dim, 300 + i);
        auto fast = encoder.Encode(vec.data());
        auto slow = encoder.EncodeSlow(vec.data());

        for (uint32_t w = 0; w < wpp; ++w) {
            EXPECT_EQ(fast.code[w], slow.code[w]) << "vec=" << i;
        }
        EXPECT_FLOAT_EQ(fast.norm, slow.norm);
        EXPECT_EQ(fast.sum_x, slow.sum_x);

        for (Dim d = 0; d < dim; ++d) {
            EXPECT_LE(static_cast<int>(fast.ex_code[d]), max_code);
        }
        EXPECT_GT(fast.xipnorm, 0.0f);
        EXPECT_TRUE(std::isfinite(fast.xipnorm));
    }
}

TEST(RaBitQEncoderTest, OfficialOnePlusNComplementsNegativeExDataCodes) {
    Dim dim = 96;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQEncoder legacy(dim, P, 3, 77);
    RaBitQConfig official_cfg;
    official_cfg.total_bits = 4;
    official_cfg.ex_bits = 3;
    official_cfg.estimator_mode = RaBitQEstimatorMode::kOfficial1PlusN;
    RaBitQEncoder official(dim, P, official_cfg, 77);

    auto vec = RandomVec(dim, 500);
    const auto legacy_code = legacy.Encode(vec.data());
    const auto official_code = official.Encode(vec.data());
    const int max_code = (1 << 3) - 1;

    ASSERT_EQ(legacy_code.ex_code.size(), official_code.ex_code.size());
    EXPECT_FALSE(legacy_code.ex_code_sign_folded);
    EXPECT_TRUE(official_code.ex_code_sign_folded);
    EXPECT_FALSE(legacy_code.ex_sign_packed.empty());
    EXPECT_TRUE(official_code.ex_sign_packed.empty());
    EXPECT_FLOAT_EQ(official_code.ex_factor_add,
                    official_code.norm * official_code.norm);
    EXPECT_FLOAT_EQ(official_code.ex_factor_rescale,
                    -2.0f * official_code.norm * official_code.xipnorm);

    for (Dim d = 0; d < dim; ++d) {
        const bool positive =
            ((legacy_code.ex_sign_packed[d / 8] >> (d % 8)) & 1u) != 0;
        const uint8_t expected = positive
            ? legacy_code.ex_code[d]
            : static_cast<uint8_t>(max_code - legacy_code.ex_code[d]);
        EXPECT_EQ(official_code.ex_code[d], expected) << "dim=" << d;
    }
}

TEST(RaBitQEncoderTest, OfficialTotalBitsOneHasNoExDataPayload) {
    Dim dim = 64;
    RotationMatrix P(dim);
    P.GenerateRandom(42);

    RaBitQConfig official_cfg;
    official_cfg.total_bits = 1;
    official_cfg.ex_bits = 0;
    official_cfg.estimator_mode = RaBitQEstimatorMode::kOfficial1PlusN;
    RaBitQEncoder official(dim, P, official_cfg, 77);

    auto vec = RandomVec(dim, 501);
    const auto code = official.Encode(vec.data());
    EXPECT_EQ(code.bits, 1u);
    EXPECT_TRUE(code.ex_code.empty());
    EXPECT_TRUE(code.ex_sign_packed.empty());
    EXPECT_FALSE(code.ex_code_sign_folded);
    EXPECT_EQ(code.ex_factor_add, 0.0f);
    EXPECT_EQ(code.ex_factor_rescale, 0.0f);
}
