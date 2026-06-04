#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "vdb/simd/ip_exrabitq.h"

namespace {

float ScalarIPExRaBitQ(const float* query,
                       const uint8_t* code_abs,
                       const uint8_t* sign,
                       uint32_t dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        const float s = sign[i] ? 1.0f : -1.0f;
        sum += query[i] * s * (static_cast<float>(code_abs[i]) + 0.5f);
    }
    return sum;
}

std::vector<float> MakeQuery(uint32_t dim) {
    std::vector<float> out(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        const float sign = (i % 3 == 0) ? -1.0f : 1.0f;
        out[i] = sign * (0.125f * static_cast<float>((i % 17) + 1));
    }
    return out;
}

std::vector<uint8_t> MakeCodeAbs(uint32_t dim) {
    std::vector<uint8_t> out(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        out[i] = static_cast<uint8_t>((i * 11u + 7u) & 0x0F);
    }
    return out;
}

std::vector<uint8_t> MakeSign(uint32_t dim, int mode) {
    std::vector<uint8_t> out(dim, 1u);
    for (uint32_t i = 0; i < dim; ++i) {
        if (mode == 0) {
            out[i] = 1u;
        } else if (mode == 1) {
            out[i] = 0u;
        } else {
            out[i] = static_cast<uint8_t>(((i * 5u) + 3u) & 1u);
        }
    }
    return out;
}

void BuildParallelCompactLayout(uint32_t dim,
                                uint32_t dim_block,
                                const std::vector<std::vector<uint8_t>>& lane_abs,
                                const std::vector<std::vector<uint8_t>>& lane_sign,
                                std::vector<uint8_t>& abs_slices,
                                std::vector<uint16_t>& sign_words) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t slices_per_dim_block = (dim_block + 15) / 16;
    abs_slices.assign(
        static_cast<size_t>(num_dim_blocks) * slices_per_dim_block * 8u * 16u,
        0);
    sign_words.assign(
        static_cast<size_t>(num_dim_blocks) * slices_per_dim_block * 8u,
        0);
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        for (uint32_t sub = 0; sub < slices_per_dim_block; ++sub) {
            const uint32_t dim_start = db * dim_block + sub * 16u;
            for (uint32_t lane = 0; lane < 8; ++lane) {
                uint16_t sign_word = 0;
                for (uint32_t offset = 0; offset < 16; ++offset) {
                    const uint32_t d = dim_start + offset;
                    const size_t abs_idx =
                        ((static_cast<size_t>(db) * slices_per_dim_block + sub) * 8u +
                         lane) * 16u + offset;
                    if (d < dim) {
                        abs_slices[abs_idx] = lane_abs[lane][d];
                        if (lane_sign[lane][d] != 0) {
                            sign_word |= static_cast<uint16_t>(1u << offset);
                        }
                    }
                }
                sign_words[(static_cast<size_t>(db) * slices_per_dim_block + sub) * 8u +
                           lane] = sign_word;
            }
        }
    }
}

}  // namespace

TEST(IPExRaBitQTest, PackedMagnitudeRoundTripBits2And4) {
    for (uint8_t bits : {2u, 4u}) {
        const uint32_t max_value = (1u << bits) - 1u;
        for (uint32_t dim : {1u, 7u, 16u, 63u, 64u, 70u, 512u}) {
            std::vector<uint8_t> decoded(dim);
            for (uint32_t i = 0; i < dim; ++i) {
                decoded[i] = static_cast<uint8_t>((i * 13u + 5u) & max_value);
            }
            const uint32_t packed_bytes =
                vdb::simd::ExRaBitQPackedMagnitudeBytes(dim, bits);
            ASSERT_GT(packed_bytes, 0u);
            std::vector<uint8_t> packed(packed_bytes, 0xA5u);
            std::vector<uint8_t> unpacked(dim, 0);

            ASSERT_TRUE(vdb::simd::ExRaBitQPackMagnitudes(
                decoded.data(), dim, bits, packed.data(), packed_bytes));
            ASSERT_TRUE(vdb::simd::ExRaBitQUnpackMagnitudes(
                packed.data(), dim, bits, unpacked.data()));
            EXPECT_EQ(unpacked, decoded) << "bits=" << static_cast<int>(bits)
                                         << " dim=" << dim;
        }
    }
}

TEST(IPExRaBitQTest, PackedMagnitudeRejectsUnsupportedBitsAndOutOfRangeValues) {
    uint8_t decoded[4] = {0, 1, 2, 3};
    uint8_t packed[4] = {};
    EXPECT_EQ(vdb::simd::ExRaBitQPackedMagnitudeBytes(64, 3), 0u);
    EXPECT_FALSE(vdb::simd::ExRaBitQPackMagnitudes(
        decoded, 4, 3, packed, sizeof(packed)));

    decoded[3] = 4;
    EXPECT_FALSE(vdb::simd::ExRaBitQPackMagnitudes(
        decoded, 4, 2, packed, sizeof(packed)));
}

TEST(IPExRaBitQTest, PackedBatchBlockDecodeMatchesCompactLayout) {
    constexpr uint32_t batch_size = 8;
    constexpr uint32_t dim_block = 64;
    constexpr uint32_t num_dim_blocks = 2;
    for (uint8_t bits : {2u, 4u}) {
        const uint32_t max_value = (1u << bits) - 1u;
        const uint32_t packed_lane_bytes =
            vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, bits);
        std::vector<uint8_t> decoded_compact(
            static_cast<size_t>(num_dim_blocks) * batch_size * dim_block, 0);
        std::vector<uint8_t> packed(
            static_cast<size_t>(num_dim_blocks) * batch_size * packed_lane_bytes,
            0);
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            for (uint32_t lane = 0; lane < batch_size; ++lane) {
                uint8_t* decoded_lane =
                    decoded_compact.data() +
                    (static_cast<size_t>(db) * batch_size + lane) * dim_block;
                for (uint32_t d = 0; d < dim_block; ++d) {
                    decoded_lane[d] = static_cast<uint8_t>(
                        (db * 17u + lane * 7u + d * 3u) & max_value);
                }
                uint8_t* packed_lane =
                    packed.data() +
                    (static_cast<size_t>(db) * batch_size + lane) * packed_lane_bytes;
                ASSERT_TRUE(vdb::simd::ExRaBitQPackMagnitudes(
                    decoded_lane, dim_block, bits, packed_lane, packed_lane_bytes));
            }
        }

        std::vector<uint8_t> unpacked(decoded_compact.size(), 0);
        ASSERT_TRUE(vdb::simd::ExRaBitQDecodePackedBatchBlockMagnitudes(
            packed.data(), num_dim_blocks, batch_size, dim_block,
            packed_lane_bytes, bits, unpacked.data()));
        EXPECT_EQ(unpacked, decoded_compact)
            << "bits=" << static_cast<int>(bits);
    }
}

TEST(IPExRaBitQTest, MatchesScalarAcrossDimsAndSignPatterns) {
    for (uint32_t dim : {16u, 32u, 64u, 70u, 512u}) {
        const auto query = MakeQuery(dim);
        const auto code_abs = MakeCodeAbs(dim);

        for (int sign_mode = 0; sign_mode < 3; ++sign_mode) {
            const auto sign = MakeSign(dim, sign_mode);

            const float actual = vdb::simd::IPExRaBitQ(
                query.data(), code_abs.data(), sign.data(), dim);
            const float expected = ScalarIPExRaBitQ(
                query.data(), code_abs.data(), sign.data(), dim);

            EXPECT_NEAR(actual, expected, 1e-4f)
                << "dim=" << dim << " sign_mode=" << sign_mode;
        }
    }
}

TEST(IPExRaBitQTest, MaskedParallelCompactMatchesFullKernelForSelectedLanes) {
    for (uint32_t dim : {512u, 768u, 1024u}) {
        for (uint32_t dim_block : {32u, 64u}) {
            const uint32_t slices_per_dim_block = (dim_block + 15) / 16;
            const auto query = MakeQuery(dim);
            std::vector<std::vector<uint8_t>> lane_abs(8);
            std::vector<std::vector<uint8_t>> lane_sign(8);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                lane_abs[lane] = MakeCodeAbs(dim);
                lane_sign[lane] = MakeSign(dim, static_cast<int>(lane % 3));
                for (uint32_t d = 0; d < dim; ++d) {
                    lane_abs[lane][d] =
                        static_cast<uint8_t>((lane_abs[lane][d] + lane * 3u + d) & 0x0F);
                }
            }

            std::vector<uint8_t> abs_slices;
            std::vector<uint16_t> sign_words;
            BuildParallelCompactLayout(
                dim, dim_block, lane_abs, lane_sign, abs_slices, sign_words);

            std::vector<float> full(8, 0.0f);
            vdb::simd::IPExRaBitQBatchPackedSignParallelCompact(
                query.data(), abs_slices.data(), sign_words.data(), 8, dim,
                dim_block, slices_per_dim_block, full.data());

            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> masked(8, 0.0f);
                vdb::simd::IPExRaBitQBatchPackedSignParallelCompactMasked(
                    query.data(), abs_slices.data(), sign_words.data(), mask, 8, dim,
                    dim_block, slices_per_dim_block, masked.data());
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        continue;
                    }
                    EXPECT_NEAR(masked[lane], full[lane], 1e-3f)
                        << "dim=" << dim << " dim_block=" << dim_block
                        << " mask=" << mask << " lane=" << lane;
                }
            }
        }
    }
}
