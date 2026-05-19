#include "vdb/simd/coarse_select.h"

#include <algorithm>
#include <numeric>

namespace vdb {
namespace simd {

void SelectTopNProbeSmall(const float* VDB_RESTRICT scores,
                          uint32_t nlist,
                          uint32_t nprobe,
                          uint32_t* VDB_RESTRICT order) {
    for (uint32_t i = 0; i < nlist; ++i) {
        order[i] = i;
    }
    const uint32_t actual_nprobe = std::min(nprobe, nlist);
    if (actual_nprobe < nlist) {
        auto score_less = [&](uint32_t lhs, uint32_t rhs) {
            return scores[lhs] < scores[rhs];
        };
        std::nth_element(order, order + actual_nprobe, order + nlist, score_less);
    }
    auto score_less = [&](uint32_t lhs, uint32_t rhs) {
        return scores[lhs] < scores[rhs];
    };
    std::sort(order, order + actual_nprobe, score_less);
}

void SelectTopNProbeSmallSpecialized(const float* VDB_RESTRICT scores,
                                     uint32_t nlist,
                                     uint32_t nprobe,
                                     uint32_t* VDB_RESTRICT order) {
    const uint32_t actual_nprobe = std::min(nprobe, nlist);
    if (actual_nprobe == 0) {
        return;
    }
    if (actual_nprobe >= nlist) {
        for (uint32_t i = 0; i < nlist; ++i) {
            order[i] = i;
        }
        auto score_less = [&](uint32_t lhs, uint32_t rhs) {
            return scores[lhs] < scores[rhs];
        };
        std::sort(order, order + nlist, score_less);
        return;
    }

    auto score_less = [&](uint32_t lhs, uint32_t rhs) {
        return scores[lhs] < scores[rhs];
    };

    for (uint32_t i = 0; i < actual_nprobe; ++i) {
        order[i] = i;
    }
    std::make_heap(order, order + actual_nprobe, [&](uint32_t lhs, uint32_t rhs) {
        return scores[lhs] < scores[rhs];
    });

    for (uint32_t i = actual_nprobe; i < nlist; ++i) {
        if (scores[i] >= scores[order[0]]) {
            continue;
        }
        std::pop_heap(order, order + actual_nprobe, [&](uint32_t lhs, uint32_t rhs) {
            return scores[lhs] < scores[rhs];
        });
        order[actual_nprobe - 1] = i;
        std::push_heap(order, order + actual_nprobe, [&](uint32_t lhs, uint32_t rhs) {
            return scores[lhs] < scores[rhs];
        });
    }

    std::sort_heap(order, order + actual_nprobe, [&](uint32_t lhs, uint32_t rhs) {
        return scores[lhs] < scores[rhs];
    });

    (void)score_less;
}

}  // namespace simd
}  // namespace vdb
