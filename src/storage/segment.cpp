#include "vdb/storage/segment.h"

namespace vdb {
namespace storage {

// ============================================================================
// Construction / Destruction
// ============================================================================

Segment::Segment() = default;

Segment::~Segment() = default;

// ============================================================================
// Open — open the unified cluster.clu + data.dat
// ============================================================================

Status Segment::Open(const std::string& dir,
                     const std::vector<ColumnSchema>& payload_schemas,
                     bool use_direct_io,
                     std::optional<Dim> raw_vector_dim) {
    const std::string clu_path = dir + "/cluster.clu";
    const std::string dat_path = dir + "/data.dat";

    // Open cluster store (reads header + lookup table)
    VDB_RETURN_IF_ERROR(clu_reader_.Open(clu_path, use_direct_io));

    // Open data file
    VDB_RETURN_IF_ERROR(dat_reader_.Open(dat_path,
                                          raw_vector_dim.has_value()
                                              ? *raw_vector_dim
                                              : clu_reader_.dim(),
                                          payload_schemas,
                                          use_direct_io));

    return Status::OK();
}

}  // namespace storage
}  // namespace vdb
