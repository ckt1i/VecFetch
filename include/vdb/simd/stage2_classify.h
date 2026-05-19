#pragma once

#include <cstdint>

#include "vdb/common/macros.h"
#include "vdb/common/types.h"

namespace vdb {
namespace simd {

struct Stage2ClassifyMasks {
    uint32_t safein = 0;
    uint32_t safeout = 0;
    uint32_t uncertain = 0;
};

Stage2ClassifyMasks Stage2ClassifyBatch(const float* VDB_RESTRICT ip_raw,
                                        const float* VDB_RESTRICT xipnorm,
                                        const float* VDB_RESTRICT norm_oc,
                                        const float* VDB_RESTRICT margin_s1,
                                        uint32_t active_mask,
                                        float norm_qc,
                                        float norm_qc_sq,
                                        float inv_margin_s2_divisor,
                                        float safein_dk,
                                        float dynamic_d_k);

}  // namespace simd
}  // namespace vdb
