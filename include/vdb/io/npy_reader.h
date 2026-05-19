#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vdb/common/status.h"

namespace vdb {
namespace io {

/// Row-major float32 array loaded from a .npy file.
struct NpyArrayFloat {
    std::vector<float> data;   // row-major
    uint32_t rows;
    uint32_t cols;             // 1 for 1-D arrays
};

/// 1-D int64 array loaded from a .npy file.
struct NpyArrayInt64 {
    std::vector<int64_t> data;
    uint32_t count;
};

/// Row-major int64 matrix loaded from a .npy file.
struct NpyArrayInt64Matrix {
    std::vector<int64_t> data;
    uint32_t rows;
    uint32_t cols;
};

/// Load a float32 .npy file. Returns error if dtype != <f4.
StatusOr<NpyArrayFloat> LoadNpyFloat32(const std::string& path);

/// Load an int64 .npy file. Returns error if dtype != <i8.
StatusOr<NpyArrayInt64> LoadNpyInt64(const std::string& path);

/// Load a row-major int64 matrix from .npy. Returns error if dtype != <i8.
StatusOr<NpyArrayInt64Matrix> LoadNpyInt64Matrix(const std::string& path);

}  // namespace io
}  // namespace vdb
