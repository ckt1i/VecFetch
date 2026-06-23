#include "vdb/storage/pack_codes.h"

#include <array>
#include <cstring>

namespace vdb {
namespace storage {

// AVX-512 VPSHUFB-friendly permutation (same as Extended-RaBitQ reference).
static constexpr uint8_t kPerm[16] = {
    0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15
};

void PackSignBitsForFastScan(const rabitq::RaBitQCode* codes,
                              uint32_t num_codes,
                              uint32_t dim,
                              uint8_t* out) {
    // M = number of 4-bit sub-quantizers = dim / 4
    // Each sub-quantizer packs 4 sign bits (one per dimension in the group).
    const uint32_t M = dim / 4;

    // Zero the output buffer: one 32-byte packed pair per 8 dimensions.
    std::memset(out, 0, FastScanPackedSize(dim));

    // Step 1: Extract per-vector nibble codes.
    // For each vector, extract M nibbles (one per dim-group of 4).
    // Store as ceil(M/2) bytes per vector (2 nibbles per byte).
    //
    // nibble[m] for vector v = sign[v][m*4+3] | sign[v][m*4+2]<<1
    //                        | sign[v][m*4+1]<<2 | sign[v][m*4+0]<<3
    const uint32_t bytes_per_vec = (M + 1) / 2;  // M nibbles, 2 per byte
    std::vector<uint8_t> flat(32 * bytes_per_vec, 0);

    for (uint32_t v = 0; v < num_codes; ++v) {
        const auto& code = codes[v];
        // Extract sign bits from code.code (first num_sign_words, which is
        // the MSB / sign plane for M-bit codes)
        for (uint32_t m = 0; m < M; ++m) {
            uint8_t nibble = 0;
            for (uint32_t b = 0; b < 4; ++b) {
                uint32_t d = m * 4 + b;
                if (d >= dim) break;
                uint32_t word_idx = d / 64;
                uint32_t bit_idx = d % 64;
                uint8_t bit = (code.code[word_idx] >> bit_idx) & 1;
                // Pack: MSB of nibble = dim m*4+0, LSB = dim m*4+3
                nibble |= bit << (3 - b);
            }
            // Store nibble into packed byte array (2 nibbles per byte)
            uint32_t byte_idx = m / 2;
            if (m % 2 == 0) {
                flat[v * bytes_per_vec + byte_idx] |= nibble;       // low nibble
            } else {
                flat[v * bytes_per_vec + byte_idx] |= (nibble << 4);  // high nibble
            }
        }
    }

    // Step 2: Pack into VPSHUFB block-32 format (same as pack_codes reference).
    // Process 2 sub-quantizers at a time, producing 32 bytes per pair.
    uint8_t* dst = out;
    for (uint32_t m = 0; m < M; m += 2) {
        std::array<uint8_t, 32> c, c0, c1;
        // Read column m/2 from the flat matrix (32 rows × bytes_per_vec cols)
        for (uint32_t k = 0; k < 32; ++k) {
            if (k < num_codes) {
                // Handle case where bytes_per_vec might have m/2 within range
                c[k] = flat[k * bytes_per_vec + m / 2];
            } else {
                c[k] = 0;
            }
        }

        // Split byte into low and high nibbles
        for (uint32_t j = 0; j < 32; ++j) {
            c0[j] = c[j] & 0x0F;
            c1[j] = c[j] >> 4;
        }

        // Permute and interleave for VPSHUFB lanes
        for (uint32_t j = 0; j < 16; ++j) {
            dst[j]      = c0[kPerm[j]] | (c0[kPerm[j] + 16] << 4);
            dst[j + 16] = c1[kPerm[j]] | (c1[kPerm[j] + 16] << 4);
        }
        dst += 32;
    }
}

// Inverse of kPerm: kInvPerm[kPerm[i]] = i
static constexpr uint8_t kInvPerm[16] = {
    0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15
};

void UnpackSignBitsFromFastScan(const uint8_t* packed_block,
                                 uint32_t vec_in_block,
                                 uint32_t dim,
                                 uint64_t* out_words) {
    const uint32_t M = dim / 4;
    const uint32_t nwords = (dim + 63) / 64;
    std::memset(out_words, 0, nwords * sizeof(uint64_t));

    const bool in_high_half = (vec_in_block >= 16);
    const uint32_t lane_idx = in_high_half ? (vec_in_block - 16) : vec_in_block;
    const uint32_t perm_pos = kInvPerm[lane_idx];

    for (uint32_t m = 0; m < M; m += 2) {
        const uint8_t* pair = packed_block + (m / 2) * 32;

        // Extract nibble for sub-quantizer m
        uint8_t byte0 = pair[perm_pos];
        uint8_t nibble0 = in_high_half ? ((byte0 >> 4) & 0x0F) : (byte0 & 0x0F);

        for (uint32_t b = 0; b < 4 && m * 4 + b < dim; ++b) {
            uint32_t d = m * 4 + b;
            uint8_t bit = (nibble0 >> (3 - b)) & 1;
            out_words[d / 64] |= static_cast<uint64_t>(bit) << (d % 64);
        }

        // Extract nibble for sub-quantizer m+1
        if (m + 1 < M) {
            uint8_t byte1 = pair[perm_pos + 16];
            uint8_t nibble1 = in_high_half ? ((byte1 >> 4) & 0x0F) : (byte1 & 0x0F);

            for (uint32_t b = 0; b < 4 && (m + 1) * 4 + b < dim; ++b) {
                uint32_t d = (m + 1) * 4 + b;
                uint8_t bit = (nibble1 >> (3 - b)) & 1;
                out_words[d / 64] |= static_cast<uint64_t>(bit) << (d % 64);
            }
        }
    }
}

}  // namespace storage
}  // namespace vdb
