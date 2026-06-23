#pragma once

#include <cstdint>
#include <vector>

#include "vdb/rabitq/rabitq_encoder.h"

namespace vdb {
namespace storage {

/// Pack sign bits from a block of up to 32 RaBitQCode vectors into
/// VPSHUFB-friendly nibble-interleaved format (FastScan layout).
///
/// Each 4-dimension group becomes one 4-bit sub-quantizer.
/// Output is permuted for AVX-512 VPSHUFB lane structure.
///
/// @param codes       Input codes (up to 32). Fewer than 32 are zero-padded.
/// @param num_codes   Number of valid codes (1..32)
/// @param dim         Vector dimensionality
/// @param out         Output buffer, must be at least fastscan_packed_size(dim) bytes.
///                    fastscan_packed_size = dim * 4 bytes.
void PackSignBitsForFastScan(const rabitq::RaBitQCode* codes,
                              uint32_t num_codes,
                              uint32_t dim,
                              uint8_t* out);

/// Extract sign bits for a single vector from a packed FastScan block.
/// Inverse of PackSignBitsForFastScan for one vector.
///
/// @param packed_block  Pointer to the packed codes region of the block
/// @param vec_in_block  Vector index within the block (0..31)
/// @param dim           Vector dimensionality
/// @param out_words     Output buffer, must be at least ceil(dim/64) uint64_t words.
void UnpackSignBitsFromFastScan(const uint8_t* packed_block,
                                 uint32_t vec_in_block,
                                 uint32_t dim,
                                 uint64_t* out_words);

/// Compute the packed code size for one FastScan block.
inline uint32_t FastScanPackedSize(uint32_t dim) { return ((dim + 7) / 8) * 32; }

/// Compute the full FastScan block size (packed codes + 32 norm_oc floats).
inline uint32_t FastScanBlockSize(uint32_t dim) {
    return FastScanPackedSize(dim) + 32 * sizeof(float);
}

}  // namespace storage
}  // namespace vdb
