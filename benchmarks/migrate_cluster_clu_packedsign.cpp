#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "vdb/common/status.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/cluster_store.h"

namespace fs = std::filesystem;

namespace {

using vdb::AddressEntry;
using vdb::Status;
using vdb::storage::AddressColumn;
using vdb::storage::ClusterStoreReader;
using vdb::storage::ClusterStoreWriter;

uint32_t GetIntArg(int argc, char* argv[], const char* name, uint32_t default_val) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) {
            return static_cast<uint32_t>(std::stoul(argv[i + 1]));
        }
    }
    return default_val;
}

std::string GetStrArg(int argc, char* argv[], const char* name) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == name) {
            return argv[i + 1];
        }
    }
    return {};
}

bool CopyIfExists(const fs::path& src, const fs::path& dst) {
    if (!fs::exists(src)) return false;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing);
    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string src_dir = GetStrArg(argc, argv, "--src-index-dir");
    const std::string dst_dir = GetStrArg(argc, argv, "--dst-index-dir");
    const uint32_t address_block_size = GetIntArg(argc, argv, "--address-block-size", 64);
    const uint32_t address_page_size = GetIntArg(argc, argv, "--address-page-size", 1);

    if (src_dir.empty() || dst_dir.empty()) {
        std::cerr << "usage: " << argv[0]
                  << " --src-index-dir <dir> --dst-index-dir <dir>"
                  << " [--address-block-size 64] [--address-page-size 1]\n";
        return 2;
    }

    try {
        fs::create_directories(dst_dir);
    } catch (const std::exception& e) {
        std::cerr << "failed to create dst dir: " << e.what() << "\n";
        return 3;
    }

    ClusterStoreReader reader;
    Status s = reader.Open(src_dir + "/cluster.clu");
    if (!s.ok()) {
        std::cerr << "failed to open source cluster.clu: " << s.ToString() << "\n";
        return 4;
    }
    s = reader.PreloadAllClusters();
    if (!s.ok()) {
        std::cerr << "failed to preload resident clusters: " << s.ToString() << "\n";
        return 14;
    }

    const auto cluster_ids = reader.cluster_ids();
    ClusterStoreWriter writer;
    s = writer.Open(dst_dir + "/cluster.clu", reader.num_clusters(), reader.dim(),
                    reader.rabitq_config());
    if (!s.ok()) {
        std::cerr << "failed to open destination cluster.clu: " << s.ToString() << "\n";
        return 5;
    }

    uint64_t total_records = 0;
    for (uint32_t cid : cluster_ids) {
        s = reader.EnsureClusterLoaded(cid);
        if (!s.ok()) {
            std::cerr << "EnsureClusterLoaded(" << cid << ") failed: "
                      << s.ToString() << "\n";
            return 6;
        }

        const uint32_t count = reader.GetNumRecords(cid);
        total_records += count;
        std::vector<uint32_t> indices(count);
        for (uint32_t i = 0; i < count; ++i) {
            indices[i] = i;
        }

        std::vector<vdb::rabitq::RaBitQCode> codes;
        s = reader.LoadCodes(cid, indices, codes);
        if (!s.ok()) {
            std::cerr << "LoadCodes(" << cid << ") failed: " << s.ToString() << "\n";
            return 7;
        }

        std::vector<AddressEntry> addrs = reader.GetAddresses(cid, indices);
        const auto* resident = reader.GetResidentClusterView(cid);
        if (resident == nullptr) {
            std::cerr << "resident cluster view missing for cid=" << cid << "\n";
            return 15;
        }
        vdb::storage::EncodedAddressColumn addr_blocks =
            resident->addresses_are_raw_v2
                ? AddressColumn::EncodeRawTableV2(addrs, resident->address_page_size)
                : AddressColumn::Encode(addrs, address_block_size, address_page_size);

        s = writer.BeginCluster(cid, count, reader.GetCentroid(cid), reader.GetEpsilon(cid));
        if (!s.ok()) {
            std::cerr << "BeginCluster(" << cid << ") failed: " << s.ToString() << "\n";
            return 8;
        }
        s = writer.WriteVectors(codes);
        if (!s.ok()) {
            std::cerr << "WriteVectors(" << cid << ") failed: " << s.ToString() << "\n";
            return 9;
        }
        s = writer.WriteAddressBlocks(addr_blocks);
        if (!s.ok()) {
            std::cerr << "WriteAddressBlocks(" << cid << ") failed: " << s.ToString() << "\n";
            return 10;
        }
        s = writer.EndCluster();
        if (!s.ok()) {
            std::cerr << "EndCluster(" << cid << ") failed: " << s.ToString() << "\n";
            return 11;
        }
    }

    s = writer.Finalize("data.dat");
    if (!s.ok()) {
        std::cerr << "Finalize failed: " << s.ToString() << "\n";
        return 12;
    }

    try {
        CopyIfExists(fs::path(src_dir) / "segment.meta", fs::path(dst_dir) / "segment.meta");
        CopyIfExists(fs::path(src_dir) / "centroids.bin", fs::path(dst_dir) / "centroids.bin");
        CopyIfExists(fs::path(src_dir) / "rotation.bin", fs::path(dst_dir) / "rotation.bin");
        CopyIfExists(fs::path(src_dir) / "rotated_centroids.bin",
                     fs::path(dst_dir) / "rotated_centroids.bin");
        CopyIfExists(fs::path(src_dir) / "data.dat", fs::path(dst_dir) / "data.dat");
        CopyIfExists(fs::path(src_dir) / "build_metadata.json",
                     fs::path(dst_dir) / "build_metadata.json");
    } catch (const std::exception& e) {
        std::cerr << "copy sidecar files failed: " << e.what() << "\n";
        return 13;
    }

    std::cout << "migrated " << cluster_ids.size() << " clusters, "
              << total_records << " records\n";
    std::cout << "src=" << src_dir << "\n";
    std::cout << "dst=" << dst_dir << "\n";
    return 0;
}
