#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "vdb/common/distance.h"
#include "vdb/common/types.h"
#include "vdb/index/ivf_builder.h"
#include "vdb/index/ivf_index.h"
#include "vdb/query/async_reader.h"
#include "vdb/query/overlap_scheduler.h"

using namespace vdb;
using namespace vdb::index;
using namespace vdb::query;

namespace fs = std::filesystem;

// ============================================================================
// Test fixture
// ============================================================================

class PayloadPipelineTest : public ::testing::Test {
 protected:
    static constexpr uint32_t N = 128;
    static constexpr Dim kDim = 64;
    static constexpr uint32_t kNlist = 4;

    void SetUp() override {
        test_dir_ = (fs::temp_directory_path() / "vdb_payload_test").string();
        fs::create_directories(test_dir_);

        // Generate random vectors
        std::mt19937 rng(42);
        std::normal_distribution<float> dist(0.0f, 1.0f);
        vectors_.resize(static_cast<size_t>(N) * kDim);
        for (auto& v : vectors_) v = dist(rng);
    }

    void TearDown() override {
        fs::remove_all(test_dir_);
    }

    IvfBuilderConfig MakeConfig() {
        IvfBuilderConfig cfg;
        cfg.nlist = kNlist;
        cfg.max_iterations = 10;
        cfg.seed = 42;
        cfg.rabitq = {1, 64, 5.75f};
        cfg.calibration_samples = 10;
        cfg.calibration_topk = 5;
        cfg.page_size = 1;
        return cfg;
    }

    void MaterializeRecordSidecar(IvfIndex& index, const fs::path& store_dir,
                                  const char* vector_file,
                                  const char* payload_file,
                                  SeparateRecordMap* map) {
        ASSERT_NE(map, nullptr);
        fs::create_directories(store_dir);
        std::ofstream vector_out(store_dir / vector_file, std::ios::binary);
        std::ofstream payload_out(store_dir / payload_file, std::ios::binary);
        ASSERT_TRUE(vector_out.good());
        ASSERT_TRUE(payload_out.good());

        const uint32_t vec_bytes = index.logical_dim() * sizeof(float);
        std::unordered_set<uint64_t> seen_offsets;
        std::vector<uint8_t> record_buf;
        uint64_t row_id = 0;

        for (uint32_t cid : index.segment().cluster_ids()) {
            auto s = index.segment().EnsureClusterLoaded(cid);
            ASSERT_TRUE(s.ok()) << s.message();
            const uint32_t count = index.segment().GetNumRecords(cid);
            for (uint32_t ridx = 0; ridx < count; ++ridx) {
                const AddressEntry addr = index.segment().GetAddress(cid, ridx);
                if (!seen_offsets.insert(addr.offset).second) continue;
                ASSERT_GE(addr.size, vec_bytes);

                record_buf.resize(addr.size);
                s = index.segment().data_reader().ReadRaw(
                    addr.offset, addr.size, record_buf.data());
                ASSERT_TRUE(s.ok()) << s.message();

                vector_out.write(
                    reinterpret_cast<const char*>(record_buf.data()),
                    static_cast<std::streamsize>(vec_bytes));
                const uint64_t payload_offset =
                    static_cast<uint64_t>(payload_out.tellp());
                const uint32_t payload_bytes = addr.size - vec_bytes;
                if (payload_bytes > 0) {
                    payload_out.write(
                        reinterpret_cast<const char*>(record_buf.data() + vec_bytes),
                        static_cast<std::streamsize>(payload_bytes));
                }
                (*map)[addr.offset] =
                    SeparateRecordLocation{row_id, payload_offset, payload_bytes};
                ++row_id;
            }
        }
        vector_out.close();
        payload_out.close();
        ASSERT_TRUE(vector_out.good());
        ASSERT_TRUE(payload_out.good());
        ASSERT_EQ(map->size(), static_cast<size_t>(N));
    }

    void MaterializeSeparateStore(IvfIndex& index, const fs::path& store_dir,
                                  SeparateRecordMap* map) {
        MaterializeRecordSidecar(index, store_dir, "vector.dat", "payload.dat",
                                 map);
    }

    void MaterializeHotColdStore(IvfIndex& index, const fs::path& store_dir,
                                 SeparateRecordMap* map) {
        MaterializeRecordSidecar(index, store_dir, "hotvec.dat",
                                 "payload.cold.dat", map);
    }

    SearchResults SearchSeparateStore(IvfIndex& index,
                                      const SeparateRecordMap& map,
                                      int vector_fd,
                                      int payload_fd,
                                      uint32_t safein_threshold_bytes) {
        PreadFallbackReader reader;
        SearchConfig search_cfg;
        search_cfg.top_k = 5;
        search_cfg.nprobe = kNlist;
        search_cfg.enable_dynamic_safeout = false;
        search_cfg.safein_epsilon_override = 0.0f;
        search_cfg.safein_threshold_bytes = safein_threshold_bytes;
        search_cfg.separate_record_store.enabled = true;
        search_cfg.separate_record_store.vector_fd = vector_fd;
        search_cfg.separate_record_store.payload_fd = payload_fd;
        search_cfg.separate_record_store.address_map = &map;

        OverlapScheduler scheduler(index, reader, search_cfg);
        return scheduler.Search(vectors_.data());
    }

    std::string test_dir_;
    std::vector<float> vectors_;
};

// ============================================================================
// Test: Build with PayloadFn → Open → Search → verify payload
// ============================================================================

TEST_F(PayloadPipelineTest, BuildWithPayload_Roundtrip) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64, false},
        {1, "data", DType::BYTES, false},
    };

    IvfBuilder builder(cfg);

    // PayloadFn: each vector gets its index as id and a synthetic data blob
    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        std::string blob = "payload_data_" + std::to_string(vec_index);
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::Bytes(std::move(blob))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    // Open index
    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    // Verify payload schemas were persisted and restored
    ASSERT_EQ(index.payload_schemas().size(), 2u);
    EXPECT_EQ(index.payload_schemas()[0].name, "id");
    EXPECT_EQ(index.payload_schemas()[0].dtype, DType::INT64);
    EXPECT_EQ(index.payload_schemas()[1].name, "data");
    EXPECT_EQ(index.payload_schemas()[1].dtype, DType::BYTES);

    // Search and verify payloads
    PreadFallbackReader reader;
    SearchConfig search_cfg;
    search_cfg.top_k = 5;
    search_cfg.nprobe = kNlist;  // Probe all clusters
    search_cfg.safein_threshold_bytes = 0;  // Read vector prefix; payload is completed later.

    OverlapScheduler scheduler(index, reader, search_cfg);

    // Use vector 0 as the query → its own record should be the closest result
    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);

    // The closest result should have distance ≈ 0 (the query itself)
    EXPECT_NEAR(results[0].distance, 0.0f, 1e-3f);

    // Verify payload was read back correctly
    ASSERT_EQ(results[0].payload.size(), 2u);
    EXPECT_EQ(results[0].payload[0].dtype, DType::INT64);

    // The id in payload[0] should correspond to some original vector index
    int64_t result_id = results[0].payload[0].fixed.i64;
    EXPECT_GE(result_id, 0);
    EXPECT_LT(result_id, static_cast<int64_t>(N));

    // The data in payload[1] should match the expected synthetic blob
    std::string expected_blob = "payload_data_" + std::to_string(result_id);
    EXPECT_EQ(results[0].payload[1].dtype, DType::BYTES);
    EXPECT_EQ(results[0].payload[1].var_data, expected_blob);
}

TEST_F(PayloadPipelineTest, LateMaterializationFetchesPayloadAfterFinalTopK) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64, false},
        {1, "data", DType::BYTES, false},
    };

    IvfBuilder builder(cfg);
    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        std::string blob = "payload_data_" + std::to_string(vec_index);
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::Bytes(std::move(blob))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    PreadFallbackReader reader;
    SearchConfig search_cfg;
    search_cfg.top_k = 5;
    search_cfg.nprobe = kNlist;
    search_cfg.safein_threshold_bytes = std::numeric_limits<uint32_t>::max();
    search_cfg.materialization_mode = MaterializationMode::Late;

    OverlapScheduler scheduler(index, reader, search_cfg);
    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);

    ASSERT_EQ(results[0].payload.size(), 2u);
    const int64_t result_id = results[0].payload[0].fixed.i64;
    EXPECT_EQ(results[0].payload[1].dtype, DType::BYTES);
    EXPECT_EQ(results[0].payload[1].var_data,
              "payload_data_" + std::to_string(result_id));

    EXPECT_EQ(results.stats().all_read_requests, 0u);
    EXPECT_GT(results.stats().vec_only_read_requests, 0u);
    EXPECT_GT(results.stats().payload_read_requests, 0u);
}

TEST_F(PayloadPipelineTest, SeparateStoreSafeInPrefixThresholdFetchesSuffix) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64, false},
        {1, "data", DType::BYTES, false},
    };

    IvfBuilder builder(cfg);
    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        std::string blob(2048, static_cast<char>('a' + (vec_index % 26)));
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::Bytes(std::move(blob))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();
    index.OverrideConANN(/*epsilon=*/0.0f, /*legacy_d_k=*/1.0e30f,
                         /*safein_d_k=*/1.0e30f,
                         /*has_safein_d_k=*/true);

    const fs::path store_dir = fs::path(test_dir_) / "separate_store";
    SeparateRecordMap map;
    MaterializeSeparateStore(index, store_dir, &map);

    const fs::path vector_path = store_dir / "vector.dat";
    const fs::path payload_path = store_dir / "payload.dat";
    int vector_fd = ::open(vector_path.c_str(), O_RDONLY);
    ASSERT_GE(vector_fd, 0);
    int payload_fd = ::open(payload_path.c_str(), O_RDONLY);
    ASSERT_GE(payload_fd, 0);

    auto vector_only_results =
        SearchSeparateStore(index, map, vector_fd, payload_fd, 0);
    ASSERT_GT(vector_only_results.size(), 0u);
    EXPECT_GT(vector_only_results.stats().safein_prefix_read_requests, 0u);
    EXPECT_EQ(vector_only_results.stats().safein_prefix_read_bytes,
              static_cast<uint64_t>(
                  vector_only_results.stats().safein_prefix_read_requests) *
                  kDim * sizeof(float));
    EXPECT_EQ(vector_only_results.stats().safein_suffix_read_requests, 0u);
    ASSERT_EQ(vector_only_results[0].payload.size(), 2u);

    auto prefix_results =
        SearchSeparateStore(index, map, vector_fd, payload_fd, 512);
    ASSERT_GT(prefix_results.size(), 0u);
    EXPECT_GT(prefix_results.stats().safein_prefix_read_requests, 0u);
    EXPECT_GT(prefix_results.stats().safein_suffix_read_requests, 0u);
    ASSERT_EQ(prefix_results[0].payload.size(), 2u);
    const int64_t prefix_id = prefix_results[0].payload[0].fixed.i64;
    EXPECT_EQ(prefix_results[0].payload[1].var_data,
              std::string(2048, static_cast<char>('a' + (prefix_id % 26))));

    auto full_results = SearchSeparateStore(
        index, map, vector_fd, payload_fd,
        std::numeric_limits<uint32_t>::max());
    ASSERT_GT(full_results.size(), 0u);
    EXPECT_GT(full_results.stats().safein_full_read_requests, 0u);
    EXPECT_EQ(full_results.stats().safein_suffix_read_requests, 0u);
    ASSERT_EQ(full_results[0].payload.size(), 2u);

    ::close(payload_fd);
    ::close(vector_fd);
}

TEST_F(PayloadPipelineTest, HotColdStoreSafeInPrefixThresholdFetchesSuffix) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64, false},
        {1, "data", DType::BYTES, false},
    };

    IvfBuilder builder(cfg);
    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        std::string blob(2048, static_cast<char>('a' + (vec_index % 26)));
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::Bytes(std::move(blob))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();
    index.OverrideConANN(/*epsilon=*/0.0f, /*legacy_d_k=*/1.0e30f,
                         /*safein_d_k=*/1.0e30f,
                         /*has_safein_d_k=*/true);

    const fs::path store_dir = fs::path(test_dir_) / "hotcold_store";
    SeparateRecordMap map;
    MaterializeHotColdStore(index, store_dir, &map);

    const fs::path vector_path = store_dir / "hotvec.dat";
    const fs::path payload_path = store_dir / "payload.cold.dat";
    int vector_fd = ::open(vector_path.c_str(), O_RDONLY);
    ASSERT_GE(vector_fd, 0);
    int payload_fd = ::open(payload_path.c_str(), O_RDONLY);
    ASSERT_GE(payload_fd, 0);

    auto prefix_results =
        SearchSeparateStore(index, map, vector_fd, payload_fd, 512);
    ASSERT_GT(prefix_results.size(), 0u);
    EXPECT_EQ(prefix_results.stats().all_read_requests, 0u);
    EXPECT_GT(prefix_results.stats().vec_only_read_requests, 0u);
    EXPECT_GT(prefix_results.stats().payload_read_requests, 0u);
    EXPECT_GT(prefix_results.stats().safein_prefix_read_requests, 0u);
    EXPECT_GT(prefix_results.stats().safein_suffix_read_requests, 0u);
    ASSERT_EQ(prefix_results[0].payload.size(), 2u);
    const int64_t prefix_id = prefix_results[0].payload[0].fixed.i64;
    EXPECT_EQ(prefix_results[0].payload[1].var_data,
              std::string(2048, static_cast<char>('a' + (prefix_id % 26))));

    ::close(payload_fd);
    ::close(vector_fd);
}

TEST_F(PayloadPipelineTest, ShadowVectorStoreKeepsCombinedPayloadPath) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id", DType::INT64, false},
        {1, "data", DType::BYTES, false},
    };

    IvfBuilder builder(cfg);
    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::Bytes("shadow_payload_" + std::to_string(vec_index))};
    };
    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();
    index.OverrideConANN(/*epsilon=*/0.0f, /*legacy_d_k=*/1.0e30f,
                         /*safein_d_k=*/1.0e30f,
                         /*has_safein_d_k=*/true);

    const fs::path store_dir = fs::path(test_dir_) / "shadow_vector_store";
    SeparateRecordMap map;
    MaterializeHotColdStore(index, store_dir, &map);
    int vector_fd = ::open((store_dir / "hotvec.dat").c_str(), O_RDONLY);
    ASSERT_GE(vector_fd, 0);

    PreadFallbackReader reader;
    SearchConfig search_cfg;
    search_cfg.top_k = 5;
    search_cfg.nprobe = kNlist;
    search_cfg.enable_dynamic_safeout = false;
    search_cfg.safein_epsilon_override = 0.0f;
    search_cfg.materialization_mode = MaterializationMode::Late;
    search_cfg.separate_record_store.enabled = true;
    search_cfg.separate_record_store.redirect_payload_reads = false;
    search_cfg.separate_record_store.vector_fd = vector_fd;
    search_cfg.separate_record_store.payload_fd = -1;
    search_cfg.separate_record_store.address_map = &map;
    std::vector<vdb::query::VectorReadTraceEntry> trace;
    search_cfg.vector_read_trace = &trace;

    OverlapScheduler scheduler(index, reader, search_cfg);
    scheduler.SetVectorReadTraceQueryIndex(7);
    SearchResults results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);
    EXPECT_EQ(results.stats().all_read_requests, 0u);
    EXPECT_GT(results.stats().vec_only_read_requests, 0u);
    EXPECT_GT(results.stats().payload_read_requests, 0u);
    EXPECT_EQ(trace.size(), results.stats().vec_only_read_requests);
    EXPECT_TRUE(std::all_of(trace.begin(), trace.end(), [](const auto& entry) {
        return entry.query_index == 7 && entry.request_type == 0;
    }));
    EXPECT_EQ(std::count_if(trace.begin(), trace.end(), [](const auto& entry) {
                  return entry.selected_topk;
              }),
              results.size());
    ASSERT_EQ(results[0].payload.size(), 2u);
    const int64_t result_id = results[0].payload[0].fixed.i64;
    EXPECT_EQ(results[0].payload[1].var_data,
              "shadow_payload_" + std::to_string(result_id));

    ::close(vector_fd);
}

// ============================================================================
// Test: Build without PayloadFn → backward compat (empty payload)
// ============================================================================

TEST_F(PayloadPipelineTest, BuildWithoutPayload_BackwardCompat) {
    auto cfg = MakeConfig();
    // No payload_schemas, no PayloadFn

    IvfBuilder builder(cfg);
    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    // No payload schemas
    EXPECT_TRUE(index.payload_schemas().empty());

    // Search still works
    PreadFallbackReader reader;
    SearchConfig search_cfg;
    search_cfg.top_k = 5;
    search_cfg.nprobe = kNlist;

    OverlapScheduler scheduler(index, reader, search_cfg);
    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);

    // Payload should be empty
    EXPECT_TRUE(results[0].payload.empty());
}

// ============================================================================
// Test: Payload schemas round-trip through segment.meta
// ============================================================================

TEST_F(PayloadPipelineTest, SegmentMeta_PayloadSchemas_Roundtrip) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64,  false},
        {1, "text", DType::STRING, true},
    };

    IvfBuilder builder(cfg);

    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::String("item_" + std::to_string(vec_index))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    const auto& schemas = index.payload_schemas();
    ASSERT_EQ(schemas.size(), 2u);

    EXPECT_EQ(schemas[0].id, 0u);
    EXPECT_EQ(schemas[0].name, "id");
    EXPECT_EQ(schemas[0].dtype, DType::INT64);
    EXPECT_EQ(schemas[0].nullable, false);

    EXPECT_EQ(schemas[1].id, 1u);
    EXPECT_EQ(schemas[1].name, "text");
    EXPECT_EQ(schemas[1].dtype, DType::STRING);
    EXPECT_EQ(schemas[1].nullable, true);
}

// ============================================================================
// Test: Build with PayloadFn returning STRING payload
// ============================================================================

TEST_F(PayloadPipelineTest, BuildWithStringPayload) {
    auto cfg = MakeConfig();
    cfg.payload_schemas = {
        {0, "id",   DType::INT64,  false},
        {1, "text", DType::STRING, false},
    };

    IvfBuilder builder(cfg);

    PayloadFn payload_fn = [](uint32_t vec_index) -> std::vector<Datum> {
        return {Datum::Int64(static_cast<int64_t>(vec_index)),
                Datum::String("text_" + std::to_string(vec_index))};
    };

    auto s = builder.Build(vectors_.data(), N, kDim, test_dir_, payload_fn);
    ASSERT_TRUE(s.ok()) << s.message();

    IvfIndex index;
    s = index.Open(test_dir_);
    ASSERT_TRUE(s.ok()) << s.message();

    PreadFallbackReader reader;
    SearchConfig search_cfg;
    search_cfg.top_k = 5;
    search_cfg.nprobe = kNlist;
    search_cfg.safein_threshold_bytes = 0;

    OverlapScheduler scheduler(index, reader, search_cfg);
    auto results = scheduler.Search(vectors_.data());
    ASSERT_GT(results.size(), 0u);
    ASSERT_EQ(results[0].payload.size(), 2u);

    int64_t result_id = results[0].payload[0].fixed.i64;
    EXPECT_EQ(results[0].payload[1].dtype, DType::STRING);
    EXPECT_EQ(results[0].payload[1].var_data, "text_" + std::to_string(result_id));
}
