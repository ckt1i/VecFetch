#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "vdb/simd/ip_exrabitq.h"

namespace {

float ScalarIPExRaBitQ(const float* query, const uint8_t* code_abs, const uint8_t* sign,
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

float ScalarOfficialExDataDot(const float* query, const uint8_t* ex_code, uint32_t dim) {
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        sum += query[i] * static_cast<float>(ex_code[i]);
    }
    return sum;
}

float ScalarOfficialExDataDotActiveBits(const float* query, const uint8_t* ex_code,
                                        uint32_t dim, uint8_t active_bits) {
    if (active_bits == 0) {
        return 0.0f;
    }
    const uint8_t active_mask = static_cast<uint8_t>((1u << active_bits) - 1u);
    float sum = 0.0f;
    for (uint32_t i = 0; i < dim; ++i) {
        sum += query[i] * static_cast<float>(ex_code[i] & active_mask);
    }
    return sum;
}

void BuildOfficialCompactLayout(uint32_t dim, uint32_t dim_block,
                                const std::vector<std::vector<uint8_t>>& lane_ex_code,
                                std::vector<uint8_t>& ex_code_blocks) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    ex_code_blocks.assign(static_cast<size_t>(num_dim_blocks) * 8u * dim_block, 0);
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        for (uint32_t lane = 0; lane < 8; ++lane) {
            uint8_t* dst =
                ex_code_blocks.data() + (static_cast<size_t>(db) * 8u + lane) * dim_block;
            for (uint32_t offset = 0; offset < dim_block; ++offset) {
                const uint32_t d = dim_start + offset;
                if (d < dim) {
                    dst[offset] = lane_ex_code[lane][d];
                }
            }
        }
    }
}

void BuildOfficialDirect3Layout(uint32_t dim, uint32_t dim_block,
                                vdb::RaBitQExDataLayout layout,
                                const std::vector<std::vector<uint8_t>>& lane_ex_code,
                                std::vector<uint8_t>& compact_blocks) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, 3);
    compact_blocks.assign(static_cast<size_t>(num_dim_blocks) * 8u * packed_lane_bytes, 0);
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        for (uint32_t lane = 0; lane < 8; ++lane) {
            uint8_t decoded[64] = {};
            const uint32_t copy = std::min(dim_block, dim - dim_start);
            for (uint32_t offset = 0; offset < copy; ++offset) {
                decoded[offset] = lane_ex_code[lane][dim_start + offset];
            }
            uint8_t* dst =
                compact_blocks.data() +
                (static_cast<size_t>(db) * 8u + lane) * packed_lane_bytes;
            ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialDirect3(decoded, dim_block, layout, dst,
                                                               packed_lane_bytes));
        }
    }
}

void BuildOfficialDirectBitplanesLayout(uint32_t dim, uint32_t dim_block, uint8_t bits,
                                        const std::vector<std::vector<uint8_t>>& lane_ex_code,
                                        std::vector<uint8_t>& compact_blocks) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, bits);
    compact_blocks.assign(static_cast<size_t>(num_dim_blocks) * 8u * packed_lane_bytes, 0);
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        const uint32_t dim_start = db * dim_block;
        for (uint32_t lane = 0; lane < 8; ++lane) {
            uint8_t decoded[64] = {};
            const uint32_t copy = std::min(dim_block, dim - dim_start);
            for (uint32_t offset = 0; offset < copy; ++offset) {
                decoded[offset] = lane_ex_code[lane][dim_start + offset];
            }
            uint8_t* dst =
                compact_blocks.data() +
                (static_cast<size_t>(db) * 8u + lane) * packed_lane_bytes;
            ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialDirectBitplanes(
                decoded, dim_block, bits, dst, packed_lane_bytes));
        }
    }
}

void BuildOfficialVectorBitplanesLayout(uint32_t dim, uint32_t dim_block, uint8_t bits,
                                        const std::vector<std::vector<uint8_t>>& lane_ex_code,
                                        std::vector<uint8_t>& compact_blocks) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, bits);
    compact_blocks.assign(static_cast<size_t>(8u) * num_dim_blocks * packed_lane_bytes, 0);
    for (uint32_t lane = 0; lane < 8; ++lane) {
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const uint32_t dim_start = db * dim_block;
            uint8_t decoded[64] = {};
            const uint32_t copy = std::min(dim_block, dim - dim_start);
            for (uint32_t offset = 0; offset < copy; ++offset) {
                decoded[offset] = lane_ex_code[lane][dim_start + offset];
            }
            uint8_t* dst =
                compact_blocks.data() +
                (static_cast<size_t>(lane) * num_dim_blocks + db) * packed_lane_bytes;
            ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialDirectBitplanes(
                decoded, dim_block, bits, dst, packed_lane_bytes));
        }
    }
}

void BuildOfficialVectorBitMajorTilesLayout(
    uint32_t dim, uint8_t bits, const std::vector<std::vector<uint8_t>>& lane_ex_code,
    std::vector<uint8_t>& compact_blocks) {
    const uint32_t vector_bytes = vdb::simd::ExRaBitQBitMajorTileVectorBytes(dim, bits);
    compact_blocks.assign(static_cast<size_t>(8u) * vector_bytes, 0);
    for (uint32_t lane = 0; lane < 8; ++lane) {
        uint8_t* dst = compact_blocks.data() + static_cast<size_t>(lane) * vector_bytes;
        ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialBitMajorTiles(
            lane_ex_code[lane].data(), dim, bits, dst, vector_bytes));
    }
}

void BuildOfficialTileLaneBitMajorLayout(
    uint32_t dim, uint8_t bits, const std::vector<std::vector<uint8_t>>& lane_ex_code,
    std::vector<uint8_t>& compact_blocks) {
    const uint32_t valid_count = static_cast<uint32_t>(lane_ex_code.size());
    const uint32_t batch_bytes =
        vdb::simd::ExRaBitQTileLaneBitMajorBatchBytes(dim, bits, valid_count);
    compact_blocks.assign(batch_bytes, 0);
    std::vector<const uint8_t*> lanes(valid_count, nullptr);
    for (uint32_t lane = 0; lane < valid_count; ++lane) {
        lanes[lane] = lane_ex_code[lane].data();
    }
    ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialTileLaneBitMajor(
        lanes.data(), valid_count, dim, bits, compact_blocks.data(), batch_bytes));
}

void BuildOfficialSmallLane4BitplanesLayout(
    uint32_t dim, uint32_t dim_block, uint8_t bits,
    const std::vector<std::vector<uint8_t>>& lane_ex_code,
    std::vector<uint8_t>& compact_blocks) {
    constexpr uint32_t kSubgroupLanes = 4;
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, bits);
    compact_blocks.assign(static_cast<size_t>(8u) * num_dim_blocks * packed_lane_bytes, 0);
    uint8_t* dst = compact_blocks.data();
    for (uint32_t group_start = 0; group_start < 8; group_start += kSubgroupLanes) {
        const uint32_t group_lanes = std::min(kSubgroupLanes, 8u - group_start);
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const uint32_t dim_start = db * dim_block;
            for (uint32_t local_lane = 0; local_lane < group_lanes; ++local_lane) {
                const uint32_t lane = group_start + local_lane;
                uint8_t decoded[64] = {};
                const uint32_t copy = std::min(dim_block, dim - dim_start);
                for (uint32_t offset = 0; offset < copy; ++offset) {
                    decoded[offset] = lane_ex_code[lane][dim_start + offset];
                }
                ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialDirectBitplanes(
                    decoded, dim_block, bits, dst, packed_lane_bytes));
                dst += packed_lane_bytes;
            }
        }
    }
}

void BuildOfficialSmallLane2BitplanesLayout(
    uint32_t dim, uint32_t dim_block, uint8_t bits,
    const std::vector<std::vector<uint8_t>>& lane_ex_code,
    std::vector<uint8_t>& compact_blocks) {
    constexpr uint32_t kSubgroupLanes = 2;
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, bits);
    compact_blocks.assign(static_cast<size_t>(8u) * num_dim_blocks * packed_lane_bytes, 0);
    uint8_t* dst = compact_blocks.data();
    for (uint32_t group_start = 0; group_start < 8; group_start += kSubgroupLanes) {
        const uint32_t group_lanes = std::min(kSubgroupLanes, 8u - group_start);
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const uint32_t dim_start = db * dim_block;
            for (uint32_t local_lane = 0; local_lane < group_lanes; ++local_lane) {
                const uint32_t lane = group_start + local_lane;
                uint8_t decoded[64] = {};
                const uint32_t copy = std::min(dim_block, dim - dim_start);
                for (uint32_t offset = 0; offset < copy; ++offset) {
                    decoded[offset] = lane_ex_code[lane][dim_start + offset];
                }
                ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialDirectBitplanes(
                    decoded, dim_block, bits, dst, packed_lane_bytes));
                dst += packed_lane_bytes;
            }
        }
    }
}

void BuildOfficialVectorNibble4Layout(uint32_t dim, uint32_t dim_block,
                                      const std::vector<std::vector<uint8_t>>& lane_ex_code,
                                      std::vector<uint8_t>& compact_blocks) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, 4);
    compact_blocks.assign(static_cast<size_t>(8u) * num_dim_blocks * packed_lane_bytes, 0);
    for (uint32_t lane = 0; lane < 8; ++lane) {
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const uint32_t dim_start = db * dim_block;
            uint8_t decoded[64] = {};
            const uint32_t copy = std::min(dim_block, dim - dim_start);
            for (uint32_t offset = 0; offset < copy; ++offset) {
                decoded[offset] = lane_ex_code[lane][dim_start + offset];
            }
            uint8_t* dst =
                compact_blocks.data() +
                (static_cast<size_t>(lane) * num_dim_blocks + db) * packed_lane_bytes;
            ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialNibble4(
                decoded, dim_block, dst, packed_lane_bytes));
        }
    }
}

void BuildOfficialVector2BitLayout(uint32_t dim, uint32_t dim_block,
                                   const std::vector<std::vector<uint8_t>>& lane_ex_code,
                                   std::vector<uint8_t>& compact_blocks) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, 2);
    compact_blocks.assign(static_cast<size_t>(8u) * num_dim_blocks * packed_lane_bytes, 0);
    for (uint32_t lane = 0; lane < 8; ++lane) {
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const uint32_t dim_start = db * dim_block;
            uint8_t decoded[64] = {};
            const uint32_t copy = std::min(dim_block, dim - dim_start);
            for (uint32_t offset = 0; offset < copy; ++offset) {
                decoded[offset] = lane_ex_code[lane][dim_start + offset];
            }
            uint8_t* dst =
                compact_blocks.data() +
                (static_cast<size_t>(lane) * num_dim_blocks + db) * packed_lane_bytes;
            ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficial2Bit(
                decoded, dim_block, dst, packed_lane_bytes));
        }
    }
}

void BuildParallelCompactLayout(uint32_t dim, uint32_t dim_block,
                                const std::vector<std::vector<uint8_t>>& lane_abs,
                                const std::vector<std::vector<uint8_t>>& lane_sign,
                                std::vector<uint8_t>& abs_slices,
                                std::vector<uint16_t>& sign_words) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t slices_per_dim_block = (dim_block + 15) / 16;
    abs_slices.assign(static_cast<size_t>(num_dim_blocks) * slices_per_dim_block * 8u * 16u, 0);
    sign_words.assign(static_cast<size_t>(num_dim_blocks) * slices_per_dim_block * 8u, 0);
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        for (uint32_t sub = 0; sub < slices_per_dim_block; ++sub) {
            const uint32_t dim_start = db * dim_block + sub * 16u;
            for (uint32_t lane = 0; lane < 8; ++lane) {
                uint16_t sign_word = 0;
                for (uint32_t offset = 0; offset < 16; ++offset) {
                    const uint32_t d = dim_start + offset;
                    const size_t abs_idx =
                        ((static_cast<size_t>(db) * slices_per_dim_block + sub) * 8u + lane) * 16u +
                        offset;
                    if (d < dim) {
                        abs_slices[abs_idx] = lane_abs[lane][d];
                        if (lane_sign[lane][d] != 0) {
                            sign_word |= static_cast<uint16_t>(1u << offset);
                        }
                    }
                }
                sign_words[(static_cast<size_t>(db) * slices_per_dim_block + sub) * 8u + lane] =
                    sign_word;
            }
        }
    }
}

} // namespace

TEST(IPExRaBitQTest, PackedMagnitudeRoundTripBits2And3And4) {
    for (uint8_t bits : {2u, 3u, 4u}) {
        const uint32_t max_value = (1u << bits) - 1u;
        for (uint32_t dim : {1u, 7u, 16u, 63u, 64u, 70u, 512u}) {
            std::vector<uint8_t> decoded(dim);
            for (uint32_t i = 0; i < dim; ++i) {
                decoded[i] = static_cast<uint8_t>((i * 13u + 5u) & max_value);
            }
            const uint32_t packed_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim, bits);
            ASSERT_GT(packed_bytes, 0u);
            std::vector<uint8_t> packed(packed_bytes, 0xA5u);
            std::vector<uint8_t> unpacked(dim, 0);

            ASSERT_TRUE(vdb::simd::ExRaBitQPackMagnitudes(decoded.data(), dim, bits, packed.data(),
                                                          packed_bytes));
            ASSERT_TRUE(
                vdb::simd::ExRaBitQUnpackMagnitudes(packed.data(), dim, bits, unpacked.data()));
            EXPECT_EQ(unpacked, decoded) << "bits=" << static_cast<int>(bits) << " dim=" << dim;
        }
    }
}

TEST(IPExRaBitQTest, OfficialDirect3RoundTripBothLayouts) {
    for (auto layout : {vdb::RaBitQExDataLayout::kSplit3TwoPlusOne,
                        vdb::RaBitQExDataLayout::kSplit3Bitplanes}) {
        for (uint32_t dim : {1u, 7u, 16u, 63u, 64u, 70u, 512u}) {
            std::vector<uint8_t> decoded(dim);
            for (uint32_t i = 0; i < dim; ++i) {
                decoded[i] = static_cast<uint8_t>((i * 13u + 5u) & 0x07u);
            }
            const uint32_t chunks = (dim + 63u) / 64u;
            const uint32_t packed_bytes = chunks * 24u;
            std::vector<uint8_t> packed(packed_bytes, 0xA5u);
            std::vector<uint8_t> unpacked(dim, 0);

            ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialDirect3(
                decoded.data(), dim, layout, packed.data(), packed_bytes));
            ASSERT_TRUE(vdb::simd::ExRaBitQUnpackOfficialDirect3(
                packed.data(), dim, layout, unpacked.data()));
            EXPECT_EQ(unpacked, decoded)
                << "layout=" << std::string(vdb::RaBitQExDataLayoutName(layout))
                << " dim=" << dim;
        }
    }
}

TEST(IPExRaBitQTest, OfficialDirectBitplanesRoundTripBits1234) {
    for (uint8_t bits : {1u, 2u, 3u, 4u}) {
        const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
        for (uint32_t dim : {1u, 7u, 16u, 63u, 64u, 70u, 512u}) {
            std::vector<uint8_t> decoded(dim);
            for (uint32_t i = 0; i < dim; ++i) {
                decoded[i] = static_cast<uint8_t>((i * 13u + 5u) & max_value);
            }
            const uint32_t chunks = (dim + 63u) / 64u;
            const uint32_t packed_bytes =
                chunks * vdb::simd::ExRaBitQPackedMagnitudeBytes(64, bits);
            std::vector<uint8_t> packed(packed_bytes, 0xA5u);
            std::vector<uint8_t> unpacked(dim, 0);

            ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialDirectBitplanes(
                decoded.data(), dim, bits, packed.data(), packed_bytes));
            ASSERT_TRUE(vdb::simd::ExRaBitQUnpackOfficialDirectBitplanes(
                packed.data(), dim, bits, unpacked.data()));
            EXPECT_EQ(unpacked, decoded)
                << "bits=" << static_cast<int>(bits) << " dim=" << dim;
        }
    }
}

TEST(IPExRaBitQTest, OfficialBitMajorTilesRoundTripBits123) {
    EXPECT_EQ(vdb::simd::ExRaBitQBitMajorTileVectorBytes(192, 1), 32u);
    EXPECT_EQ(vdb::simd::ExRaBitQBitMajorTileVectorBytes(512, 1), 64u);
    EXPECT_EQ(vdb::simd::ExRaBitQBitMajorTileVectorBytes(768, 1), 96u);
    for (uint8_t bits : {1u, 2u, 3u}) {
        const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
        for (uint32_t dim : {1u, 7u, 16u, 63u, 64u, 70u, 128u, 192u, 512u, 768u}) {
            std::vector<uint8_t> decoded(dim);
            for (uint32_t i = 0; i < dim; ++i) {
                decoded[i] = static_cast<uint8_t>((i * 11u + 7u) & max_value);
            }
            const uint32_t packed_bytes =
                vdb::simd::ExRaBitQBitMajorTileVectorBytes(dim, bits);
            std::vector<uint8_t> packed(packed_bytes, 0xA5u);
            std::vector<uint8_t> unpacked(dim, 0);

            ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialBitMajorTiles(
                decoded.data(), dim, bits, packed.data(), packed_bytes));
            ASSERT_TRUE(vdb::simd::ExRaBitQUnpackOfficialBitMajorTiles(
                packed.data(), dim, bits, unpacked.data()));
            EXPECT_EQ(unpacked, decoded)
                << "bits=" << static_cast<int>(bits) << " dim=" << dim;
        }
    }
}

TEST(IPExRaBitQTest, OfficialNibble4RoundTrip) {
    for (uint32_t dim : {1u, 7u, 16u, 31u, 64u, 70u, 512u}) {
        std::vector<uint8_t> decoded(dim);
        for (uint32_t i = 0; i < dim; ++i) {
            decoded[i] = static_cast<uint8_t>((i * 5u + 3u) & 0x0Fu);
        }
        const uint32_t groups = (dim + 15u) / 16u;
        std::vector<uint8_t> packed(groups * 8u, 0xA5u);
        std::vector<uint8_t> unpacked(dim, 0);

        ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficialNibble4(
            decoded.data(), dim, packed.data(), static_cast<uint32_t>(packed.size())));
        ASSERT_TRUE(vdb::simd::ExRaBitQUnpackOfficialNibble4(
            packed.data(), dim, unpacked.data()));
        EXPECT_EQ(unpacked, decoded) << "dim=" << dim;
    }

    uint8_t invalid[16] = {};
    invalid[3] = 16;
    uint8_t packed[8] = {};
    EXPECT_FALSE(vdb::simd::ExRaBitQPackOfficialNibble4(
        invalid, 16, packed, static_cast<uint32_t>(sizeof(packed))));
}

TEST(IPExRaBitQTest, Official2BitRoundTrip) {
    for (uint32_t dim : {1u, 7u, 16u, 31u, 64u, 70u, 512u}) {
        std::vector<uint8_t> decoded(dim);
        for (uint32_t i = 0; i < dim; ++i) {
            decoded[i] = static_cast<uint8_t>((i * 3u + 1u) & 0x03u);
        }
        const uint32_t blocks = (dim + 63u) / 64u;
        std::vector<uint8_t> packed(blocks * 16u, 0xA5u);
        std::vector<uint8_t> unpacked(dim, 0);

        ASSERT_TRUE(vdb::simd::ExRaBitQPackOfficial2Bit(
            decoded.data(), dim, packed.data(), static_cast<uint32_t>(packed.size())));
        ASSERT_TRUE(vdb::simd::ExRaBitQUnpackOfficial2Bit(
            packed.data(), dim, unpacked.data()));
        EXPECT_EQ(unpacked, decoded) << "dim=" << dim;
    }

    uint8_t invalid[64] = {};
    invalid[4] = 4;
    uint8_t packed[16] = {};
    EXPECT_FALSE(vdb::simd::ExRaBitQPackOfficial2Bit(
        invalid, 64, packed, static_cast<uint32_t>(sizeof(packed))));
}

TEST(IPExRaBitQTest, PackedMagnitudeRejectsUnsupportedBitsAndOutOfRangeValues) {
    uint8_t decoded[4] = {0, 1, 0, 1};
    uint8_t packed[4] = {};
    EXPECT_EQ(vdb::simd::ExRaBitQPackedMagnitudeBytes(64, 1), 8u);
    EXPECT_TRUE(vdb::simd::ExRaBitQPackMagnitudes(decoded, 4, 1, packed, sizeof(packed)));

    decoded[3] = 2;
    EXPECT_FALSE(vdb::simd::ExRaBitQPackMagnitudes(decoded, 4, 1, packed, sizeof(packed)));

    decoded[3] = 4;
    EXPECT_FALSE(vdb::simd::ExRaBitQPackMagnitudes(decoded, 4, 2, packed, sizeof(packed)));

    decoded[3] = 8;
    EXPECT_FALSE(vdb::simd::ExRaBitQPackMagnitudes(decoded, 4, 3, packed, sizeof(packed)));
}

TEST(IPExRaBitQTest, PackedBatchBlockDecodeMatchesCompactLayout) {
    constexpr uint32_t batch_size = 8;
    constexpr uint32_t dim_block = 64;
    constexpr uint32_t num_dim_blocks = 2;
    for (uint8_t bits : {1u, 2u, 3u, 4u}) {
        const uint32_t max_value = (1u << bits) - 1u;
        const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, bits);
        std::vector<uint8_t> decoded_compact(
            static_cast<size_t>(num_dim_blocks) * batch_size * dim_block, 0);
        std::vector<uint8_t> packed(
            static_cast<size_t>(num_dim_blocks) * batch_size * packed_lane_bytes, 0);
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            for (uint32_t lane = 0; lane < batch_size; ++lane) {
                uint8_t* decoded_lane = decoded_compact.data() +
                                        (static_cast<size_t>(db) * batch_size + lane) * dim_block;
                for (uint32_t d = 0; d < dim_block; ++d) {
                    decoded_lane[d] =
                        static_cast<uint8_t>((db * 17u + lane * 7u + d * 3u) & max_value);
                }
                uint8_t* packed_lane =
                    packed.data() +
                    (static_cast<size_t>(db) * batch_size + lane) * packed_lane_bytes;
                ASSERT_TRUE(vdb::simd::ExRaBitQPackMagnitudes(decoded_lane, dim_block, bits,
                                                              packed_lane, packed_lane_bytes));
            }
        }

        std::vector<uint8_t> unpacked(decoded_compact.size(), 0);
        ASSERT_TRUE(vdb::simd::ExRaBitQDecodePackedBatchBlockMagnitudes(
            packed.data(), num_dim_blocks, batch_size, dim_block, packed_lane_bytes, bits,
            unpacked.data()));
        EXPECT_EQ(unpacked, decoded_compact) << "bits=" << static_cast<int>(bits);
    }
}

TEST(IPExRaBitQTest, MatchesScalarAcrossDimsAndSignPatterns) {
    for (uint32_t dim : {16u, 32u, 64u, 70u, 512u}) {
        const auto query = MakeQuery(dim);
        const auto code_abs = MakeCodeAbs(dim);

        for (int sign_mode = 0; sign_mode < 3; ++sign_mode) {
            const auto sign = MakeSign(dim, sign_mode);

            const float actual =
                vdb::simd::IPExRaBitQ(query.data(), code_abs.data(), sign.data(), dim);
            const float expected =
                ScalarIPExRaBitQ(query.data(), code_abs.data(), sign.data(), dim);

            EXPECT_NEAR(actual, expected, 1e-4f) << "dim=" << dim << " sign_mode=" << sign_mode;
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
            BuildParallelCompactLayout(dim, dim_block, lane_abs, lane_sign, abs_slices, sign_words);

            std::vector<float> full(8, 0.0f);
            vdb::simd::IPExRaBitQBatchPackedSignParallelCompact(
                query.data(), abs_slices.data(), sign_words.data(), 8, dim, dim_block,
                slices_per_dim_block, full.data());

            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> masked(8, 0.0f);
                vdb::simd::IPExRaBitQBatchPackedSignParallelCompactMasked(
                    query.data(), abs_slices.data(), sign_words.data(), mask, 8, dim, dim_block,
                    slices_per_dim_block, masked.data());
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        continue;
                    }
                    EXPECT_NEAR(masked[lane], full[lane], 1e-3f)
                        << "dim=" << dim << " dim_block=" << dim_block << " mask=" << mask
                        << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialExDataDotMatchesScalarAcrossDims) {
    for (uint32_t dim : {1u, 7u, 16u, 63u, 64u, 70u, 512u}) {
        const auto query = MakeQuery(dim);
        for (uint8_t bits : {1u, 2u, 3u, 4u}) {
            const uint32_t max_value = (1u << bits) - 1u;
            std::vector<uint8_t> ex_code(dim);
            for (uint32_t i = 0; i < dim; ++i) {
                ex_code[i] = static_cast<uint8_t>((i * 5u + bits * 3u) & max_value);
            }

            const float actual =
                vdb::simd::IPOfficialRaBitQExData(query.data(), ex_code.data(), dim);
            const float expected = ScalarOfficialExDataDot(query.data(), ex_code.data(), dim);
            EXPECT_NEAR(actual, expected, 1e-4f)
                << "dim=" << dim << " bits=" << static_cast<int>(bits);
        }
    }
}

TEST(IPExRaBitQTest, OfficialCompactMaskedMatchesFullAndScalarForSelectedLanes) {
    for (uint32_t dim : {31u, 64u, 70u, 512u}) {
        for (uint32_t dim_block : {32u, 64u}) {
            const auto query = MakeQuery(dim);
            for (uint8_t bits : {2u, 3u, 4u}) {
                const uint32_t max_value = (1u << bits) - 1u;
                std::vector<std::vector<uint8_t>> lane_ex_code(8);
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    lane_ex_code[lane].resize(dim);
                    for (uint32_t d = 0; d < dim; ++d) {
                        lane_ex_code[lane][d] =
                            static_cast<uint8_t>((lane * 7u + d * 3u + bits) & max_value);
                    }
                }

                std::vector<uint8_t> ex_code_blocks;
                BuildOfficialCompactLayout(dim, dim_block, lane_ex_code, ex_code_blocks);

                std::vector<float> full(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompact(query.data(), ex_code_blocks.data(), 8, dim,
                                                        dim_block, full.data());

                for (uint32_t lane = 0; lane < 8; ++lane) {
                    const float expected =
                        ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                    EXPECT_NEAR(full[lane], expected, 1e-3f)
                        << "dim=" << dim << " dim_block=" << dim_block
                        << " bits=" << static_cast<int>(bits) << " lane=" << lane;
                }

                for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                    std::vector<float> masked(8, 0.0f);
                    vdb::simd::IPOfficialRaBitQBatchCompactMasked(query.data(),
                                                                  ex_code_blocks.data(), mask, 8,
                                                                  dim, dim_block, masked.data());
                    for (uint32_t lane = 0; lane < 8; ++lane) {
                        if ((mask & (1u << lane)) == 0) {
                            EXPECT_FLOAT_EQ(masked[lane], 0.0f);
                            continue;
                        }
                        EXPECT_NEAR(masked[lane], full[lane], 1e-3f)
                            << "dim=" << dim << " dim_block=" << dim_block
                            << " bits=" << static_cast<int>(bits) << " mask=" << mask
                            << " lane=" << lane;
                    }
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialDirect3CompactMaskedMatchesDecodedAndScalar) {
    for (auto layout : {vdb::RaBitQExDataLayout::kSplit3TwoPlusOne,
                        vdb::RaBitQExDataLayout::kSplit3Bitplanes}) {
        for (uint32_t dim : {31u, 64u, 70u, 512u}) {
            constexpr uint32_t dim_block = 64;
            const auto query = MakeQuery(dim);
            std::vector<std::vector<uint8_t>> lane_ex_code(8);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                lane_ex_code[lane].resize(dim);
                for (uint32_t d = 0; d < dim; ++d) {
                    lane_ex_code[lane][d] =
                        static_cast<uint8_t>((lane * 7u + d * 3u + 3u) & 0x07u);
                }
            }

            std::vector<uint8_t> decoded_blocks;
            BuildOfficialCompactLayout(dim, dim_block, lane_ex_code, decoded_blocks);
            std::vector<float> decoded_full(8, 0.0f);
            vdb::simd::IPOfficialRaBitQBatchCompact(query.data(), decoded_blocks.data(), 8,
                                                    dim, dim_block, decoded_full.data());

            std::vector<uint8_t> direct_blocks;
            BuildOfficialDirect3Layout(dim, dim_block, layout, lane_ex_code, direct_blocks);

            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactDirect3Masked(
                    query.data(), direct_blocks.data(), layout, mask, 8, dim, dim_block,
                    direct.data());
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        continue;
                    }
                    const float expected =
                        ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "layout=" << std::string(vdb::RaBitQExDataLayoutName(layout))
                        << " dim=" << dim << " lane=" << lane;
                    EXPECT_NEAR(direct[lane], decoded_full[lane], 1e-3f)
                        << "layout=" << std::string(vdb::RaBitQExDataLayoutName(layout))
                        << " dim=" << dim << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialDirectBitplanesCompactMaskedMatchesDecodedAndScalar) {
    for (uint8_t bits : {1u, 2u, 3u, 4u}) {
        const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
        for (uint32_t dim : {31u, 64u, 70u, 512u}) {
            constexpr uint32_t dim_block = 64;
            const auto query = MakeQuery(dim);
            std::vector<std::vector<uint8_t>> lane_ex_code(8);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                lane_ex_code[lane].resize(dim);
                for (uint32_t d = 0; d < dim; ++d) {
                    lane_ex_code[lane][d] =
                        static_cast<uint8_t>((lane * 7u + d * 3u + 3u) & max_value);
                }
            }

            std::vector<uint8_t> decoded_blocks;
            BuildOfficialCompactLayout(dim, dim_block, lane_ex_code, decoded_blocks);
            std::vector<float> decoded_full(8, 0.0f);
            vdb::simd::IPOfficialRaBitQBatchCompact(query.data(), decoded_blocks.data(), 8,
                                                    dim, dim_block, decoded_full.data());

            std::vector<uint8_t> direct_blocks;
            BuildOfficialDirectBitplanesLayout(dim, dim_block, bits, lane_ex_code,
                                               direct_blocks);

            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactDirectBitplanesMasked(
                    query.data(), direct_blocks.data(), bits, mask, 8, dim, dim_block,
                    direct.data());
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        continue;
                    }
                    const float expected =
                        ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "bits=" << static_cast<int>(bits)
                        << " dim=" << dim << " lane=" << lane;
                    EXPECT_NEAR(direct[lane], decoded_full[lane], 1e-3f)
                        << "bits=" << static_cast<int>(bits)
                        << " dim=" << dim << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialVectorBitplanesMaskedMatchesDecodedAndScalar) {
    for (uint8_t bits : {1u, 2u, 3u, 4u}) {
        const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
        for (uint32_t dim : {31u, 64u, 70u, 512u}) {
            constexpr uint32_t dim_block = 64;
            const auto query = MakeQuery(dim);
            std::vector<std::vector<uint8_t>> lane_ex_code(8);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                lane_ex_code[lane].resize(dim);
                for (uint32_t d = 0; d < dim; ++d) {
                    lane_ex_code[lane][d] =
                        static_cast<uint8_t>((lane * 7u + d * 3u + bits) & max_value);
                }
            }

            std::vector<uint8_t> vector_blocks;
            BuildOfficialVectorBitplanesLayout(dim, dim_block, bits, lane_ex_code, vector_blocks);

            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesMasked(
                    query.data(), vector_blocks.data(), bits, mask, 8, dim, dim_block,
                    direct.data());
                std::vector<float> prefetch(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesPrefetchMasked(
                    query.data(), vector_blocks.data(), bits, mask, 8, dim, dim_block,
                    prefetch.data());
                std::vector<float> microbatch(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesMicroBatchMasked(
                    query.data(), vector_blocks.data(), bits, mask, 8, dim, dim_block,
                    microbatch.data());
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        EXPECT_FLOAT_EQ(prefetch[lane], 0.0f);
                        EXPECT_FLOAT_EQ(microbatch[lane], 0.0f);
                        continue;
                    }
                    const float expected =
                        ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "bits=" << static_cast<int>(bits) << " dim=" << dim
                        << " lane=" << lane;
                    EXPECT_NEAR(prefetch[lane], expected, 1e-3f)
                        << "prefetch bits=" << static_cast<int>(bits) << " dim=" << dim
                        << " lane=" << lane;
                    EXPECT_NEAR(microbatch[lane], expected, 1e-3f)
                        << "microbatch bits=" << static_cast<int>(bits) << " dim=" << dim
                        << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialVectorBitplanesMaskedSupportsPartialActiveBits) {
    constexpr uint8_t stored_bits = 3;
    constexpr uint32_t dim_block = 64;
    constexpr uint8_t max_value = static_cast<uint8_t>((1u << stored_bits) - 1u);
    for (uint32_t dim : {31u, 64u, 70u, 512u}) {
        const auto query = MakeQuery(dim);
        std::vector<std::vector<uint8_t>> lane_ex_code(8);
        for (uint32_t lane = 0; lane < 8; ++lane) {
            lane_ex_code[lane].resize(dim);
            for (uint32_t d = 0; d < dim; ++d) {
                lane_ex_code[lane][d] =
                    static_cast<uint8_t>((lane * 7u + d * 3u + stored_bits) & max_value);
            }
        }

        std::vector<uint8_t> vector_blocks;
        BuildOfficialVectorBitplanesLayout(
            dim, dim_block, stored_bits, lane_ex_code, vector_blocks);

        for (uint8_t active_bits : {0u, 1u, 2u, 3u}) {
            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesMasked(
                    query.data(), vector_blocks.data(), stored_bits, mask, 8, dim,
                    dim_block, direct.data(), nullptr, active_bits);
                std::vector<float> prefetch(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesPrefetchMasked(
                    query.data(), vector_blocks.data(), stored_bits, mask, 8, dim,
                    dim_block, prefetch.data(), nullptr, active_bits);
                std::vector<float> microbatch(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesMicroBatchMasked(
                    query.data(), vector_blocks.data(), stored_bits, mask, 8, dim,
                    dim_block, microbatch.data(), nullptr, active_bits);
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        EXPECT_FLOAT_EQ(prefetch[lane], 0.0f);
                        EXPECT_FLOAT_EQ(microbatch[lane], 0.0f);
                        continue;
                    }
                    const float expected = ScalarOfficialExDataDotActiveBits(
                        query.data(), lane_ex_code[lane].data(), dim, active_bits);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "stored_bits=" << static_cast<int>(stored_bits)
                        << " active_bits=" << static_cast<int>(active_bits)
                        << " dim=" << dim << " lane=" << lane;
                    EXPECT_NEAR(prefetch[lane], expected, 1e-3f)
                        << "prefetch stored_bits=" << static_cast<int>(stored_bits)
                        << " active_bits=" << static_cast<int>(active_bits)
                        << " dim=" << dim << " lane=" << lane;
                    EXPECT_NEAR(microbatch[lane], expected, 1e-3f)
                        << "microbatch stored_bits=" << static_cast<int>(stored_bits)
                        << " active_bits=" << static_cast<int>(active_bits)
                        << " dim=" << dim << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialVectorBitMajorTilesMaskedMatchesDecodedAndScalar) {
    for (uint8_t bits : {1u, 2u, 3u}) {
        const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
        for (uint32_t dim : {31u, 64u, 70u, 192u, 512u, 768u}) {
            constexpr uint32_t dim_block = 64;
            const auto query = MakeQuery(dim);
            std::vector<std::vector<uint8_t>> lane_ex_code(8);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                lane_ex_code[lane].resize(dim);
                for (uint32_t d = 0; d < dim; ++d) {
                    lane_ex_code[lane][d] =
                        static_cast<uint8_t>((lane * 17u + d * 5u + bits) & max_value);
                }
            }

            std::vector<uint8_t> tile_blocks;
            BuildOfficialVectorBitMajorTilesLayout(dim, bits, lane_ex_code, tile_blocks);

            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactVectorBitMajorTilesMasked(
                    query.data(), tile_blocks.data(), bits, mask, 8, dim, dim_block,
                    direct.data());
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        continue;
                    }
                    const float expected =
                        ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "bits=" << static_cast<int>(bits) << " dim=" << dim
                        << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialVectorBitMajorTilesMaskedSupportsPartialActiveBits) {
    constexpr uint8_t stored_bits = 3;
    constexpr uint32_t dim_block = 64;
    constexpr uint8_t max_value = static_cast<uint8_t>((1u << stored_bits) - 1u);
    for (uint32_t dim : {31u, 64u, 70u, 192u, 512u, 768u}) {
        const auto query = MakeQuery(dim);
        std::vector<std::vector<uint8_t>> lane_ex_code(8);
        for (uint32_t lane = 0; lane < 8; ++lane) {
            lane_ex_code[lane].resize(dim);
            for (uint32_t d = 0; d < dim; ++d) {
                lane_ex_code[lane][d] =
                    static_cast<uint8_t>((lane * 17u + d * 5u + stored_bits) & max_value);
            }
        }

        std::vector<uint8_t> tile_blocks;
        BuildOfficialVectorBitMajorTilesLayout(dim, stored_bits, lane_ex_code, tile_blocks);

        for (uint8_t active_bits : {0u, 1u, 2u, 3u}) {
            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactVectorBitMajorTilesMasked(
                    query.data(), tile_blocks.data(), stored_bits, mask, 8, dim,
                    dim_block, direct.data(), nullptr, active_bits);
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        continue;
                    }
                    const float expected = ScalarOfficialExDataDotActiveBits(
                        query.data(), lane_ex_code[lane].data(), dim, active_bits);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "stored_bits=" << static_cast<int>(stored_bits)
                        << " active_bits=" << static_cast<int>(active_bits)
                        << " dim=" << dim << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialTileLaneBitMajorRoundTripBits123) {
    for (uint8_t bits : {1u, 2u, 3u}) {
        const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
        for (uint32_t dim : {31u, 64u, 70u, 192u, 512u, 768u}) {
            std::vector<std::vector<uint8_t>> lane_ex_code(8);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                lane_ex_code[lane].resize(dim);
                for (uint32_t d = 0; d < dim; ++d) {
                    lane_ex_code[lane][d] =
                        static_cast<uint8_t>((lane * 11u + d * 7u + bits) & max_value);
                }
            }

            std::vector<uint8_t> tile_blocks;
            BuildOfficialTileLaneBitMajorLayout(dim, bits, lane_ex_code, tile_blocks);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                std::vector<uint8_t> decoded(dim, 0);
                ASSERT_TRUE(vdb::simd::ExRaBitQUnpackOfficialTileLaneBitMajor(
                    tile_blocks.data(), 8, lane, dim, bits, decoded.data()));
                EXPECT_EQ(decoded, lane_ex_code[lane])
                    << "bits=" << static_cast<int>(bits) << " dim=" << dim
                    << " lane=" << lane;
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialTileLaneBitMajorMaskedSupportsPartialActiveBits) {
    constexpr uint8_t stored_bits = 3;
    constexpr uint32_t dim_block = 64;
    constexpr uint8_t max_value = static_cast<uint8_t>((1u << stored_bits) - 1u);
    for (uint32_t dim : {31u, 64u, 70u, 192u, 512u, 768u}) {
        const auto query = MakeQuery(dim);
        std::vector<std::vector<uint8_t>> lane_ex_code(8);
        for (uint32_t lane = 0; lane < 8; ++lane) {
            lane_ex_code[lane].resize(dim);
            for (uint32_t d = 0; d < dim; ++d) {
                lane_ex_code[lane][d] =
                    static_cast<uint8_t>((lane * 19u + d * 5u + stored_bits) & max_value);
            }
        }

        std::vector<uint8_t> tile_blocks;
        BuildOfficialTileLaneBitMajorLayout(dim, stored_bits, lane_ex_code, tile_blocks);

        for (uint8_t active_bits : {0u, 1u, 2u, 3u}) {
            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactTileLaneBitMajorMasked(
                    query.data(), tile_blocks.data(), stored_bits, mask, 8, dim,
                    dim_block, direct.data(), nullptr, active_bits);
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        continue;
                    }
                    const float expected = ScalarOfficialExDataDotActiveBits(
                        query.data(), lane_ex_code[lane].data(), dim, active_bits);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "stored_bits=" << static_cast<int>(stored_bits)
                        << " active_bits=" << static_cast<int>(active_bits)
                        << " dim=" << dim << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialTileLaneBitMajorBitDeltaAccumulatesToActiveBits) {
    constexpr uint8_t stored_bits = 3;
    constexpr uint32_t dim_block = 64;
    constexpr uint8_t max_value = static_cast<uint8_t>((1u << stored_bits) - 1u);
    for (uint32_t dim : {31u, 64u, 70u, 192u, 512u, 768u}) {
        const auto query = MakeQuery(dim);
        std::vector<std::vector<uint8_t>> lane_ex_code(8);
        for (uint32_t lane = 0; lane < 8; ++lane) {
            lane_ex_code[lane].resize(dim);
            for (uint32_t d = 0; d < dim; ++d) {
                lane_ex_code[lane][d] =
                    static_cast<uint8_t>((lane * 23u + d * 9u + stored_bits) & max_value);
            }
        }

        std::vector<uint8_t> tile_blocks;
        BuildOfficialTileLaneBitMajorLayout(dim, stored_bits, lane_ex_code, tile_blocks);

        for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
            std::vector<float> full(8, 0.0f);
            vdb::simd::IPOfficialRaBitQBatchCompactTileLaneBitMajorMasked(
                query.data(), tile_blocks.data(), stored_bits, mask, 8, dim, dim_block,
                full.data(), nullptr, stored_bits);

            std::vector<float> delta(8, 0.0f);
            for (uint8_t bit_id = 0; bit_id < stored_bits; ++bit_id) {
                vdb::simd::IPOfficialRaBitQBatchCompactTileLaneBitMajorBitDeltaMasked(
                    query.data(), tile_blocks.data(), stored_bits, bit_id, mask, 8, dim,
                    dim_block, delta.data());
            }

            std::vector<float> first_two(8, 0.0f);
            for (uint8_t bit_id = 0; bit_id < 2; ++bit_id) {
                vdb::simd::IPOfficialRaBitQBatchCompactTileLaneBitMajorBitDeltaMasked(
                    query.data(), tile_blocks.data(), stored_bits, bit_id, mask, 8, dim,
                    dim_block, first_two.data());
            }

            std::vector<float> range_tail(8, 0.0f);
            vdb::simd::IPOfficialRaBitQBatchCompactTileLaneBitMajorBitDeltaMasked(
                query.data(), tile_blocks.data(), stored_bits, /*bit_id=*/0, mask, 8, dim,
                dim_block, range_tail.data());
            vdb::simd::IPOfficialRaBitQBatchCompactTileLaneBitMajorBitRangeDeltaMasked(
                query.data(), tile_blocks.data(), stored_bits, /*first_bit=*/1,
                /*bit_count=*/2, mask, 8, dim, dim_block, range_tail.data());

            for (uint32_t lane = 0; lane < 8; ++lane) {
                if ((mask & (1u << lane)) == 0) {
                    EXPECT_FLOAT_EQ(delta[lane], 0.0f);
                    EXPECT_FLOAT_EQ(first_two[lane], 0.0f);
                    EXPECT_FLOAT_EQ(range_tail[lane], 0.0f);
                    continue;
                }
                const float expected_full = ScalarOfficialExDataDotActiveBits(
                    query.data(), lane_ex_code[lane].data(), dim, stored_bits);
                const float expected_two = ScalarOfficialExDataDotActiveBits(
                    query.data(), lane_ex_code[lane].data(), dim, 2);
                EXPECT_NEAR(full[lane], expected_full, 1e-3f)
                    << "dim=" << dim << " lane=" << lane;
                EXPECT_NEAR(delta[lane], full[lane], 1e-3f)
                    << "dim=" << dim << " lane=" << lane;
                EXPECT_NEAR(range_tail[lane], full[lane], 1e-3f)
                    << "dim=" << dim << " lane=" << lane;
                EXPECT_NEAR(first_two[lane], expected_two, 1e-3f)
                    << "dim=" << dim << " lane=" << lane;
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialSmallLane4BitplanesMaskedMatchesDecodedAndScalar) {
    for (uint8_t bits : {1u, 2u, 3u, 4u}) {
        const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
        for (uint32_t dim : {31u, 64u, 70u, 512u}) {
            constexpr uint32_t dim_block = 64;
            const auto query = MakeQuery(dim);
            std::vector<std::vector<uint8_t>> lane_ex_code(8);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                lane_ex_code[lane].resize(dim);
                for (uint32_t d = 0; d < dim; ++d) {
                    lane_ex_code[lane][d] =
                        static_cast<uint8_t>((lane * 13u + d * 5u + bits) & max_value);
                }
            }

            std::vector<uint8_t> compact_blocks;
            BuildOfficialSmallLane4BitplanesLayout(
                dim, dim_block, bits, lane_ex_code, compact_blocks);

            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactSmallLane4BitplanesMasked(
                    query.data(), compact_blocks.data(), bits, mask, 8, dim, dim_block,
                    direct.data());
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        continue;
                    }
                    const float expected =
                        ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "bits=" << static_cast<int>(bits) << " dim=" << dim
                        << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialSmallLane2BitplanesMaskedMatchesDecodedAndScalar) {
    for (uint8_t bits : {1u, 2u, 3u, 4u}) {
        const uint8_t max_value = static_cast<uint8_t>((1u << bits) - 1u);
        for (uint32_t dim : {31u, 64u, 70u, 512u}) {
            constexpr uint32_t dim_block = 64;
            const auto query = MakeQuery(dim);
            std::vector<std::vector<uint8_t>> lane_ex_code(8);
            for (uint32_t lane = 0; lane < 8; ++lane) {
                lane_ex_code[lane].resize(dim);
                for (uint32_t d = 0; d < dim; ++d) {
                    lane_ex_code[lane][d] =
                        static_cast<uint8_t>((lane * 17u + d * 7u + bits) & max_value);
                }
            }

            std::vector<uint8_t> compact_blocks;
            BuildOfficialSmallLane2BitplanesLayout(
                dim, dim_block, bits, lane_ex_code, compact_blocks);

            for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
                std::vector<float> direct(8, 0.0f);
                vdb::simd::IPOfficialRaBitQBatchCompactSmallLane2BitplanesMasked(
                    query.data(), compact_blocks.data(), bits, mask, 8, dim, dim_block,
                    direct.data());
                for (uint32_t lane = 0; lane < 8; ++lane) {
                    if ((mask & (1u << lane)) == 0) {
                        EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                        continue;
                    }
                    const float expected =
                        ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                    EXPECT_NEAR(direct[lane], expected, 1e-3f)
                        << "bits=" << static_cast<int>(bits) << " dim=" << dim
                        << " lane=" << lane;
                }
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialVectorNibble4MaskedMatchesDecodedAndScalar) {
    for (uint32_t dim : {31u, 64u, 70u, 512u}) {
        constexpr uint32_t dim_block = 64;
        const auto query = MakeQuery(dim);
        std::vector<std::vector<uint8_t>> lane_ex_code(8);
        for (uint32_t lane = 0; lane < 8; ++lane) {
            lane_ex_code[lane].resize(dim);
            for (uint32_t d = 0; d < dim; ++d) {
                lane_ex_code[lane][d] =
                    static_cast<uint8_t>((lane * 11u + d * 5u + 7u) & 0x0Fu);
            }
        }

        std::vector<uint8_t> nibble_blocks;
        BuildOfficialVectorNibble4Layout(dim, dim_block, lane_ex_code, nibble_blocks);

        for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
            std::vector<float> direct(8, 0.0f);
            vdb::simd::IPOfficialRaBitQBatchCompactVectorNibble4Masked(
                query.data(), nibble_blocks.data(), mask, 8, dim, dim_block, direct.data());
            for (uint32_t lane = 0; lane < 8; ++lane) {
                if ((mask & (1u << lane)) == 0) {
                    EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                    continue;
                }
                const float expected =
                    ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                EXPECT_NEAR(direct[lane], expected, 1e-3f)
                    << "dim=" << dim << " lane=" << lane;
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialVector2BitMaskedMatchesDecodedAndScalar) {
    for (uint32_t dim : {31u, 64u, 70u, 512u}) {
        constexpr uint32_t dim_block = 64;
        const auto query = MakeQuery(dim);
        std::vector<std::vector<uint8_t>> lane_ex_code(8);
        for (uint32_t lane = 0; lane < 8; ++lane) {
            lane_ex_code[lane].resize(dim);
            for (uint32_t d = 0; d < dim; ++d) {
                lane_ex_code[lane][d] =
                    static_cast<uint8_t>((lane * 5u + d * 3u + 1u) & 0x03u);
            }
        }

        std::vector<uint8_t> compact_blocks;
        BuildOfficialVector2BitLayout(dim, dim_block, lane_ex_code, compact_blocks);

        for (uint32_t mask : {0x00u, 0x01u, 0x80u, 0x55u, 0xA6u, 0xFFu}) {
            std::vector<float> direct(8, 0.0f);
            vdb::simd::IPOfficialRaBitQBatchCompactVector2BitMasked(
                query.data(), compact_blocks.data(), mask, 8, dim, dim_block, direct.data());
            for (uint32_t lane = 0; lane < 8; ++lane) {
                if ((mask & (1u << lane)) == 0) {
                    EXPECT_FLOAT_EQ(direct[lane], 0.0f);
                    continue;
                }
                const float expected =
                    ScalarOfficialExDataDot(query.data(), lane_ex_code[lane].data(), dim);
                EXPECT_NEAR(direct[lane], expected, 1e-3f)
                    << "dim=" << dim << " lane=" << lane;
            }
        }
    }
}

TEST(IPExRaBitQTest, OfficialCombineAndDistanceMatchReferenceFormula) {
    constexpr uint32_t dim = 37;
    const auto query = MakeQuery(dim);
    const auto sign = MakeSign(dim, 2);
    for (uint8_t bits : {2u, 3u, 4u}) {
        const uint32_t max_value = (1u << bits) - 1u;
        std::vector<uint8_t> ex_code(dim);
        float ip_x0_qr = 0.0f;
        float ip_ex_code = 0.0f;
        float expected_ip = 0.0f;
        const float cb = vdb::simd::OfficialRaBitQExBias(bits);
        for (uint32_t i = 0; i < dim; ++i) {
            ex_code[i] = static_cast<uint8_t>((i * 11u + bits) & max_value);
            if (sign[i] != 0) {
                ip_x0_qr += query[i];
            }
            ip_ex_code += query[i] * static_cast<float>(ex_code[i]);
            expected_ip += query[i] * (static_cast<float>((sign[i] != 0) ? (1u << bits) : 0u) +
                                       static_cast<float>(ex_code[i]) + cb);
        }

        const float combined = vdb::simd::OfficialRaBitQCombineNormalizedIP(ip_x0_qr, ip_ex_code,
                                                                            /*sum_q=*/0.0f, bits);
        float sum_q = 0.0f;
        for (float v : query)
            sum_q += v;
        const float combined_with_bias =
            vdb::simd::OfficialRaBitQCombineNormalizedIP(ip_x0_qr, ip_ex_code, sum_q, bits);
        EXPECT_NEAR(combined, expected_ip - cb * sum_q, 1e-4f);
        EXPECT_NEAR(combined_with_bias, expected_ip, 1e-4f) << "bits=" << static_cast<int>(bits);

        const float query_norm = 2.5f;
        const float db_norm = 1.75f;
        const float xipnorm = 0.125f;
        const float factor_add = db_norm * db_norm;
        const float factor_rescale = -2.0f * db_norm * xipnorm;
        const float actual_dist = vdb::simd::OfficialRaBitQEstimateDistance(
            query_norm * query_norm, factor_add, factor_rescale, query_norm * combined_with_bias);
        const float expected_dist = std::max(
            query_norm * query_norm + factor_add + factor_rescale * query_norm * expected_ip, 0.0f);
        EXPECT_NEAR(actual_dist, expected_dist, 1e-4f) << "bits=" << static_cast<int>(bits);
    }
}
