#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace vdb {

// ============================================================================
// Basic Type Aliases
// ============================================================================

/// Vector ID type (64-bit to support large datasets)
using VecID = uint64_t;

/// Row ID type (same as VecID for consistency)
using RowID = uint64_t;

/// Segment ID type
using SegmentID = uint32_t;

/// List ID for IVF (cluster ID)
using ListID = uint32_t;

/// Cluster ID (semantic alias for ListID in new architecture)
using ClusterID = uint32_t;

/// DataFile ID type (1:1 with cluster, reserved for multi-shard expansion)
using FileID = uint16_t;

/// Column ID
using ColumnID = uint32_t;

/// Dimension type
using Dim = uint32_t;

/// Invalid/sentinel values
constexpr VecID kInvalidVecID = std::numeric_limits<VecID>::max();
constexpr RowID kInvalidRowID = std::numeric_limits<RowID>::max();
constexpr SegmentID kInvalidSegmentID = std::numeric_limits<SegmentID>::max();
constexpr ListID kInvalidListID = std::numeric_limits<ListID>::max();
constexpr ClusterID kInvalidClusterID = std::numeric_limits<ClusterID>::max();
constexpr FileID kInvalidFileID = std::numeric_limits<FileID>::max();
constexpr ColumnID kInvalidColumnID = std::numeric_limits<ColumnID>::max();

// ============================================================================
// Distance Metric Types
// ============================================================================

enum class MetricType : uint8_t {
  L2 = 0,           // Euclidean distance (squared)
  InnerProduct = 1,           // Inner product (larger = more similar)
  COSINE = 2,       // Cosine similarity (normalized InnerProduct)
};

inline std::string_view MetricTypeName(MetricType metric) {
  switch (metric) {
    case MetricType::L2: return "L2";
    case MetricType::InnerProduct: return "InnerProduct";
    case MetricType::COSINE: return "COSINE";
    default: return "UNKNOWN";
  }
}

// ============================================================================
// Data Types for Columns
// ============================================================================

enum class DType : uint8_t {
  // Integer types
  INT8 = 0,
  INT16 = 1,
  INT32 = 2,
  INT64 = 3,
  UINT8 = 4,
  UINT16 = 5,
  UINT32 = 6,
  UINT64 = 7,
  
  // Floating point types
  FLOAT16 = 10,
  FLOAT32 = 11,
  FLOAT64 = 12,
  
  // Variable length types
  STRING = 20,
  BYTES = 21,
  
  // Vector types (fixed-length arrays)
  VECTOR_FLOAT32 = 30,
  VECTOR_FLOAT16 = 31,
  VECTOR_INT8 = 32,     // Quantized vectors
  VECTOR_UINT8 = 33,    // PQ codes
  
  // Special
  BOOL = 40,
  TIMESTAMP = 41,
};

/// Get byte size for fixed-width types, returns 0 for variable-length types
inline size_t DTypeSize(DType dtype) {
  switch (dtype) {
    case DType::INT8:
    case DType::UINT8:
    case DType::BOOL:
      return 1;
    case DType::INT16:
    case DType::UINT16:
    case DType::FLOAT16:
      return 2;
    case DType::INT32:
    case DType::UINT32:
    case DType::FLOAT32:
      return 4;
    case DType::INT64:
    case DType::UINT64:
    case DType::FLOAT64:
    case DType::TIMESTAMP:
      return 8;
    // Variable length and vector types
    default:
      return 0;
  }
}

inline bool DTypeIsFixedWidth(DType dtype) {
  return DTypeSize(dtype) > 0;
}

inline bool DTypeIsVector(DType dtype) {
  return dtype == DType::VECTOR_FLOAT32 ||
         dtype == DType::VECTOR_FLOAT16 ||
         dtype == DType::VECTOR_INT8 ||
         dtype == DType::VECTOR_UINT8;
}

inline std::string_view DTypeName(DType dtype) {
  switch (dtype) {
    case DType::INT8: return "INT8";
    case DType::INT16: return "INT16";
    case DType::INT32: return "INT32";
    case DType::INT64: return "INT64";
    case DType::UINT8: return "UINT8";
    case DType::UINT16: return "UINT16";
    case DType::UINT32: return "UINT32";
    case DType::UINT64: return "UINT64";
    case DType::FLOAT16: return "FLOAT16";
    case DType::FLOAT32: return "FLOAT32";
    case DType::FLOAT64: return "FLOAT64";
    case DType::STRING: return "STRING";
    case DType::BYTES: return "BYTES";
    case DType::VECTOR_FLOAT32: return "VECTOR_FLOAT32";
    case DType::VECTOR_FLOAT16: return "VECTOR_FLOAT16";
    case DType::VECTOR_INT8: return "VECTOR_INT8";
    case DType::VECTOR_UINT8: return "VECTOR_UINT8";
    case DType::BOOL: return "BOOL";
    case DType::TIMESTAMP: return "TIMESTAMP";
    default: return "UNKNOWN";
  }
}

// ============================================================================
// Search Result (forward declare, will be defined after Datum)
// ============================================================================

// Defined after Datum struct below

// ============================================================================
// Payload Mode (inline vs extern)
// ============================================================================

/// Payload storage mode — small payloads are stored inline in the record file,
/// large payloads (>= kInlineThreshold) are stored in external blob files.
enum class PayloadMode : uint8_t {
  kInline = 0,   // Payload bytes embedded in record
  kExtern = 1,   // Payload stored in external blob file
};

inline std::string_view PayloadModeName(PayloadMode mode) {
  switch (mode) {
    case PayloadMode::kInline: return "Inline";
    case PayloadMode::kExtern: return "Extern";
    default: return "UNKNOWN";
  }
}

// ============================================================================
// External Blob Reference
// ============================================================================

/// Reference to a payload chunk stored in an external blob file.
struct ExternRef {
  uint32_t file_id;   // Blob file identifier (index into FileNameDirectory)
  uint64_t offset;     // Byte offset within the blob file
  uint32_t length;     // Payload byte length

  bool operator==(const ExternRef& other) const {
    return file_id == other.file_id &&
           offset  == other.offset  &&
           length  == other.length;
  }
  bool operator!=(const ExternRef& other) const { return !(*this == other); }
};

// ============================================================================
// Record Locator
// ============================================================================

/// Locates a record inside the segment's record file.
/// ANNS returns (VecID, approx_dist, RecordLocator) so the reader can
/// pread() directly — no row-id → offset translation needed.
struct RecordLocator {
  uint64_t    offset;   // Byte offset in the record file
  uint32_t    length;   // Total serialised record length
  PayloadMode mode;     // Inline or Extern

  bool operator==(const RecordLocator& other) const {
    return offset == other.offset &&
           length == other.length &&
           mode   == other.mode;
  }
  bool operator!=(const RecordLocator& other) const { return !(*this == other); }
};

// ============================================================================
// Column Locator — physical address of one value within a column chunk
// ============================================================================

/// Locates a single cell within a column file.
/// Used as the physical-address token returned by the writer and consumed by
/// the reader (pread-style I/O).
struct ColumnLocator {
  uint32_t chunk_id;           // Which chunk inside the column
  uint64_t data_offset;        // Byte offset of the value within the file
  uint32_t data_length;        // Byte length of the value
  uint32_t offset_table_pos;   // Slot index in the variable-length offset table
                               //   (0 for fixed-width columns)

  bool operator==(const ColumnLocator& other) const {
    return chunk_id        == other.chunk_id &&
           data_offset     == other.data_offset &&
           data_length     == other.data_length &&
           offset_table_pos == other.offset_table_pos;
  }
  bool operator!=(const ColumnLocator& other) const { return !(*this == other); }
};

// ============================================================================
// Column Schema — lightweight column descriptor
// ============================================================================

struct ColumnSchema {
  ColumnID    id;
  std::string name;
  DType       dtype;
  bool        nullable = false;
};

// ============================================================================
// Datum — a single column value (type-erased)
// ============================================================================

/// Holds one cell value.  Fixed-width types are stored inline; variable-length
/// types carry their bytes in the string member.
struct Datum {
  DType dtype;
  union {
    int64_t  i64;
    uint64_t u64;
    double   f64;
    float    f32;
    int32_t  i32;
    uint32_t u32;
    int16_t  i16;
    uint16_t u16;
    int8_t   i8;
    uint8_t  u8;
    bool     b;
  } fixed;
  std::string var_data;   // For STRING / BYTES

  Datum() : dtype(DType::INT64) { fixed.i64 = 0; }

  static Datum Int64(int64_t v)  { Datum d; d.dtype = DType::INT64;  d.fixed.i64 = v; return d; }
  static Datum UInt64(uint64_t v){ Datum d; d.dtype = DType::UINT64; d.fixed.u64 = v; return d; }
  static Datum Int32(int32_t v)  { Datum d; d.dtype = DType::INT32;  d.fixed.i32 = v; return d; }
  static Datum UInt32(uint32_t v){ Datum d; d.dtype = DType::UINT32; d.fixed.u32 = v; return d; }
  static Datum Float32(float v)  { Datum d; d.dtype = DType::FLOAT32;d.fixed.f32 = v; return d; }
  static Datum Float64(double v) { Datum d; d.dtype = DType::FLOAT64;d.fixed.f64 = v; return d; }
  static Datum Bool(bool v)      { Datum d; d.dtype = DType::BOOL;   d.fixed.b   = v; return d; }
  static Datum Timestamp(uint64_t v){ Datum d; d.dtype = DType::TIMESTAMP; d.fixed.u64 = v; return d; }
  static Datum String(std::string v){
    Datum d; d.dtype = DType::STRING; d.var_data = std::move(v); return d;
  }
  static Datum Bytes(std::string v) {
    Datum d; d.dtype = DType::BYTES;  d.var_data = std::move(v); return d;
  }

  /// Returns pointer to the raw bytes of the fixed-width value.
  const void* fixed_data() const { return &fixed; }

  /// Returns byte size of the value (for serialisation).
  size_t byte_size() const {
    if (DTypeIsFixedWidth(dtype)) return DTypeSize(dtype);
    return var_data.size();
  }

  /// Returns pointer to the raw bytes of the value.
  const void* data() const {
    if (DTypeIsFixedWidth(dtype)) return &fixed;
    return var_data.data();
  }
};

// ============================================================================
// Search Result (now that Datum is defined)
// ============================================================================

/// Single search result entry (optionally includes payload columns)
struct SearchResult {
  VecID id;
  float distance;
  std::map<ColumnID, Datum> payload;  // Payload columns indexed by ColumnID
  
  bool operator<(const SearchResult& other) const {
    return distance < other.distance;
  }
  
  bool operator>(const SearchResult& other) const {
    return distance > other.distance;
  }
};

// ============================================================================
// Record Physical Address — locates a full record and its per-column cells
// ============================================================================

/// DEPRECATED: Use AddressEntry instead.
/// Locates a record inside the segment's record file.
struct [[deprecated("Use AddressEntry instead for Phase 2.5+")]] RecordPhysicalAddr {
  RecordLocator                             record;   // Record-file position
  std::vector<std::pair<ColumnID, ColumnLocator>> columns;  // Per-column positions

  bool operator==(const RecordPhysicalAddr& other) const {
    return record == other.record && columns == other.columns;
  }
  bool operator!=(const RecordPhysicalAddr& other) const { return !(*this == other); }
};

// ============================================================================
// New Types for Phase 2.5+ (IVF+ConANN+RaBitQ Architecture)
// ============================================================================

/// Result classification from ConANN (Cluster Architecture Nearest Neighbor Analysis)
enum class ResultClass : uint8_t {
  SafeIn = 0,      // Approx dist < tau_in: definitely in Top-K, fetch all data
  SafeOut = 1,     // Approx dist > tau_out: definitely not in Top-K, skip
  Uncertain = 2,   // tau_in <= approx_dist <= tau_out: need exact distance verification
};

inline std::string_view ResultClassName(ResultClass cls) {
  switch (cls) {
    case ResultClass::SafeIn: return "SafeIn";
    case ResultClass::SafeOut: return "SafeOut";
    case ResultClass::Uncertain: return "Uncertain";
    default: return "UNKNOWN";
  }
}

/// Physical address of a record in DataFile (offset + size)
struct AddressEntry {
  uint64_t offset;  // Byte offset in DataFile
  uint32_t size;    // Record byte length

  bool operator==(const AddressEntry& other) const {
    return offset == other.offset && size == other.size;
  }
  bool operator!=(const AddressEntry& other) const { return !(*this == other); }
};

/// I/O task type for query pipeline read classification
enum class ReadTaskType : uint8_t {
  VEC_ONLY = 0,    // Read only vector part (dim * sizeof(float) bytes)
  ALL = 1,         // Read entire record (vec + payload, for SafeIn ≤256KB)
  PAYLOAD = 2,     // Read only payload part (from offset + dim*4)
};

inline std::string_view ReadTaskTypeName(ReadTaskType ty) {
  switch (ty) {
    case ReadTaskType::VEC_ONLY: return "VEC_ONLY";
    case ReadTaskType::ALL: return "ALL";
    case ReadTaskType::PAYLOAD: return "PAYLOAD";
    default: return "UNKNOWN";
  }
}

/// Candidate vector for progressive reranking
struct Candidate {
  float approx_dist;         // RaBitQ estimated distance
  ResultClass result_class;  // ConANN classification
  ClusterID cluster_id;      // Source cluster
  uint32_t local_idx;        // Local index in cluster

  bool operator<(const Candidate& other) const {
    // For max-heap: larger distance is "less than"
    return approx_dist > other.approx_dist;
  }
  bool operator>(const Candidate& other) const {
    return approx_dist < other.approx_dist;
  }
};

enum class RaBitQEstimatorMode : uint8_t {
  kLegacySignedMagnitude = 0,
  kOfficial1PlusN = 1,
};

constexpr std::string_view RaBitQEstimatorModeName(RaBitQEstimatorMode mode) {
  switch (mode) {
    case RaBitQEstimatorMode::kOfficial1PlusN:
      return "official_1_plus_n";
    case RaBitQEstimatorMode::kLegacySignedMagnitude:
    default:
      return "legacy_signed_magnitude";
  }
}

inline bool ParseRaBitQEstimatorMode(std::string_view value,
                                     RaBitQEstimatorMode* mode) {
  if (value == "legacy" || value == "legacy_signed_magnitude") {
    if (mode != nullptr) {
      *mode = RaBitQEstimatorMode::kLegacySignedMagnitude;
    }
    return true;
  }
  if (value == "official" || value == "official_1_plus_n") {
    if (mode != nullptr) {
      *mode = RaBitQEstimatorMode::kOfficial1PlusN;
    }
    return true;
  }
  return false;
}

constexpr RaBitQEstimatorMode RaBitQEstimatorModeFromByte(uint8_t mode) {
  return mode == static_cast<uint8_t>(RaBitQEstimatorMode::kOfficial1PlusN)
      ? RaBitQEstimatorMode::kOfficial1PlusN
      : RaBitQEstimatorMode::kLegacySignedMagnitude;
}

enum class RaBitQExDataLayout : uint8_t {
  kGenericPacked = 0,
  kSplit3TwoPlusOne = 1,
  kSplit3Bitplanes = 2,
  kSelectedDirect = 3,
  kSplit1Bitplane = 4,
  kSplit2Bitplanes = 5,
  kSplit3TrimmedBitplanes = 6,
  kSplit3ZeroPlaneElide = 7,
  kVectorBitplanes = 8,
  kVectorBitplanesPrefetch = 9,
  kVectorNibble4 = 10,
  kVector2Bit = 11,
  kSmallLane4Bitplanes = 12,
  kSmallLane2Bitplanes = 13,
  kVectorBitplanesMicroBatch = 14,
  kVectorBitMajorTiles = 15,
  kTileLaneBitMajor = 16,
};

constexpr std::string_view RaBitQExDataLayoutName(RaBitQExDataLayout layout) {
  switch (layout) {
    case RaBitQExDataLayout::kSplit3TwoPlusOne:
      return "split3_2plus1";
    case RaBitQExDataLayout::kSplit3Bitplanes:
      return "split3_bitplanes";
    case RaBitQExDataLayout::kSplit1Bitplane:
      return "split1_bitplane";
    case RaBitQExDataLayout::kSplit2Bitplanes:
      return "split2_bitplanes";
    case RaBitQExDataLayout::kSplit3TrimmedBitplanes:
      return "split3_trimmed_bitplanes";
    case RaBitQExDataLayout::kSplit3ZeroPlaneElide:
      return "split3_zero_plane_elide";
    case RaBitQExDataLayout::kVectorBitplanes:
      return "vector_bitplanes";
    case RaBitQExDataLayout::kVectorBitplanesPrefetch:
      return "vector_bitplanes_prefetch";
    case RaBitQExDataLayout::kVectorBitplanesMicroBatch:
      return "vector_bitplanes_microbatch";
    case RaBitQExDataLayout::kVectorBitMajorTiles:
      return "vector_bitmajor_tiles";
    case RaBitQExDataLayout::kTileLaneBitMajor:
      return "tile_lane_bitmajor";
    case RaBitQExDataLayout::kVectorNibble4:
      return "vector_nibble4";
    case RaBitQExDataLayout::kVector2Bit:
      return "vector_2bit";
    case RaBitQExDataLayout::kSmallLane4Bitplanes:
      return "small_lane4_bitplanes";
    case RaBitQExDataLayout::kSmallLane2Bitplanes:
      return "small_lane2_bitplanes";
    case RaBitQExDataLayout::kSelectedDirect:
      return "selected_direct";
    case RaBitQExDataLayout::kGenericPacked:
    default:
      return "generic_packed";
  }
}

inline bool ParseRaBitQExDataLayout(std::string_view value,
                                    RaBitQExDataLayout* layout) {
  if (value == "" || value == "generic" || value == "generic_packed") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kGenericPacked;
    }
    return true;
  }
  if (value == "split3_2plus1" || value == "2plus1" ||
      value == "two_plus_one") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSplit3TwoPlusOne;
    }
    return true;
  }
  if (value == "split3_bitplanes" || value == "bitplanes" ||
      value == "bitplanes3" || value == "1plus1plus1") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSplit3Bitplanes;
    }
    return true;
  }
  if (value == "split2_bitplanes" || value == "bitplanes2" ||
      value == "1plus1") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSplit2Bitplanes;
    }
    return true;
  }
  if (value == "split1_bitplane" || value == "split1_bitplanes" ||
      value == "bitplane1" || value == "bitplanes1") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSplit1Bitplane;
    }
    return true;
  }
  if (value == "split3_trimmed_bitplanes" || value == "trimmed_bitplanes" ||
      value == "trim_valid_lanes") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSplit3TrimmedBitplanes;
    }
    return true;
  }
  if (value == "split3_zero_plane_elide" || value == "zero_plane_elide" ||
      value == "zero_elide") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSplit3ZeroPlaneElide;
    }
    return true;
  }
  if (value == "vector_bitplanes" || value == "per_vector_bitplanes" ||
      value == "vector_compact" || value == "lane_major_bitplanes") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kVectorBitplanes;
    }
    return true;
  }
  if (value == "vector_bitplanes_prefetch" || value == "per_vector_bitplanes_prefetch" ||
      value == "vector_compact_prefetch" || value == "lane_major_bitplanes_prefetch") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kVectorBitplanesPrefetch;
    }
    return true;
  }
  if (value == "vector_bitplanes_microbatch" ||
      value == "per_vector_bitplanes_microbatch" ||
      value == "vector_compact_microbatch" ||
      value == "lane_major_bitplanes_microbatch" ||
      value == "vector_microbatch") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kVectorBitplanesMicroBatch;
    }
    return true;
  }
  if (value == "vector_bitmajor_tiles" || value == "bitmajor_tiles" ||
      value == "vector_bitplane_tiles" || value == "cacheline_bitplanes" ||
      value == "cacheline_tiles") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kVectorBitMajorTiles;
    }
    return true;
  }
  if (value == "tile_lane_bitmajor" || value == "batch_tile_bitmajor" ||
      value == "tile_lane_bitplanes" || value == "batch_bitmajor_tiles") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kTileLaneBitMajor;
    }
    return true;
  }
  if (value == "vector_nibble4" || value == "per_vector_nibble4" ||
      value == "vector_4bit_nibble" || value == "official_4bit_nibble") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kVectorNibble4;
    }
    return true;
  }
  if (value == "vector_2bit" || value == "per_vector_2bit" ||
      value == "vector_twobit" || value == "official_2bit") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kVector2Bit;
    }
    return true;
  }
  if (value == "small_lane4_bitplanes" || value == "small_lane4" ||
      value == "lane4_bitplanes" || value == "batch4_bitplanes") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSmallLane4Bitplanes;
    }
    return true;
  }
  if (value == "small_lane2_bitplanes" || value == "small_lane2" ||
      value == "lane2_bitplanes" || value == "batch2_bitplanes") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSmallLane2Bitplanes;
    }
    return true;
  }
  if (value == "selected" || value == "selected_direct") {
    if (layout != nullptr) {
      *layout = RaBitQExDataLayout::kSelectedDirect;
    }
    return true;
  }
  return false;
}

constexpr RaBitQExDataLayout RaBitQExDataLayoutFromByte(uint8_t layout) {
  switch (layout) {
    case static_cast<uint8_t>(RaBitQExDataLayout::kSplit3TwoPlusOne):
      return RaBitQExDataLayout::kSplit3TwoPlusOne;
    case static_cast<uint8_t>(RaBitQExDataLayout::kSplit3Bitplanes):
      return RaBitQExDataLayout::kSplit3Bitplanes;
    case static_cast<uint8_t>(RaBitQExDataLayout::kSelectedDirect):
      return RaBitQExDataLayout::kSelectedDirect;
    case static_cast<uint8_t>(RaBitQExDataLayout::kSplit1Bitplane):
      return RaBitQExDataLayout::kSplit1Bitplane;
    case static_cast<uint8_t>(RaBitQExDataLayout::kSplit2Bitplanes):
      return RaBitQExDataLayout::kSplit2Bitplanes;
    case static_cast<uint8_t>(RaBitQExDataLayout::kSplit3TrimmedBitplanes):
      return RaBitQExDataLayout::kSplit3TrimmedBitplanes;
    case static_cast<uint8_t>(RaBitQExDataLayout::kSplit3ZeroPlaneElide):
      return RaBitQExDataLayout::kSplit3ZeroPlaneElide;
    case static_cast<uint8_t>(RaBitQExDataLayout::kVectorBitplanes):
      return RaBitQExDataLayout::kVectorBitplanes;
    case static_cast<uint8_t>(RaBitQExDataLayout::kVectorBitplanesPrefetch):
      return RaBitQExDataLayout::kVectorBitplanesPrefetch;
    case static_cast<uint8_t>(RaBitQExDataLayout::kVectorBitplanesMicroBatch):
      return RaBitQExDataLayout::kVectorBitplanesMicroBatch;
    case static_cast<uint8_t>(RaBitQExDataLayout::kVectorBitMajorTiles):
      return RaBitQExDataLayout::kVectorBitMajorTiles;
    case static_cast<uint8_t>(RaBitQExDataLayout::kTileLaneBitMajor):
      return RaBitQExDataLayout::kTileLaneBitMajor;
    case static_cast<uint8_t>(RaBitQExDataLayout::kVectorNibble4):
      return RaBitQExDataLayout::kVectorNibble4;
    case static_cast<uint8_t>(RaBitQExDataLayout::kVector2Bit):
      return RaBitQExDataLayout::kVector2Bit;
    case static_cast<uint8_t>(RaBitQExDataLayout::kSmallLane4Bitplanes):
      return RaBitQExDataLayout::kSmallLane4Bitplanes;
    case static_cast<uint8_t>(RaBitQExDataLayout::kSmallLane2Bitplanes):
      return RaBitQExDataLayout::kSmallLane2Bitplanes;
    case static_cast<uint8_t>(RaBitQExDataLayout::kGenericPacked):
    default:
      return RaBitQExDataLayout::kGenericPacked;
  }
}

constexpr bool RaBitQExDataLayoutByteValid(uint8_t layout) {
  return layout == static_cast<uint8_t>(RaBitQExDataLayout::kGenericPacked) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSplit3TwoPlusOne) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSplit3Bitplanes) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSelectedDirect) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSplit1Bitplane) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSplit2Bitplanes) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSplit3TrimmedBitplanes) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSplit3ZeroPlaneElide) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kVectorBitplanes) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kVectorBitplanesPrefetch) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kVectorBitplanesMicroBatch) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kVectorBitMajorTiles) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kTileLaneBitMajor) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kVectorNibble4) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kVector2Bit) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSmallLane4Bitplanes) ||
         layout == static_cast<uint8_t>(RaBitQExDataLayout::kSmallLane2Bitplanes);
}

constexpr RaBitQExDataLayout RaBitQResolveSelectedExDataLayoutForBits(
    RaBitQExDataLayout layout, uint8_t ex_bits) {
  if (layout != RaBitQExDataLayout::kSelectedDirect) {
    return layout;
  }
  if (ex_bits >= 1 && ex_bits <= 3) return RaBitQExDataLayout::kVectorBitMajorTiles;
  if (ex_bits == 4) return RaBitQExDataLayout::kVectorNibble4;
  return RaBitQExDataLayout::kSelectedDirect;
}

constexpr RaBitQExDataLayout RaBitQResolveSelectedExDataLayout(
    RaBitQExDataLayout layout) {
  return RaBitQResolveSelectedExDataLayoutForBits(layout, 3);
}

constexpr RaBitQExDataLayout RaBitQDefaultOfficialExDataLayoutForBits(
    uint8_t ex_bits) {
  if (ex_bits >= 1 && ex_bits <= 3) return RaBitQExDataLayout::kVectorBitMajorTiles;
  if (ex_bits == 4) return RaBitQExDataLayout::kVectorNibble4;
  return RaBitQExDataLayout::kGenericPacked;
}

constexpr uint8_t RaBitQExDataLayoutDirectBits(RaBitQExDataLayout layout) {
  const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
  switch (resolved) {
    case RaBitQExDataLayout::kSplit1Bitplane:
      return 1;
    case RaBitQExDataLayout::kSplit2Bitplanes:
      return 2;
    case RaBitQExDataLayout::kSplit3TwoPlusOne:
    case RaBitQExDataLayout::kSplit3Bitplanes:
    case RaBitQExDataLayout::kSplit3TrimmedBitplanes:
    case RaBitQExDataLayout::kSplit3ZeroPlaneElide:
      return 3;
    case RaBitQExDataLayout::kVectorBitplanes:
    case RaBitQExDataLayout::kVectorBitplanesPrefetch:
    case RaBitQExDataLayout::kVectorBitplanesMicroBatch:
    case RaBitQExDataLayout::kVectorBitMajorTiles:
    case RaBitQExDataLayout::kTileLaneBitMajor:
    case RaBitQExDataLayout::kSmallLane4Bitplanes:
    case RaBitQExDataLayout::kSmallLane2Bitplanes:
      return 0;
    case RaBitQExDataLayout::kVectorNibble4:
      return 4;
    case RaBitQExDataLayout::kVector2Bit:
      return 2;
    case RaBitQExDataLayout::kGenericPacked:
    case RaBitQExDataLayout::kSelectedDirect:
    default:
      return 0;
  }
}

constexpr bool RaBitQExDataLayoutIsDirect(RaBitQExDataLayout layout) {
  const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
  return resolved == RaBitQExDataLayout::kSplit3TwoPlusOne ||
         resolved == RaBitQExDataLayout::kSplit3Bitplanes ||
         resolved == RaBitQExDataLayout::kSplit1Bitplane ||
         resolved == RaBitQExDataLayout::kSplit2Bitplanes ||
         resolved == RaBitQExDataLayout::kSplit3TrimmedBitplanes ||
         resolved == RaBitQExDataLayout::kSplit3ZeroPlaneElide ||
         resolved == RaBitQExDataLayout::kVectorBitplanes ||
         resolved == RaBitQExDataLayout::kVectorBitplanesPrefetch ||
         resolved == RaBitQExDataLayout::kVectorBitplanesMicroBatch ||
         resolved == RaBitQExDataLayout::kVectorBitMajorTiles ||
         resolved == RaBitQExDataLayout::kTileLaneBitMajor ||
         resolved == RaBitQExDataLayout::kVectorNibble4 ||
         resolved == RaBitQExDataLayout::kVector2Bit ||
         resolved == RaBitQExDataLayout::kSmallLane4Bitplanes ||
         resolved == RaBitQExDataLayout::kSmallLane2Bitplanes;
}

constexpr bool RaBitQExDataLayoutSupportsActiveExBits(RaBitQExDataLayout layout) {
  const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
  return resolved == RaBitQExDataLayout::kSplit1Bitplane ||
         resolved == RaBitQExDataLayout::kSplit2Bitplanes ||
         resolved == RaBitQExDataLayout::kSplit3Bitplanes ||
         resolved == RaBitQExDataLayout::kSplit3TrimmedBitplanes ||
         resolved == RaBitQExDataLayout::kVectorBitplanes ||
         resolved == RaBitQExDataLayout::kVectorBitplanesPrefetch ||
         resolved == RaBitQExDataLayout::kVectorBitplanesMicroBatch ||
         resolved == RaBitQExDataLayout::kVectorBitMajorTiles ||
         resolved == RaBitQExDataLayout::kTileLaneBitMajor;
}

/// RaBitQ (Reduced-Bit Quantization) configuration
struct RaBitQConfig {
  uint8_t bits = 1;          // Legacy Stage2 payload bits; official mode uses ex_bits.
  uint32_t block_size = 64;  // Block granularity for SIMD (typically 64)
  float c_factor = 5.75f;    // Error bound factor: epsilon = c * 2^(-B/2) / sqrt(D)
  uint8_t storage_version = 7;  // On-disk format version (7 = dual-region FastScan)
  uint8_t total_bits = 1;    // Official reported precision: 1 + ex_bits.
  uint8_t ex_bits = 0;       // Official Stage2 ExData payload bits.
  RaBitQEstimatorMode estimator_mode =
      RaBitQEstimatorMode::kLegacySignedMagnitude;
  RaBitQExDataLayout exdata_layout = RaBitQExDataLayout::kGenericPacked;

  bool uses_official_1_plus_n() const {
    return estimator_mode == RaBitQEstimatorMode::kOfficial1PlusN;
  }

  uint8_t effective_total_bits() const {
    return uses_official_1_plus_n() ? total_bits : bits;
  }

  uint8_t stage2_payload_bits() const {
    return uses_official_1_plus_n() ? ex_bits : (bits > 1 ? bits : 0);
  }

  uint8_t active_code_bits() const {
    const uint8_t payload_bits = stage2_payload_bits();
    return payload_bits > 0 ? payload_bits : 1;
  }

  bool has_stage2_payload() const { return stage2_payload_bits() > 0; }

  bool official_bits_valid() const {
    return !uses_official_1_plus_n() || total_bits == static_cast<uint8_t>(ex_bits + 1u);
  }

  RaBitQExDataLayout effective_exdata_layout() const {
    return uses_official_1_plus_n()
        ? RaBitQResolveSelectedExDataLayoutForBits(exdata_layout, ex_bits)
        : RaBitQExDataLayout::kGenericPacked;
  }

  bool exdata_layout_valid() const {
    if (!uses_official_1_plus_n()) {
      return exdata_layout == RaBitQExDataLayout::kGenericPacked;
    }
    const RaBitQExDataLayout effective = effective_exdata_layout();
    if (effective == RaBitQExDataLayout::kGenericPacked) {
      return true;
    }
    if (effective == RaBitQExDataLayout::kSplit3TwoPlusOne) {
      return ex_bits == 3;
    }
    if (effective == RaBitQExDataLayout::kVectorBitplanes ||
        effective == RaBitQExDataLayout::kVectorBitplanesPrefetch ||
        effective == RaBitQExDataLayout::kVectorBitplanesMicroBatch ||
        effective == RaBitQExDataLayout::kSmallLane4Bitplanes ||
        effective == RaBitQExDataLayout::kSmallLane2Bitplanes) {
      return ex_bits >= 1 && ex_bits <= 4;
    }
    if (effective == RaBitQExDataLayout::kVectorBitMajorTiles ||
        effective == RaBitQExDataLayout::kTileLaneBitMajor) {
      return ex_bits >= 1 && ex_bits <= 3;
    }
    const uint8_t direct_bits = RaBitQExDataLayoutDirectBits(effective);
    return direct_bits != 0 && direct_bits == ex_bits;
  }

  bool operator==(const RaBitQConfig& other) const {
    return bits == other.bits &&
           block_size == other.block_size &&
           c_factor == other.c_factor &&
           storage_version == other.storage_version &&
           total_bits == other.total_bits &&
           ex_bits == other.ex_bits &&
           estimator_mode == other.estimator_mode &&
           exdata_layout == other.exdata_layout;
  }
  bool operator!=(const RaBitQConfig& other) const { return !(*this == other); }
};

inline std::string RaBitQFormatKey(const RaBitQConfig& config) {
  if (config.uses_official_1_plus_n()) {
    std::string key =
        "official_1_plus_n_total" +
        std::to_string(static_cast<uint32_t>(config.effective_total_bits())) +
        "_ex" +
        std::to_string(static_cast<uint32_t>(config.stage2_payload_bits()));
    const RaBitQExDataLayout layout = config.effective_exdata_layout();
    if (RaBitQExDataLayoutIsDirect(layout)) {
      key += "_";
      key += std::string(RaBitQExDataLayoutName(layout));
    } else if (config.exdata_layout == RaBitQExDataLayout::kSelectedDirect) {
      key += "_selected_direct";
    }
    return key;
  }
  return "legacy_signed_magnitude_bits" +
         std::to_string(static_cast<uint32_t>(config.effective_total_bits()));
}

// ============================================================================
// Constants
// ============================================================================

/// Default block size for I/O alignment (4KB)
constexpr size_t kDefaultBlockSize = 4096;

/// Default page size for page-aligned record storage (4KB)
constexpr uint32_t kDefaultPageSize = 4096;

/// Cache line size for SIMD alignment
constexpr size_t kCacheLineSize = 64;

/// SIMD register width (AVX2 = 256 bits = 32 bytes)
constexpr size_t kSimdWidth = 32;

/// AVX-512 register width
constexpr size_t kSimd512Width = 64;

/// Inline/Extern threshold: payloads smaller than this are stored inline
/// in the record file; payloads >= this size go to an external blob file.
constexpr size_t kInlineThreshold = 4096;

}  // namespace vdb
