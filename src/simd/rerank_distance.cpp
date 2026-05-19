#include "vdb/simd/rerank_distance.h"

#include "vdb/common/distance.h"

namespace vdb {
namespace simd {

void L2SqrBatch1xN(const float* VDB_RESTRICT query,
                   const float* const* VDB_RESTRICT vectors,
                   uint32_t count,
                   Dim dim,
                   float* VDB_RESTRICT out_dist) {
    for (uint32_t i = 0; i < count; ++i) {
        out_dist[i] = L2Sqr(query, vectors[i], dim);
    }
}

}  // namespace simd
}  // namespace vdb
