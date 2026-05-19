#include "vdb/simd/address_decode.h"

namespace vdb {
namespace simd {

void DecodeAddressBatch(const storage::RawAddressEntryV2* VDB_RESTRICT raw_entries,
                        const uint32_t* VDB_RESTRICT vec_idxs,
                        uint32_t count,
                        uint32_t page_size,
                        AddressEntry* VDB_RESTRICT out) {
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = storage::AddressColumn::DecodeRawEntryV2(
            raw_entries[vec_idxs[i]], page_size);
    }
}

void DecodeAddressBatch(const AddressEntry* VDB_RESTRICT decoded_entries,
                        const uint32_t* VDB_RESTRICT vec_idxs,
                        uint32_t count,
                        AddressEntry* VDB_RESTRICT out) {
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = decoded_entries[vec_idxs[i]];
    }
}

}  // namespace simd
}  // namespace vdb
