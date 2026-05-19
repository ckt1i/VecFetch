#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <random>
#include <unistd.h>
#include <vector>

#include "vdb/common/types.h"
#include "vdb/query/parsed_cluster.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/cluster_store.h"

using namespace vdb;
using namespace vdb::storage;
using namespace vdb::rabitq;

namespace fs = std::filesystem;

class ClusterStoreTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_cluster_store_test";
        fs::create_directories(test_dir_);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    std::string TestPath(const std::string& name) {
        return (test_dir_ / name).string();
    }

    fs::path test_dir_;
};

// ============================================================================
// Writer tests
// ============================================================================

TEST_F(ClusterStoreTest, Writer_EmptyCluster) {
    ClusterStoreWriter writer;
    RaBitQConfig config{1, 64, 5.75f};
    const Dim dim = 128;

    ASSERT_TRUE(writer.Open(TestPath("test.clu"), 1, dim, config).ok());

    float centroid[128] = {0};
    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid).ok());

    std::vector<RaBitQCode> codes;
    {
        const auto s = writer.WriteVectors(codes);
        ASSERT_TRUE(s.ok()) << s.ToString();
    }

    EncodedAddressColumn blocks;
    ASSERT_TRUE(writer.WriteAddressBlocks(blocks).ok());

    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    auto& info = writer.info();
    EXPECT_EQ(info.num_clusters, 1u);
    EXPECT_EQ(info.dim, dim);
    EXPECT_EQ(info.lookup_table[0].num_records, 0u);
    EXPECT_EQ(info.data_file_path, "data.dat");
}

TEST_F(ClusterStoreTest, Writer_MultipleClusters) {
    const Dim dim = 64;
    const uint32_t K = 3;
    const uint32_t N_per_cluster = 10;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(123);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    for (uint32_t k = 0; k < K; ++k) {
        std::vector<float> centroid(dim, 0.0f);
        for (uint32_t d = 0; d < dim; ++d) centroid[d] = dist(rng);

        std::vector<RaBitQCode> codes;
        for (uint32_t i = 0; i < N_per_cluster; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
            codes.push_back(encoder.Encode(vec.data(), centroid.data()));
        }

        std::vector<AddressEntry> addrs;
        uint64_t off = k * 10000;
        for (uint32_t i = 0; i < N_per_cluster; ++i) {
            uint32_t sz = dim * sizeof(float);
            addrs.push_back({off, sz});
            off += sz;
        }
        auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

        float epsilon = 0.1f * (k + 1);
        ASSERT_TRUE(writer.BeginCluster(k, N_per_cluster, centroid.data(), epsilon).ok());
        ASSERT_TRUE(writer.WriteVectors(codes).ok());
        ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
        ASSERT_TRUE(writer.EndCluster().ok());
    }

    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    auto& info = writer.info();
    EXPECT_EQ(info.num_clusters, K);
    for (uint32_t k = 0; k < K; ++k) {
        EXPECT_EQ(info.lookup_table[k].cluster_id, k);
        EXPECT_EQ(info.lookup_table[k].num_records, N_per_cluster);
        EXPECT_GT(info.lookup_table[k].block_size, 0u);
        EXPECT_FLOAT_EQ(info.lookup_table[k].epsilon, 0.1f * (k + 1));
    }
}

// ============================================================================
// Reader tests
// ============================================================================

TEST_F(ClusterStoreTest, Reader_OpenAndReadLookupTable) {
    const Dim dim = 16;
    const uint32_t K = 2;
    std::string path = TestPath("test.clu");

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    std::vector<float> centroid0(dim);
    std::vector<float> centroid1(dim);
    for (uint32_t d = 0; d < dim; ++d) {
        centroid0[d] = static_cast<float>(d);
        centroid1[d] = static_cast<float>(d + 100);
    }

    // Cluster 0: empty, epsilon=0.25
    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid0.data(), 0.25f).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());

    // Cluster 1: empty, epsilon=0.50
    ASSERT_TRUE(writer.BeginCluster(1, 0, centroid1.data(), 0.50f).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());

    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    // Read back
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    EXPECT_EQ(reader.num_clusters(), K);
    EXPECT_EQ(reader.dim(), dim);
    EXPECT_EQ(reader.data_file_path(), "data.dat");

    auto ids = reader.cluster_ids();
    ASSERT_EQ(ids.size(), K);
    EXPECT_EQ(ids[0], 0u);
    EXPECT_EQ(ids[1], 1u);

    EXPECT_EQ(reader.GetNumRecords(0), 0u);
    EXPECT_EQ(reader.GetNumRecords(1), 0u);

    // Verify epsilon roundtrip
    EXPECT_FLOAT_EQ(reader.GetEpsilon(0), 0.25f);
    EXPECT_FLOAT_EQ(reader.GetEpsilon(1), 0.50f);

    // Verify centroids
    const float* c0 = reader.GetCentroid(0);
    ASSERT_NE(c0, nullptr);
    for (uint32_t d = 0; d < dim; ++d) {
        EXPECT_FLOAT_EQ(c0[d], centroid0[d]);
    }

    const float* c1 = reader.GetCentroid(1);
    ASSERT_NE(c1, nullptr);
    for (uint32_t d = 0; d < dim; ++d) {
        EXPECT_FLOAT_EQ(c1[d], centroid1[d]);
    }
}

TEST_F(ClusterStoreTest, Reader_LoadCodes) {
    const Dim dim = 128;
    const uint32_t N = 20;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(456);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    // Address entries
    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({off, dim * 4u});
        off += dim * 4;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    // Write single cluster
    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, N, centroid.data()).ok());
    {
        const auto s = writer.WriteVectors(codes);
        ASSERT_TRUE(s.ok()) << s.ToString();
    }
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    // Read individual codes
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());

    for (uint32_t i = 0; i < N; ++i) {
        std::vector<uint64_t> out_code;
        ASSERT_TRUE(reader.LoadCode(0, i, out_code).ok());
        ASSERT_EQ(out_code.size(), codes[i].code.size());
        for (size_t w = 0; w < out_code.size(); ++w) {
            EXPECT_EQ(out_code[w], codes[i].code[w])
                << "record " << i << " word " << w;
        }
    }
}

TEST_F(ClusterStoreTest, Reader_LoadCodes_Batch) {
    const Dim dim = 64;
    const uint32_t N = 30;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(789);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({off, dim * 4u});
        off += dim * 4;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, N, centroid.data()).ok());
    {
        const auto s = writer.WriteVectors(codes);
        ASSERT_TRUE(s.ok()) << s.ToString();
    }
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());

    // Batch load subset
    std::vector<uint32_t> indices = {0, 5, 10, 15, 29};
    std::vector<RaBitQCode> out_codes;
    ASSERT_TRUE(reader.LoadCodes(0, indices, out_codes).ok());

    ASSERT_EQ(out_codes.size(), indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        uint32_t idx = indices[i];
        ASSERT_EQ(out_codes[i].code.size(), codes[idx].code.size());
        for (size_t w = 0; w < out_codes[i].code.size(); ++w) {
            EXPECT_EQ(out_codes[i].code[w], codes[idx].code[w]);
        }
        // Verify norm and sum_x roundtrip (v5 format)
        EXPECT_FLOAT_EQ(out_codes[i].norm, codes[idx].norm)
            << "norm mismatch at index " << idx;
        EXPECT_EQ(out_codes[i].sum_x, codes[idx].sum_x)
            << "sum_x mismatch at index " << idx;
    }
}

TEST_F(ClusterStoreTest, Reader_LoadCodes_Batch_Bits4Roundtrip) {
    const Dim dim = 64;
    const uint32_t N = 37;
    std::string path = TestPath("test_bits4.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation, 4);

    std::mt19937 rng(1234);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t sz = dim * 4u + i;
        addrs.push_back({off, sz});
        off += sz;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{4, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, N, centroid.data()).ok());
    {
        const auto s = writer.WriteVectors(codes);
        ASSERT_TRUE(s.ok()) << s.ToString();
    }
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());

    std::vector<uint32_t> indices = {0, 1, 15, 31, 36};
    std::vector<RaBitQCode> out_codes;
    ASSERT_TRUE(reader.LoadCodes(0, indices, out_codes).ok());

    for (size_t i = 0; i < indices.size(); ++i) {
        const uint32_t idx = indices[i];
        EXPECT_FLOAT_EQ(out_codes[i].norm, codes[idx].norm);
        EXPECT_EQ(out_codes[i].sum_x, codes[idx].sum_x);
        ASSERT_EQ(out_codes[i].code.size(), 1u);
        ASSERT_FALSE(codes[idx].code.empty());
        EXPECT_EQ(out_codes[i].code[0], codes[idx].code[0]);
        EXPECT_EQ(out_codes[i].ex_code, codes[idx].ex_code);
        EXPECT_EQ(out_codes[i].ex_sign_packed, codes[idx].ex_sign_packed);
        EXPECT_FLOAT_EQ(out_codes[i].xipnorm, codes[idx].xipnorm);

        const auto addr = reader.GetAddress(0, idx);
        EXPECT_EQ(addr.offset, addrs[idx].offset);
        EXPECT_EQ(addr.size, addrs[idx].size);
    }
}

TEST_F(ClusterStoreTest, Reader_GetAddress) {
    const Dim dim = 32;
    const uint32_t N = 100;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation, 4);

    std::mt19937 rng(222);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    // Variable-size address entries
    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t sz = dim * 4u + i * 10;  // variable size
        addrs.push_back({off, sz});
        off += sz;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(7, N, centroid.data()).ok());
    {
        const auto s = writer.WriteVectors(codes);
        ASSERT_TRUE(s.ok()) << s.ToString();
    }
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c7.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(7).ok());

    // Verify address lookup
    for (uint32_t i = 0; i < N; ++i) {
        auto addr = reader.GetAddress(7, i);
        EXPECT_EQ(addr.offset, addrs[i].offset) << "record " << i;
        EXPECT_EQ(addr.size, addrs[i].size) << "record " << i;
    }

    // Batch addresses
    std::vector<uint32_t> indices = {0, 50, 99};
    auto batch_addrs = reader.GetAddresses(7, indices);
    ASSERT_EQ(batch_addrs.size(), 3u);
    for (size_t i = 0; i < indices.size(); ++i) {
        EXPECT_EQ(batch_addrs[i].offset, addrs[indices[i]].offset);
        EXPECT_EQ(batch_addrs[i].size, addrs[indices[i]].size);
    }
}

TEST_F(ClusterStoreTest, Writer_OrderConstraints) {
    const Dim dim = 16;
    std::string path = TestPath("test.clu");

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());

    float centroid[16] = {0};

    // Can't write vectors without BeginCluster
    EXPECT_FALSE(writer.WriteVectors({}).ok());

    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());

    // Can't write vectors twice
    EXPECT_FALSE(writer.WriteVectors({}).ok());

    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("test.dat").ok());

    // Can't finalize twice
    EXPECT_FALSE(writer.Finalize("test.dat").ok());
}

// ============================================================================
// Multi-cluster roundtrip: write multiple clusters, read back all data
// ============================================================================

TEST_F(ClusterStoreTest, MultiCluster_Roundtrip) {
    const Dim dim = 32;
    const uint32_t K = 4;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(200);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    // Storage for verification
    struct ClusterData {
        std::vector<float> centroid;
        std::vector<RaBitQCode> codes;
        std::vector<AddressEntry> addrs;
        uint32_t cluster_id;
        uint32_t N;
    };
    std::vector<ClusterData> cluster_data(K);

    for (uint32_t k = 0; k < K; ++k) {
        uint32_t N = 10 + k * 5;  // variable sizes
        cluster_data[k].cluster_id = k;
        cluster_data[k].N = N;
        cluster_data[k].centroid.resize(dim);
        for (uint32_t d = 0; d < dim; ++d)
            cluster_data[k].centroid[d] = dist(rng);

        for (uint32_t i = 0; i < N; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
            cluster_data[k].codes.push_back(
                encoder.Encode(vec.data(), cluster_data[k].centroid.data()));
        }

        uint64_t off = k * 100000;
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t sz = dim * 4u + i * 8;
            cluster_data[k].addrs.push_back({off, sz});
            off += sz;
        }
        auto addr_blocks = AddressColumn::Encode(
            cluster_data[k].addrs, 64, 1);

        ASSERT_TRUE(
            writer.BeginCluster(k, N, cluster_data[k].centroid.data()).ok());
        ASSERT_TRUE(writer.WriteVectors(cluster_data[k].codes).ok());
        ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
        ASSERT_TRUE(writer.EndCluster().ok());
    }

    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    // --- Read back ---
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    EXPECT_EQ(reader.num_clusters(), K);
    EXPECT_EQ(reader.rabitq_config().bits, 1u);
    EXPECT_EQ(reader.rabitq_config().block_size, 64u);
    EXPECT_FLOAT_EQ(reader.rabitq_config().c_factor, 5.75f);
    EXPECT_EQ(reader.data_file_path(), "data.dat");

    uint64_t expected_total = 0;
    for (uint32_t k = 0; k < K; ++k) {
        expected_total += cluster_data[k].N;
    }
    EXPECT_EQ(reader.total_records(), expected_total);

    for (uint32_t k = 0; k < K; ++k) {
        auto& cd = cluster_data[k];

        EXPECT_EQ(reader.GetNumRecords(k), cd.N);

        // Verify centroid
        const float* c = reader.GetCentroid(k);
        ASSERT_NE(c, nullptr);
        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_FLOAT_EQ(c[d], cd.centroid[d]);
        }

        // Load cluster data
        ASSERT_TRUE(reader.EnsureClusterLoaded(k).ok());

        // Verify codes
        for (uint32_t i = 0; i < cd.N; ++i) {
            std::vector<uint64_t> code;
            ASSERT_TRUE(reader.LoadCode(k, i, code).ok());
            ASSERT_EQ(code.size(), cd.codes[i].code.size());
            for (size_t w = 0; w < code.size(); ++w) {
                EXPECT_EQ(code[w], cd.codes[i].code[w]);
            }
        }

        // Verify addresses
        for (uint32_t i = 0; i < cd.N; ++i) {
            auto addr = reader.GetAddress(k, i);
            EXPECT_EQ(addr.offset, cd.addrs[i].offset);
            EXPECT_EQ(addr.size, cd.addrs[i].size);
        }
    }
}

TEST_F(ClusterStoreTest, Reader_EnsureClusterLoaded_Idempotent) {
    const Dim dim = 32;
    std::string path = TestPath("test.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(333);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    const uint32_t N = 5;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({off, dim * 4u});
        off += dim * 4;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(42, N, centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("d.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    // First load
    ASSERT_TRUE(reader.EnsureClusterLoaded(42).ok());
    // Second load should be no-op / idempotent
    ASSERT_TRUE(reader.EnsureClusterLoaded(42).ok());

    // Data should still be correct
    for (uint32_t i = 0; i < N; ++i) {
        auto addr = reader.GetAddress(42, i);
        EXPECT_EQ(addr.offset, addrs[i].offset);
        EXPECT_EQ(addr.size, addrs[i].size);
    }
}

TEST_F(ClusterStoreTest, Reader_InvalidCluster) {
    const Dim dim = 16;
    std::string path = TestPath("test.clu");

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    float centroid[16] = {0};
    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("d.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    // Non-existent cluster
    EXPECT_FALSE(reader.EnsureClusterLoaded(999).ok());
    EXPECT_EQ(reader.GetNumRecords(999), 0u);
    EXPECT_EQ(reader.GetCentroid(999), nullptr);
}

TEST_F(ClusterStoreTest, Reader_InvalidFile) {
    ClusterStoreReader reader;

    // Non-existent file
    EXPECT_FALSE(reader.Open("/nonexistent/path.clu").ok());

    // File with wrong magic
    std::string bad_magic = TestPath("bad.clu");
    {
        std::ofstream f(bad_magic, std::ios::binary);
        uint32_t bad = 0xDEADBEEF;
        f.write(reinterpret_cast<const char*>(&bad), 4);
        uint32_t ver = 2;
        f.write(reinterpret_cast<const char*>(&ver), 4);
    }
    EXPECT_FALSE(reader.Open(bad_magic).ok());
}

TEST_F(ClusterStoreTest, Reader_RejectsOldVersion) {
    std::string path = TestPath("old_version.clu");
    {
        std::ofstream f(path, std::ios::binary);
        std::vector<uint8_t> header(285, 0);
        uint32_t magic = 0x4C4D4356;
        uint32_t version = 2;
        std::memcpy(header.data(), &magic, sizeof(magic));
        std::memcpy(header.data() + 4, &version, sizeof(version));
        f.write(reinterpret_cast<const char*>(header.data()), header.size());
    }

    ClusterStoreReader reader;
    auto s = reader.Open(path);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.IsNotSupported());
}

TEST_F(ClusterStoreTest, Reader_RejectsVersion3) {
    // Version 3 used u64 base_offsets; version 4 uses u32.
    // Reader must reject version 3 files.
    std::string path = TestPath("v3.clu");
    {
        std::ofstream f(path, std::ios::binary);
        std::vector<uint8_t> header(285, 0);
        uint32_t magic = 0x4C4D4356;
        uint32_t version = 3;
        std::memcpy(header.data(), &magic, sizeof(magic));
        std::memcpy(header.data() + 4, &version, sizeof(version));
        f.write(reinterpret_cast<const char*>(header.data()), header.size());
    }

    ClusterStoreReader reader;
    auto s = reader.Open(path);
    EXPECT_FALSE(s.ok());
    EXPECT_TRUE(s.IsNotSupported());
}

// ============================================================================
// GetCodePtr — verify consistency with LoadCode
// ============================================================================

TEST_F(ClusterStoreTest, GetCodePtr_MatchesLoadCode) {
    const Dim dim = 64;
    RaBitQConfig config{1, 64, 5.75f};
    const uint32_t N = 16;

    // Generate random vectors and encode
    std::mt19937 rng(42);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::vector<float> centroid(dim, 0.0f);

    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (auto& v : vec) v = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    std::vector<AddressEntry> addrs;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({i * 512ULL, 512});
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    // Build a cluster store with one cluster
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(TestPath("codes.clu"), 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, N, centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    // Read back and verify GetCodePtr matches LoadCode
    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(TestPath("codes.clu")).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());

    const uint32_t nwords = (dim + 63) / 64;

    // v7: GetCodePtr returns block start, not per-entry pointer.
    // Verify LoadCode round-trips correctly (pack → unpack sign bits).
    for (uint32_t i = 0; i < N; ++i) {
        const uint8_t* ptr = reader.GetCodePtr(0, i);
        ASSERT_NE(ptr, nullptr);

        std::vector<uint64_t> loaded_code;
        ASSERT_TRUE(reader.LoadCode(0, i, loaded_code).ok());
        ASSERT_EQ(loaded_code.size(), nwords);

        // Verify the unpacked sign bits match the original code
        for (uint32_t w = 0; w < nwords; ++w) {
            EXPECT_EQ(loaded_code[w], codes[i].code[w])
                << "Code word " << w << " mismatch at record " << i;
        }
    }

    // Out-of-range: GetCodePtr returns nullptr (block beyond buffer)
    EXPECT_EQ(reader.GetCodePtr(0, N + 32), nullptr);
    // Non-loaded cluster returns nullptr
    EXPECT_EQ(reader.GetCodePtr(999, 0), nullptr);
}

// ============================================================================
// Phase 8: Async query support — clu_fd, GetBlockLocation, ParseClusterBlock
// ============================================================================

TEST_F(ClusterStoreTest, CluFd) {
    const Dim dim = 16;
    std::string path = TestPath("fd_test.clu");

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    float centroid[16] = {0};
    ASSERT_TRUE(writer.BeginCluster(0, 0, centroid).ok());
    ASSERT_TRUE(writer.WriteVectors({}).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(EncodedAddressColumn{}).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("d.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    EXPECT_GE(reader.clu_fd(), 0);

    reader.Close();
    EXPECT_LT(reader.clu_fd(), 0);
}

TEST_F(ClusterStoreTest, GetBlockLocation) {
    const Dim dim = 32;
    const uint32_t K = 3;
    const uint32_t N_per_cluster = 10;
    std::string path = TestPath("bloc_loc.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(500);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    for (uint32_t k = 0; k < K; ++k) {
        std::vector<float> centroid(dim, 0.0f);
        for (uint32_t d = 0; d < dim; ++d) centroid[d] = dist(rng);

        std::vector<RaBitQCode> codes;
        for (uint32_t i = 0; i < N_per_cluster; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
            codes.push_back(encoder.Encode(vec.data(), centroid.data()));
        }

        std::vector<AddressEntry> addrs;
        uint64_t off = k * 10000;
        for (uint32_t i = 0; i < N_per_cluster; ++i) {
            addrs.push_back({off, dim * 4u});
            off += dim * 4;
        }
        auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

        ASSERT_TRUE(writer.BeginCluster(k, N_per_cluster, centroid.data()).ok());
        ASSERT_TRUE(writer.WriteVectors(codes).ok());
        ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
        ASSERT_TRUE(writer.EndCluster().ok());
    }
    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    for (uint32_t k = 0; k < K; ++k) {
        auto loc = reader.GetBlockLocation(k);
        ASSERT_TRUE(loc.has_value()) << "cluster " << k;
        EXPECT_EQ(loc->offset, writer.info().lookup_table[k].block_offset);
        EXPECT_EQ(loc->size, writer.info().lookup_table[k].block_size);
        EXPECT_EQ(loc->num_records, N_per_cluster);
    }

    // Non-existent cluster
    EXPECT_FALSE(reader.GetBlockLocation(999).has_value());
}

TEST_F(ClusterStoreTest, ParseClusterBlock_MatchesEnsureClusterLoaded) {
    const Dim dim = 32;
    const uint32_t K = 3;
    std::string path = TestPath("parse_block.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(42);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(600);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    // Variable cluster sizes
    std::vector<uint32_t> cluster_sizes = {15, 0, 25};  // include empty cluster
    std::vector<std::vector<AddressEntry>> all_addrs(K);

    for (uint32_t k = 0; k < K; ++k) {
        uint32_t N = cluster_sizes[k];
        std::vector<float> centroid(dim, 0.0f);
        for (uint32_t d = 0; d < dim; ++d) centroid[d] = dist(rng);

        std::vector<RaBitQCode> codes;
        for (uint32_t i = 0; i < N; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
            codes.push_back(encoder.Encode(vec.data(), centroid.data()));
        }

        uint64_t off = k * 100000;
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t sz = dim * 4u + i * 8;
            all_addrs[k].push_back({off, sz});
            off += sz;
        }
        auto addr_blocks = (N > 0) ? AddressColumn::Encode(all_addrs[k], 64, 1)
                                     : EncodedAddressColumn{};

        ASSERT_TRUE(writer.BeginCluster(k, N, centroid.data()).ok());
        ASSERT_TRUE(writer.WriteVectors(codes).ok());
        ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
        ASSERT_TRUE(writer.EndCluster().ok());
    }
    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());

    for (uint32_t k = 0; k < K; ++k) {
        uint32_t N = cluster_sizes[k];

        // --- Path A: EnsureClusterLoaded (existing sync path) ---
        ASSERT_TRUE(reader.EnsureClusterLoaded(k).ok());

        // --- Path B: ParseClusterBlock (new async path) ---
        auto loc = reader.GetBlockLocation(k);
        ASSERT_TRUE(loc.has_value());

        constexpr size_t kAlignment = 4096;
        size_t alloc_size = static_cast<size_t>(loc->size);
        size_t padded_size = ((alloc_size + kAlignment - 1) / kAlignment) * kAlignment;
        auto* raw_block = static_cast<uint8_t*>(std::aligned_alloc(kAlignment, padded_size));
        ASSERT_NE(raw_block, nullptr);
        vdb::query::AlignedBufPtr block_buf(raw_block);
        ssize_t bytes = ::pread(reader.clu_fd(),
                                block_buf.get(),
                                static_cast<size_t>(loc->size),
                                static_cast<off_t>(loc->offset));
        ASSERT_EQ(bytes, static_cast<ssize_t>(loc->size));

        vdb::query::ParsedCluster pc;
        ASSERT_TRUE(reader.ParseClusterBlock(
            k, std::move(block_buf), loc->size, pc).ok());

        // --- Verify new layout consistency ---
        EXPECT_EQ(pc.num_records, N);
        EXPECT_EQ(pc.decoded_addresses.size(), N);

        uint32_t expected_blocks = (N + 31) / 32;
        EXPECT_EQ(pc.num_fastscan_blocks, expected_blocks);
        EXPECT_GT(pc.fastscan_block_size, 0u);

        if (N > 0) {
            ASSERT_NE(pc.fastscan_blocks, nullptr);
        }

        // Verify round-trip: unpack sign bits from ParsedCluster blocks
        // must match what LoadCode returns
        for (uint32_t i = 0; i < N; ++i) {
            // Verify addresses match
            auto sync_addr = reader.GetAddress(k, i);
            EXPECT_EQ(pc.decoded_addresses[i].offset, sync_addr.offset)
                << "cluster " << k << " record " << i;
            EXPECT_EQ(pc.decoded_addresses[i].size, sync_addr.size)
                << "cluster " << k << " record " << i;

            // Verify norm_oc matches (from FastScan block factors)
            uint32_t block_idx = i / 32;
            uint32_t vec_in_block = i % 32;
            float async_norm = pc.norm_oc(block_idx, vec_in_block);

            std::vector<rabitq::RaBitQCode> loaded;
            ASSERT_TRUE(reader.LoadCodes(k, {i}, loaded).ok());
            EXPECT_FLOAT_EQ(async_norm, loaded[0].norm)
                << "cluster " << k << " record " << i << " norm mismatch";
        }
    }
}

TEST_F(ClusterStoreTest, FullPreload_MatchesEnsureClusterLoaded) {
    const Dim dim = 32;
    const uint32_t K = 4;
    std::string path = TestPath("full_preload.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(7);
    RaBitQEncoder encoder(dim, rotation);

    std::mt19937 rng(321);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    RaBitQConfig config{1, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, K, dim, config).ok());

    std::vector<uint32_t> cluster_sizes = {11, 19, 0, 7};
    std::vector<std::vector<AddressEntry>> all_addrs(K);

    for (uint32_t k = 0; k < K; ++k) {
        uint32_t N = cluster_sizes[k];
        std::vector<float> centroid(dim, 0.0f);
        for (uint32_t d = 0; d < dim; ++d) centroid[d] = dist(rng);

        std::vector<RaBitQCode> codes;
        for (uint32_t i = 0; i < N; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
            codes.push_back(encoder.Encode(vec.data(), centroid.data()));
        }

        uint64_t off = k * 100000;
        for (uint32_t i = 0; i < N; ++i) {
            uint32_t sz = dim * 4u + i * 16;
            all_addrs[k].push_back({off, sz});
            off += sz;
        }
        auto addr_blocks = (N > 0) ? AddressColumn::Encode(all_addrs[k], 64, 1)
                                     : EncodedAddressColumn{};

        ASSERT_TRUE(writer.BeginCluster(k, N, centroid.data()).ok());
        ASSERT_TRUE(writer.WriteVectors(codes).ok());
        ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
        ASSERT_TRUE(writer.EndCluster().ok());
    }
    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.PreloadAllClusters().ok());
    ASSERT_TRUE(reader.resident_preload_enabled());
    EXPECT_GT(reader.resident_preload_bytes(), 0u);
    EXPECT_GT(reader.resident_cluster_mem_bytes(), 0u);

    for (uint32_t k = 0; k < K; ++k) {
        ASSERT_TRUE(reader.EnsureClusterLoaded(k).ok());
        const auto* resident = reader.GetResidentClusterView(k);
        const auto* resident_pc = reader.GetResidentParsedCluster(k);
        ASSERT_NE(resident, nullptr);
        ASSERT_NE(resident_pc, nullptr);

        EXPECT_EQ(resident->num_records, cluster_sizes[k]);
        EXPECT_EQ(resident->decoded_addresses.size(), cluster_sizes[k]);
        EXPECT_EQ(resident_pc->num_fastscan_blocks, (cluster_sizes[k] + 31) / 32);

        for (uint32_t i = 0; i < cluster_sizes[k]; ++i) {
            auto sync_addr = reader.GetAddress(k, i);
            EXPECT_EQ(resident->decoded_addresses[i].offset, sync_addr.offset)
                << "cluster " << k << " record " << i;
            EXPECT_EQ(resident->decoded_addresses[i].size, sync_addr.size)
                << "cluster " << k << " record " << i;

            uint32_t block_idx = i / 32;
            uint32_t vec_in_block = i % 32;
            float resident_norm = resident_pc->norm_oc(block_idx, vec_in_block);

            std::vector<rabitq::RaBitQCode> loaded;
            ASSERT_TRUE(reader.LoadCodes(k, {i}, loaded).ok());
            EXPECT_FLOAT_EQ(resident_norm, loaded[0].norm)
                << "cluster " << k << " record " << i << " norm mismatch";
        }
    }
}

TEST_F(ClusterStoreTest, ParseClusterBlock_Bits4MatchesLoadedData) {
    const Dim dim = 64;
    const uint32_t N = 37;
    std::string path = TestPath("parse_bits4.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(9);
    RaBitQEncoder encoder(dim, rotation, 4);

    std::mt19937 rng(999);
    std::normal_distribution<float> dist(0.0f, 1.0f);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) vec[d] = dist(rng);
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    std::vector<AddressEntry> addrs;
    uint64_t off = 0;
    for (uint32_t i = 0; i < N; ++i) {
        addrs.push_back({off, dim * 4u + i * 3u});
        off += dim * 4u + i * 3u;
    }
    auto addr_blocks = AddressColumn::Encode(addrs, 64, 1);

    RaBitQConfig config{4, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, N, centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());
    auto loc = reader.GetBlockLocation(0);
    ASSERT_TRUE(loc.has_value());

    constexpr size_t kAlignment = 4096;
    const size_t alloc_size = static_cast<size_t>(loc->size);
    const size_t padded_size =
        ((alloc_size + kAlignment - 1) / kAlignment) * kAlignment;
    auto* raw_block = static_cast<uint8_t*>(std::aligned_alloc(kAlignment, padded_size));
    ASSERT_NE(raw_block, nullptr);
    query::AlignedBufPtr block_buf(raw_block);
    const ssize_t bytes = ::pread(reader.clu_fd(), block_buf.get(), alloc_size,
                                  static_cast<off_t>(loc->offset));
    ASSERT_EQ(bytes, static_cast<ssize_t>(alloc_size));

    query::ParsedCluster parsed;
    ASSERT_TRUE(reader.ParseClusterBlock(0, std::move(block_buf), loc->size, parsed).ok());
    ASSERT_EQ(parsed.num_records, N);
    ASSERT_NE(parsed.exrabitq_entries, nullptr);

    for (uint32_t i = 0; i < N; ++i) {
        std::vector<RaBitQCode> loaded;
        ASSERT_TRUE(reader.LoadCodes(0, {i}, loaded).ok());
        const auto ex_view = parsed.exrabitq_view(i, dim);
        ASSERT_NE(ex_view.code_abs, nullptr);
        ASSERT_NE(ex_view.sign, nullptr);
        EXPECT_TRUE(std::equal(ex_view.code_abs, ex_view.code_abs + dim,
                               loaded[0].ex_code.begin()));
        EXPECT_TRUE(std::equal(ex_view.sign, ex_view.sign + dim,
                               loaded[0].ex_sign_packed.begin()));
        EXPECT_FLOAT_EQ(ex_view.xipnorm, loaded[0].xipnorm);
        EXPECT_EQ(parsed.decoded_addresses[i].offset, addrs[i].offset);
        EXPECT_EQ(parsed.decoded_addresses[i].size, addrs[i].size);
    }
}

TEST_F(ClusterStoreTest, ParseClusterBlock_V9RawAddressTableSupportsGaps) {
    const Dim dim = 32;
    const uint32_t N = 4;
    std::string path = TestPath("parse_v9_raw_gaps.clu");

    RotationMatrix rotation(dim);
    rotation.GenerateRandom(123);
    RaBitQEncoder encoder(dim, rotation, 4);

    std::vector<float> centroid(dim, 0.0f);
    std::vector<RaBitQCode> codes;
    for (uint32_t i = 0; i < N; ++i) {
        std::vector<float> vec(dim, static_cast<float>(i));
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    const uint32_t page_size = 4096;
    std::vector<AddressEntry> addrs = {
        {0ull * page_size, 1u * page_size},
        {4ull * page_size, 2u * page_size},
        {9ull * page_size, 1u * page_size},
        {20ull * page_size, 3u * page_size},
    };
    auto raw_table = AddressColumn::EncodeRawTableV2(addrs, page_size);

    RaBitQConfig config{4, 64, 5.75f};
    ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, N, centroid.data()).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(raw_table).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("c.dat").ok());

    ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    EXPECT_EQ(reader.file_version(), 9u);
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());
    auto loc = reader.GetBlockLocation(0);
    ASSERT_TRUE(loc.has_value());

    constexpr size_t kAlignment = 4096;
    const size_t alloc_size = static_cast<size_t>(loc->size);
    const size_t padded_size =
        ((alloc_size + kAlignment - 1) / kAlignment) * kAlignment;
    auto* raw_block = static_cast<uint8_t*>(std::aligned_alloc(kAlignment, padded_size));
    ASSERT_NE(raw_block, nullptr);
    query::AlignedBufPtr block_buf(raw_block);
    const ssize_t bytes = ::pread(reader.clu_fd(), block_buf.get(), alloc_size,
                                  static_cast<off_t>(loc->offset));
    ASSERT_EQ(bytes, static_cast<ssize_t>(alloc_size));

    query::ParsedCluster parsed;
    ASSERT_TRUE(reader.ParseClusterBlock(0, std::move(block_buf), loc->size, parsed).ok());
    ASSERT_TRUE(parsed.addresses_are_raw_v2);
    ASSERT_NE(parsed.raw_addresses, nullptr);
    ASSERT_EQ(parsed.address_page_size, page_size);
    ASSERT_EQ(parsed.num_records, N);

    for (uint32_t i = 0; i < N; ++i) {
        auto addr = parsed.AddressAt(i);
        EXPECT_EQ(addr.offset, addrs[i].offset);
        EXPECT_EQ(addr.size, addrs[i].size);
    }
}

TEST_F(ClusterStoreTest, ParsedClusterExRaBitQViewMatchesLegacyAccessors) {
    constexpr Dim dim = 32;
    constexpr uint32_t num_records = 5;
    const uint32_t ex_entry_size = 2 * dim + sizeof(float);
    std::vector<uint8_t> ex_entries(static_cast<size_t>(num_records) * ex_entry_size, 0);
    query::ParsedCluster pc;
    pc.exrabitq_entries = ex_entries.data();
    pc.exrabitq_entry_size = ex_entry_size;

    for (uint32_t i = 0; i < num_records; ++i) {
        for (uint32_t d = 0; d < dim; ++d) {
            ex_entries[static_cast<size_t>(i) * ex_entry_size + d] =
                static_cast<uint8_t>((i * 13u + d * 7u) & 0x0F);
            ex_entries[static_cast<size_t>(i) * ex_entry_size + dim + d] =
                static_cast<uint8_t>((i + d) & 1u);
        }
        const float xipnorm = 0.25f * static_cast<float>(i + 1);
        std::memcpy(ex_entries.data() + static_cast<size_t>(i) * ex_entry_size + 2 * dim,
                    &xipnorm, sizeof(float));
    }

    for (uint32_t i = 0; i < num_records; ++i) {
        const query::ParsedCluster::ExRaBitQView view = pc.exrabitq_view(i, dim);
        ASSERT_NE(view.code_abs, nullptr);
        ASSERT_NE(view.sign, nullptr);
        EXPECT_EQ(view.code_abs, pc.ex_code(i));
        EXPECT_EQ(view.sign, pc.ex_sign(i, dim));
        EXPECT_FLOAT_EQ(view.xipnorm, pc.xipnorm(i, dim));

        for (uint32_t d = 0; d < dim; ++d) {
            EXPECT_EQ(view.code_abs[d], pc.ex_code(i)[d])
                << "record=" << i << " dim=" << d;
            EXPECT_EQ(view.sign[d], pc.ex_sign(i, dim)[d])
                << "record=" << i << " dim=" << d;
        }
    }
}
