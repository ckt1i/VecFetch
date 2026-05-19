#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/storage/cluster_store.h"
#include "vdb/storage/data_file_reader.h"

namespace vdb {
namespace storage {

// ============================================================================
// Segment — manages a single .clu + .dat file pair for one IVF segment
// ============================================================================

/// A Segment represents the on-disk storage for one IVF index.
///
/// On disk (unified layout):
///   segment_dir/
///   ├── cluster.clu      ← all clusters in a single file
///   ├── data.dat         ← all records in a single file
///   ├── centroids.bin
///   ├── rotation.bin
///   └── segment.meta
///
/// Open() reads the unified .clu file header + lookup table,
/// and opens the .dat file for record access.
///
/// Per-cluster data (codes, decoded addresses) is lazily loaded
/// via EnsureClusterLoaded().
///
class Segment {
 public:
    Segment();
    ~Segment();

    VDB_DISALLOW_COPY_AND_MOVE(Segment);

    /// Open a segment from a directory.
    ///
    /// Opens cluster.clu (reading header + full lookup table) and
    /// data.dat for random-access record reading.
    ///
    /// @param dir              Segment directory path
    /// @param payload_schemas  Payload column schemas for DataFileReader
    /// @return Status
    Status Open(const std::string& dir,
                const std::vector<ColumnSchema>& payload_schemas = {},
                bool use_direct_io = false,
                std::optional<Dim> raw_vector_dim = std::nullopt);

    /// Whether the segment is open.
    bool is_open() const { return clu_reader_.is_open(); }

    // ---- Forwarded from ClusterStoreReader ----

    /// Number of clusters.
    uint32_t num_clusters() const { return clu_reader_.num_clusters(); }

    /// All cluster IDs (file order).
    std::vector<uint32_t> cluster_ids() const { return clu_reader_.cluster_ids(); }

    /// Total records across all clusters.
    uint64_t total_records() const { return clu_reader_.total_records(); }

    /// Number of records in a cluster.
    uint32_t GetNumRecords(uint32_t cluster_id) const {
        return clu_reader_.GetNumRecords(cluster_id);
    }

    /// Get centroid for a cluster.
    const float* GetCentroid(uint32_t cluster_id) const {
        return clu_reader_.GetCentroid(cluster_id);
    }

    /// Ensure per-cluster data is loaded (codes region + decoded addresses).
    Status EnsureClusterLoaded(uint32_t cluster_id) {
        return clu_reader_.EnsureClusterLoaded(cluster_id);
    }

    /// Get a decoded address entry.
    AddressEntry GetAddress(uint32_t cluster_id, uint32_t record_idx) const {
        return clu_reader_.GetAddress(cluster_id, record_idx);
    }

    /// Load a single RaBitQ code.
    Status LoadCode(uint32_t cluster_id, uint32_t record_idx,
                    std::vector<uint64_t>& out_code) const {
        return clu_reader_.LoadCode(cluster_id, record_idx, out_code);
    }

    /// Load multiple RaBitQ codes.
    Status LoadCodes(uint32_t cluster_id,
                     const std::vector<uint32_t>& indices,
                     std::vector<rabitq::RaBitQCode>& out_codes) const {
        return clu_reader_.LoadCodes(cluster_id, indices, out_codes);
    }

    /// Get raw pointer to a code entry in the cached codes_buffer.
    const uint8_t* GetCodePtr(uint32_t cluster_id,
                              uint32_t record_idx) const {
        return clu_reader_.GetCodePtr(cluster_id, record_idx);
    }

    // ---- Async query support (Phase 8) ----

    /// .clu file descriptor for io_uring direct submission.
    int clu_fd() const { return clu_reader_.clu_fd(); }

    /// Look up cluster block location (pure memory).
    std::optional<query::ClusterBlockLocation> GetBlockLocation(
        uint32_t cluster_id) const {
        return clu_reader_.GetBlockLocation(cluster_id);
    }

    /// Parse a raw block buffer into a ParsedCluster (pure CPU, no I/O).
    Status ParseClusterBlock(uint32_t cluster_id,
                              query::AlignedBufPtr block_buf,
                              uint64_t block_size,
                              query::ParsedCluster& out) {
        return clu_reader_.ParseClusterBlock(
            cluster_id, std::move(block_buf), block_size, out);
    }

    Status PreloadAllClusters() {
        return clu_reader_.PreloadAllClusters();
    }

    bool resident_preload_enabled() const {
        return clu_reader_.resident_preload_enabled();
    }

    uint64_t resident_preload_bytes() const {
        return clu_reader_.resident_preload_bytes();
    }

    double resident_preload_time_ms() const {
        return clu_reader_.resident_preload_time_ms();
    }
    uint64_t resident_cluster_mem_bytes() const {
        return clu_reader_.resident_cluster_mem_bytes();
    }
    uint64_t resident_parallel_view_bytes() const {
        return clu_reader_.resident_parallel_view_bytes();
    }
    double resident_parallel_view_build_ms() const {
        return clu_reader_.resident_parallel_view_build_ms();
    }

    const ClusterStoreReader::ResidentClusterView* GetResidentClusterView(
        uint32_t cluster_id) const {
        return clu_reader_.GetResidentClusterView(cluster_id);
    }
    const query::ParsedCluster* GetResidentParsedCluster(
        uint32_t cluster_id) const {
        return clu_reader_.GetResidentParsedCluster(cluster_id);
    }

    // ---- DataFileReader forwarding ----

    /// Read a raw vector from a record.
    Status ReadVector(const AddressEntry& addr, float* out_vec) const {
        return dat_reader_.ReadVector(addr, out_vec);
    }

    /// Read a full record (vector + payload).
    Status ReadRecord(const AddressEntry& addr,
                      float* out_vec,
                      std::vector<Datum>& out_payload) const {
        return dat_reader_.ReadRecord(addr, out_vec, out_payload);
    }

    // ---- Direct reader access (for advanced use) ----

    /// Access the underlying ClusterStoreReader.
    ClusterStoreReader& cluster_reader() { return clu_reader_; }
    const ClusterStoreReader& cluster_reader() const { return clu_reader_; }

    /// Access the underlying DataFileReader.
    DataFileReader& data_reader() { return dat_reader_; }
    const DataFileReader& data_reader() const { return dat_reader_; }

    /// Vector dimensionality.
    Dim dim() const { return clu_reader_.dim(); }

    /// RaBitQ configuration.
    const RaBitQConfig& rabitq_config() const {
        return clu_reader_.rabitq_config();
    }

 private:
    ClusterStoreReader clu_reader_;
    DataFileReader dat_reader_;
};

}  // namespace storage
}  // namespace vdb
