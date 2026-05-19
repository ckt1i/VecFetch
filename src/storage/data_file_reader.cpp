#include "vdb/storage/data_file_reader.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace vdb {
namespace storage {

// ============================================================================
// Construction / Destruction
// ============================================================================

DataFileReader::DataFileReader() = default;

DataFileReader::~DataFileReader() {
    Close();
}

DataFileReader::DataFileReader(DataFileReader&& other) noexcept
    : fd_(other.fd_),
      path_(std::move(other.path_)),
      dim_(other.dim_),
      payload_schemas_(std::move(other.payload_schemas_)) {
    other.fd_ = -1;
}

DataFileReader& DataFileReader::operator=(DataFileReader&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        dim_ = other.dim_;
        payload_schemas_ = std::move(other.payload_schemas_);
        other.fd_ = -1;
    }
    return *this;
}

// ============================================================================
// Open / Close
// ============================================================================

Status DataFileReader::Open(const std::string& path,
                             Dim raw_dim,
                             const std::vector<ColumnSchema>& payload_schemas,
                             bool use_direct_io) {
    if (fd_ >= 0) {
        return Status::InvalidArgument("DataFileReader already open");
    }

    int flags = O_RDONLY;
    if (use_direct_io) flags |= O_DIRECT;
    fd_ = ::open(path.c_str(), flags);
    if (fd_ < 0) {
        return Status::IOError("Failed to open DataFile: " + path);
    }

    path_ = path;
    dim_ = raw_dim;
    payload_schemas_ = payload_schemas;

    return Status::OK();
}

void DataFileReader::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ============================================================================
// ReadRaw — generic pread
// ============================================================================

Status DataFileReader::ReadRaw(uint64_t offset, uint32_t length,
                                uint8_t* out_buffer) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("DataFileReader not open");
    }

    uint32_t total_read = 0;
    while (total_read < length) {
        ssize_t n = ::pread(fd_, out_buffer + total_read,
                            length - total_read,
                            static_cast<off_t>(offset + total_read));
        if (n < 0) {
            return Status::IOError("pread failed");
        }
        if (n == 0) {
            return Status::IOError("Unexpected EOF in DataFile");
        }
        total_read += static_cast<uint32_t>(n);
    }

    return Status::OK();
}

// ============================================================================
// ReadVector — read only the vector part of a record
// ============================================================================

Status DataFileReader::ReadVector(const AddressEntry& addr,
                                   float* out_vec) const {
    const uint32_t vec_bytes = dim_ * sizeof(float);
    if (addr.size < vec_bytes) {
        return Status::InvalidArgument("Record too small for vector");
    }

    return ReadRaw(addr.offset, vec_bytes,
                   reinterpret_cast<uint8_t*>(out_vec));
}

// ============================================================================
// ReadRecord — read complete record (vector + payload)
// ============================================================================

Status DataFileReader::ReadRecord(const AddressEntry& addr,
                                   float* out_vec,
                                   std::vector<Datum>& out_payload) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("DataFileReader not open");
    }

    // Read the entire record into a temporary buffer
    std::vector<uint8_t> buf(addr.size);
    VDB_RETURN_IF_ERROR(ReadRaw(addr.offset, addr.size, buf.data()));

    // Extract vector
    const uint32_t vec_bytes = dim_ * sizeof(float);
    if (addr.size < vec_bytes) {
        return Status::InvalidArgument("Record too small for vector");
    }
    std::memcpy(out_vec, buf.data(), vec_bytes);

    // Parse payload columns
    if (!payload_schemas_.empty()) {
        VDB_RETURN_IF_ERROR(ParsePayload(buf.data(), addr.size,
                                          vec_bytes, out_payload));
    } else {
        out_payload.clear();
    }

    return Status::OK();
}

// ============================================================================
// ParsePayload — deserialize payload columns from raw buffer
// ============================================================================

Status DataFileReader::ParsePayload(const uint8_t* buf, uint32_t buf_len,
                                     uint32_t buf_offset,
                                     std::vector<Datum>& out_payload) const {
    out_payload.clear();
    out_payload.reserve(payload_schemas_.size());

    uint32_t offset = buf_offset;

    for (const auto& schema : payload_schemas_) {
        if (DTypeIsFixedWidth(schema.dtype)) {
            size_t sz = DTypeSize(schema.dtype);
            if (offset + sz > buf_len) {
                return Status::IOError("Buffer underflow reading fixed column");
            }

            Datum d;
            d.dtype = schema.dtype;
            std::memset(&d.fixed, 0, sizeof(d.fixed));
            std::memcpy(&d.fixed, buf + offset, sz);
            out_payload.push_back(std::move(d));
            offset += static_cast<uint32_t>(sz);
        } else {
            // Variable-length: uint32_t length prefix + data
            if (offset + sizeof(uint32_t) > buf_len) {
                return Status::IOError(
                    "Buffer underflow reading var-length prefix");
            }

            uint32_t len;
            std::memcpy(&len, buf + offset, sizeof(uint32_t));
            offset += sizeof(uint32_t);

            if (offset + len > buf_len) {
                return Status::IOError(
                    "Buffer underflow reading var-length data");
            }

            Datum d;
            d.dtype = schema.dtype;
            d.var_data.assign(reinterpret_cast<const char*>(buf + offset),
                              len);
            out_payload.push_back(std::move(d));
            offset += len;
        }
    }

    return Status::OK();
}

}  // namespace storage
}  // namespace vdb
