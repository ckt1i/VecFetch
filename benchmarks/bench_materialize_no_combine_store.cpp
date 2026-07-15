#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/storage/hot_record.h"

namespace fs = std::filesystem;

using vdb::AddressEntry;
using vdb::Status;
using vdb::index::IvfIndex;
using vdb::storage::DecodeHotPayloadDescriptor;
using vdb::storage::HotPayloadDescriptor;
using vdb::storage::HotPayloadStorageType;
using vdb::storage::ValidateHotPayloadDescriptor;

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

enum class MaterializedLayout {
    NoCombineFlatStor,
    HotColdRecordStore,
};

struct LayoutPaths {
    const char* layout_name;
    const char* vector_file;
    const char* payload_file;
    const char* map_file;
    char magic[8];
};

static LayoutPaths PathsForLayout(MaterializedLayout layout) {
    if (layout == MaterializedLayout::HotColdRecordStore) {
        LayoutPaths paths{};
        paths.layout_name = "hotcold_record_store";
        paths.vector_file = "hotvec.dat";
        paths.payload_file = "payload.cold.dat";
        paths.map_file = "hotcold_map.bin";
        const char magic[8] = {'R', 'G', 'H', 'C', 'M', 'A', 'P', '1'};
        std::memcpy(paths.magic, magic, sizeof(magic));
        return paths;
    }
    LayoutPaths paths{};
    paths.layout_name = "no_combine_flatstor";
    paths.vector_file = "vector.dat";
    paths.payload_file = "payload.dat";
    paths.map_file = "address_map.bin";
    const char magic[8] = {'N', 'C', 'M', 'B', 'M', 'A', 'P', '1'};
    std::memcpy(paths.magic, magic, sizeof(magic));
    return paths;
}

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
                 "--index-dir DIR --output DIR [--layout no-combine|hotcold] "
                 "[--source-inline-hot-record-store] "
                 "[--direct-io]\n"
                 "\n"
                 "Materializes a benchmark-only sidecar store from an "
                 "existing combined index:\n"
                 "  no-combine: vector.dat / payload.dat / address_map.bin\n"
                 "  hotcold:    hotvec.dat / payload.cold.dat / hotcold_map.bin\n"
                 "For an inline hot-record index, --source-inline-hot-record-store "
                 "reconstructs complete payloads from descriptors and "
                 "payload.cold.dat.\n"
                 "The hotcold mode keeps the same cluster-addressed lookup key "
                 "but uses a distinct map magic and layout manifest.\n");
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

static Status PReadAll(int fd, void* data, size_t len, uint64_t offset) {
    auto* p = static_cast<uint8_t*>(data);
    size_t read_bytes = 0;
    while (read_bytes < len) {
        ssize_t n = ::pread(fd, p + read_bytes, len - read_bytes,
                            static_cast<off_t>(offset + read_bytes));
        if (n < 0) return Status::IOError("pread failed");
        if (n == 0) return Status::IOError("short pread");
        read_bytes += static_cast<size_t>(n);
    }
    return Status::OK();
}

static SeparateStoreMapHeader MakeMapHeader(uint64_t count,
                                            uint32_t vec_bytes,
                                            const char magic[8]) {
    SeparateStoreMapHeader hdr{};
    std::memcpy(hdr.magic, magic, sizeof(hdr.magic));
    hdr.version = 1;
    hdr.record_size = sizeof(SeparateStoreMapRecord);
    hdr.count = count;
    hdr.vec_bytes = vec_bytes;
    hdr.reserved = 0;
    return hdr;
}

static std::string Basename(const char* path) {
    return fs::path(path == nullptr ? "" : path).filename().string();
}

static MaterializedLayout ResolveLayout(int argc, char** argv) {
    std::string layout = GetStringArg(argc, argv, "--layout", "");
    if (layout.empty() && HasFlag(argc, argv, "--hotcold")) {
        layout = "hotcold";
    }
    if (layout.empty() &&
        Basename(argv[0]).find("hotcold") != std::string::npos) {
        layout = "hotcold";
    }
    if (layout == "hotcold" || layout == "hot-cold" ||
        layout == "hotcold_record_store") {
        return MaterializedLayout::HotColdRecordStore;
    }
    return MaterializedLayout::NoCombineFlatStor;
}

int main(int argc, char** argv) {
    if (HasFlag(argc, argv, "--help") || HasFlag(argc, argv, "-h")) {
        return Usage();
    }
    const std::string index_dir = GetStringArg(argc, argv, "--index-dir", "");
    const std::string output_dir = GetStringArg(argc, argv, "--output", "");
    const bool direct_io = HasFlag(argc, argv, "--direct-io");
    const bool source_inline_hot_record_store =
        HasFlag(argc, argv, "--source-inline-hot-record-store");
    const MaterializedLayout layout = ResolveLayout(argc, argv);
    const LayoutPaths paths = PathsForLayout(layout);
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
    const std::string vector_path = output_dir + "/" + paths.vector_file;
    const std::string payload_path = output_dir + "/" + paths.payload_file;
    const std::string map_path = output_dir + "/" + paths.map_file;

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
    int cold_fd = -1;
    if (source_inline_hot_record_store) {
        const std::string cold_path = index_dir + "/payload.cold.dat";
        cold_fd = ::open(cold_path.c_str(), O_RDONLY);
        if (cold_fd < 0) {
            std::perror(cold_path.c_str());
            ::close(map_fd);
            ::close(payload_fd);
            ::close(vector_fd);
            return 1;
        }
    }

    SeparateStoreMapHeader placeholder = MakeMapHeader(0, vec_bytes, paths.magic);
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
    std::vector<uint8_t> cold_buf;
    uint64_t unique_records = 0;
    uint64_t duplicate_refs = 0;
    uint64_t payload_offset = 0;
    uint64_t cluster_records_seen = 0;

    std::printf("=== Materialize Record Sidecar Store ===\n");
    std::printf("Layout: %s\n", paths.layout_name);
    std::printf("Index: %s\n", index_dir.c_str());
    std::printf("Output: %s\n", output_dir.c_str());
    std::printf("Files: vector=%s payload=%s map=%s\n",
                paths.vector_file, paths.payload_file, paths.map_file);
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

            uint64_t logical_payload_bytes = addr.size - vec_bytes;
            if (source_inline_hot_record_store) {
                if (addr.size < vec_bytes + sizeof(HotPayloadDescriptor)) {
                    std::fprintf(stderr,
                                 "Hot record too small for descriptor: offset=%llu size=%u\n",
                                 static_cast<unsigned long long>(addr.offset),
                                 addr.size);
                    return 1;
                }
                const auto desc = DecodeHotPayloadDescriptor(
                    record_buf.data() + vec_bytes);
                s = ValidateHotPayloadDescriptor(desc);
                if (!s.ok()) {
                    std::fprintf(stderr,
                                 "Invalid hot payload descriptor at offset=%llu: %s\n",
                                 static_cast<unsigned long long>(addr.offset),
                                 s.ToString().c_str());
                    return 1;
                }
                logical_payload_bytes = desc.payload_bytes;
                const uint64_t inline_end =
                    static_cast<uint64_t>(vec_bytes) + sizeof(desc) +
                    desc.inline_bytes;
                if (inline_end > record_buf.size()) {
                    std::fprintf(stderr,
                                 "Hot record inline payload exceeds record: offset=%llu\n",
                                 static_cast<unsigned long long>(addr.offset));
                    return 1;
                }
                if (desc.inline_bytes > 0) {
                    s = WriteAll(payload_fd,
                                 record_buf.data() + vec_bytes + sizeof(desc),
                                 desc.inline_bytes);
                    if (!s.ok()) {
                        std::fprintf(stderr, "Write inline payload failed: %s\n",
                                     s.ToString().c_str());
                        return 1;
                    }
                }
                const uint64_t cold_bytes =
                    desc.payload_bytes - desc.inline_bytes;
                if (cold_bytes > 0) {
                    cold_buf.resize(static_cast<size_t>(cold_bytes));
                    s = PReadAll(cold_fd, cold_buf.data(), cold_buf.size(),
                                 desc.payload_offset);
                    if (!s.ok()) {
                        std::fprintf(stderr,
                                     "Read cold payload failed at offset=%llu: %s\n",
                                     static_cast<unsigned long long>(desc.payload_offset),
                                     s.ToString().c_str());
                        return 1;
                    }
                    s = WriteAll(payload_fd, cold_buf.data(), cold_buf.size());
                    if (!s.ok()) {
                        std::fprintf(stderr, "Write cold payload failed: %s\n",
                                     s.ToString().c_str());
                        return 1;
                    }
                }
            } else if (logical_payload_bytes > 0) {
                s = WriteAll(payload_fd, record_buf.data() + vec_bytes,
                             static_cast<size_t>(logical_payload_bytes));
                if (!s.ok()) {
                    std::fprintf(stderr, "Write payload failed: %s\n",
                                 s.ToString().c_str());
                    return 1;
                }
            }

            if (logical_payload_bytes > UINT32_MAX) {
                std::fprintf(stderr,
                             "Payload exceeds no-combine map limit: offset=%llu bytes=%llu\n",
                             static_cast<unsigned long long>(addr.offset),
                             static_cast<unsigned long long>(logical_payload_bytes));
                return 1;
            }
            const uint32_t payload_bytes =
                static_cast<uint32_t>(logical_payload_bytes);

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
        MakeMapHeader(unique_records, vec_bytes, paths.magic);
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
    if (cold_fd >= 0) ::close(cold_fd);

    const double wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::ofstream manifest(output_dir + "/manifest.json",
                           std::ios::binary | std::ios::trunc);
    if (manifest.good()) {
        manifest << "{\n";
        manifest << "  \"layout\": \"" << paths.layout_name << "\",\n";
        manifest << "  \"source_inline_hot_record_store\": "
                 << (source_inline_hot_record_store ? "true" : "false")
                 << ",\n";
        manifest << "  \"index_dir\": \"" << index_dir << "\",\n";
        manifest << "  \"vector_file\": \"" << paths.vector_file << "\",\n";
        manifest << "  \"payload_file\": \"" << paths.payload_file << "\",\n";
        manifest << "  \"map_file\": \"" << paths.map_file << "\",\n";
        manifest << "  \"unique_records\": " << unique_records << ",\n";
        manifest << "  \"duplicate_refs\": " << duplicate_refs << ",\n";
        manifest << "  \"cluster_records_seen\": " << cluster_records_seen << ",\n";
        manifest << "  \"vec_bytes\": " << vec_bytes << ",\n";
        manifest << "  \"vector_bytes\": "
                 << (unique_records * static_cast<uint64_t>(vec_bytes)) << ",\n";
        manifest << "  \"payload_bytes\": " << payload_offset << ",\n";
        manifest << "  \"wall_ms\": " << wall_ms << "\n";
        manifest << "}\n";
    }
    std::printf("Done: unique=%llu duplicate_refs=%llu payload_bytes=%llu wall_ms=%.3f\n",
                static_cast<unsigned long long>(unique_records),
                static_cast<unsigned long long>(duplicate_refs),
                static_cast<unsigned long long>(payload_offset),
                wall_ms);
    return 0;
}
