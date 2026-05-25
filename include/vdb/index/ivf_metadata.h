#pragma once

#include <cstdint>

namespace vdb {
namespace index {

enum class AssignmentMode : uint8_t {
    Single = 0,
    RedundantTop2Naive = 1,
    RedundantTop2Rair = 2,
};

enum class ClusteringSource : uint8_t {
    Auto = 0,
    Precomputed = 1,
};

enum class CoarseBuilder : uint8_t {
    Auto = 0,
    SuperKMeans = 1,
    HierarchicalSuperKMeans = 2,
    FaissKMeans = 3,
};

enum class SafeInDkSpace : uint8_t {
    ExactL2 = 0,
    RabitqS2 = 1,
};

enum class SafeInDkSearchScope : uint8_t {
    FullDatabase = 0,
    NProbe = 1,
};

}  // namespace index
}  // namespace vdb
