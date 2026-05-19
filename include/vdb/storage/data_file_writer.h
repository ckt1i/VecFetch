#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"

namespace vdb {
namespace storage {

// ============================================================================
// DataFileWriter — writes row-major records to a .dat file
// ============================================================================

/// Writes a DataFile for one IVF cluster in row-major format.
///
/// Each record is laid out as:
///   [raw_vector (dim × 4 bytes) | payload_col0 | payload_col1 | ...]
///
/// Fixed-width payload columns are written directly; variable-length columns
/// are prefixed with a uint32_t length.
///
/// Usage:
///   DataFileWriter writer;
///   writer.Open(path, dim, payload_columns);
///   for each record:
///     AddressEntry addr;
///     writer.WriteRecord(vec, payload, addr);
///   writer.Finalize();
///
/// The returned AddressEntry from each WriteRecord() call should be collected
/// and passed to AddressColumn::Encode() to build the address index.
///
class DataFileWriter {
 public:
    DataFileWriter();
    ~DataFileWriter();

    VDB_DISALLOW_COPY_AND_MOVE(DataFileWriter);

    /// Open a new DataFile for writing.
    ///
    /// @param path             Output file path
    /// @param cluster_id       Associated cluster ID
    /// @param dim              Vector dimensionality
    /// @param payload_schemas  Payload column definitions (may be empty)
    /// @param page_size        Page alignment granularity in bytes.
    ///                         Each record is padded to this boundary.
    ///                         Use 1 to disable padding.
    /// @return Status
    Status Open(const std::string& path,
                uint32_t cluster_id,
                Dim raw_dim,
                const std::vector<ColumnSchema>& payload_schemas = {},
                uint32_t page_size = kDefaultPageSize);

    /// Write a single record (vector + payload).
    ///
    /// @param vec         Raw float vector (length = dim)
    /// @param payload     Payload values ordered by payload_schemas. Pass empty
    ///                    vector if no payload columns.
    /// @param out_entry   Receives the record's physical address (offset, size)
    /// @return Status
    Status WriteRecord(const float* vec,
                       const std::vector<Datum>& payload,
                       AddressEntry& out_entry);

    /// Finalize the file (flush and close).
    Status Finalize();

    /// Number of records written so far.
    uint64_t num_records() const { return num_records_; }

    /// Current write offset (total bytes written to data region).
    uint64_t current_offset() const { return current_offset_; }

    /// The file path.
    const std::string& path() const { return path_; }

 private:
    std::string path_;
    std::ofstream file_;
    uint32_t cluster_id_ = 0;
    Dim dim_ = 0;
    std::vector<ColumnSchema> payload_schemas_;
    uint32_t page_size_ = kDefaultPageSize;
    uint64_t current_offset_ = 0;
    uint64_t num_records_ = 0;
    bool finalized_ = false;

    /// Write a single Datum value.  Fixed-width → raw bytes; variable-length →
    /// length-prefix + data.
    Status WriteDatum(const Datum& datum, const ColumnSchema& schema);
};

}  // namespace storage
}  // namespace vdb
