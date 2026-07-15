#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "vdb/common/status.h"
#include "vdb/index/ivf_index.h"
#include "vdb/storage/hot_record.h"

namespace fs = std::filesystem;

namespace {

std::string Arg(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return {};
}

uint64_t UintArg(int argc, char** argv, const char* name, uint64_t fallback) {
    const std::string value = Arg(argc, argv, name);
    return value.empty() ? fallback : std::strtoull(value.c_str(), nullptr, 10);
}

bool ReadExact(int fd, uint64_t offset, uint8_t* out, size_t length) {
    size_t done = 0;
    while (done < length) {
        const ssize_t n = ::pread(fd, out + done, length - done,
                                  static_cast<off_t>(offset + done));
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool SameCode(const vdb::rabitq::RaBitQCode& a,
              const vdb::rabitq::RaBitQCode& b) {
    return a.code == b.code && a.norm == b.norm && a.sum_x == b.sum_x &&
           a.bits == b.bits && a.ex_code == b.ex_code &&
           a.ex_sign_packed == b.ex_sign_packed && a.xipnorm == b.xipnorm &&
           a.ex_code_sign_folded == b.ex_code_sign_folded &&
           a.ex_factor_add == b.ex_factor_add &&
           a.ex_factor_rescale == b.ex_factor_rescale;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string source_dir = Arg(argc, argv, "--source-index");
    const std::string derived_dir = Arg(argc, argv, "--derived-index");
    const uint64_t max_records = UintArg(argc, argv, "--max-records", 20000);
    if (source_dir.empty() || derived_dir.empty()) {
        std::fprintf(stderr,
                     "Usage: bench_verify_inline_hot_record_store "
                     "--source-index DIR --derived-index DIR "
                     "[--max-records N]\n");
        return 2;
    }

    vdb::index::IvfIndex source;
    vdb::index::IvfIndex derived;
    vdb::Status status = source.Open(source_dir);
    if (!status.ok()) {
        std::fprintf(stderr, "source open failed: %s\n", status.ToString().c_str());
        return 1;
    }
    status = derived.Open(derived_dir);
    if (!status.ok()) {
        std::fprintf(stderr, "derived open failed: %s\n", status.ToString().c_str());
        return 1;
    }
    if (source.logical_dim() != derived.logical_dim() ||
        source.nlist() != derived.nlist()) {
        std::fprintf(stderr, "index geometry mismatch\n");
        return 1;
    }

    const uint32_t vec_bytes = source.logical_dim() * sizeof(float);
    const int hot_fd = ::open((fs::path(derived_dir) / "data.dat").c_str(), O_RDONLY);
    const int cold_fd = ::open(
        (fs::path(derived_dir) / "payload.cold.dat").c_str(), O_RDONLY);
    if (hot_fd < 0 || cold_fd < 0) {
        std::perror("open derived record files");
        return 1;
    }

    auto& source_reader = source.segment().cluster_reader();
    auto& derived_reader = derived.segment().cluster_reader();
    const auto clusters = source_reader.cluster_ids();
    const uint32_t per_cluster = static_cast<uint32_t>(
        std::max<uint64_t>(1, (max_records + clusters.size() - 1) /
                                  std::max<size_t>(1, clusters.size())));
    uint64_t checked = 0;
    uint64_t vector_mismatches = 0;
    uint64_t payload_mismatches = 0;
    uint64_t code_mismatches = 0;
    uint64_t descriptor_errors = 0;

    std::vector<uint8_t> source_record;
    std::vector<uint8_t> derived_vector;
    std::vector<uint8_t> derived_payload;
    for (uint32_t cluster_id : clusters) {
        if (checked >= max_records) break;
        status = source_reader.EnsureClusterLoaded(cluster_id);
        if (!status.ok()) return 1;
        status = derived_reader.EnsureClusterLoaded(cluster_id);
        if (!status.ok()) return 1;
        const uint32_t source_count = source_reader.GetNumRecords(cluster_id);
        const uint32_t derived_count = derived_reader.GetNumRecords(cluster_id);
        if (source_count != derived_count) {
            std::fprintf(stderr, "cluster count mismatch: %u\n", cluster_id);
            return 1;
        }
        const uint32_t take = std::min<uint32_t>(
            source_count, static_cast<uint32_t>(std::min<uint64_t>(
                              per_cluster, max_records - checked)));
        std::vector<uint32_t> indices(take);
        for (uint32_t i = 0; i < take; ++i) indices[i] = i;
        const auto source_addrs = source_reader.GetAddresses(cluster_id, indices);
        const auto derived_addrs = derived_reader.GetAddresses(cluster_id, indices);
        std::vector<vdb::rabitq::RaBitQCode> source_codes;
        std::vector<vdb::rabitq::RaBitQCode> derived_codes;
        status = source_reader.LoadCodes(cluster_id, indices, source_codes);
        if (!status.ok()) return 1;
        status = derived_reader.LoadCodes(cluster_id, indices, derived_codes);
        if (!status.ok()) return 1;

        for (uint32_t i = 0; i < take; ++i) {
            const auto src = source_addrs[i];
            const auto dst = derived_addrs[i];
            source_record.resize(src.size);
            status = source.segment().data_reader().ReadRaw(
                src.offset, src.size, source_record.data());
            if (!status.ok()) return 1;
            derived_vector.resize(vec_bytes);
            if (!ReadExact(hot_fd, dst.offset, derived_vector.data(), vec_bytes)) {
                return 1;
            }
            if (std::memcmp(source_record.data(), derived_vector.data(),
                            vec_bytes) != 0) {
                ++vector_mismatches;
            }

            uint8_t raw_desc[sizeof(vdb::storage::HotPayloadDescriptor)] = {};
            if (!ReadExact(hot_fd, dst.offset + vec_bytes, raw_desc,
                           sizeof(raw_desc))) {
                return 1;
            }
            const auto desc = vdb::storage::DecodeHotPayloadDescriptor(raw_desc);
            if (!vdb::storage::ValidateHotPayloadDescriptor(desc).ok() ||
                desc.payload_bytes != src.size - vec_bytes) {
                ++descriptor_errors;
            } else {
                derived_payload.resize(static_cast<size_t>(desc.payload_bytes));
                if (desc.inline_bytes > 0 &&
                    !ReadExact(hot_fd,
                               dst.offset + vec_bytes + sizeof(raw_desc),
                               derived_payload.data(), desc.inline_bytes)) {
                    return 1;
                }
                const uint64_t suffix_bytes =
                    desc.payload_bytes - desc.inline_bytes;
                if (suffix_bytes > 0 &&
                    !ReadExact(cold_fd, desc.payload_offset,
                               derived_payload.data() + desc.inline_bytes,
                               static_cast<size_t>(suffix_bytes))) {
                    return 1;
                }
                if (std::memcmp(source_record.data() + vec_bytes,
                                derived_payload.data(),
                                static_cast<size_t>(desc.payload_bytes)) != 0) {
                    ++payload_mismatches;
                }
            }
            if (!SameCode(source_codes[i], derived_codes[i])) {
                ++code_mismatches;
            }
            ++checked;
        }
        (void)source_reader.UnloadCluster(cluster_id);
        (void)derived_reader.UnloadCluster(cluster_id);
    }
    ::close(hot_fd);
    ::close(cold_fd);

    std::printf(
        "checked=%llu vector_mismatches=%llu payload_mismatches=%llu "
        "code_mismatches=%llu descriptor_errors=%llu\n",
        static_cast<unsigned long long>(checked),
        static_cast<unsigned long long>(vector_mismatches),
        static_cast<unsigned long long>(payload_mismatches),
        static_cast<unsigned long long>(code_mismatches),
        static_cast<unsigned long long>(descriptor_errors));
    return vector_mismatches == 0 && payload_mismatches == 0 &&
                   code_mismatches == 0 && descriptor_errors == 0
               ? 0
               : 1;
}
