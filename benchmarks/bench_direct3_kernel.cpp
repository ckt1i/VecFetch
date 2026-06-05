#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/simd/ip_exrabitq.h"

namespace {

int GetIntArg(int argc, char* argv[], const char* name, int def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    }
    return def;
}

void FillDecoded(uint32_t dim, uint32_t dim_block,
                 std::vector<uint8_t>* decoded_blocks) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    decoded_blocks->assign(static_cast<size_t>(num_dim_blocks) * 8u * dim_block, 0);
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        for (uint32_t lane = 0; lane < 8; ++lane) {
            uint8_t* dst =
                decoded_blocks->data() +
                (static_cast<size_t>(db) * 8u + lane) * dim_block;
            for (uint32_t d = 0; d < dim_block; ++d) {
                dst[d] = static_cast<uint8_t>((db * 17u + lane * 7u + d * 3u) & 0x07u);
            }
        }
    }
}

bool PackDirectBlocks(const std::vector<uint8_t>& decoded_blocks,
                      uint32_t dim, uint32_t dim_block,
                      vdb::RaBitQExDataLayout layout,
                      std::vector<uint8_t>* packed_blocks) {
    const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
    const uint32_t packed_lane_bytes = vdb::simd::ExRaBitQPackedMagnitudeBytes(dim_block, 3);
    packed_blocks->assign(static_cast<size_t>(num_dim_blocks) * 8u * packed_lane_bytes, 0);
    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
        for (uint32_t lane = 0; lane < 8; ++lane) {
            const uint8_t* src =
                decoded_blocks.data() +
                (static_cast<size_t>(db) * 8u + lane) * dim_block;
            uint8_t* dst =
                packed_blocks->data() +
                (static_cast<size_t>(db) * 8u + lane) * packed_lane_bytes;
            if (!vdb::simd::ExRaBitQPackOfficialDirect3(
                    src, dim_block, layout, dst, packed_lane_bytes)) {
                return false;
            }
        }
    }
    return true;
}

template <typename Fn>
double TimeKernel(uint32_t iters, Fn&& fn, volatile float* sink) {
    alignas(64) float out[8] = {};
    for (uint32_t i = 0; i < 1000; ++i) {
        std::fill(std::begin(out), std::end(out), 0.0f);
        fn(out);
        *sink += out[i & 7u];
    }
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < iters; ++i) {
        std::fill(std::begin(out), std::end(out), 0.0f);
        fn(out);
        *sink += out[i & 7u];
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::nano>(elapsed).count() /
           static_cast<double>(iters);
}

}  // namespace

int main(int argc, char* argv[]) {
    const uint32_t dim = static_cast<uint32_t>(GetIntArg(argc, argv, "--dim", 512));
    const uint32_t iters = static_cast<uint32_t>(GetIntArg(argc, argv, "--iters", 200000));
    constexpr uint32_t kDimBlock = 64;
    constexpr uint32_t kValidCount = 8;
    constexpr uint32_t kLaneMask = 0xFFu;

    std::vector<float> query(dim);
    for (uint32_t i = 0; i < dim; ++i) {
        const float sign = (i % 3 == 0) ? -1.0f : 1.0f;
        query[i] = sign * (0.125f * static_cast<float>((i % 17) + 1));
    }

    std::vector<uint8_t> decoded_blocks;
    std::vector<uint8_t> split_blocks;
    std::vector<uint8_t> bitplane_blocks;
    FillDecoded(dim, kDimBlock, &decoded_blocks);
    if (!PackDirectBlocks(decoded_blocks, dim, kDimBlock,
                          vdb::RaBitQExDataLayout::kSplit3TwoPlusOne,
                          &split_blocks) ||
        !PackDirectBlocks(decoded_blocks, dim, kDimBlock,
                          vdb::RaBitQExDataLayout::kSplit3Bitplanes,
                          &bitplane_blocks)) {
        std::fprintf(stderr, "failed to pack direct 3-bit blocks\n");
        return 1;
    }

    volatile float sink = 0.0f;
    const double decoded_ns = TimeKernel(iters, [&](float* out) {
        vdb::simd::IPOfficialRaBitQBatchCompactMasked(
            query.data(), decoded_blocks.data(), kLaneMask, kValidCount, dim, kDimBlock, out);
    }, &sink);
    const double split_ns = TimeKernel(iters, [&](float* out) {
        vdb::simd::IPOfficialRaBitQBatchCompactDirect3Masked(
            query.data(), split_blocks.data(), vdb::RaBitQExDataLayout::kSplit3TwoPlusOne,
            kLaneMask, kValidCount, dim, kDimBlock, out);
    }, &sink);
    const double bitplane_ns = TimeKernel(iters, [&](float* out) {
        vdb::simd::IPOfficialRaBitQBatchCompactDirect3Masked(
            query.data(), bitplane_blocks.data(), vdb::RaBitQExDataLayout::kSplit3Bitplanes,
            kLaneMask, kValidCount, dim, kDimBlock, out);
    }, &sink);

    std::printf("layout,dim,iters,avg_ns_per_call,sink\n");
    std::printf("decoded_generic,%u,%u,%.3f,%.3f\n", dim, iters, decoded_ns,
                static_cast<double>(sink));
    std::printf("split3_2plus1,%u,%u,%.3f,%.3f\n", dim, iters, split_ns,
                static_cast<double>(sink));
    std::printf("split3_bitplanes,%u,%u,%.3f,%.3f\n", dim, iters, bitplane_ns,
                static_cast<double>(sink));
    return 0;
}
