#include "vdb/io/npy_reader.h"

#include <cstring>
#include <fstream>
#include <string>

namespace vdb {
namespace io {

// =============================================================================
// Internal helpers
// =============================================================================

namespace {

// Npy magic: \x93NUMPY
static constexpr uint8_t kNpyMagic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};

/// Extract the value for a given key from a Python dict string.
/// e.g. from "{'descr': '<f4', 'fortran_order': False, 'shape': (1000, 512), }"
/// ExtractValue(header, "descr") => "'<f4'"
std::string ExtractValue(const std::string& header, const std::string& key) {
    auto pos = header.find("'" + key + "'");
    if (pos == std::string::npos) {
        pos = header.find("\"" + key + "\"");
    }
    if (pos == std::string::npos) return "";

    // Find the colon after the key
    auto colon = header.find(':', pos);
    if (colon == std::string::npos) return "";

    // Skip whitespace after colon
    size_t start = colon + 1;
    while (start < header.size() && header[start] == ' ') ++start;

    // Find end: comma or closing brace/paren
    size_t end = start;
    int paren_depth = 0;
    while (end < header.size()) {
        char c = header[end];
        if (c == '(' || c == '[') ++paren_depth;
        else if (c == ')' || c == ']') {
            if (paren_depth > 0) --paren_depth;
            else break;
        }
        else if (c == ',' && paren_depth == 0) break;
        else if (c == '}' && paren_depth == 0) break;
        ++end;
    }

    // Trim trailing whitespace
    while (end > start && header[end - 1] == ' ') --end;

    return header.substr(start, end - start);
}

/// Parse shape tuple, e.g. "(1000, 512)" or "(1000,)"
std::vector<uint32_t> ParseShape(const std::string& shape_str) {
    std::vector<uint32_t> shape;
    size_t i = 0;
    // Skip to first digit
    while (i < shape_str.size() && !std::isdigit(shape_str[i])) ++i;
    while (i < shape_str.size()) {
        if (std::isdigit(shape_str[i])) {
            uint32_t val = 0;
            while (i < shape_str.size() && std::isdigit(shape_str[i])) {
                val = val * 10 + (shape_str[i] - '0');
                ++i;
            }
            shape.push_back(val);
        } else {
            ++i;
        }
    }
    return shape;
}

/// Strip surrounding quotes from a string value.
std::string StripQuotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '\'' || s.front() == '"')) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

struct NpyHeader {
    std::string descr;
    bool fortran_order;
    std::vector<uint32_t> shape;
    size_t data_offset;  // byte offset where raw data begins
};

StatusOr<NpyHeader> ParseNpyHeader(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        return Status::IOError("Failed to open npy file: " + path);
    }

    // Read magic
    uint8_t magic[6];
    f.read(reinterpret_cast<char*>(magic), 6);
    if (!f.good() || std::memcmp(magic, kNpyMagic, 6) != 0) {
        return Status::Corruption("Invalid npy magic in: " + path);
    }

    // Read version
    uint8_t major, minor;
    f.read(reinterpret_cast<char*>(&major), 1);
    f.read(reinterpret_cast<char*>(&minor), 1);

    // Read header length
    uint32_t header_len = 0;
    if (major == 1) {
        uint16_t len16;
        f.read(reinterpret_cast<char*>(&len16), 2);
        header_len = len16;
    } else if (major == 2) {
        f.read(reinterpret_cast<char*>(&header_len), 4);
    } else {
        return Status::InvalidArgument(
            "Unsupported npy version: " + std::to_string(major) + "." + std::to_string(minor));
    }

    // Read header string
    std::string header(header_len, '\0');
    f.read(&header[0], header_len);
    if (!f.good()) {
        return Status::IOError("Failed to read npy header in: " + path);
    }

    NpyHeader result;
    result.data_offset = static_cast<size_t>(f.tellg());

    // Parse header dict
    result.descr = StripQuotes(ExtractValue(header, "descr"));
    std::string fortran_str = ExtractValue(header, "fortran_order");
    result.fortran_order = (fortran_str == "True");
    result.shape = ParseShape(ExtractValue(header, "shape"));

    return result;
}

}  // namespace

// =============================================================================
// LoadNpyFloat32
// =============================================================================

StatusOr<NpyArrayFloat> LoadNpyFloat32(const std::string& path) {
    auto hdr_result = ParseNpyHeader(path);
    if (!hdr_result.ok()) return hdr_result.status();

    const auto& hdr = hdr_result.value();

    // Validate dtype
    if (hdr.descr != "<f4" && hdr.descr != "=f4") {
        return Status::InvalidArgument(
            "Expected float32 (<f4) dtype, got: " + hdr.descr);
    }
    if (hdr.fortran_order) {
        return Status::InvalidArgument("Fortran order not supported");
    }
    if (hdr.shape.empty()) {
        return Status::InvalidArgument("Empty shape in npy file");
    }

    NpyArrayFloat arr;
    arr.rows = hdr.shape[0];
    arr.cols = (hdr.shape.size() >= 2) ? hdr.shape[1] : 1;

    size_t total = static_cast<size_t>(arr.rows) * arr.cols;
    arr.data.resize(total);

    std::ifstream f(path, std::ios::binary);
    f.seekg(hdr.data_offset);
    f.read(reinterpret_cast<char*>(arr.data.data()),
           static_cast<std::streamsize>(total * sizeof(float)));
    if (!f.good()) {
        return Status::IOError("Failed to read npy data from: " + path);
    }

    return arr;
}

// =============================================================================
// LoadNpyInt64
// =============================================================================

StatusOr<NpyArrayInt64> LoadNpyInt64(const std::string& path) {
    auto hdr_result = ParseNpyHeader(path);
    if (!hdr_result.ok()) return hdr_result.status();

    const auto& hdr = hdr_result.value();

    // Validate dtype
    if (hdr.descr != "<i8" && hdr.descr != "=i8") {
        return Status::InvalidArgument(
            "Expected int64 (<i8) dtype, got: " + hdr.descr);
    }
    if (hdr.fortran_order) {
        return Status::InvalidArgument("Fortran order not supported");
    }
    if (hdr.shape.empty()) {
        return Status::InvalidArgument("Empty shape in npy file");
    }

    NpyArrayInt64 arr;
    arr.count = hdr.shape[0];

    arr.data.resize(arr.count);

    std::ifstream f(path, std::ios::binary);
    f.seekg(hdr.data_offset);
    f.read(reinterpret_cast<char*>(arr.data.data()),
           static_cast<std::streamsize>(arr.count * sizeof(int64_t)));
    if (!f.good()) {
        return Status::IOError("Failed to read npy data from: " + path);
    }

    return arr;
}

// =============================================================================
// LoadNpyInt64Matrix
// =============================================================================

StatusOr<NpyArrayInt64Matrix> LoadNpyInt64Matrix(const std::string& path) {
    auto hdr_result = ParseNpyHeader(path);
    if (!hdr_result.ok()) return hdr_result.status();

    const auto& hdr = hdr_result.value();

    if (hdr.descr != "<i8" && hdr.descr != "=i8") {
        return Status::InvalidArgument(
            "Expected int64 (<i8) dtype, got: " + hdr.descr);
    }
    if (hdr.fortran_order) {
        return Status::InvalidArgument("Fortran order not supported");
    }
    if (hdr.shape.size() != 2) {
        return Status::InvalidArgument(
            "Expected 2-D int64 matrix in npy file: " + path);
    }

    NpyArrayInt64Matrix arr;
    arr.rows = hdr.shape[0];
    arr.cols = hdr.shape[1];
    arr.data.resize(static_cast<size_t>(arr.rows) * arr.cols);

    std::ifstream f(path, std::ios::binary);
    f.seekg(hdr.data_offset);
    f.read(reinterpret_cast<char*>(arr.data.data()),
           static_cast<std::streamsize>(arr.data.size() * sizeof(int64_t)));
    if (!f.good()) {
        return Status::IOError("Failed to read npy data from: " + path);
    }

    return arr;
}

}  // namespace io
}  // namespace vdb
