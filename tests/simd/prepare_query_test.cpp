#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/simd/fastscan.h"
#include "vdb/simd/prepare_query.h"
#include "vdb/storage/pack_codes.h"

namespace {

struct FreeByteBuffer {
    void operator()(uint8_t* ptr) const {
        std::free(ptr);
    }
};

using AlignedByteBuffer = std::unique_ptr<uint8_t, FreeByteBuffer>;

AlignedByteBuffer MakeAlignedByteBuffer(size_t size, size_t alignment = 64) {
    const size_t aligned_size = ((size + alignment - 1) / alignment) * alignment;
    auto* ptr = static_cast<uint8_t*>(std::aligned_alloc(alignment, aligned_size));
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    std::memset(ptr, 0, aligned_size);
    return AlignedByteBuffer(ptr);
}

std::vector<float> MakeInput(uint32_t dim, float offset = 0.0f) {
    std::vector<float> out(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        const float sign = (i % 3 == 0) ? -1.0f : 1.0f;
        out[i] = sign * (0.15f * static_cast<float>((i % 11) + 1)) + offset;
    }
    return out;
}

float ScalarSubtractAndNormSq(const float* a, const float* b,
                              float* out, uint32_t dim) {
    float acc = 0.0f;
    if (b != nullptr) {
        for (uint32_t i = 0; i < dim; ++i) {
            out[i] = a[i] - b[i];
            acc += out[i] * out[i];
        }
    } else {
        for (uint32_t i = 0; i < dim; ++i) {
            out[i] = a[i];
            acc += out[i] * out[i];
        }
    }
    return acc;
}

float ScalarNormalizeSignSum(float* vec, float inv_norm,
                             uint64_t* sign_code, uint32_t dim) {
    const uint32_t words = (dim + 63u) / 64u;
    for (uint32_t w = 0; w < words; ++w) {
        sign_code[w] = 0;
    }
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        vec[i] *= inv_norm;
        sum += vec[i];
        if (vec[i] >= 0.0f) {
            sign_code[i / 64u] |= (1ULL << (i % 64u));
        }
    }
    return sum;
}

float ScalarQuantizeQuery14Bit(const float* query, int16_t* out, uint32_t dim) {
    constexpr float kMaxVal = 8191.0f;
    float vmax = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        vmax = std::max(vmax, std::abs(query[i]));
    }
    if (vmax < 1e-10f) {
        std::memset(out, 0, static_cast<size_t>(dim) * sizeof(int16_t));
        return 1.0f;
    }
    const float width = vmax / kMaxVal;
    for (uint32_t i = 0; i < dim; ++i) {
        float tmp = query[i] / width;
        out[i] = static_cast<int16_t>(tmp + 0.5f - (tmp < 0.0f));
    }
    return width;
}

int32_t BuildFastScanLUTReference(const int16_t* quant_query,
                                  uint8_t* lut_out,
                                  uint32_t dim) {
    const uint32_t groups = dim >> 2;
    int32_t total_shift = 0;

#if defined(VDB_USE_AVX512)
    constexpr size_t n_lut_per_iter = 4;
    constexpr size_t n_code_per_iter = 128;
    constexpr size_t n_code_per_lane = 16;
    constexpr size_t hi_offset = 64;
#else
    constexpr size_t n_lut_per_iter = 2;
    constexpr size_t n_code_per_iter = 64;
    constexpr size_t n_code_per_lane = 16;
    constexpr size_t hi_offset = 32;
#endif

    auto lowbit = [](int x) { return x & (-x); };
    static constexpr int kLutPos[16] = {
        3, 3, 2, 3, 1, 3, 2, 3, 0, 3, 2, 3, 1, 3, 2, 3
    };

    const int16_t* qq = quant_query;
    for (uint32_t i = 0; i < groups; ++i) {
        int lut[16];
        int v_min = 0;
        lut[0] = 0;
        for (int j = 1; j < 16; ++j) {
            lut[j] = lut[j - lowbit(j)] + qq[kLutPos[j]];
            v_min = std::min(v_min, lut[j]);
        }

        uint8_t* fill_lo = lut_out + (i / n_lut_per_iter) * n_code_per_iter +
                           (i % n_lut_per_iter) * n_code_per_lane;
        uint8_t* fill_hi = fill_lo + hi_offset;
        for (int j = 0; j < 16; ++j) {
            uint32_t val = static_cast<uint32_t>(lut[j] - v_min);
            fill_lo[j] = static_cast<uint8_t>(val & 0xFF);
            fill_hi[j] = static_cast<uint8_t>((val >> 8) & 0xFF);
        }
        total_shift += v_min;
        qq += 4;
    }
    return total_shift;
}

uint32_t ScalarFastScanAccuForVector(const uint8_t* packed_block,
                                     const uint8_t* lut,
                                     uint32_t vec_in_block,
                                     uint32_t dim) {
    const uint32_t words = (dim + 63u) / 64u;
    std::vector<uint64_t> sign_words(words, 0);
    vdb::storage::UnpackSignBitsFromFastScan(
        packed_block, vec_in_block, dim, sign_words.data());

#if defined(VDB_USE_AVX512)
    constexpr size_t n_lut_per_iter = 4;
    constexpr size_t n_code_per_iter = 128;
    constexpr size_t n_code_per_lane = 16;
    constexpr size_t hi_offset = 64;
#else
    constexpr size_t n_lut_per_iter = 2;
    constexpr size_t n_code_per_iter = 64;
    constexpr size_t n_code_per_lane = 16;
    constexpr size_t hi_offset = 32;
#endif

    uint32_t raw = 0;
    const uint32_t groups = dim / 4;
    for (uint32_t m = 0; m < groups; ++m) {
        uint8_t nibble = 0;
        for (uint32_t b = 0; b < 4; ++b) {
            const uint32_t d = m * 4 + b;
            const uint8_t bit =
                static_cast<uint8_t>((sign_words[d / 64u] >> (d % 64u)) & 1ULL);
            nibble |= static_cast<uint8_t>(bit << (3 - b));
        }

        const size_t group_base =
            (m / n_lut_per_iter) * n_code_per_iter +
            (m % n_lut_per_iter) * n_code_per_lane;
        raw += static_cast<uint32_t>(lut[group_base + nibble]) +
               (static_cast<uint32_t>(lut[group_base + hi_offset + nibble]) << 8);
    }
    return raw;
}

void ExpectFastScanPipelineMatchesScalarReference(uint32_t dim) {
    constexpr uint32_t num_codes = 32;

    std::vector<int16_t> quant_query(dim, 0);
    for (uint32_t i = 0; i < dim; ++i) {
        const int sign = (i % 4 == 0 || i % 9 == 0) ? -1 : 1;
        quant_query[i] = static_cast<int16_t>(
            sign * static_cast<int>((i * 53u + 11u) % 2048u));
    }

    std::vector<vdb::rabitq::RaBitQCode> codes(num_codes);
    const uint32_t words = (dim + 63u) / 64u;
    for (uint32_t v = 0; v < num_codes; ++v) {
        codes[v].code.assign(words, 0ULL);
        for (uint32_t d = 0; d < dim; ++d) {
            const uint8_t bit = static_cast<uint8_t>(((v * 7u + d * 3u) & 1u));
            if (bit) {
                codes[v].code[d / 64u] |= (1ULL << (d % 64u));
            }
        }
    }

    std::vector<uint8_t> packed(vdb::storage::FastScanPackedSize(dim), 0);
    vdb::storage::PackSignBitsForFastScan(
        codes.data(), num_codes, dim, packed.data());

    const size_t lut_size = static_cast<size_t>(dim) * 8u;
    auto lut = MakeAlignedByteBuffer(lut_size);
    vdb::simd::BuildFastScanLUT(quant_query.data(), lut.get(), dim);

    alignas(64) uint32_t actual[32] = {0};
    vdb::simd::AccumulateBlock(packed.data(), lut.get(), actual, dim);

    for (uint32_t v = 0; v < num_codes; ++v) {
        const uint32_t expected =
            ScalarFastScanAccuForVector(packed.data(), lut.get(), v, dim);
        EXPECT_EQ(actual[v], expected) << "dim=" << dim << " vector=" << v;
    }
}

void ExpectVecNear(const std::vector<float>& a,
                   const std::vector<float>& b,
                   float tol) {
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_NEAR(a[i], b[i], tol) << "mismatch at index " << i;
    }
}

}  // namespace

TEST(PrepareQuerySimdTest, SimdSubtractAndNormSqMatchesScalarWithCentroid) {
    for (uint32_t dim : {8u, 16u, 23u, 32u, 64u, 512u}) {
        auto a = MakeInput(dim, 0.1f);
        auto b = MakeInput(dim, -0.2f);
        std::vector<float> out_simd(dim, 0.0f), out_scalar(dim, 0.0f);

        float simd_norm = vdb::simd::SimdSubtractAndNormSq(
            a.data(), b.data(), out_simd.data(), dim);
        float scalar_norm = ScalarSubtractAndNormSq(
            a.data(), b.data(), out_scalar.data(), dim);

        EXPECT_NEAR(simd_norm, scalar_norm, 3e-4f);
        ExpectVecNear(out_simd, out_scalar, 1e-6f);
    }
}

TEST(PrepareQuerySimdTest, SimdSubtractAndNormSqMatchesScalarWithoutCentroid) {
    for (uint32_t dim : {8u, 16u, 31u, 64u, 512u}) {
        auto a = MakeInput(dim, 0.3f);
        std::vector<float> out_simd(dim, 0.0f), out_scalar(dim, 0.0f);

        float simd_norm = vdb::simd::SimdSubtractAndNormSq(
            a.data(), nullptr, out_simd.data(), dim);
        float scalar_norm = ScalarSubtractAndNormSq(
            a.data(), nullptr, out_scalar.data(), dim);

        EXPECT_NEAR(simd_norm, scalar_norm, 3e-4f);
        ExpectVecNear(out_simd, out_scalar, 1e-6f);
    }
}

TEST(PrepareQuerySimdTest, SimdNormalizeSignSumMatchesScalar) {
    for (uint32_t dim : {8u, 16u, 29u, 64u, 512u}) {
        auto vec_simd = MakeInput(dim);
        auto vec_scalar = vec_simd;
        const uint32_t words = (dim + 63u) / 64u;
        std::vector<uint64_t> sign_simd(words, 0), sign_scalar(words, 0);

        const float inv_norm = 0.37f;
        float simd_sum = vdb::simd::SimdNormalizeSignSum(
            vec_simd.data(), inv_norm, sign_simd.data(), words, dim);
        float scalar_sum = ScalarNormalizeSignSum(
            vec_scalar.data(), inv_norm, sign_scalar.data(), dim);

        EXPECT_NEAR(simd_sum, scalar_sum, 1e-4f);
        ExpectVecNear(vec_simd, vec_scalar, 1e-6f);
        EXPECT_EQ(sign_simd, sign_scalar);
    }
}

TEST(PrepareQuerySimdTest, QuantizeQuery14BitZeroVector) {
    std::vector<float> query(37, 0.0f);
    std::vector<int16_t> out(query.size(), -1);
    const float width = vdb::simd::QuantizeQuery14Bit(
        query.data(), out.data(), static_cast<uint32_t>(query.size()));
    EXPECT_FLOAT_EQ(width, 1.0f);
    for (int16_t v : out) {
        EXPECT_EQ(v, 0);
    }
}

TEST(PrepareQuerySimdTest, QuantizeQuery14BitMatchesScalar) {
    for (uint32_t dim : {7u, 16u, 37u, 64u, 512u}) {
        auto query = MakeInput(dim, 0.05f);
        std::vector<int16_t> simd_out(dim, 0), scalar_out(dim, 0);

        const float simd_width = vdb::simd::QuantizeQuery14Bit(
            query.data(), simd_out.data(), dim);
        const float scalar_width = ScalarQuantizeQuery14Bit(
            query.data(), scalar_out.data(), dim);

        EXPECT_FLOAT_EQ(simd_width, scalar_width);
        EXPECT_EQ(simd_out, scalar_out);
    }
}

TEST(PrepareQuerySimdTest, BuildFastScanLUTMatchesReference) {
    for (uint32_t dim : {16u, 64u, 512u}) {
        std::vector<int16_t> quant_query(dim, 0);
        for (uint32_t i = 0; i < dim; ++i) {
            int sign = (i % 5 == 0 || i % 7 == 0) ? -1 : 1;
            quant_query[i] = static_cast<int16_t>(
                sign * static_cast<int>((i * 97u) % 8191u));
        }

        const size_t lut_size = static_cast<size_t>(dim) * 8u;
        auto actual = MakeAlignedByteBuffer(lut_size);
        auto expected = MakeAlignedByteBuffer(lut_size);

        const int32_t actual_shift =
            vdb::simd::BuildFastScanLUT(quant_query.data(), actual.get(), dim);
        const int32_t expected_shift =
            BuildFastScanLUTReference(quant_query.data(), expected.get(), dim);

        EXPECT_EQ(actual_shift, expected_shift);
        EXPECT_EQ(std::memcmp(actual.get(), expected.get(), lut_size), 0);
    }
}

TEST(PrepareQuerySimdTest, FastScanPipelineMatchesScalarReference) {
    for (uint32_t dim : {16u, 32u, 64u, 96u, 512u, 544u, 768u, 1024u, 1056u, 1536u}) {
        ExpectFastScanPipelineMatchesScalarReference(dim);
    }
}
