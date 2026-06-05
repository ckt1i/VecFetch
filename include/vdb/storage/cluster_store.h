#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "vdb/common/macros.h"
#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/storage/address_column.h"

namespace vdb {
namespace storage {

class ClusterStoreWriter {
 public:
    ClusterStoreWriter();
    ~ClusterStoreWriter();

    VDB_DISALLOW_COPY_AND_MOVE(ClusterStoreWriter);

    struct ClusterLookupEntry {
        uint32_t cluster_id;
        uint32_t num_records;
        float epsilon = 0.0f;
        std::vector<float> centroid;
        uint64_t block_offset;
        uint64_t block_size;
        uint32_t num_fastscan_blocks = 0;
        uint32_t exrabitq_region_offset = 0;
    };

    struct GlobalInfo {
        uint32_t num_clusters;
        Dim dim;
        RaBitQConfig rabitq_config;
        std::string data_file_path;
        std::vector<ClusterLookupEntry> lookup_table;
    };

    Status Open(const std::string& path,
                uint32_t num_clusters,
                Dim dim,
                const RaBitQConfig& rabitq_config);

    Status BeginCluster(uint32_t cluster_id,
                        uint32_t num_records,
                        const float* centroid,
                        float epsilon = 0.0f);

    Status WriteVectors(const std::vector<rabitq::RaBitQCode>& codes);
    Status WriteAddressBlocks(const EncodedAddressColumn& column);
    Status EndCluster();
    Status Finalize(const std::string& data_file_path);

    const GlobalInfo& info() const { return info_; }

 private:
    std::fstream file_;
    std::string path_;
    GlobalInfo info_;
    uint64_t current_offset_ = 0;
    uint64_t lookup_table_start_ = 0;
    uint64_t header_data_file_path_offset_ = 0;
    uint32_t current_cluster_index_ = 0;
    uint64_t block_start_ = 0;
    bool in_cluster_ = false;
    bool vectors_written_ = false;
    bool address_written_ = false;
    bool finalized_ = false;
    EncodedAddressColumn current_address_column_;
    uint32_t current_num_fastscan_blocks_ = 0;
    uint32_t current_exrabitq_region_offset_ = 0;

    uint64_t lookup_entry_size() const;
};

class ClusterStoreReader {
 public:
    struct ResidentClusterView {
        std::vector<uint8_t> code_storage;
        const uint8_t* fastscan_blocks = nullptr;
        uint32_t fastscan_block_size = 0;
        uint32_t num_fastscan_blocks = 0;
        const uint8_t* exrabitq_entries = nullptr;
        uint32_t exrabitq_entry_size = 0;
        uint32_t exrabitq_sign_bytes = 0;
        bool exrabitq_sign_packed = false;
        uint32_t exrabitq_storage_version = 0;
        const uint8_t* exrabitq_batch_blocks = nullptr;
        uint32_t exrabitq_batch_block_size = 0;
        uint32_t exrabitq_batch_size = 0;
        uint32_t exrabitq_dim_block = 0;
        uint32_t exrabitq_num_dim_blocks = 0;
        uint32_t exrabitq_num_batch_blocks = 0;
        uint32_t exrabitq_abs_bytes_per_lane_dim_block = 0;
        uint8_t exrabitq_magnitude_bits = 0;
        bool exrabitq_magnitude_packed = false;
        uint8_t rabitq_total_bits = 1;
        uint8_t rabitq_ex_bits = 0;
        RaBitQEstimatorMode rabitq_estimator_mode =
            RaBitQEstimatorMode::kLegacySignedMagnitude;
        RaBitQExDataLayout rabitq_exdata_layout = RaBitQExDataLayout::kGenericPacked;
        std::vector<uint8_t> exrabitq_parallel_abs_blocks_storage;
        std::vector<uint16_t> exrabitq_parallel_sign_words_storage;
        uint32_t exrabitq_parallel_abs_block_size = 0;
        uint32_t exrabitq_parallel_sign_words_per_block = 0;
        uint32_t exrabitq_parallel_slices_per_dim_block = 0;
        uint32_t num_records = 0;
        float epsilon = 0.0f;
        const RawAddressEntryV2* raw_addresses = nullptr;
        uint32_t address_page_size = 0;
        bool addresses_are_raw_v2 = false;
        std::vector<AddressEntry> decoded_addresses;
        uint64_t code_storage_bytes = 0;
        uint64_t decoded_address_bytes = 0;
        uint64_t raw_address_bytes = 0;

        query::ParsedCluster ToParsedCluster() const {
            query::ParsedCluster pc;
            pc.fastscan_blocks = fastscan_blocks;
            pc.fastscan_block_size = fastscan_block_size;
            pc.num_fastscan_blocks = num_fastscan_blocks;
            pc.exrabitq_entries = exrabitq_entries;
            pc.exrabitq_entry_size = exrabitq_entry_size;
            pc.exrabitq_sign_bytes = exrabitq_sign_bytes;
            pc.exrabitq_sign_packed = exrabitq_sign_packed;
            pc.exrabitq_storage_version = exrabitq_storage_version;
            pc.exrabitq_batch_blocks = exrabitq_batch_blocks;
            pc.exrabitq_batch_block_size = exrabitq_batch_block_size;
            pc.exrabitq_batch_size = exrabitq_batch_size;
            pc.exrabitq_dim_block = exrabitq_dim_block;
            pc.exrabitq_num_dim_blocks = exrabitq_num_dim_blocks;
            pc.exrabitq_num_batch_blocks = exrabitq_num_batch_blocks;
            pc.exrabitq_abs_bytes_per_lane_dim_block =
                exrabitq_abs_bytes_per_lane_dim_block;
            pc.exrabitq_magnitude_bits = exrabitq_magnitude_bits;
            pc.exrabitq_magnitude_packed = exrabitq_magnitude_packed;
            pc.rabitq_total_bits = rabitq_total_bits;
            pc.rabitq_ex_bits = rabitq_ex_bits;
            pc.rabitq_estimator_mode = rabitq_estimator_mode;
            pc.rabitq_exdata_layout = rabitq_exdata_layout;
            pc.exrabitq_parallel_abs_blocks =
                exrabitq_parallel_abs_blocks_storage.empty()
                ? nullptr
                : exrabitq_parallel_abs_blocks_storage.data();
            pc.exrabitq_parallel_sign_words =
                exrabitq_parallel_sign_words_storage.empty()
                ? nullptr
                : exrabitq_parallel_sign_words_storage.data();
            pc.exrabitq_parallel_abs_block_size = exrabitq_parallel_abs_block_size;
            pc.exrabitq_parallel_sign_words_per_block =
                exrabitq_parallel_sign_words_per_block;
            pc.exrabitq_parallel_slices_per_dim_block =
                exrabitq_parallel_slices_per_dim_block;
            pc.num_records = num_records;
            pc.epsilon = epsilon;
            pc.raw_addresses = raw_addresses;
            pc.address_page_size = address_page_size;
            pc.addresses_are_raw_v2 = addresses_are_raw_v2;
            pc.decoded_addresses_data =
                decoded_addresses.empty() ? nullptr : decoded_addresses.data();
            pc.decoded_address_count =
                static_cast<uint32_t>(decoded_addresses.size());
            pc.fastscan_region_offset = 0;
            pc.exrabitq_region_offset =
                (exrabitq_entries != nullptr && fastscan_blocks != nullptr)
                    ? static_cast<uint64_t>(exrabitq_entries - fastscan_blocks)
                    : ((exrabitq_batch_blocks != nullptr && fastscan_blocks != nullptr)
                           ? static_cast<uint64_t>(exrabitq_batch_blocks - fastscan_blocks)
                           : 0);
            pc.code_region_bytes = code_storage_bytes;
            pc.address_payload_bytes = raw_address_bytes;
            pc.codes_start = fastscan_blocks;
            pc.code_entry_size = 0;
            return pc;
        }
    };

    ClusterStoreReader();
    ~ClusterStoreReader();

    VDB_DISALLOW_COPY(ClusterStoreReader);
    ClusterStoreReader(ClusterStoreReader&& other) noexcept;
    ClusterStoreReader& operator=(ClusterStoreReader&& other) noexcept;

    Status Open(const std::string& path, bool use_direct_io = false);
    void Close();

    uint32_t num_clusters() const { return info_.num_clusters; }
    uint32_t file_version() const { return file_version_; }
    Dim dim() const { return info_.dim; }
    const RaBitQConfig& rabitq_config() const { return info_.rabitq_config; }
    const std::string& data_file_path() const { return info_.data_file_path; }

    std::vector<uint32_t> cluster_ids() const;
    uint32_t GetNumRecords(uint32_t cluster_id) const;
    const float* GetCentroid(uint32_t cluster_id) const;
    float GetEpsilon(uint32_t cluster_id) const;
    uint64_t total_records() const;

    Status EnsureClusterLoaded(uint32_t cluster_id);
    Status UnloadCluster(uint32_t cluster_id);
    AddressEntry GetAddress(uint32_t cluster_id, uint32_t record_idx) const;
    std::vector<AddressEntry> GetAddresses(
        uint32_t cluster_id,
        const std::vector<uint32_t>& indices) const;

    Status LoadCode(uint32_t cluster_id,
                    uint32_t record_idx,
                    std::vector<uint64_t>& out_code) const;
    Status LoadCodes(uint32_t cluster_id,
                     const std::vector<uint32_t>& indices,
                     std::vector<rabitq::RaBitQCode>& out_codes) const;
    const uint8_t* GetCodePtr(uint32_t cluster_id,
                              uint32_t record_idx) const;

    int clu_fd() const { return fd_; }
    std::optional<query::ClusterBlockLocation> GetBlockLocation(
        uint32_t cluster_id) const;
    Status ParseClusterBlock(uint32_t cluster_id,
                             query::AlignedBufPtr block_buf,
                             uint64_t block_size,
                             query::ParsedCluster& out);

    Status PreloadAllClusters();
    bool resident_preload_enabled() const { return resident_preload_ready_; }
    uint64_t resident_preload_bytes() const { return resident_preload_bytes_; }
    double resident_preload_time_ms() const { return resident_preload_time_ms_; }
    const ResidentClusterView* GetResidentClusterView(uint32_t cluster_id) const;
    const query::ParsedCluster* GetResidentParsedCluster(uint32_t cluster_id) const;
    uint64_t resident_cluster_mem_bytes() const { return resident_cluster_mem_bytes_; }
    uint64_t resident_file_size_bytes() const { return resident_file_size_bytes_; }
    uint64_t resident_file_buffer_bytes() const { return resident_file_buffer_bytes_; }
    uint64_t resident_code_storage_bytes() const { return resident_code_storage_bytes_; }
    uint64_t resident_decoded_address_bytes() const { return resident_decoded_address_bytes_; }
    uint64_t resident_raw_address_bytes() const { return resident_raw_address_bytes_; }
    uint64_t resident_parsed_address_duplicate_bytes() const {
        return resident_parsed_address_duplicate_bytes_;
    }
    uint32_t resident_preload_batch_size() const { return resident_preload_batch_size_; }
    const std::string& resident_preload_mode() const { return resident_preload_mode_; }
    uint64_t resident_parallel_view_bytes() const { return resident_parallel_view_bytes_; }
    double resident_parallel_view_build_ms() const { return resident_parallel_view_build_ms_; }
    bool is_open() const { return fd_ >= 0; }

 private:
    int fd_ = -1;
    uint32_t file_version_ = 0;
    ClusterStoreWriter::GlobalInfo info_;
    std::map<uint32_t, uint32_t> cluster_index_;

    struct ClusterData {
        uint64_t codes_offset = 0;
        uint32_t codes_length = 0;
        std::vector<uint8_t> codes_buffer;
        AddressFormat address_format = AddressFormat::V1Packed;
        AddressColumnLayout address_layout;
        std::vector<AddressBlock> address_blocks;
        std::vector<RawAddressEntryV2> raw_addresses_v2;
        std::vector<AddressEntry> decoded_addresses;
    };

    std::map<uint32_t, ClusterData> loaded_clusters_;
    std::vector<uint8_t> resident_file_buffer_;
    std::map<uint32_t, ResidentClusterView> resident_clusters_;
    std::map<uint32_t, query::ParsedCluster> resident_parsed_clusters_;
    bool resident_preload_ready_ = false;
    uint64_t resident_preload_bytes_ = 0;
    uint64_t resident_cluster_mem_bytes_ = 0;
    uint64_t resident_file_size_bytes_ = 0;
    uint64_t resident_file_buffer_bytes_ = 0;
    uint64_t resident_code_storage_bytes_ = 0;
    uint64_t resident_decoded_address_bytes_ = 0;
    uint64_t resident_raw_address_bytes_ = 0;
    uint64_t resident_parsed_address_duplicate_bytes_ = 0;
    uint32_t resident_preload_batch_size_ = 0;
    std::string resident_preload_mode_;
    double resident_preload_time_ms_ = 0.0;
    uint64_t resident_parallel_view_bytes_ = 0;
    double resident_parallel_view_build_ms_ = 0.0;

    uint32_t num_code_words() const { return (info_.dim + 63) / 64; }
    uint32_t fastscan_packed_size() const { return info_.dim * 4; }
    uint32_t fastscan_block_bytes() const {
        return fastscan_packed_size() + 32 * sizeof(float);
    }
    uint32_t exrabitq_sign_bytes() const {
        if (info_.rabitq_config.uses_official_1_plus_n()) return 0;
        if (info_.rabitq_config.bits <= 1) return 0;
        if (file_version_ >= 10) return (info_.dim + 7) / 8;
        return info_.dim;
    }
    uint32_t exrabitq_entry_size() const {
        if (info_.rabitq_config.bits <= 1) return 0;
        if (file_version_ >= 11) return 0;
        return info_.dim + exrabitq_sign_bytes() + sizeof(float);
    }
    uint32_t exrabitq_batch_size() const { return 8; }
    uint32_t exrabitq_dim_block() const { return 64; }
    uint32_t exrabitq_num_batch_blocks(uint32_t num_records) const {
        const uint32_t bs = exrabitq_batch_size();
        return bs == 0 ? 0 : (num_records + bs - 1) / bs;
    }
    uint32_t exrabitq_dim_block_count() const {
        const uint32_t db = exrabitq_dim_block();
        return db == 0 ? 0 : (info_.dim + db - 1) / db;
    }
    bool exrabitq_magnitude_packed() const {
        return info_.rabitq_config.stage2_payload_bits() > 0 && file_version_ >= 12;
    }
    uint32_t exrabitq_abs_bytes_per_lane_dim_block() const {
        const uint8_t stage2_bits = info_.rabitq_config.stage2_payload_bits();
        if (stage2_bits == 0 || file_version_ < 11) return 0;
        const uint32_t db = exrabitq_dim_block();
        if (file_version_ >= 12) {
            return (db * static_cast<uint32_t>(stage2_bits) + 7u) / 8u;
        }
        return db;
    }
    uint32_t exrabitq_batch_block_size() const {
        if (info_.rabitq_config.stage2_payload_bits() == 0 || file_version_ < 11) {
            return 0;
        }
        const uint32_t batch = exrabitq_batch_size();
        const uint32_t db = exrabitq_dim_block();
        const uint32_t blocks = exrabitq_dim_block_count();
        const uint32_t abs_bytes =
            blocks * batch * exrabitq_abs_bytes_per_lane_dim_block();
        const uint32_t sign_bytes = info_.rabitq_config.uses_official_1_plus_n()
            ? 0
            : blocks * batch * (db / 8);
        const uint32_t factor_count = info_.rabitq_config.uses_official_1_plus_n()
            ? batch * 2u
            : batch;
        return sizeof(uint32_t) + abs_bytes + sign_bytes +
               factor_count * sizeof(float);
    }

    Status ParseClusterBlockView(uint32_t cluster_id,
                                 const uint8_t* block_ptr,
                                 uint64_t block_size,
                                 query::ParsedCluster& out) const;
};

}  // namespace storage
}  // namespace vdb
