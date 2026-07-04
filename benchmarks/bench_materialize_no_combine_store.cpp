#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"

namespace fs = std::filesystem;

using vdb::AddressEntry;
using vdb::Status;
using vdb::index::IvfIndex;

struct SeparateStoreMapHeader {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint64_t count;
    uint32_t vec_bytes;
    uint32_t reserved;
};

struct SeparateStoreMapRecord {
    uint64_t combined_offset;
    uint64_t row_id;
    uint64_t payload_offset;
    uint32_t payload_bytes;
    uint32_t reserved;
};

static_assert(sizeof(SeparateStoreMapHeader) == 32,
              "Unexpected SeparateStoreMapHeader layout");
static_assert(sizeof(SeparateStoreMapRecord) == 32,
              "Unexpected SeparateStoreMapRecord layout");

static bool HasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

static std::string GetStringArg(int argc, char** argv, const char* key,
                                const std::string& def = "") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return def;
}

static int Usage() {
    std::fprintf(stderr,
                 "Usage: bench_materialize_no_combine_store "
                 "--index-dir DIR --output DIR [--direct-io]\n"
                 "\n"
                 "Materializes a benchmark-only No Combine store from an "
                 "existing combined index:\n"
                 "  vector.dat       dense raw-vector rows\n"
                 "  payload.dat      concatenated original payload bytes\n"
                 "  address_map.bin  combined data.dat offset -> vector row + "
                 "payload slice\n");
    return 2;
}

static Status WriteAll(int fd, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < len) {
        ssize_t n = ::write(fd, p + written, len - written);
        if (n < 0) return Status::IOError("write failed");
        if (n == 0) return Status::IOError("short write");
        written += static_cast<size_t>(n);
    }
    return Status::OK();
}

static Status PWriteAll(int fd, const void* data, size_t len, uint64_t offset) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < len) {
        ssize_t n = ::pwrite(fd, p + written, len - written,
                             static_cast<off_t>(offset + written));
        if (n < 0) return Status::IOError("pwrite failed");
        if (n == 0) return Status::IOError("short pwrite");
        written += static_cast<size_t>(n);
    }
    return Status::OK();
}

static SeparateStoreMapHeader MakeMapHeader(uint64_t count,
                                            uint32_t vec_bytes) {
    SeparateStoreMapHeader hdr{};
    const char magic[8] = {'N', 'C', 'M', 'B', 'M', 'A', 'P', '1'};
    std::memcpy(hdr.magic, magic, sizeof(magic));
    hdr.version = 1;
    hdr.record_size = sizeof(SeparateStoreMapRecord);
    hdr.count = count;
    hdr.vec_bytes = vec_bytes;
    hdr.reserved = 0;
    return hdr;
}

int main(int argc, char** argv) {
    if (HasFlag(argc, argv, "--help") || HasFlag(argc, argv, "-h")) {
        return Usage();
    }
    const std::string index_dir = GetStringArg(argc, argv, "--index-dir", "");
    const std::string output_dir = GetStringArg(argc, argv, "--output", "");
    const bool direct_io = HasFlag(argc, argv, "--direct-io");
    if (index_dir.empty() || output_dir.empty()) return Usage();

    const auto start = std::chrono::steady_clock::now();
    IvfIndex index;
    Status s = index.Open(index_dir, direct_io);
    if (!s.ok()) {
        std::fprintf(stderr, "Open index failed: %s\n", s.ToString().c_str());
        return 1;
    }

    const uint32_t vec_bytes = index.logical_dim() * sizeof(float);
    fs::create_directories(output_dir);
    const std::string vector_path = output_dir + "/vector.dat";
    const std::string payload_path = output_dir + "/payload.dat";
    const std::string map_path = output_dir + "/address_map.bin";

    int vector_fd = ::open(vector_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
                           0644);
    if (vector_fd < 0) {
        std::perror(vector_path.c_str());
        return 1;
    }
    int payload_fd = ::open(payload_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY,
                            0644);
    if (payload_fd < 0) {
        std::perror(payload_path.c_str());
        ::close(vector_fd);
        return 1;
    }
    int map_fd = ::open(map_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (map_fd < 0) {
        std::perror(map_path.c_str());
        ::close(payload_fd);
        ::close(vector_fd);
        return 1;
    }

    SeparateStoreMapHeader placeholder = MakeMapHeader(0, vec_bytes);
    s = WriteAll(map_fd, &placeholder, sizeof(placeholder));
    if (!s.ok()) {
        std::fprintf(stderr, "Write map header failed: %s\n",
                     s.ToString().c_str());
        return 1;
    }

    const auto cluster_ids = index.segment().cluster_ids();
    const uint64_t total_cluster_records = index.segment().total_records();
    std::unordered_set<uint64_t> seen_offsets;
    seen_offsets.reserve(static_cast<size_t>(total_cluster_records));

    std::vector<uint8_t> record_buf;
    uint64_t unique_records = 0;
    uint64_t duplicate_refs = 0;
    uint64_t payload_offset = 0;
    uint64_t cluster_records_seen = 0;

    std::printf("=== Materialize No Combine Store ===\n");
    std::printf("Index: %s\n", index_dir.c_str());
    std::printf("Output: %s\n", output_dir.c_str());
    std::printf("Clusters: %zu total_cluster_records=%llu vec_bytes=%u\n",
                cluster_ids.size(),
                static_cast<unsigned long long>(total_cluster_records),
                vec_bytes);
    std::fflush(stdout);

    for (size_t cidx = 0; cidx < cluster_ids.size(); ++cidx) {
        const uint32_t cid = cluster_ids[cidx];
        s = index.segment().EnsureClusterLoaded(cid);
        if (!s.ok()) {
            std::fprintf(stderr, "Load cluster %u failed: %s\n", cid,
                         s.ToString().c_str());
            return 1;
        }

        const uint32_t count = index.segment().GetNumRecords(cid);
        for (uint32_t ridx = 0; ridx < count; ++ridx) {
            ++cluster_records_seen;
            const AddressEntry addr = index.segment().GetAddress(cid, ridx);
            if (addr.size < vec_bytes) {
                std::fprintf(stderr,
                             "Record too small: cluster=%u ridx=%u offset=%llu size=%u vec_bytes=%u\n",
                             cid, ridx,
                             static_cast<unsigned long long>(addr.offset),
                             addr.size, vec_bytes);
                return 1;
            }
            if (!seen_offsets.insert(addr.offset).second) {
                ++duplicate_refs;
                continue;
            }

            record_buf.resize(addr.size);
            s = index.segment().data_reader().ReadRaw(
                addr.offset, addr.size, record_buf.data());
            if (!s.ok()) {
                std::fprintf(stderr,
                             "Read record failed: cluster=%u ridx=%u offset=%llu: %s\n",
                             cid, ridx,
                             static_cast<unsigned long long>(addr.offset),
                             s.ToString().c_str());
                return 1;
            }

            const uint64_t row_id = unique_records;
            s = WriteAll(vector_fd, record_buf.data(), vec_bytes);
            if (!s.ok()) {
                std::fprintf(stderr, "Write vector failed: %s\n",
                             s.ToString().c_str());
                return 1;
            }

            const uint32_t payload_bytes = addr.size - vec_bytes;
            if (payload_bytes > 0) {
                s = WriteAll(payload_fd, record_buf.data() + vec_bytes,
                             payload_bytes);
                if (!s.ok()) {
                    std::fprintf(stderr, "Write payload failed: %s\n",
                                 s.ToString().c_str());
                    return 1;
                }
            }

            SeparateStoreMapRecord map_rec{};
            map_rec.combined_offset = addr.offset;
            map_rec.row_id = row_id;
            map_rec.payload_offset = payload_offset;
            map_rec.payload_bytes = payload_bytes;
            map_rec.reserved = 0;
            s = WriteAll(map_fd, &map_rec, sizeof(map_rec));
            if (!s.ok()) {
                std::fprintf(stderr, "Write map record failed: %s\n",
                             s.ToString().c_str());
                return 1;
            }

            ++unique_records;
            payload_offset += payload_bytes;
        }

        (void)index.segment().UnloadCluster(cid);
        if ((cidx + 1) % 64 == 0 || cidx + 1 == cluster_ids.size()) {
            std::printf("  progress: clusters=%zu/%zu cluster_records=%llu unique=%llu duplicates=%llu\n",
                        cidx + 1, cluster_ids.size(),
                        static_cast<unsigned long long>(cluster_records_seen),
                        static_cast<unsigned long long>(unique_records),
                        static_cast<unsigned long long>(duplicate_refs));
            std::fflush(stdout);
        }
    }

    const SeparateStoreMapHeader final_hdr =
        MakeMapHeader(unique_records, vec_bytes);
    s = PWriteAll(map_fd, &final_hdr, sizeof(final_hdr), 0);
    if (!s.ok()) {
        std::fprintf(stderr, "Patch map header failed: %s\n",
                     s.ToString().c_str());
        return 1;
    }
    ::fsync(vector_fd);
    ::fsync(payload_fd);
    ::fsync(map_fd);
    ::close(map_fd);
    ::close(payload_fd);
    ::close(vector_fd);

    const double wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("Done: unique=%llu duplicate_refs=%llu payload_bytes=%llu wall_ms=%.3f\n",
                static_cast<unsigned long long>(unique_records),
                static_cast<unsigned long long>(duplicate_refs),
                static_cast<unsigned long long>(payload_offset),
                wall_ms);
    return 0;
}
