#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "vdb/common/status.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_index.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/cluster_store.h"
#include "vdb/storage/hot_record.h"

namespace fs = std::filesystem;

using vdb::AddressEntry;
using vdb::Status;
using vdb::index::IvfIndex;
using vdb::storage::AddressColumn;
using vdb::storage::ClusterStoreWriter;
using vdb::storage::HotPayloadDescriptor;
using vdb::storage::HotPayloadStorageType;

namespace {

constexpr uint32_t kDefaultHotRecordPageSize = 512;

struct InlineDescriptorMapHeader {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint64_t count;
    uint32_t vec_bytes;
    uint32_t reserved;
};

struct InlineDescriptorMapRecord {
    uint64_t combined_offset;
    uint64_t payload_offset;
    uint32_t payload_bytes;
    uint32_t inline_bytes;
    uint8_t payload_storage_type;
    uint8_t reserved[7];
};

static_assert(sizeof(InlineDescriptorMapHeader) == 32,
              "Unexpected InlineDescriptorMapHeader layout");
static_assert(sizeof(InlineDescriptorMapRecord) == 32,
              "Unexpected InlineDescriptorMapRecord layout");

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

static uint64_t GetUint64Arg(int argc, char** argv, const char* key,
                             uint64_t def = 0) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) {
            return static_cast<uint64_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }
    return def;
}

static int Usage() {
    std::fprintf(
        stderr,
        "Usage: bench_materialize_inline_hot_record_store "
        "--index-dir DIR --output DIR "
        "[--inline-payload-threshold BYTES] "
        "[--inline-full-record-threshold BYTES] "
        "[--hot-payload-prefix-bytes BYTES] "
        "[--hot-record-page-size BYTES]\n"
        "\n"
        "Creates a derived RecordGate index whose cluster addresses point "
        "directly to packed hot records in data.dat.  Each hot record is "
        "[raw_vector][HotPayloadDescriptor][inline payload if small].  "
        "Large payload bodies are stored in payload.cold.dat and are not "
        "read during SafeIn.\n");
    return 2;
}

static uint64_t AlignUp(uint64_t value, uint64_t align) {
    if (align <= 1) return value;
    return ((value + align - 1) / align) * align;
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

static bool CopyIfExists(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src)) return false;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    return true;
}

static void CopyIndexSidecars(const fs::path& src_dir, const fs::path& dst_dir) {
    static const char* kSkip[] = {
        "cluster.clu",
        "data.dat",
        "payload.cold.dat",
        "inline_descriptor_map.bin",
        "manifest.json",
        "materialize.log",
    };
    for (const auto& entry : fs::directory_iterator(src_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        bool skip = false;
        for (const char* s : kSkip) {
            if (name == s) {
                skip = true;
                break;
            }
        }
        if (skip) continue;
        CopyIfExists(entry.path(), dst_dir / name);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (HasFlag(argc, argv, "--help") || HasFlag(argc, argv, "-h")) {
        return Usage();
    }

    const std::string index_dir = GetStringArg(argc, argv, "--index-dir", "");
    const std::string output_dir = GetStringArg(argc, argv, "--output", "");
    const uint64_t inline_payload_threshold =
        GetUint64Arg(argc, argv, "--inline-payload-threshold", 0);
    const uint64_t inline_full_record_threshold =
        GetUint64Arg(argc, argv, "--inline-full-record-threshold", 0);
    const uint64_t hot_payload_prefix_bytes =
        GetUint64Arg(argc, argv, "--hot-payload-prefix-bytes", 0);
    const uint32_t hot_record_page_size = static_cast<uint32_t>(
        GetUint64Arg(argc, argv, "--hot-record-page-size",
                     kDefaultHotRecordPageSize));
    if (index_dir.empty() || output_dir.empty() ||
        hot_record_page_size == 0) {
        return Usage();
    }

    auto t0 = std::chrono::steady_clock::now();
    try {
        fs::create_directories(output_dir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "create output dir failed: %s\n", e.what());
        return 1;
    }

    IvfIndex index;
    Status s = index.Open(index_dir);
    if (!s.ok()) {
        std::fprintf(stderr, "Open index failed: %s\n", s.ToString().c_str());
        return 1;
    }

    auto& reader = index.segment().cluster_reader();
    const uint32_t vec_bytes = index.logical_dim() * sizeof(float);
    const uint32_t descriptor_bytes = sizeof(HotPayloadDescriptor);

    const fs::path hot_path = fs::path(output_dir) / "data.dat";
    const fs::path cold_path = fs::path(output_dir) / "payload.cold.dat";
    const fs::path descriptor_map_path =
        fs::path(output_dir) / "inline_descriptor_map.bin";
    int hot_fd = ::open(hot_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (hot_fd < 0) {
        std::perror(("open " + hot_path.string()).c_str());
        return 1;
    }
    int cold_fd = ::open(cold_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (cold_fd < 0) {
        std::perror(("open " + cold_path.string()).c_str());
        ::close(hot_fd);
        return 1;
    }
    int descriptor_map_fd = ::open(descriptor_map_path.c_str(),
                                   O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (descriptor_map_fd < 0) {
        std::perror(("open " + descriptor_map_path.string()).c_str());
        ::close(cold_fd);
        ::close(hot_fd);
        return 1;
    }
    InlineDescriptorMapHeader map_header{};
    const char map_magic[8] = {'R', 'G', 'I', 'D', 'M', 'A', 'P', '1'};
    std::memcpy(map_header.magic, map_magic, sizeof(map_magic));
    map_header.version = 1;
    map_header.record_size = sizeof(InlineDescriptorMapRecord);
    map_header.vec_bytes = vec_bytes;
    s = WriteAll(descriptor_map_fd, &map_header, sizeof(map_header));
    if (!s.ok()) {
        std::fprintf(stderr, "Write descriptor map header failed: %s\n",
                     s.ToString().c_str());
        return 1;
    }

    ClusterStoreWriter writer;
    s = writer.Open((fs::path(output_dir) / "cluster.clu").string(),
                    reader.num_clusters(), reader.dim(),
                    reader.rabitq_config());
    if (!s.ok()) {
        std::fprintf(stderr, "Open cluster writer failed: %s\n",
                     s.ToString().c_str());
        return 1;
    }

    uint64_t hot_offset = 0;
    uint64_t cold_offset = 0;
    uint64_t unique_records = 0;
    uint64_t duplicate_refs = 0;
    uint64_t inline_records = 0;
    uint64_t cold_records = 0;
    uint64_t prefix_cold_records = 0;
    uint64_t inline_payload_bytes = 0;
    uint64_t cold_payload_bytes = 0;
    uint64_t cluster_records_seen = 0;

    std::unordered_map<uint64_t, AddressEntry> remapped_by_old_offset;
    std::vector<uint8_t> record_buf;
    std::vector<uint8_t> hot_record_buf;
    std::vector<uint8_t> zero_pad(hot_record_page_size, 0);

    const auto cluster_ids = reader.cluster_ids();
    std::printf("=== Materialize Inline Hot Record Store ===\n");
    std::printf("Index: %s\n", index_dir.c_str());
    std::printf("Output: %s\n", output_dir.c_str());
    std::printf("vec_bytes=%u descriptor_bytes=%u inline_threshold=%llu "
                "full_record_threshold=%llu hot_prefix=%llu page_size=%u\n",
                vec_bytes, descriptor_bytes,
                static_cast<unsigned long long>(inline_payload_threshold),
                static_cast<unsigned long long>(inline_full_record_threshold),
                static_cast<unsigned long long>(hot_payload_prefix_bytes),
                hot_record_page_size);
    std::fflush(stdout);

    for (size_t cidx = 0; cidx < cluster_ids.size(); ++cidx) {
        const uint32_t cid = cluster_ids[cidx];
        s = reader.EnsureClusterLoaded(cid);
        if (!s.ok()) {
            std::fprintf(stderr, "EnsureClusterLoaded(%u) failed: %s\n",
                         cid, s.ToString().c_str());
            return 1;
        }

        const uint32_t count = reader.GetNumRecords(cid);
        std::vector<uint32_t> indices(count);
        for (uint32_t i = 0; i < count; ++i) indices[i] = i;

        std::vector<vdb::rabitq::RaBitQCode> codes;
        s = reader.LoadCodes(cid, indices, codes);
        if (!s.ok()) {
            std::fprintf(stderr, "LoadCodes(%u) failed: %s\n", cid,
                         s.ToString().c_str());
            return 1;
        }
        std::vector<AddressEntry> old_addrs = reader.GetAddresses(cid, indices);
        std::vector<AddressEntry> new_addrs;
        new_addrs.reserve(old_addrs.size());

        for (const AddressEntry& old_addr : old_addrs) {
            ++cluster_records_seen;
            auto found = remapped_by_old_offset.find(old_addr.offset);
            if (found != remapped_by_old_offset.end()) {
                ++duplicate_refs;
                new_addrs.push_back(found->second);
                continue;
            }
            if (old_addr.size < vec_bytes) {
                std::fprintf(stderr,
                             "record too small: cid=%u offset=%llu size=%u vec_bytes=%u\n",
                             cid,
                             static_cast<unsigned long long>(old_addr.offset),
                             old_addr.size, vec_bytes);
                return 1;
            }

            record_buf.resize(old_addr.size);
            s = index.segment().data_reader().ReadRaw(old_addr.offset,
                                                      old_addr.size,
                                                      record_buf.data());
            if (!s.ok()) {
                std::fprintf(stderr, "ReadRaw failed: %s\n",
                             s.ToString().c_str());
                return 1;
            }

            const uint32_t payload_bytes = old_addr.size - vec_bytes;
            const uint64_t full_record_bytes =
                static_cast<uint64_t>(vec_bytes) + payload_bytes;
            const bool inline_payload = inline_full_record_threshold > 0
                ? full_record_bytes <= inline_full_record_threshold
                : static_cast<uint64_t>(payload_bytes) <= inline_payload_threshold;
            const uint32_t inline_prefix_bytes = inline_payload
                ? payload_bytes
                : static_cast<uint32_t>(std::min<uint64_t>(
                      hot_payload_prefix_bytes, payload_bytes));
            HotPayloadDescriptor desc{};
            desc.header_size = sizeof(HotPayloadDescriptor);
            desc.payload_bytes = payload_bytes;
            if (inline_payload) {
                desc.payload_storage_type =
                    static_cast<uint8_t>(HotPayloadStorageType::kInlinePayload);
                desc.inline_bytes = payload_bytes;
                desc.payload_offset = 0;
                ++inline_records;
                inline_payload_bytes += payload_bytes;
            } else if (inline_prefix_bytes > 0 &&
                       inline_prefix_bytes < payload_bytes) {
                desc.payload_storage_type = static_cast<uint8_t>(
                    HotPayloadStorageType::kPrefixColdPointer);
                desc.inline_bytes = inline_prefix_bytes;
                desc.payload_offset = cold_offset;
                ++prefix_cold_records;
                ++cold_records;
                inline_payload_bytes += inline_prefix_bytes;
                const uint32_t suffix_bytes = payload_bytes - inline_prefix_bytes;
                cold_payload_bytes += suffix_bytes;
                s = WriteAll(cold_fd,
                             record_buf.data() + vec_bytes + inline_prefix_bytes,
                             suffix_bytes);
                if (!s.ok()) {
                    std::fprintf(stderr, "Write cold payload suffix failed: %s\n",
                                 s.ToString().c_str());
                    return 1;
                }
                cold_offset += suffix_bytes;
            } else {
                desc.payload_storage_type =
                    static_cast<uint8_t>(HotPayloadStorageType::kColdPointer);
                desc.inline_bytes = 0;
                desc.payload_offset = cold_offset;
                ++cold_records;
                cold_payload_bytes += payload_bytes;
                if (payload_bytes > 0) {
                    s = WriteAll(cold_fd, record_buf.data() + vec_bytes,
                                 payload_bytes);
                    if (!s.ok()) {
                        std::fprintf(stderr, "Write cold payload failed: %s\n",
                                     s.ToString().c_str());
                        return 1;
                    }
                    cold_offset += payload_bytes;
                }
            }

            const uint64_t logical_hot_size =
                static_cast<uint64_t>(vec_bytes) + descriptor_bytes +
                inline_prefix_bytes;
            const uint64_t padded_hot_size =
                AlignUp(logical_hot_size, hot_record_page_size);
            hot_record_buf.resize(static_cast<size_t>(logical_hot_size));
            std::memcpy(hot_record_buf.data(), record_buf.data(), vec_bytes);
            std::memcpy(hot_record_buf.data() + vec_bytes, &desc,
                        sizeof(desc));
            if (inline_prefix_bytes > 0) {
                std::memcpy(hot_record_buf.data() + vec_bytes + descriptor_bytes,
                            record_buf.data() + vec_bytes, inline_prefix_bytes);
            }
            s = WriteAll(hot_fd, hot_record_buf.data(), hot_record_buf.size());
            if (!s.ok()) {
                std::fprintf(stderr, "Write hot record failed: %s\n",
                             s.ToString().c_str());
                return 1;
            }
            const uint64_t pad = padded_hot_size - logical_hot_size;
            if (pad > 0) {
                s = WriteAll(hot_fd, zero_pad.data(), static_cast<size_t>(pad));
                if (!s.ok()) {
                    std::fprintf(stderr, "Write hot padding failed: %s\n",
                                 s.ToString().c_str());
                    return 1;
                }
            }

            AddressEntry new_addr{hot_offset,
                                  static_cast<uint32_t>(padded_hot_size)};
            if (new_addr.offset / hot_record_page_size >
                static_cast<uint64_t>(UINT32_MAX)) {
                std::fprintf(stderr,
                             "hot record offset exceeds raw address table limit\n");
                return 1;
            }
            if (new_addr.size / hot_record_page_size > UINT32_MAX) {
                std::fprintf(stderr,
                             "hot record size exceeds raw address table limit\n");
                return 1;
            }
            if (desc.payload_bytes > UINT32_MAX) {
                std::fprintf(stderr,
                             "payload exceeds descriptor map limit: "
                             "offset=%llu bytes=%llu\n",
                             static_cast<unsigned long long>(hot_offset),
                             static_cast<unsigned long long>(desc.payload_bytes));
                return 1;
            }
            InlineDescriptorMapRecord map_record{};
            map_record.combined_offset = hot_offset;
            map_record.payload_offset = desc.payload_offset;
            map_record.payload_bytes =
                static_cast<uint32_t>(desc.payload_bytes);
            map_record.inline_bytes = desc.inline_bytes;
            map_record.payload_storage_type = desc.payload_storage_type;
            s = WriteAll(descriptor_map_fd, &map_record, sizeof(map_record));
            if (!s.ok()) {
                std::fprintf(stderr, "Write descriptor map record failed: %s\n",
                             s.ToString().c_str());
                return 1;
            }
            remapped_by_old_offset.emplace(old_addr.offset, new_addr);
            new_addrs.push_back(new_addr);
            hot_offset += padded_hot_size;
            ++unique_records;
        }

        auto addr_column =
            AddressColumn::EncodeRawTableV2(new_addrs, hot_record_page_size);
        s = writer.BeginCluster(cid, count, reader.GetCentroid(cid),
                                reader.GetEpsilon(cid));
        if (!s.ok()) {
            std::fprintf(stderr, "BeginCluster(%u) failed: %s\n", cid,
                         s.ToString().c_str());
            return 1;
        }
        s = writer.WriteVectors(codes);
        if (!s.ok()) {
            std::fprintf(stderr, "WriteVectors(%u) failed: %s\n", cid,
                         s.ToString().c_str());
            return 1;
        }
        s = writer.WriteAddressBlocks(addr_column);
        if (!s.ok()) {
            std::fprintf(stderr, "WriteAddressBlocks(%u) failed: %s\n", cid,
                         s.ToString().c_str());
            return 1;
        }
        s = writer.EndCluster();
        if (!s.ok()) {
            std::fprintf(stderr, "EndCluster(%u) failed: %s\n", cid,
                         s.ToString().c_str());
            return 1;
        }
        (void)reader.UnloadCluster(cid);

        if ((cidx + 1) % 64 == 0 || cidx + 1 == cluster_ids.size()) {
            std::printf("  progress: clusters=%zu/%zu seen=%llu unique=%llu hot=%.2f GiB cold=%.2f GiB\n",
                        cidx + 1, cluster_ids.size(),
                        static_cast<unsigned long long>(cluster_records_seen),
                        static_cast<unsigned long long>(unique_records),
                        static_cast<double>(hot_offset) / (1024.0 * 1024.0 * 1024.0),
                        static_cast<double>(cold_offset) / (1024.0 * 1024.0 * 1024.0));
            std::fflush(stdout);
        }
    }

    s = writer.Finalize("data.dat");
    if (!s.ok()) {
        std::fprintf(stderr, "Finalize cluster writer failed: %s\n",
                     s.ToString().c_str());
        return 1;
    }
    ::fsync(hot_fd);
    ::fsync(cold_fd);
    map_header.count = unique_records;
    if (::lseek(descriptor_map_fd, 0, SEEK_SET) < 0) {
        std::perror("seek inline descriptor map");
        return 1;
    }
    s = WriteAll(descriptor_map_fd, &map_header, sizeof(map_header));
    if (!s.ok()) {
        std::fprintf(stderr, "Finalize descriptor map header failed: %s\n",
                     s.ToString().c_str());
        return 1;
    }
    ::fsync(descriptor_map_fd);
    ::close(hot_fd);
    ::close(cold_fd);
    ::close(descriptor_map_fd);

    try {
        CopyIndexSidecars(index_dir, output_dir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "copy sidecars failed: %s\n", e.what());
        return 1;
    }

    const double wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    const fs::path manifest_path = fs::path(output_dir) / "manifest.json";
    std::ofstream manifest(manifest_path);
    manifest << "{\n";
    manifest << "  \"layout\": \"inline_hot_record_store\",\n";
    manifest << "  \"index_dir\": \"" << index_dir << "\",\n";
    manifest << "  \"hot_record_file\": \"data.dat\",\n";
    manifest << "  \"cold_payload_file\": \"payload.cold.dat\",\n";
    manifest << "  \"descriptor_map_file\": "
             << "\"inline_descriptor_map.bin\",\n";
    manifest << "  \"descriptor_bytes\": " << descriptor_bytes << ",\n";
    manifest << "  \"vec_bytes\": " << vec_bytes << ",\n";
    manifest << "  \"hot_record_page_size\": " << hot_record_page_size << ",\n";
    manifest << "  \"inline_payload_threshold\": "
             << inline_payload_threshold << ",\n";
    manifest << "  \"inline_full_record_threshold\": "
             << inline_full_record_threshold << ",\n";
    manifest << "  \"hot_payload_prefix_bytes\": "
             << hot_payload_prefix_bytes << ",\n";
    manifest << "  \"effective_safein_inline_threshold\": "
             << inline_payload_threshold << ",\n";
    manifest << "  \"record_count\": " << unique_records << ",\n";
    manifest << "  \"cluster_records_seen\": " << cluster_records_seen << ",\n";
    manifest << "  \"duplicate_refs\": " << duplicate_refs << ",\n";
    manifest << "  \"inline_record_count\": " << inline_records << ",\n";
    manifest << "  \"cold_record_count\": " << cold_records << ",\n";
    manifest << "  \"prefix_cold_record_count\": "
             << prefix_cold_records << ",\n";
    manifest << "  \"inline_payload_bytes\": " << inline_payload_bytes << ",\n";
    manifest << "  \"cold_payload_bytes\": " << cold_payload_bytes << ",\n";
    manifest << "  \"hot_record_bytes\": " << hot_offset << ",\n";
    manifest << "  \"address_map_bytes\": "
             << (sizeof(InlineDescriptorMapHeader) +
                 unique_records * sizeof(InlineDescriptorMapRecord)) << ",\n";
    manifest << "  \"wall_ms\": " << wall_ms << "\n";
    manifest << "}\n";

    std::printf("Done: unique=%llu inline=%llu cold=%llu hot=%.2f GiB cold=%.2f GiB wall=%.2f ms\n",
                static_cast<unsigned long long>(unique_records),
                static_cast<unsigned long long>(inline_records),
                static_cast<unsigned long long>(cold_records),
                static_cast<double>(hot_offset) / (1024.0 * 1024.0 * 1024.0),
                static_cast<double>(cold_offset) / (1024.0 * 1024.0 * 1024.0),
                wall_ms);
    return 0;
}
