#pragma once

#include <cstdint>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"
#include "vdb/storage/address_column.h"

namespace vdb {
namespace simd {

void DecodeAddressBatch(const storage::RawAddressEntryV2* VDB_RESTRICT raw_entries,
                        const uint32_t* VDB_RESTRICT vec_idxs,
                        uint32_t count,
                        uint32_t page_size,
                        AddressEntry* VDB_RESTRICT out);

void DecodeAddressBatch(const AddressEntry* VDB_RESTRICT decoded_entries,
                        const uint32_t* VDB_RESTRICT vec_idxs,
                        uint32_t count,
                        AddressEntry* VDB_RESTRICT out);

}  // namespace simd
}  // namespace vdb
