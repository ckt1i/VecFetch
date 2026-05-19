#pragma once

#include <cstdint>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace simd {

void L2SqrBatch1xN(const float* VDB_RESTRICT query,
                   const float* const* VDB_RESTRICT vectors,
                   uint32_t count,
                   Dim dim,
                   float* VDB_RESTRICT out_dist);

}  // namespace simd
}  // namespace vdb
