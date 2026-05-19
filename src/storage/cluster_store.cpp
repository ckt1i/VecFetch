#include "vdb/storage/cluster_store.h"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "vdb/simd/popcount.h"
#include "vdb/storage/pack_codes.h"

namespace vdb {
namespace storage {

static constexpr uint32_t kGlobalMagic = 0x4C4D4356;
static constexpr uint32_t kFileVersion = 11;
static constexpr uint32_t kFileVersionV10 = 10;
static constexpr uint32_t kFileVersionV9 = 9;
static constexpr uint32_t kFileVersionV8 = 8;
static constexpr uint32_t kFileVersionV7 = 7;
static constexpr uint32_t kAlignSize = 4096;
static constexpr uint32_t kBlockMagic = 0x424C4356;
static constexpr uint32_t kAddressFormatV2 = 2;

namespace {

inline uint32_t PackedSignBytes(Dim dim) {
    return (dim + 7) / 8;
}

inline uint32_t ExRaBitQBatchSize() {
    return 8;
}

inline uint32_t ExRaBitQDimBlock() {
    return 64;
}

inline uint32_t EffectiveWriterFileVersion() {
    const char* env = std::getenv("VDB_CLUSTER_STORE_VERSION");
    if (env == nullptr || env[0] == '\0') {
        return kFileVersion;
    }
    const long ver = std::strtol(env, nullptr, 10);
    if (ver == static_cast<long>(kFileVersionV10)) return kFileVersionV10;
    if (ver == static_cast<long>(kFileVersion)) return kFileVersion;
    return kFileVersion;
}

inline void WriteRaw(std::fstream& f, const void* data, size_t len) {
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
}

template <typename T>
inline void WriteVal(std::fstream& f, T value) {
    WriteRaw(f, &value, sizeof(T));
}

template <typename T>
inline bool PreadValue(int fd, off_t offset, T& out) {
    ssize_t n = ::pread(fd, &out, sizeof(T), offset);
    return n == static_cast<ssize_t>(sizeof(T));
}

inline bool PreadBytes(int fd, off_t offset, void* out, size_t len) {
    constexpr size_t kMaxChunk = 1u << 30;  // 1 GiB per syscall, stay below kernel short-read limits.
    uint8_t* dst = static_cast<uint8_t*>(out);
    size_t remaining = len;
    off_t cur_off = offset;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, kMaxChunk);
        ssize_t n = ::pread(fd, dst, chunk, cur_off);
        if (n != static_cast<ssize_t>(chunk)) {
            return false;
        }
        dst += chunk;
        cur_off += static_cast<off_t>(chunk);
        remaining -= chunk;
    }
    return true;
}

inline uint64_t RoundUp4K(uint64_t v) {
    return (v + kAlignSize - 1) & ~(static_cast<uint64_t>(kAlignSize) - 1);
}

inline void PadTo4K(std::fstream& f, uint64_t& offset) {
    uint64_t aligned = RoundUp4K(offset);
    if (aligned > offset) {
        uint64_t pad = aligned - offset;
        char zeros[kAlignSize] = {0};
        while (pad > 0) {
            uint64_t chunk = std::min(pad, static_cast<uint64_t>(kAlignSize));
            f.write(zeros, static_cast<std::streamsize>(chunk));
            pad -= chunk;
        }
        offset = aligned;
    }
}

void BuildResidentParallelStage2View(const query::ParsedCluster& parsed,
                                     ClusterStoreReader::ResidentClusterView* view) {
    if (view == nullptr) return;
    if (parsed.exrabitq_storage_version < 11 ||
        parsed.exrabitq_batch_blocks == nullptr ||
        parsed.exrabitq_batch_size == 0 ||
        parsed.exrabitq_dim_block == 0 ||
        parsed.exrabitq_num_dim_blocks == 0 ||
        parsed.exrabitq_num_batch_blocks == 0) {
        return;
    }

    const uint32_t batch_size = parsed.exrabitq_batch_size;
    const uint32_t dim_block = parsed.exrabitq_dim_block;
    const uint32_t num_dim_blocks = parsed.exrabitq_num_dim_blocks;
    const uint32_t num_batch_blocks = parsed.exrabitq_num_batch_blocks;
    const uint32_t slices_per_dim_block = dim_block / 16;
    const uint32_t sign_block_bytes = dim_block / 8;
    const uint32_t abs_block_size = num_dim_blocks * slices_per_dim_block * batch_size * 16;
    const uint32_t sign_words_per_block =
        num_dim_blocks * slices_per_dim_block * batch_size;

    view->exrabitq_parallel_abs_block_size = abs_block_size;
    view->exrabitq_parallel_sign_words_per_block = sign_words_per_block;
    view->exrabitq_parallel_slices_per_dim_block = slices_per_dim_block;
    view->exrabitq_parallel_abs_blocks_storage.resize(
        static_cast<size_t>(num_batch_blocks) * abs_block_size);
    view->exrabitq_parallel_sign_words_storage.resize(
        static_cast<size_t>(num_batch_blocks) * sign_words_per_block);

    for (uint32_t bb = 0; bb < num_batch_blocks; ++bb) {
        const auto block_view = parsed.exrabitq_batch_block_view(bb);
        uint8_t* abs_out =
            view->exrabitq_parallel_abs_blocks_storage.data() +
            static_cast<size_t>(bb) * abs_block_size;
        uint16_t* sign_out =
            view->exrabitq_parallel_sign_words_storage.data() +
            static_cast<size_t>(bb) * sign_words_per_block;
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const uint8_t* abs_src_block =
                block_view.abs_blocks + static_cast<size_t>(db) * batch_size * dim_block;
            const uint8_t* sign_src_block =
                block_view.sign_blocks + static_cast<size_t>(db) * batch_size * sign_block_bytes;
            for (uint32_t sub = 0; sub < slices_per_dim_block; ++sub) {
                for (uint32_t lane = 0; lane < batch_size; ++lane) {
                    const size_t abs_idx =
                        (((static_cast<size_t>(db) * slices_per_dim_block + sub) * batch_size) +
                         lane) * 16;
                    std::memcpy(abs_out + abs_idx,
                                abs_src_block + static_cast<size_t>(lane) * dim_block + sub * 16,
                                16);
                    uint16_t sign_word = 0;
                    std::memcpy(&sign_word,
                                sign_src_block + static_cast<size_t>(lane) * sign_block_bytes +
                                    sub * sizeof(uint16_t),
                                sizeof(uint16_t));
                    sign_out[(static_cast<size_t>(db) * slices_per_dim_block + sub) *
                                 batch_size +
                             lane] = sign_word;
                }
            }
        }
    }
}

}  // namespace

ClusterStoreWriter::ClusterStoreWriter() = default;

ClusterStoreWriter::~ClusterStoreWriter() {
    if (file_.is_open() && !finalized_) {
        file_.close();
    }
}

uint64_t ClusterStoreWriter::lookup_entry_size() const {
    return 4 + 4 + 4 + static_cast<uint64_t>(info_.dim) * 4 + 8 + 8 + 4 + 4;
}

static constexpr uint32_t kMaxPathLen = 256;

Status ClusterStoreWriter::Open(const std::string& path,
                                uint32_t num_clusters,
                                Dim dim,
                                const RaBitQConfig& rabitq_config) {
    if (file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter already open");
    }

    path_ = path;
    info_.num_clusters = num_clusters;
    info_.dim = dim;
    info_.rabitq_config = rabitq_config;
    info_.lookup_table.resize(num_clusters);
    current_cluster_index_ = 0;
    in_cluster_ = false;
    finalized_ = false;

    file_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    const uint32_t file_version = EffectiveWriterFileVersion();
    WriteVal(file_, kGlobalMagic);
    WriteVal(file_, file_version);
    WriteVal(file_, num_clusters);
    WriteVal(file_, dim);
    WriteVal(file_, rabitq_config.bits);
    WriteVal(file_, rabitq_config.block_size);
    WriteVal(file_, rabitq_config.c_factor);

    header_data_file_path_offset_ = static_cast<uint64_t>(file_.tellp());
    uint32_t zero_path_len = 0;
    WriteVal(file_, zero_path_len);
    char zero_buf[kMaxPathLen] = {0};
    WriteRaw(file_, zero_buf, kMaxPathLen);

    if (!file_.good()) {
        return Status::IOError("Failed to write global header");
    }

    lookup_table_start_ = static_cast<uint64_t>(file_.tellp());
    const uint64_t entry_sz = lookup_entry_size();
    std::vector<uint8_t> zero_entry(entry_sz, 0);
    for (uint32_t i = 0; i < num_clusters; ++i) {
        WriteRaw(file_, zero_entry.data(), entry_sz);
    }

    if (!file_.good()) {
        return Status::IOError("Failed to write lookup table placeholders");
    }

    current_offset_ = static_cast<uint64_t>(file_.tellp());
    PadTo4K(file_, current_offset_);
    return Status::OK();
}

Status ClusterStoreWriter::BeginCluster(uint32_t cluster_id,
                                        uint32_t num_records,
                                        const float* centroid,
                                        float epsilon) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (in_cluster_) {
        return Status::InvalidArgument("Previous cluster not ended");
    }
    if (current_cluster_index_ >= info_.num_clusters) {
        return Status::InvalidArgument("All clusters already written");
    }

    in_cluster_ = true;
    vectors_written_ = false;
    address_written_ = false;

    auto& entry = info_.lookup_table[current_cluster_index_];
    entry.cluster_id = cluster_id;
    entry.num_records = num_records;
    entry.epsilon = epsilon;
    entry.centroid.assign(centroid, centroid + info_.dim);
    block_start_ = current_offset_;
    return Status::OK();
}

Status ClusterStoreWriter::WriteVectors(
    const std::vector<rabitq::RaBitQCode>& codes) {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (vectors_written_) {
        return Status::InvalidArgument("Vectors already written");
    }

    const uint32_t N = static_cast<uint32_t>(codes.size());
    const uint32_t dim = info_.dim;
    const uint32_t num_blocks = (N + 31) / 32;
    const uint32_t packed_size = FastScanPackedSize(dim);
    const uint32_t block_bytes = FastScanBlockSize(dim);

    current_num_fastscan_blocks_ = num_blocks;
    std::vector<uint8_t> packed_buf(packed_size, 0);

    for (uint32_t b = 0; b < num_blocks; ++b) {
        const uint32_t start = b * 32;
        const uint32_t count = std::min(32u, N - start);

        PackSignBitsForFastScan(&codes[start], count, dim, packed_buf.data());
        WriteRaw(file_, packed_buf.data(), packed_size);

        float norms[32] = {0};
        for (uint32_t j = 0; j < count; ++j) {
            norms[j] = codes[start + j].norm;
        }
        WriteRaw(file_, norms, 32 * sizeof(float));

        if (!file_.good()) {
            return Status::IOError("Failed to write FastScan block");
        }
        current_offset_ += block_bytes;
    }

    current_exrabitq_region_offset_ =
        static_cast<uint32_t>(current_offset_ - block_start_);

    if (info_.rabitq_config.bits > 1) {
        const uint32_t sign_bytes =
            PackedSignBytes(dim);
        if (EffectiveWriterFileVersion() >= 11) {
            const uint32_t batch_size = ExRaBitQBatchSize();
            const uint32_t dim_block = ExRaBitQDimBlock();
            const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
            const uint32_t num_batch_blocks = (N + batch_size - 1) / batch_size;
            const uint32_t sign_block_bytes = dim_block / 8;

            for (uint32_t bb = 0; bb < num_batch_blocks; ++bb) {
                const uint32_t start = bb * batch_size;
                const uint32_t valid_count = std::min(batch_size, N - start);
                WriteVal(file_, valid_count);

                for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                    const uint32_t dim_start = db * dim_block;
                    for (uint32_t lane = 0; lane < batch_size; ++lane) {
                        uint8_t abs_buf[64] = {0};
                        if (lane < valid_count) {
                            const auto& code = codes[start + lane];
                            if (code.ex_code.size() != dim ||
                                code.ex_sign_packed.size() != sign_bytes) {
                                return Status::InvalidArgument(
                                    "ExRaBitQ code/sign size mismatch with dim");
                            }
                            const uint32_t copy = std::min(dim_block, dim - dim_start);
                            std::memcpy(abs_buf, code.ex_code.data() + dim_start, copy);
                        }
                        WriteRaw(file_, abs_buf, dim_block);
                    }
                }
                for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                    const uint32_t dim_start = db * dim_block;
                    for (uint32_t lane = 0; lane < batch_size; ++lane) {
                        uint8_t sign_buf[8] = {0};
                        if (lane < valid_count) {
                            const auto& code = codes[start + lane];
                            const uint8_t* src = code.ex_sign_packed.data() + dim_start / 8;
                            std::memcpy(sign_buf, src, sign_block_bytes);
                        }
                        WriteRaw(file_, sign_buf, sign_block_bytes);
                    }
                }
                for (uint32_t lane = 0; lane < batch_size; ++lane) {
                    const float xipnorm =
                        (lane < valid_count) ? codes[start + lane].xipnorm : 0.0f;
                    WriteVal(file_, xipnorm);
                }
                if (!file_.good()) {
                    return Status::IOError("Failed to write ExRaBitQ compact block");
                }
                const uint32_t block_bytes =
                    sizeof(uint32_t) +
                    num_dim_blocks * batch_size * dim_block +
                    num_dim_blocks * batch_size * sign_block_bytes +
                    batch_size * sizeof(float);
                current_offset_ += block_bytes;
            }
        } else {
            for (const auto& code : codes) {
                if (code.ex_code.size() != dim ||
                    code.ex_sign_packed.size() != sign_bytes) {
                    return Status::InvalidArgument(
                        "ExRaBitQ code/sign size mismatch with dim");
                }
                WriteRaw(file_, code.ex_code.data(), dim);
                WriteRaw(file_, code.ex_sign_packed.data(), sign_bytes);
                WriteVal(file_, code.xipnorm);
                if (!file_.good()) {
                    return Status::IOError("Failed to write ExRaBitQ entry");
                }
                current_offset_ += dim + sign_bytes + sizeof(float);
            }
        }
    }

    vectors_written_ = true;
    return Status::OK();
}

Status ClusterStoreWriter::WriteAddressBlocks(
    const EncodedAddressColumn& column) {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (!vectors_written_) {
        return Status::InvalidArgument("Must write vectors before address blocks");
    }
    if (address_written_) {
        return Status::InvalidArgument("Address blocks already written");
    }

    const auto& entry = info_.lookup_table[current_cluster_index_];
    if (column.total_records != entry.num_records) {
        return Status::InvalidArgument("Address column record count does not match cluster");
    }
    if (column.format == AddressFormat::V1Packed) {
        if (column.blocks.size() != column.layout.num_address_blocks) {
            return Status::InvalidArgument("Address column block count does not match layout");
        }
        if (entry.num_records > 0 && column.layout.num_address_blocks == 0) {
            return Status::InvalidArgument("Non-empty cluster requires address blocks");
        }
    } else {
        if (column.raw_entries.size() != entry.num_records) {
            return Status::InvalidArgument("Raw address table entry count mismatch");
        }
    }

    current_address_column_ = column;
    if (current_address_column_.format == AddressFormat::V1Packed) {
        std::vector<AddressEntry> decoded;
        VDB_RETURN_IF_ERROR(AddressColumn::Decode(current_address_column_, decoded));
        current_address_column_ = AddressColumn::EncodeRawTableV2(
            decoded, current_address_column_.layout.page_size);
    }

    const size_t bytes =
        current_address_column_.raw_entries.size() * sizeof(RawAddressEntryV2);
    if (bytes > 0) {
        file_.write(reinterpret_cast<const char*>(current_address_column_.raw_entries.data()),
                    static_cast<std::streamsize>(bytes));
        if (!file_.good()) {
            return Status::IOError("Failed to write raw address table");
        }
        current_offset_ += bytes;
    }

    address_written_ = true;
    return Status::OK();
}

Status ClusterStoreWriter::EndCluster() {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (!address_written_) {
        return Status::InvalidArgument("Must write address blocks before EndCluster");
    }

    const uint64_t trailer_start = current_offset_;
    uint32_t ex_region_bytes = 0;
    if (info_.rabitq_config.bits > 1) {
        if (EffectiveWriterFileVersion() >= 11) {
            const uint32_t batch_size = ExRaBitQBatchSize();
            const uint32_t dim_block = ExRaBitQDimBlock();
            const uint32_t num_dim_blocks = (info_.dim + dim_block - 1) / dim_block;
            const uint32_t sign_block_bytes = dim_block / 8;
            const uint32_t batch_block_size =
                sizeof(uint32_t) +
                num_dim_blocks * batch_size * dim_block +
                num_dim_blocks * batch_size * sign_block_bytes +
                batch_size * sizeof(float);
            ex_region_bytes =
                ((info_.lookup_table[current_cluster_index_].num_records + batch_size - 1) /
                 batch_size) *
                batch_block_size;
        } else {
            ex_region_bytes =
                info_.lookup_table[current_cluster_index_].num_records *
                (info_.dim + PackedSignBytes(info_.dim) + sizeof(float));
        }
    }
    const uint32_t address_entry_size = sizeof(RawAddressEntryV2);
    const uint32_t num_entries =
        static_cast<uint32_t>(current_address_column_.raw_entries.size());
    const uint32_t address_payload_bytes = num_entries * address_entry_size;
    const uint32_t address_payload_offset =
        current_exrabitq_region_offset_ + ex_region_bytes;

    WriteVal(file_, kAddressFormatV2);
    WriteVal(file_, current_address_column_.layout.page_size);
    WriteVal(file_, address_entry_size);
    WriteVal(file_, num_entries);
    WriteVal(file_, address_payload_offset);
    WriteVal(file_, address_payload_bytes);

    const uint64_t after_blocks_pos = static_cast<uint64_t>(file_.tellp());
    const uint32_t mini_trailer_size =
        static_cast<uint32_t>((after_blocks_pos + 8) - trailer_start);
    WriteVal(file_, mini_trailer_size);
    WriteVal(file_, kBlockMagic);

    if (!file_.good()) {
        return Status::IOError("Failed to write block mini-trailer");
    }

    current_offset_ = static_cast<uint64_t>(file_.tellp());
    auto& entry = info_.lookup_table[current_cluster_index_];
    entry.block_offset = block_start_;
    entry.block_size = current_offset_ - block_start_;
    entry.num_fastscan_blocks = current_num_fastscan_blocks_;
    entry.exrabitq_region_offset = current_exrabitq_region_offset_;

    const uint64_t entry_offset =
        lookup_table_start_ +
        static_cast<uint64_t>(current_cluster_index_) * lookup_entry_size();
    file_.seekp(static_cast<std::streamoff>(entry_offset));

    WriteVal(file_, entry.cluster_id);
    WriteVal(file_, entry.num_records);
    WriteVal(file_, entry.epsilon);
    file_.write(reinterpret_cast<const char*>(entry.centroid.data()),
                static_cast<std::streamsize>(info_.dim * sizeof(float)));
    WriteVal(file_, entry.block_offset);
    WriteVal(file_, entry.block_size);
    WriteVal(file_, entry.num_fastscan_blocks);
    WriteVal(file_, entry.exrabitq_region_offset);

    if (!file_.good()) {
        return Status::IOError("Failed to patch lookup table entry");
    }

    file_.seekp(static_cast<std::streamoff>(current_offset_));
    PadTo4K(file_, current_offset_);

    current_cluster_index_++;
    in_cluster_ = false;
    vectors_written_ = false;
    address_written_ = false;
    current_address_column_ = EncodedAddressColumn{};
    current_num_fastscan_blocks_ = 0;
    current_exrabitq_region_offset_ = 0;
    return Status::OK();
}

Status ClusterStoreWriter::Finalize(const std::string& data_file_path) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (finalized_) {
        return Status::InvalidArgument("Already finalized");
    }
    if (in_cluster_) {
        return Status::InvalidArgument("Cluster not ended");
    }

    info_.data_file_path = data_file_path;
    file_.seekp(static_cast<std::streamoff>(header_data_file_path_offset_));
    const uint32_t path_len = static_cast<uint32_t>(data_file_path.size());
    if (path_len > kMaxPathLen) {
        return Status::InvalidArgument("data_file_path exceeds max length");
    }
    WriteVal(file_, path_len);
    char path_buf[kMaxPathLen] = {0};
    std::memcpy(path_buf, data_file_path.data(), path_len);
    WriteRaw(file_, path_buf, kMaxPathLen);

    if (!file_.good()) {
        return Status::IOError("Failed to patch data_file_path");
    }

    file_.flush();
    file_.close();
    finalized_ = true;
    return Status::OK();
}

ClusterStoreReader::ClusterStoreReader() = default;

ClusterStoreReader::~ClusterStoreReader() {
    Close();
}

ClusterStoreReader::ClusterStoreReader(ClusterStoreReader&& other) noexcept
    : fd_(other.fd_),
      info_(std::move(other.info_)),
      cluster_index_(std::move(other.cluster_index_)),
      loaded_clusters_(std::move(other.loaded_clusters_)),
      resident_file_buffer_(std::move(other.resident_file_buffer_)),
      resident_clusters_(std::move(other.resident_clusters_)),
      resident_parsed_clusters_(std::move(other.resident_parsed_clusters_)),
      resident_preload_ready_(other.resident_preload_ready_),
      resident_preload_bytes_(other.resident_preload_bytes_),
      resident_cluster_mem_bytes_(other.resident_cluster_mem_bytes_),
      resident_preload_time_ms_(other.resident_preload_time_ms_),
      resident_parallel_view_bytes_(other.resident_parallel_view_bytes_),
      resident_parallel_view_build_ms_(other.resident_parallel_view_build_ms_) {
    other.fd_ = -1;
    other.resident_preload_ready_ = false;
    other.resident_preload_bytes_ = 0;
    other.resident_cluster_mem_bytes_ = 0;
    other.resident_preload_time_ms_ = 0.0;
    other.resident_parallel_view_bytes_ = 0;
    other.resident_parallel_view_build_ms_ = 0.0;
}

ClusterStoreReader& ClusterStoreReader::operator=(
    ClusterStoreReader&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        info_ = std::move(other.info_);
        cluster_index_ = std::move(other.cluster_index_);
        loaded_clusters_ = std::move(other.loaded_clusters_);
        resident_file_buffer_ = std::move(other.resident_file_buffer_);
        resident_clusters_ = std::move(other.resident_clusters_);
        resident_parsed_clusters_ = std::move(other.resident_parsed_clusters_);
        resident_preload_ready_ = other.resident_preload_ready_;
        resident_preload_bytes_ = other.resident_preload_bytes_;
        resident_cluster_mem_bytes_ = other.resident_cluster_mem_bytes_;
        resident_preload_time_ms_ = other.resident_preload_time_ms_;
        resident_parallel_view_bytes_ = other.resident_parallel_view_bytes_;
        resident_parallel_view_build_ms_ = other.resident_parallel_view_build_ms_;
        other.fd_ = -1;
        other.resident_preload_ready_ = false;
        other.resident_preload_bytes_ = 0;
        other.resident_cluster_mem_bytes_ = 0;
        other.resident_preload_time_ms_ = 0.0;
        other.resident_parallel_view_bytes_ = 0;
        other.resident_parallel_view_build_ms_ = 0.0;
    }
    return *this;
}

void ClusterStoreReader::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    loaded_clusters_.clear();
    resident_clusters_.clear();
    resident_parsed_clusters_.clear();
    resident_file_buffer_.clear();
    resident_preload_ready_ = false;
    resident_preload_bytes_ = 0;
    resident_cluster_mem_bytes_ = 0;
    resident_preload_time_ms_ = 0.0;
    resident_parallel_view_bytes_ = 0;
    resident_parallel_view_build_ms_ = 0.0;
    cluster_index_.clear();
    file_version_ = 0;
}

Status ClusterStoreReader::Open(const std::string& path, bool use_direct_io) {
    if (fd_ >= 0) {
        return Status::InvalidArgument("ClusterStoreReader already open");
    }

    int flags = O_RDONLY;
    if (use_direct_io) flags |= O_DIRECT;
    fd_ = ::open(path.c_str(), flags);
    if (fd_ < 0) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    {
        uint8_t* hdr_buf = static_cast<uint8_t*>(
            std::aligned_alloc(kAlignSize, kAlignSize));
        if (!hdr_buf) {
            Close();
            return Status::IOError("aligned_alloc failed for header");
        }
        const ssize_t n = ::pread(fd_, hdr_buf, kAlignSize, 0);
        if (n < 285) {
            std::free(hdr_buf);
            Close();
            return Status::IOError("Failed to read header");
        }

        const uint8_t* p = hdr_buf;
        uint32_t magic = 0;
        uint32_t version = 0;
        std::memcpy(&magic, p, 4);
        p += 4;
        if (magic != kGlobalMagic) {
            std::free(hdr_buf);
            Close();
            return Status::Corruption("Invalid ClusterStore magic");
        }
        std::memcpy(&version, p, 4);
        p += 4;
        if (version != kFileVersion && version != kFileVersionV10 && version != kFileVersionV9 &&
            version != kFileVersionV8 &&
            version != kFileVersionV7) {
            std::free(hdr_buf);
            Close();
            return Status::NotSupported(
                "Unsupported ClusterStore version: " + std::to_string(version));
        }
        file_version_ = version;

        std::memcpy(&info_.num_clusters, p, 4);
        p += 4;
        std::memcpy(&info_.dim, p, 4);
        p += 4;
        std::memcpy(&info_.rabitq_config.bits, p, 1);
        p += 1;
        std::memcpy(&info_.rabitq_config.block_size, p, 4);
        p += 4;
        std::memcpy(&info_.rabitq_config.c_factor, p, 4);
        p += 4;

        uint32_t path_len = 0;
        std::memcpy(&path_len, p, 4);
        p += 4;
        if (path_len > kMaxPathLen) {
            std::free(hdr_buf);
            Close();
            return Status::Corruption("data_file_path length exceeds max");
        }
        info_.data_file_path.assign(reinterpret_cast<const char*>(p), path_len);
        p += kMaxPathLen;

        off_t pos = static_cast<off_t>(p - hdr_buf);
        std::free(hdr_buf);

        info_.lookup_table.resize(info_.num_clusters);
        cluster_index_.clear();

        const uint64_t entry_sz =
            4 + 4 + 4 + static_cast<uint64_t>(info_.dim) * 4 + 8 + 8 + 4 + 4;
        const uint64_t total_lookup_size =
            static_cast<uint64_t>(info_.num_clusters) * entry_sz;
        const uint64_t aligned_lookup_size = RoundUp4K(total_lookup_size);
        uint8_t* lookup_raw = static_cast<uint8_t*>(
            std::aligned_alloc(kAlignSize, aligned_lookup_size));
        if (!lookup_raw) {
            Close();
            return Status::IOError("aligned_alloc failed for lookup");
        }

        const off_t aligned_pos = pos & ~(static_cast<off_t>(kAlignSize) - 1);
        const off_t pos_delta = pos - aligned_pos;
        const uint64_t aligned_read =
            RoundUp4K(total_lookup_size + static_cast<uint64_t>(pos_delta));
        uint8_t* aligned_read_buf = static_cast<uint8_t*>(
            std::aligned_alloc(kAlignSize, aligned_read));
        if (!aligned_read_buf) {
            std::free(lookup_raw);
            Close();
            return Status::IOError("aligned_alloc failed");
        }
        const ssize_t nr = ::pread(fd_, aligned_read_buf, aligned_read, aligned_pos);
        if (nr < static_cast<ssize_t>(total_lookup_size + pos_delta)) {
            std::free(aligned_read_buf);
            std::free(lookup_raw);
            Close();
            return Status::IOError("Failed to bulk read lookup table");
        }
        std::memcpy(lookup_raw, aligned_read_buf + pos_delta, total_lookup_size);
        std::free(aligned_read_buf);

        const uint8_t* ptr = lookup_raw;
        for (uint32_t i = 0; i < info_.num_clusters; ++i) {
            auto& entry = info_.lookup_table[i];
            std::memcpy(&entry.cluster_id, ptr, 4);
            ptr += 4;
            std::memcpy(&entry.num_records, ptr, 4);
            ptr += 4;
            std::memcpy(&entry.epsilon, ptr, 4);
            ptr += 4;
            entry.centroid.resize(info_.dim);
            std::memcpy(entry.centroid.data(), ptr, info_.dim * sizeof(float));
            ptr += info_.dim * sizeof(float);
            std::memcpy(&entry.block_offset, ptr, 8);
            ptr += 8;
            std::memcpy(&entry.block_size, ptr, 8);
            ptr += 8;
            std::memcpy(&entry.num_fastscan_blocks, ptr, 4);
            ptr += 4;
            std::memcpy(&entry.exrabitq_region_offset, ptr, 4);
            ptr += 4;
            cluster_index_[entry.cluster_id] = i;
        }
        std::free(lookup_raw);
        pos += static_cast<off_t>(total_lookup_size);
        if (version != kFileVersionV7) {
            pos = static_cast<off_t>(RoundUp4K(static_cast<uint64_t>(pos)));
        }
    }

    return Status::OK();
}

std::vector<uint32_t> ClusterStoreReader::cluster_ids() const {
    std::vector<uint32_t> ids;
    ids.reserve(info_.lookup_table.size());
    for (const auto& e : info_.lookup_table) {
        ids.push_back(e.cluster_id);
    }
    return ids;
}

uint32_t ClusterStoreReader::GetNumRecords(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return 0;
    return info_.lookup_table[it->second].num_records;
}

const float* ClusterStoreReader::GetCentroid(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return nullptr;
    return info_.lookup_table[it->second].centroid.data();
}

float ClusterStoreReader::GetEpsilon(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return 0.0f;
    return info_.lookup_table[it->second].epsilon;
}

uint64_t ClusterStoreReader::total_records() const {
    uint64_t total = 0;
    for (const auto& e : info_.lookup_table) {
        total += e.num_records;
    }
    return total;
}

Status ClusterStoreReader::EnsureClusterLoaded(uint32_t cluster_id) {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }
    if (loaded_clusters_.count(cluster_id)) {
        return Status::OK();
    }

    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) {
        return Status::InvalidArgument(
            "Cluster " + std::to_string(cluster_id) + " not found");
    }

    const auto& entry = info_.lookup_table[it->second];
    ClusterData data;
    const uint64_t block_end = entry.block_offset + entry.block_size;
    uint32_t mini_trailer_size = 0;
    uint32_t block_magic = 0;

    if (!PreadValue(fd_, static_cast<off_t>(block_end - 8), mini_trailer_size)) {
        return Status::IOError("Failed to read mini_trailer_size");
    }
    if (!PreadValue(fd_, static_cast<off_t>(block_end - 4), block_magic)) {
        return Status::IOError("Failed to read block_magic");
    }
    if (block_magic != kBlockMagic) {
        return Status::Corruption("Invalid block magic");
    }

    const uint64_t trailer_start = block_end - mini_trailer_size;
    std::vector<uint8_t> trailer_buf(mini_trailer_size);
    if (!PreadBytes(fd_, static_cast<off_t>(trailer_start),
                    trailer_buf.data(), mini_trailer_size)) {
        return Status::IOError("Failed to read block mini-trailer");
    }

    size_t tpos = 0;
    auto ReadT = [&](auto& val) -> bool {
        if (tpos + sizeof(val) > trailer_buf.size()) return false;
        std::memcpy(&val, trailer_buf.data() + tpos, sizeof(val));
        tpos += sizeof(val);
        return true;
    };

    uint32_t address_format_version = 0;
    uint32_t v9_page_size = 0;
    uint32_t v9_entry_size = 0;
    uint32_t v9_num_entries = 0;
    uint32_t v9_payload_offset = 0;
    uint32_t v9_payload_bytes = 0;
    uint32_t num_blocks = 0;

    if (file_version_ >= kFileVersionV9) {
        if (!ReadT(address_format_version) ||
            !ReadT(v9_page_size) ||
            !ReadT(v9_entry_size) ||
            !ReadT(v9_num_entries) ||
            !ReadT(v9_payload_offset) ||
            !ReadT(v9_payload_bytes)) {
            return Status::Corruption("Mini-trailer: failed to read V9 address table metadata");
        }
        if (address_format_version != kAddressFormatV2) {
            return Status::Corruption("Unsupported V9 address format");
        }
        if (v9_entry_size != sizeof(RawAddressEntryV2)) {
            return Status::Corruption("Unexpected V9 raw address entry size");
        }
        if (v9_num_entries != entry.num_records) {
            return Status::Corruption("V9 raw address entry count mismatch");
        }
        data.address_format = AddressFormat::V2RawTable;
        data.address_layout.page_size = v9_page_size;
    } else {
        if (!ReadT(data.address_layout.page_size) ||
            !ReadT(data.address_layout.bit_width) ||
            !ReadT(data.address_layout.block_granularity) ||
            !ReadT(data.address_layout.fixed_packed_size) ||
            !ReadT(data.address_layout.last_packed_size) ||
            !ReadT(data.address_layout.num_address_blocks)) {
            return Status::Corruption("Mini-trailer: failed to read shared address layout");
        }

        num_blocks = data.address_layout.num_address_blocks;
        if (entry.num_records == 0) {
            if (num_blocks != 0 || data.address_layout.last_packed_size != 0) {
                return Status::Corruption("Empty cluster has invalid address layout");
            }
        } else if (num_blocks == 0) {
            return Status::Corruption("Non-empty cluster has zero address blocks");
        }

        data.address_blocks.resize(num_blocks);
        for (uint32_t i = 0; i < num_blocks; ++i) {
            if (!ReadT(data.address_blocks[i].base_offset)) {
                return Status::Corruption(
                    "Mini-trailer: failed to parse block " + std::to_string(i));
            }
        }
        data.address_format = AddressFormat::V1Packed;
    }

    uint32_t stored_trailer_size = 0;
    uint32_t stored_block_magic = 0;
    if (!ReadT(stored_trailer_size) || !ReadT(stored_block_magic)) {
        return Status::Corruption("Mini-trailer: failed to read trailer footer");
    }
    if (stored_trailer_size != mini_trailer_size || stored_block_magic != kBlockMagic) {
        return Status::Corruption("Mini-trailer footer mismatch");
    }
    if (tpos != trailer_buf.size()) {
        return Status::Corruption("Mini-trailer has trailing bytes");
    }
    if (data.address_format == AddressFormat::V1Packed &&
        num_blocks > 0 && data.address_layout.block_granularity == 0) {
        return Status::Corruption("Invalid address block granularity");
    }

    data.codes_offset = entry.block_offset;
    const uint32_t region1_size = entry.num_fastscan_blocks * fastscan_block_bytes();
    const uint32_t region2_size =
        (file_version_ >= kFileVersion)
            ? exrabitq_num_batch_blocks(entry.num_records) * exrabitq_batch_block_size()
            : entry.num_records * exrabitq_entry_size();
    data.codes_length = region1_size + region2_size;

    if (data.codes_length > 0) {
        data.codes_buffer.resize(data.codes_length);
        if (!PreadBytes(fd_, static_cast<off_t>(data.codes_offset),
                        data.codes_buffer.data(), data.codes_length)) {
            return Status::IOError("Failed to read codes buffer");
        }
    }

    const uint64_t default_payload_offset = entry.block_offset + data.codes_length;
    if (default_payload_offset > block_end || data.codes_length > entry.block_size) {
        return Status::Corruption("Cluster block shorter than RaBitQ code region");
    }

    uint64_t expected_payload_bytes = 0;
    uint64_t address_payload_offset = default_payload_offset;
    if (data.address_format == AddressFormat::V2RawTable) {
        address_payload_offset = entry.block_offset + static_cast<uint64_t>(v9_payload_offset);
        expected_payload_bytes = v9_payload_bytes;
    } else if (num_blocks > 0) {
        expected_payload_bytes =
            static_cast<uint64_t>(num_blocks - 1) * data.address_layout.fixed_packed_size +
            data.address_layout.last_packed_size;
    }
    const uint64_t payload_and_trailer = entry.block_size - data.codes_length;
    if (payload_and_trailer < mini_trailer_size) {
        return Status::Corruption("Cluster block shorter than trailer");
    }
    const uint64_t actual_payload_bytes = payload_and_trailer - mini_trailer_size;
    if (actual_payload_bytes != expected_payload_bytes) {
        return Status::Corruption("Address payload size mismatch");
    }

    if (data.address_format == AddressFormat::V2RawTable) {
        data.raw_addresses_v2.resize(entry.num_records);
        if (v9_payload_bytes > 0 &&
            !PreadBytes(fd_, static_cast<off_t>(address_payload_offset),
                        data.raw_addresses_v2.data(), v9_payload_bytes)) {
            return Status::IOError("Failed to read raw address table");
        }
        EncodedAddressColumn raw_column;
        raw_column.format = AddressFormat::V2RawTable;
        raw_column.layout.page_size = v9_page_size;
        raw_column.total_records = entry.num_records;
        raw_column.raw_entries = data.raw_addresses_v2;
        VDB_RETURN_IF_ERROR(AddressColumn::DecodeRawTableV2(
            raw_column, data.decoded_addresses));
    } else {
        uint64_t addr_read_offset = address_payload_offset;
        for (uint32_t i = 0; i < num_blocks; ++i) {
            auto& block = data.address_blocks[i];
            const uint32_t packed_size =
                AddressColumn::BlockPackedSize(data.address_layout, i);
            block.packed.resize(packed_size);
            if (packed_size == 0) continue;

            if (!PreadBytes(fd_, static_cast<off_t>(addr_read_offset),
                            block.packed.data(), packed_size)) {
                return Status::IOError("Failed to read address block packed data");
            }
            addr_read_offset += packed_size;
        }

        VDB_RETURN_IF_ERROR(AddressColumn::DecodeBatchBlocks(
            data.address_layout, data.address_blocks, entry.num_records,
            data.decoded_addresses));
    }

    loaded_clusters_[cluster_id] = std::move(data);
    return Status::OK();
}

std::optional<query::ClusterBlockLocation>
ClusterStoreReader::GetBlockLocation(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return std::nullopt;
    const auto& entry = info_.lookup_table[it->second];
    return query::ClusterBlockLocation{entry.block_offset, entry.block_size,
                                       entry.num_records};
}

Status ClusterStoreReader::ParseClusterBlockView(
    uint32_t cluster_id,
    const uint8_t* block_ptr,
    uint64_t block_size,
    query::ParsedCluster& out) const {
    auto idx_it = cluster_index_.find(cluster_id);
    if (idx_it == cluster_index_.end()) {
        return Status::InvalidArgument(
            "Cluster " + std::to_string(cluster_id) + " not found");
    }
    const auto& entry = info_.lookup_table[idx_it->second];

    if (block_ptr == nullptr) {
        return Status::InvalidArgument("block_ptr is null");
    }
    if (block_size != entry.block_size) {
        return Status::InvalidArgument("block_size mismatch");
    }
    if (block_size < 8) {
        return Status::Corruption("Block too small for trailer footer");
    }

    uint32_t mini_trailer_size = 0;
    uint32_t block_magic = 0;
    std::memcpy(&mini_trailer_size, block_ptr + block_size - 8, sizeof(uint32_t));
    std::memcpy(&block_magic, block_ptr + block_size - 4, sizeof(uint32_t));
    if (block_magic != kBlockMagic) {
        return Status::Corruption("Invalid block magic");
    }
    if (mini_trailer_size > block_size) {
        return Status::Corruption("Mini-trailer size exceeds block");
    }

    const uint8_t* trailer_ptr = block_ptr + block_size - mini_trailer_size;
    size_t tpos = 0;
    auto ReadT = [&](auto& val) -> bool {
        if (tpos + sizeof(val) > mini_trailer_size) return false;
        std::memcpy(&val, trailer_ptr + tpos, sizeof(val));
        tpos += sizeof(val);
        return true;
    };

    AddressColumnLayout address_layout;
    std::vector<AddressBlock> address_blocks;
    const RawAddressEntryV2* raw_addresses = nullptr;
    uint32_t address_page_size = 0;
    bool addresses_are_raw_v2 = false;
    uint32_t num_blocks = 0;
    uint32_t v9_payload_offset = 0;
    uint32_t v9_payload_bytes = 0;

    if (file_version_ >= kFileVersionV9) {
        uint32_t address_format_version = 0;
        uint32_t address_entry_size = 0;
        uint32_t num_entries = 0;
        if (!ReadT(address_format_version) ||
            !ReadT(address_page_size) ||
            !ReadT(address_entry_size) ||
            !ReadT(num_entries) ||
            !ReadT(v9_payload_offset) ||
            !ReadT(v9_payload_bytes)) {
            return Status::Corruption("Mini-trailer: failed to read V9 address metadata");
        }
        if (address_format_version != kAddressFormatV2) {
            return Status::Corruption("Unsupported V9 address format");
        }
        if (address_entry_size != sizeof(RawAddressEntryV2)) {
            return Status::Corruption("Unexpected V9 raw address entry size");
        }
        if (num_entries != entry.num_records) {
            return Status::Corruption("V9 raw address entry count mismatch");
        }
        addresses_are_raw_v2 = true;
    } else {
        if (!ReadT(address_layout.page_size) ||
            !ReadT(address_layout.bit_width) ||
            !ReadT(address_layout.block_granularity) ||
            !ReadT(address_layout.fixed_packed_size) ||
            !ReadT(address_layout.last_packed_size) ||
            !ReadT(address_layout.num_address_blocks)) {
            return Status::Corruption("Mini-trailer: failed to read shared address layout");
        }

        num_blocks = address_layout.num_address_blocks;
        if (entry.num_records == 0) {
            if (num_blocks != 0 || address_layout.last_packed_size != 0) {
                return Status::Corruption("Empty cluster has invalid address layout");
            }
        } else if (num_blocks == 0) {
            return Status::Corruption("Non-empty cluster has zero address blocks");
        }

        address_blocks.resize(num_blocks);
        for (uint32_t i = 0; i < num_blocks; ++i) {
            if (!ReadT(address_blocks[i].base_offset)) {
                return Status::Corruption(
                    "Mini-trailer: failed to parse block " + std::to_string(i));
            }
        }
    }

    uint32_t stored_trailer_size = 0;
    uint32_t stored_block_magic = 0;
    if (!ReadT(stored_trailer_size) || !ReadT(stored_block_magic)) {
        return Status::Corruption("Mini-trailer: failed to read trailer footer");
    }
    if (stored_trailer_size != mini_trailer_size || stored_block_magic != kBlockMagic) {
        return Status::Corruption("Mini-trailer footer mismatch");
    }
    if (tpos != mini_trailer_size) {
        return Status::Corruption("Mini-trailer has trailing bytes");
    }
    if (!addresses_are_raw_v2 &&
        num_blocks > 0 && address_layout.block_granularity == 0) {
        return Status::Corruption("Invalid address block granularity");
    }

    const uint32_t fb_size = fastscan_block_bytes();
    const uint32_t region1_size = entry.num_fastscan_blocks * fb_size;
    const uint32_t ex_entry_size = exrabitq_entry_size();
    const uint32_t region2_size =
        (file_version_ >= kFileVersion)
            ? exrabitq_num_batch_blocks(entry.num_records) * exrabitq_batch_block_size()
            : entry.num_records * ex_entry_size;
    const uint32_t codes_length = region1_size + region2_size;
    if (codes_length > block_size) {
        return Status::Corruption("Cluster block shorter than code regions");
    }

    uint64_t expected_payload_bytes = 0;
    if (addresses_are_raw_v2) {
        expected_payload_bytes = v9_payload_bytes;
    } else if (num_blocks > 0) {
        expected_payload_bytes =
            static_cast<uint64_t>(num_blocks - 1) * address_layout.fixed_packed_size +
            address_layout.last_packed_size;
    }
    const uint64_t payload_and_trailer = block_size - codes_length;
    if (payload_and_trailer < mini_trailer_size) {
        return Status::Corruption("Cluster block shorter than trailer");
    }
    const uint64_t actual_payload_bytes = payload_and_trailer - mini_trailer_size;
    if (actual_payload_bytes != expected_payload_bytes) {
        return Status::Corruption("Address payload size mismatch");
    }

    std::vector<AddressEntry> decoded;
    if (addresses_are_raw_v2) {
        if (static_cast<uint64_t>(v9_payload_offset) + v9_payload_bytes >
            block_size - mini_trailer_size) {
            return Status::Corruption("V9 raw address payload exceeds block");
        }
        raw_addresses = reinterpret_cast<const RawAddressEntryV2*>(
            block_ptr + v9_payload_offset);
        decoded.resize(entry.num_records);
        for (uint32_t i = 0; i < entry.num_records; ++i) {
            decoded[i] = AddressColumn::DecodeRawEntryV2(raw_addresses[i],
                                                         address_page_size);
        }
    } else {
        uint64_t addr_offset = codes_length;
        for (uint32_t i = 0; i < num_blocks; ++i) {
            auto& block = address_blocks[i];
            const uint32_t packed_size =
                AddressColumn::BlockPackedSize(address_layout, i);
            if (packed_size > 0) {
                block.packed.assign(block_ptr + addr_offset,
                                    block_ptr + addr_offset + packed_size);
            }
            addr_offset += packed_size;
        }

        VDB_RETURN_IF_ERROR(AddressColumn::DecodeBatchBlocks(
            address_layout, address_blocks, entry.num_records, decoded));
    }

    out.fastscan_blocks = block_ptr;
    out.fastscan_block_size = fb_size;
    out.num_fastscan_blocks = entry.num_fastscan_blocks;
    out.exrabitq_entries =
        (ex_entry_size > 0) ? block_ptr + region1_size : nullptr;
    out.exrabitq_entry_size = ex_entry_size;
    out.exrabitq_sign_bytes = exrabitq_sign_bytes();
    out.exrabitq_sign_packed = (file_version_ >= kFileVersionV10);
    out.exrabitq_storage_version = file_version_;
    out.exrabitq_batch_blocks =
        (file_version_ >= kFileVersion && info_.rabitq_config.bits > 1)
            ? block_ptr + region1_size
            : nullptr;
    out.exrabitq_batch_block_size = exrabitq_batch_block_size();
    out.exrabitq_batch_size = exrabitq_batch_size();
    out.exrabitq_dim_block = exrabitq_dim_block();
    out.exrabitq_num_dim_blocks = exrabitq_dim_block_count();
    out.exrabitq_num_batch_blocks = exrabitq_num_batch_blocks(entry.num_records);
    out.num_records = entry.num_records;
    out.epsilon = entry.epsilon;
    out.raw_addresses = raw_addresses;
    out.address_page_size = address_page_size;
    out.addresses_are_raw_v2 = addresses_are_raw_v2;
    out.decoded_addresses = std::move(decoded);
    out.codes_start = out.fastscan_blocks;
    out.code_entry_size = 0;
    return Status::OK();
}

Status ClusterStoreReader::ParseClusterBlock(
    uint32_t cluster_id,
    query::AlignedBufPtr block_buf,
    uint64_t block_size,
    query::ParsedCluster& out) {
    VDB_RETURN_IF_ERROR(
        ParseClusterBlockView(cluster_id, block_buf.get(), block_size, out));
    out.block_buf = std::move(block_buf);
    return Status::OK();
}

Status ClusterStoreReader::PreloadAllClusters() {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }
    if (resident_preload_ready_) {
        return Status::OK();
    }

    auto t0 = std::chrono::steady_clock::now();
    const off_t file_size = ::lseek(fd_, 0, SEEK_END);
    if (file_size < 0) {
        return Status::IOError("Failed to determine .clu file size");
    }

    resident_file_buffer_.resize(static_cast<size_t>(file_size));
    if (file_size > 0 &&
        !PreadBytes(fd_, 0, resident_file_buffer_.data(),
                    resident_file_buffer_.size())) {
        resident_file_buffer_.clear();
        return Status::IOError("Failed to preload .clu file");
    }

    std::map<uint32_t, ResidentClusterView> resident_clusters;
    std::map<uint32_t, query::ParsedCluster> resident_parsed_clusters;
    uint64_t resident_cluster_mem_bytes = 0;
    uint64_t resident_parallel_view_bytes = 0;
    auto parallel_build_start = std::chrono::steady_clock::now();
    for (const auto& entry : info_.lookup_table) {
        if (entry.block_offset + entry.block_size >
            static_cast<uint64_t>(resident_file_buffer_.size())) {
            resident_file_buffer_.clear();
            return Status::Corruption("Cluster block exceeds resident .clu buffer");
        }

        query::ParsedCluster parsed;
        const uint8_t* block_ptr =
            resident_file_buffer_.data() + static_cast<size_t>(entry.block_offset);
        VDB_RETURN_IF_ERROR(
            ParseClusterBlockView(entry.cluster_id, block_ptr, entry.block_size, parsed));

        ResidentClusterView view;
        view.fastscan_blocks = parsed.fastscan_blocks;
        view.fastscan_block_size = parsed.fastscan_block_size;
        view.num_fastscan_blocks = parsed.num_fastscan_blocks;
        view.exrabitq_entries = parsed.exrabitq_entries;
        view.exrabitq_entry_size = parsed.exrabitq_entry_size;
        view.exrabitq_sign_bytes = parsed.exrabitq_sign_bytes;
        view.exrabitq_sign_packed = parsed.exrabitq_sign_packed;
        view.exrabitq_storage_version = parsed.exrabitq_storage_version;
        view.exrabitq_batch_blocks = parsed.exrabitq_batch_blocks;
        view.exrabitq_batch_block_size = parsed.exrabitq_batch_block_size;
        view.exrabitq_batch_size = parsed.exrabitq_batch_size;
        view.exrabitq_dim_block = parsed.exrabitq_dim_block;
        view.exrabitq_num_dim_blocks = parsed.exrabitq_num_dim_blocks;
        view.exrabitq_num_batch_blocks = parsed.exrabitq_num_batch_blocks;
        view.num_records = parsed.num_records;
        view.epsilon = parsed.epsilon;
        view.raw_addresses = parsed.raw_addresses;
        view.address_page_size = parsed.address_page_size;
        view.addresses_are_raw_v2 = parsed.addresses_are_raw_v2;
        view.decoded_addresses = parsed.decoded_addresses;
        BuildResidentParallelStage2View(parsed, &view);
        resident_parallel_view_bytes +=
            static_cast<uint64_t>(view.exrabitq_parallel_abs_blocks_storage.size());
        resident_parallel_view_bytes +=
            static_cast<uint64_t>(view.exrabitq_parallel_sign_words_storage.size()) *
            sizeof(uint16_t);
        auto inserted = resident_clusters.emplace(entry.cluster_id, std::move(view));
        resident_cluster_mem_bytes += entry.block_size;
        resident_cluster_mem_bytes +=
            static_cast<uint64_t>(parsed.decoded_addresses.size()) *
            sizeof(AddressEntry);
        resident_cluster_mem_bytes +=
            static_cast<uint64_t>(inserted.first->second.exrabitq_parallel_abs_blocks_storage.size());
        resident_cluster_mem_bytes +=
            static_cast<uint64_t>(inserted.first->second.exrabitq_parallel_sign_words_storage.size()) *
            sizeof(uint16_t);
        resident_parsed_clusters.emplace(
            entry.cluster_id, inserted.first->second.ToParsedCluster());
    }

    resident_clusters_ = std::move(resident_clusters);
    resident_parsed_clusters_ = std::move(resident_parsed_clusters);
    resident_preload_ready_ = true;
    resident_preload_bytes_ = static_cast<uint64_t>(resident_file_buffer_.size());
    resident_cluster_mem_bytes_ = resident_cluster_mem_bytes;
    resident_preload_time_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    resident_parallel_view_bytes_ = resident_parallel_view_bytes;
    resident_parallel_view_build_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - parallel_build_start).count();
    return Status::OK();
}

const ClusterStoreReader::ResidentClusterView*
ClusterStoreReader::GetResidentClusterView(uint32_t cluster_id) const {
    auto it = resident_clusters_.find(cluster_id);
    if (it == resident_clusters_.end()) return nullptr;
    return &it->second;
}

const query::ParsedCluster* ClusterStoreReader::GetResidentParsedCluster(
    uint32_t cluster_id) const {
    auto it = resident_parsed_clusters_.find(cluster_id);
    if (it == resident_parsed_clusters_.end()) return nullptr;
    return &it->second;
}

AddressEntry ClusterStoreReader::GetAddress(uint32_t cluster_id,
                                            uint32_t record_idx) const {
    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end()) {
        return AddressEntry{0, 0};
    }
    if (it->second.address_format == AddressFormat::V2RawTable) {
        if (record_idx >= it->second.raw_addresses_v2.size()) {
            return AddressEntry{0, 0};
        }
        return AddressColumn::DecodeRawEntryV2(
            it->second.raw_addresses_v2[record_idx],
            it->second.address_layout.page_size);
    }
    if (record_idx >= it->second.decoded_addresses.size()) {
        return AddressEntry{0, 0};
    }
    return it->second.decoded_addresses[record_idx];
}

std::vector<AddressEntry> ClusterStoreReader::GetAddresses(
    uint32_t cluster_id,
    const std::vector<uint32_t>& indices) const {
    std::vector<AddressEntry> results(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        results[i] = GetAddress(cluster_id, indices[i]);
    }
    return results;
}

Status ClusterStoreReader::LoadCode(uint32_t cluster_id,
                                    uint32_t record_idx,
                                    std::vector<uint64_t>& out_code) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end()) {
        return Status::InvalidArgument(
            "Cluster not loaded — call EnsureClusterLoaded first");
    }

    auto cid_it = cluster_index_.find(cluster_id);
    if (cid_it == cluster_index_.end()) {
        return Status::InvalidArgument("Cluster not found");
    }
    const auto& entry = info_.lookup_table[cid_it->second];
    if (record_idx >= entry.num_records) {
        return Status::InvalidArgument("Record index out of range");
    }

    const uint32_t nwords = num_code_words();
    const uint32_t dim = info_.dim;
    const uint32_t block_idx = record_idx / 32;
    const uint32_t vec_in_block = record_idx % 32;
    const uint32_t fb_bytes = fastscan_block_bytes();
    const uint8_t* block_data =
        it->second.codes_buffer.data() + static_cast<size_t>(block_idx) * fb_bytes;

    out_code.resize(nwords);
    UnpackSignBitsFromFastScan(block_data, vec_in_block, dim, out_code.data());
    return Status::OK();
}

Status ClusterStoreReader::LoadCodes(
    uint32_t cluster_id,
    const std::vector<uint32_t>& indices,
    std::vector<rabitq::RaBitQCode>& out_codes) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    out_codes.resize(indices.size());
    const uint32_t dim = info_.dim;
    const uint32_t fb_bytes = fastscan_block_bytes();
    const uint32_t packed_sz = fastscan_packed_size();
    const uint32_t ex_entry_sz = exrabitq_entry_size();

    auto cid_it = cluster_index_.find(cluster_id);
    if (cid_it == cluster_index_.end()) {
        return Status::InvalidArgument("Cluster not found");
    }
    const auto& entry = info_.lookup_table[cid_it->second];

    for (size_t i = 0; i < indices.size(); ++i) {
        std::vector<uint64_t> code;
        VDB_RETURN_IF_ERROR(LoadCode(cluster_id, indices[i], code));
        out_codes[i].code = std::move(code);

        const auto& cluster_data = loaded_clusters_.at(cluster_id);
        const uint32_t block_idx = indices[i] / 32;
        const uint32_t vec_in_block = indices[i] % 32;
        const uint8_t* block_data =
            cluster_data.codes_buffer.data() + static_cast<size_t>(block_idx) * fb_bytes;
        const float* norms = reinterpret_cast<const float*>(block_data + packed_sz);
        out_codes[i].norm = norms[vec_in_block];

        out_codes[i].sum_x = 0;
        for (auto w : out_codes[i].code) {
            out_codes[i].sum_x += __builtin_popcountll(w);
        }

        if (file_version_ >= kFileVersion || ex_entry_sz > 0) {
            const uint32_t region1_size = entry.num_fastscan_blocks * fb_bytes;
            const uint32_t sign_bytes = exrabitq_sign_bytes();
            out_codes[i].ex_sign_packed.resize(PackedSignBytes(dim));
            if (file_version_ >= kFileVersion) {
                const uint32_t batch_size = exrabitq_batch_size();
                const uint32_t dim_block = exrabitq_dim_block();
                const uint32_t num_dim_blocks = exrabitq_dim_block_count();
                const uint32_t sign_block_bytes = dim_block / 8;
                const uint32_t batch_block_size = exrabitq_batch_block_size();
                const uint32_t block_id = indices[i] / batch_size;
                const uint32_t lane_id = indices[i] % batch_size;
                const uint8_t* block =
                    cluster_data.codes_buffer.data() + region1_size +
                    static_cast<size_t>(block_id) * batch_block_size;
                const uint8_t* payload = block + sizeof(uint32_t);
                const uint8_t* abs_base = payload;
                const uint8_t* sign_base =
                    abs_base + num_dim_blocks * batch_size * dim_block;
                out_codes[i].ex_code.resize(dim);
                std::fill(out_codes[i].ex_sign_packed.begin(),
                          out_codes[i].ex_sign_packed.end(), 0);
                for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                    const uint32_t dim_start = db * dim_block;
                    const uint32_t copy = std::min(dim_block, dim - dim_start);
                    const uint8_t* abs_ptr =
                        abs_base + static_cast<size_t>(db) * batch_size * dim_block +
                        lane_id * dim_block;
                    std::memcpy(out_codes[i].ex_code.data() + dim_start, abs_ptr, copy);
                    const uint8_t* sign_ptr =
                        sign_base + static_cast<size_t>(db) * batch_size * sign_block_bytes +
                        lane_id * sign_block_bytes;
                    std::memcpy(out_codes[i].ex_sign_packed.data() + dim_start / 8,
                                sign_ptr, sign_block_bytes);
                }
                const float* xipnorms = reinterpret_cast<const float*>(
                    block + batch_block_size - batch_size * sizeof(float));
                out_codes[i].xipnorm = xipnorms[lane_id];
            } else {
                const uint8_t* ex_ptr = cluster_data.codes_buffer.data() + region1_size +
                                        static_cast<size_t>(indices[i]) * ex_entry_sz;
                out_codes[i].ex_code.assign(ex_ptr, ex_ptr + dim);
                if (file_version_ >= kFileVersionV10) {
                    out_codes[i].ex_sign_packed.assign(ex_ptr + dim,
                                                       ex_ptr + dim + sign_bytes);
                } else {
                    std::fill(out_codes[i].ex_sign_packed.begin(),
                              out_codes[i].ex_sign_packed.end(), 0);
                    const uint8_t* legacy_sign = ex_ptr + dim;
                    for (uint32_t d = 0; d < dim; ++d) {
                        if (legacy_sign[d]) {
                            out_codes[i].ex_sign_packed[d / 8] |=
                                static_cast<uint8_t>(1u << (d % 8));
                        }
                    }
                }
                std::memcpy(&out_codes[i].xipnorm, ex_ptr + dim + sign_bytes,
                            sizeof(float));
            }
        }
    }

    return Status::OK();
}

const uint8_t* ClusterStoreReader::GetCodePtr(uint32_t cluster_id,
                                              uint32_t record_idx) const {
    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end()) return nullptr;

    const uint32_t fb_bytes = fastscan_block_bytes();
    const uint32_t block_idx = record_idx / 32;
    const uint32_t offset = block_idx * fb_bytes;
    if (offset + fb_bytes > it->second.codes_buffer.size()) return nullptr;
    return it->second.codes_buffer.data() + offset;
}

}  // namespace storage
}  // namespace vdb
