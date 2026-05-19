#pragma once

#include <cstdint>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace simd {

void SelectTopNProbeSmall(const float* VDB_RESTRICT scores,
                          uint32_t nlist,
                          uint32_t nprobe,
                          uint32_t* VDB_RESTRICT order);

void SelectTopNProbeSmallSpecialized(const float* VDB_RESTRICT scores,
                                     uint32_t nlist,
                                     uint32_t nprobe,
                                     uint32_t* VDB_RESTRICT order);

}  // namespace simd
}  // namespace vdb
