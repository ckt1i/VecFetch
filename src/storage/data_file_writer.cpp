#include "vdb/storage/data_file_writer.h"

#include <cstring>

namespace vdb {
namespace storage {

// ============================================================================
// Construction / Destruction
// ============================================================================

DataFileWriter::DataFileWriter() = default;

DataFileWriter::~DataFileWriter() {
    if (file_.is_open() && !finalized_) {
        file_.close();
    }
}

// ============================================================================
// Open
// ============================================================================

Status DataFileWriter::Open(const std::string& path,
                             uint32_t cluster_id,
                             Dim raw_dim,
                             const std::vector<ColumnSchema>& payload_schemas,
                             uint32_t page_size) {
    if (file_.is_open()) {
        return Status::InvalidArgument("DataFileWriter already open");
    }

    path_ = path;
    cluster_id_ = cluster_id;
    dim_ = raw_dim;
    payload_schemas_ = payload_schemas;
    page_size_ = page_size;
    current_offset_ = 0;
    num_records_ = 0;
    finalized_ = false;

    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        return Status::IOError("Failed to open DataFile for writing: " +
                               path);
    }

    return Status::OK();
}

// ============================================================================
// WriteRecord
// ============================================================================

Status DataFileWriter::WriteRecord(const float* vec,
                                    const std::vector<Datum>& payload,
                                    AddressEntry& out_entry) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("DataFileWriter not open");
    }
    if (finalized_) {
        return Status::InvalidArgument("DataFileWriter already finalized");
    }
    if (payload.size() != payload_schemas_.size()) {
        return Status::InvalidArgument(
            "Payload size mismatch: expected " +
            std::to_string(payload_schemas_.size()) + ", got " +
            std::to_string(payload.size()));
    }

    out_entry.offset = current_offset_;

    // Write raw vector: raw_dim × sizeof(float) bytes
    const uint32_t vec_bytes = dim_ * sizeof(float);
    file_.write(reinterpret_cast<const char*>(vec), vec_bytes);
    if (!file_.good()) {
        return Status::IOError("Failed to write vector data");
    }
    current_offset_ += vec_bytes;

    // Write each payload column
    for (size_t i = 0; i < payload.size(); ++i) {
        VDB_RETURN_IF_ERROR(WriteDatum(payload[i], payload_schemas_[i]));
    }

    // Pad to page boundary (if page_size > 1)
    if (page_size_ > 1) {
        uint64_t raw_size = current_offset_ - out_entry.offset;
        uint64_t padded = ((raw_size + page_size_ - 1) / page_size_) * page_size_;
        uint64_t pad_bytes = padded - raw_size;
        if (pad_bytes > 0) {
            // Write zero padding in a stack buffer (max page_size - 1 bytes per record)
            std::vector<uint8_t> padding(pad_bytes, 0);
            file_.write(reinterpret_cast<const char*>(padding.data()),
                        static_cast<std::streamsize>(pad_bytes));
            if (!file_.good()) {
                return Status::IOError("Failed to write page padding");
            }
            current_offset_ += pad_bytes;
        }
    }

    out_entry.size = static_cast<uint32_t>(current_offset_ - out_entry.offset);
    ++num_records_;

    return Status::OK();
}

// ============================================================================
// WriteDatum
// ============================================================================

Status DataFileWriter::WriteDatum(const Datum& datum,
                                   const ColumnSchema& schema) {
    if (DTypeIsFixedWidth(schema.dtype)) {
        // Fixed-width: write raw bytes
        size_t sz = DTypeSize(schema.dtype);
        file_.write(reinterpret_cast<const char*>(datum.fixed_data()), sz);
        if (!file_.good()) {
            return Status::IOError("Failed to write fixed datum");
        }
        current_offset_ += sz;
    } else {
        // Variable-length: length-prefix (uint32_t) + raw bytes
        uint32_t len = static_cast<uint32_t>(datum.var_data.size());
        file_.write(reinterpret_cast<const char*>(&len), sizeof(uint32_t));
        if (!file_.good()) {
            return Status::IOError("Failed to write var-length prefix");
        }
        current_offset_ += sizeof(uint32_t);

        if (len > 0) {
            file_.write(datum.var_data.data(), len);
            if (!file_.good()) {
                return Status::IOError("Failed to write var-length data");
            }
            current_offset_ += len;
        }
    }

    return Status::OK();
}

// ============================================================================
// Finalize
// ============================================================================

Status DataFileWriter::Finalize() {
    if (!file_.is_open()) {
        return Status::InvalidArgument("DataFileWriter not open");
    }
    if (finalized_) {
        return Status::InvalidArgument("DataFileWriter already finalized");
    }

    file_.flush();
    file_.close();
    finalized_ = true;

    return Status::OK();
}

}  // namespace storage
}  // namespace vdb
