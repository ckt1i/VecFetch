#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "vdb/index/cluster_prober.h"
#include "vdb/index/conann.h"
#include "vdb/rabitq/rabitq_encoder.h"
#include "vdb/rabitq/rabitq_estimator.h"
#include "vdb/rabitq/rabitq_rotation.h"
#include "vdb/simd/ip_exrabitq.h"
#include "vdb/storage/address_column.h"
#include "vdb/storage/cluster_store.h"
#include "vdb/storage/pack_codes.h"

namespace {

class CollectingSink : public vdb::index::ProbeResultSink {
  public:
    void OnCandidates(const vdb::index::CandidateBatch& batch) override {
        batches.push_back(batch);
    }

    std::vector<vdb::index::CandidateBatch> batches;
};

std::filesystem::path TestDir() {
    return std::filesystem::temp_directory_path() /
           ("vdb_cluster_prober_test_" + std::to_string(::getpid()));
}

} // namespace

TEST(ClusterProberTest, OfficialV13Stage2ScoresUseOfficialFactors) {
    constexpr vdb::Dim dim = 64;
    constexpr uint32_t kRecords = 8;

    const std::filesystem::path test_dir = TestDir();
    std::filesystem::remove_all(test_dir);
    std::filesystem::create_directories(test_dir);
    const std::string path = (test_dir / "official.clu").string();

    vdb::RaBitQConfig config;
    config.estimator_mode = vdb::RaBitQEstimatorMode::kOfficial1PlusN;
    config.total_bits = 4;
    config.ex_bits = 3;
    config.bits = 3;

    vdb::rabitq::RotationMatrix rotation(dim);
    rotation.GenerateRandom(17);
    vdb::rabitq::RaBitQEncoder encoder(dim, rotation, config, 23);

    std::vector<float> centroid(dim);
    for (uint32_t d = 0; d < dim; ++d) {
        centroid[d] = 0.01f * static_cast<float>((static_cast<int>(d) % 5) - 2);
    }

    std::vector<vdb::rabitq::RaBitQCode> codes;
    codes.reserve(kRecords);
    for (uint32_t i = 0; i < kRecords; ++i) {
        std::vector<float> vec(dim);
        for (uint32_t d = 0; d < dim; ++d) {
            vec[d] = 0.02f * static_cast<float>(static_cast<int>(i) + 1) +
                     0.03f * static_cast<float>((static_cast<int>(d) % 11) - 5);
        }
        codes.push_back(encoder.Encode(vec.data(), centroid.data()));
    }

    std::vector<vdb::AddressEntry> addrs;
    uint64_t offset = 0;
    for (uint32_t i = 0; i < kRecords; ++i) {
        addrs.push_back({offset, dim * sizeof(float)});
        offset += dim * sizeof(float);
    }
    const auto addr_blocks = vdb::storage::AddressColumn::Encode(addrs, 64, 1);

    vdb::storage::ClusterStoreWriter writer;
    ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
    ASSERT_TRUE(writer.BeginCluster(0, kRecords, centroid.data(), 1.0f).ok());
    ASSERT_TRUE(writer.WriteVectors(codes).ok());
    ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
    ASSERT_TRUE(writer.EndCluster().ok());
    ASSERT_TRUE(writer.Finalize("data.dat").ok());

    vdb::storage::ClusterStoreReader reader;
    ASSERT_TRUE(reader.Open(path).ok());
    ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());
    const auto loc = reader.GetBlockLocation(0);
    ASSERT_TRUE(loc.has_value());

    constexpr size_t kAlignment = 4096;
    const size_t alloc_size = static_cast<size_t>(loc->size);
    const size_t padded_size = ((alloc_size + kAlignment - 1) / kAlignment) * kAlignment;
    auto* raw_block = static_cast<uint8_t*>(std::aligned_alloc(kAlignment, padded_size));
    ASSERT_NE(raw_block, nullptr);
    vdb::query::AlignedBufPtr block_buf(raw_block);
    const ssize_t bytes =
        ::pread(reader.clu_fd(), block_buf.get(), alloc_size, static_cast<off_t>(loc->offset));
    ASSERT_EQ(bytes, static_cast<ssize_t>(alloc_size));

    vdb::query::ParsedCluster parsed;
    ASSERT_TRUE(reader.ParseClusterBlock(0, std::move(block_buf), loc->size, parsed).ok());
    ASSERT_TRUE(parsed.uses_official_1_plus_n());
    ASSERT_EQ(parsed.rabitq_ex_bits, 3u);
    ASSERT_TRUE(parsed.exrabitq_magnitude_packed);

    std::vector<float> query(dim);
    for (uint32_t d = 0; d < dim; ++d) {
        query[d] = 0.04f * static_cast<float>((static_cast<int>(d) % 13) - 6);
    }

    vdb::rabitq::RaBitQEstimator estimator(dim, config.active_code_bits());
    vdb::rabitq::PreparedQuery pq;
    vdb::rabitq::ClusterPreparedScratch scratch;
    estimator.PrepareQueryInto(query.data(), centroid.data(), rotation, &pq, &scratch);

    vdb::rabitq::PreparedClusterQueryView view;
    view.prepared = &pq;
    view.scratch = &scratch;
    view.safein_margin_factor = 0.0f;
    view.safeout_margin_factor = 0.0f;

    const uint32_t packed_sz = vdb::storage::FastScanPackedSize(dim);
    const uint8_t* block_ptr = parsed.fastscan_blocks;
    const float* block_norms = reinterpret_cast<const float*>(block_ptr + packed_sz);
    alignas(64) float stage1_dists[32] = {};
    const auto stage1_eval = estimator.EvaluateStage1FastScan(
        view, block_ptr, block_norms, kRecords, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        /*enable_safein=*/false, stage1_dists);

    const auto block_view = parsed.exrabitq_batch_block_view(0);
    ASSERT_EQ(block_view.valid_count, kRecords);
    ASSERT_NE(block_view.official_factor_adds, nullptr);
    ASSERT_NE(block_view.official_factor_rescales, nullptr);
    std::vector<uint8_t> decoded_ex(static_cast<size_t>(parsed.exrabitq_num_dim_blocks) *
                                    parsed.exrabitq_batch_size * parsed.exrabitq_dim_block);
    ASSERT_TRUE(vdb::simd::ExRaBitQDecodePackedBatchBlockMagnitudes(
        block_view.abs_blocks, parsed.exrabitq_num_dim_blocks, parsed.exrabitq_batch_size,
        parsed.exrabitq_dim_block, block_view.abs_bytes_per_lane_dim_block,
        block_view.magnitude_bits, decoded_ex.data()));
    alignas(64) float ip_ex[kRecords] = {};
    vdb::simd::IPOfficialRaBitQBatchCompact(pq.rotated.data(), decoded_ex.data(), kRecords, dim,
                                            parsed.exrabitq_dim_block, ip_ex);

    vdb::index::ConANN conann(0.0f, 0.0f);
    vdb::index::ClusterProber prober(conann, dim, config.active_code_bits(),
                                     config.effective_total_bits());
    CollectingSink sink;
    vdb::index::ProbeStats stats;
    prober.Probe(parsed, 0, view, std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity(),
                 /*enable_address_decode_simd=*/true,
                 /*enable_fine_grained_timing=*/true,
                 /*enable_stage1_safein=*/false,
                 /*enable_stage2_collect_block_first=*/true,
                 /*enable_stage2_scatter_batch_classify=*/true, nullptr, nullptr, sink, stats);

    ASSERT_EQ(stats.num_stage2_candidates, kRecords);
    EXPECT_GT(stats.stage2_decode_blocks, 0u);
    ASSERT_EQ(sink.batches.size(), 1u);
    const auto& batch = sink.batches[0];
    ASSERT_EQ(batch.count, kRecords);

    for (uint32_t lane = 0; lane < kRecords; ++lane) {
        const float normalized_ip = vdb::simd::OfficialRaBitQCombineNormalizedIP(
            stage1_eval.ip_x0_qr[lane], ip_ex[lane], pq.sum_q, parsed.rabitq_ex_bits);
        const float expected = vdb::simd::OfficialRaBitQEstimateDistance(
            pq.norm_qc_sq, block_view.official_factor_adds[lane],
            block_view.official_factor_rescales[lane], pq.norm_qc * normalized_ip);
        EXPECT_EQ(batch.global_idx[lane], lane);
        EXPECT_EQ(batch.cls[lane], vdb::index::CandidateClass::Uncertain);
        EXPECT_NEAR(batch.est_dist[lane], expected, 1e-3f) << "lane=" << lane;
    }

    constexpr float kLowerEpsilon = 0.02f;
    constexpr float kUpperEpsilon = 0.05f;
    vdb::index::ClusterProber asymmetric_prober(
        conann, dim, config.active_code_bits(), config.effective_total_bits(),
        config.ex_bits, vdb::index::Stage2ErrorEnvelope{
                            true, kLowerEpsilon, kUpperEpsilon});
    CollectingSink asymmetric_sink;
    vdb::index::ProbeStats asymmetric_stats;
    asymmetric_prober.Probe(
        parsed, 0, view, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), true, false, false, true, true,
        nullptr, nullptr, asymmetric_sink, asymmetric_stats);
    ASSERT_EQ(asymmetric_sink.batches.size(), 1u);
    const auto& asymmetric_batch = asymmetric_sink.batches[0];
    ASSERT_EQ(asymmetric_batch.count, kRecords);
    for (uint32_t lane = 0; lane < kRecords; ++lane) {
        const float geometric_scale =
            2.0f * pq.norm_qc * block_norms[lane];
        EXPECT_NEAR(asymmetric_batch.est_dist[lane] -
                        asymmetric_batch.estimate_lower_bound[lane],
                    geometric_scale * kLowerEpsilon, 1e-5f);
        EXPECT_NEAR(asymmetric_batch.est_error[lane],
                    geometric_scale * kUpperEpsilon, 1e-5f);
        EXPECT_NEAR(asymmetric_batch.safein_upper_bound[lane] -
                        asymmetric_batch.est_dist[lane],
                    geometric_scale * kUpperEpsilon, 1e-5f);
    }

    vdb::rabitq::PreparedClusterQueryView stage1_view = view;
    stage1_view.safeout_margin_factor = 0.03f;
    stage1_view.safein_margin_factor = 0.07f;
    vdb::index::ClusterProber stage1_only_prober(
        conann, dim, config.active_code_bits(), config.effective_total_bits(), 0);
    CollectingSink stage1_sink;
    vdb::index::ProbeStats stage1_stats;
    stage1_only_prober.Probe(
        parsed, 0, stage1_view, std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), true, false, false, true, true,
        nullptr, nullptr, stage1_sink, stage1_stats);
    ASSERT_EQ(stage1_sink.batches.size(), 1u);
    const auto& stage1_batch = stage1_sink.batches[0];
    ASSERT_EQ(stage1_batch.count, kRecords);
    for (uint32_t lane = 0; lane < kRecords; ++lane) {
        EXPECT_NEAR(stage1_batch.est_dist[lane] -
                        stage1_batch.estimate_lower_bound[lane],
                    block_norms[lane] * stage1_view.safeout_margin_factor, 1e-5f);
        EXPECT_NEAR(stage1_batch.est_error[lane],
                    block_norms[lane] * stage1_view.safein_margin_factor, 1e-5f);
        EXPECT_NEAR(stage1_batch.safein_upper_bound[lane] -
                        stage1_batch.est_dist[lane],
                    block_norms[lane] * stage1_view.safein_margin_factor, 1e-5f);
    }

    std::filesystem::remove_all(test_dir);
}

TEST(ClusterProberTest, OfficialV14DirectStage2ScoresBypassDecode) {
    constexpr vdb::Dim dim = 64;
    constexpr uint32_t kRecords = 8;

    struct Case {
        vdb::RaBitQExDataLayout layout;
        uint8_t ex_bits;
        uint32_t expected_version;
    };
    for (const auto tc : {Case{vdb::RaBitQExDataLayout::kSplit1Bitplane, 1, 14},
                          Case{vdb::RaBitQExDataLayout::kSplit2Bitplanes, 2, 14},
                          Case{vdb::RaBitQExDataLayout::kSplit3TwoPlusOne, 3, 14},
                          Case{vdb::RaBitQExDataLayout::kSplit3Bitplanes, 3, 14},
                          Case{vdb::RaBitQExDataLayout::kSplit3TrimmedBitplanes, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kSplit3ZeroPlaneElide, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanes, 1, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanes, 2, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanes, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanes, 4, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanesPrefetch, 1, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanesPrefetch, 2, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanesPrefetch, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanesPrefetch, 4, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanesMicroBatch, 1, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanesMicroBatch, 2, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanesMicroBatch, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitplanesMicroBatch, 4, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitMajorTiles, 1, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitMajorTiles, 2, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorBitMajorTiles, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kTileLaneBitMajor, 1, 15},
                          Case{vdb::RaBitQExDataLayout::kTileLaneBitMajor, 2, 15},
                          Case{vdb::RaBitQExDataLayout::kTileLaneBitMajor, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kSmallLane4Bitplanes, 1, 15},
                          Case{vdb::RaBitQExDataLayout::kSmallLane4Bitplanes, 2, 15},
                          Case{vdb::RaBitQExDataLayout::kSmallLane4Bitplanes, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kSmallLane4Bitplanes, 4, 15},
                          Case{vdb::RaBitQExDataLayout::kSmallLane2Bitplanes, 1, 15},
                          Case{vdb::RaBitQExDataLayout::kSmallLane2Bitplanes, 2, 15},
                          Case{vdb::RaBitQExDataLayout::kSmallLane2Bitplanes, 3, 15},
                          Case{vdb::RaBitQExDataLayout::kSmallLane2Bitplanes, 4, 15},
                          Case{vdb::RaBitQExDataLayout::kVectorNibble4, 4, 15},
                          Case{vdb::RaBitQExDataLayout::kVector2Bit, 2, 15}}) {
        const auto layout = tc.layout;
        const uint8_t ex_bits = tc.ex_bits;
        const std::filesystem::path test_dir = TestDir();
        std::filesystem::remove_all(test_dir);
        std::filesystem::create_directories(test_dir);
        const std::string path =
            (test_dir / (std::string("official_") +
                         std::string(vdb::RaBitQExDataLayoutName(layout)) + ".clu"))
                .string();

        vdb::RaBitQConfig config;
        config.estimator_mode = vdb::RaBitQEstimatorMode::kOfficial1PlusN;
        config.total_bits = static_cast<uint8_t>(ex_bits + 1u);
        config.ex_bits = ex_bits;
        config.bits = ex_bits;
        config.exdata_layout = layout;

        vdb::rabitq::RotationMatrix rotation(dim);
        rotation.GenerateRandom(17);
        vdb::rabitq::RaBitQEncoder encoder(dim, rotation, config, 23);

        std::vector<float> centroid(dim);
        for (uint32_t d = 0; d < dim; ++d) {
            centroid[d] = 0.01f * static_cast<float>((static_cast<int>(d) % 5) - 2);
        }

        std::vector<vdb::rabitq::RaBitQCode> codes;
        codes.reserve(kRecords);
        for (uint32_t i = 0; i < kRecords; ++i) {
            std::vector<float> vec(dim);
            for (uint32_t d = 0; d < dim; ++d) {
                vec[d] = 0.02f * static_cast<float>(static_cast<int>(i) + 1) +
                         0.03f * static_cast<float>((static_cast<int>(d) % 11) - 5);
            }
            codes.push_back(encoder.Encode(vec.data(), centroid.data()));
        }

        std::vector<vdb::AddressEntry> addrs;
        uint64_t offset = 0;
        for (uint32_t i = 0; i < kRecords; ++i) {
            addrs.push_back({offset, dim * sizeof(float)});
            offset += dim * sizeof(float);
        }
        const auto addr_blocks = vdb::storage::AddressColumn::Encode(addrs, 64, 1);

        vdb::storage::ClusterStoreWriter writer;
        ASSERT_TRUE(writer.Open(path, 1, dim, config).ok());
        ASSERT_TRUE(writer.BeginCluster(0, kRecords, centroid.data(), 1.0f).ok());
        ASSERT_TRUE(writer.WriteVectors(codes).ok());
        ASSERT_TRUE(writer.WriteAddressBlocks(addr_blocks).ok());
        ASSERT_TRUE(writer.EndCluster().ok());
        ASSERT_TRUE(writer.Finalize("data.dat").ok());

        vdb::storage::ClusterStoreReader reader;
        ASSERT_TRUE(reader.Open(path).ok());
        EXPECT_EQ(reader.file_version(), tc.expected_version);
        ASSERT_TRUE(reader.EnsureClusterLoaded(0).ok());
        const auto loc = reader.GetBlockLocation(0);
        ASSERT_TRUE(loc.has_value());

        constexpr size_t kAlignment = 4096;
        const size_t alloc_size = static_cast<size_t>(loc->size);
        const size_t padded_size = ((alloc_size + kAlignment - 1) / kAlignment) * kAlignment;
        auto* raw_block = static_cast<uint8_t*>(std::aligned_alloc(kAlignment, padded_size));
        ASSERT_NE(raw_block, nullptr);
        vdb::query::AlignedBufPtr block_buf(raw_block);
        const ssize_t bytes =
            ::pread(reader.clu_fd(), block_buf.get(), alloc_size, static_cast<off_t>(loc->offset));
        ASSERT_EQ(bytes, static_cast<ssize_t>(alloc_size));

        vdb::query::ParsedCluster parsed;
        ASSERT_TRUE(reader.ParseClusterBlock(0, std::move(block_buf), loc->size, parsed).ok());
        ASSERT_TRUE(parsed.uses_official_1_plus_n());
        ASSERT_EQ(parsed.rabitq_ex_bits, ex_bits);
        ASSERT_EQ(parsed.rabitq_exdata_layout, layout);

        std::vector<float> query(dim);
        for (uint32_t d = 0; d < dim; ++d) {
            query[d] = 0.04f * static_cast<float>((static_cast<int>(d) % 13) - 6);
        }

        vdb::rabitq::RaBitQEstimator estimator(dim, config.active_code_bits());
        vdb::rabitq::PreparedQuery pq;
        vdb::rabitq::ClusterPreparedScratch scratch;
        estimator.PrepareQueryInto(query.data(), centroid.data(), rotation, &pq, &scratch);

        vdb::rabitq::PreparedClusterQueryView view;
        view.prepared = &pq;
        view.scratch = &scratch;
        view.safein_margin_factor = 0.0f;
        view.safeout_margin_factor = 0.0f;

        const uint32_t packed_sz = vdb::storage::FastScanPackedSize(dim);
        const uint8_t* block_ptr = parsed.fastscan_blocks;
        const float* block_norms = reinterpret_cast<const float*>(block_ptr + packed_sz);
        alignas(64) float stage1_dists[32] = {};
        const auto stage1_eval = estimator.EvaluateStage1FastScan(
            view, block_ptr, block_norms, kRecords, std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            /*enable_safein=*/false, stage1_dists);

        const auto block_view = parsed.exrabitq_batch_block_view(0);
        ASSERT_EQ(block_view.valid_count, kRecords);
        ASSERT_EQ(block_view.exdata_layout, layout);
        ASSERT_NE(block_view.official_factor_adds, nullptr);
        ASSERT_NE(block_view.official_factor_rescales, nullptr);

        alignas(64) float ip_ex[kRecords] = {};
        if (layout == vdb::RaBitQExDataLayout::kSplit3TwoPlusOne) {
            vdb::simd::IPOfficialRaBitQBatchCompactDirect3Masked(
                pq.rotated.data(), block_view.abs_blocks, layout, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kSplit3TrimmedBitplanes) {
            vdb::simd::IPOfficialRaBitQBatchCompactDirectBitplanesStridedMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, block_view.valid_count,
                0xFFu, kRecords, dim, parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kVectorBitplanes) {
            vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kVectorBitplanesPrefetch) {
            vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesPrefetchMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kVectorBitplanesMicroBatch) {
            vdb::simd::IPOfficialRaBitQBatchCompactVectorBitplanesMicroBatchMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kVectorBitMajorTiles) {
            vdb::simd::IPOfficialRaBitQBatchCompactVectorBitMajorTilesMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kTileLaneBitMajor) {
            vdb::simd::IPOfficialRaBitQBatchCompactTileLaneBitMajorMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kSmallLane4Bitplanes) {
            vdb::simd::IPOfficialRaBitQBatchCompactSmallLane4BitplanesMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kSmallLane2Bitplanes) {
            vdb::simd::IPOfficialRaBitQBatchCompactSmallLane2BitplanesMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kVectorNibble4) {
            vdb::simd::IPOfficialRaBitQBatchCompactVectorNibble4Masked(
                pq.rotated.data(), block_view.abs_blocks, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kVector2Bit) {
            vdb::simd::IPOfficialRaBitQBatchCompactVector2BitMasked(
                pq.rotated.data(), block_view.abs_blocks, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else if (layout == vdb::RaBitQExDataLayout::kSplit3ZeroPlaneElide) {
            vdb::simd::IPOfficialRaBitQBatchCompactDirect3ZeroPlaneElideMasked(
                pq.rotated.data(), block_view.abs_blocks, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        } else {
            vdb::simd::IPOfficialRaBitQBatchCompactDirectBitplanesMasked(
                pq.rotated.data(), block_view.abs_blocks, ex_bits, 0xFFu, kRecords, dim,
                parsed.exrabitq_dim_block, ip_ex);
        }

        vdb::index::ConANN conann(0.0f, 0.0f);
        vdb::index::ClusterProber prober(conann, dim, config.active_code_bits(),
                                         config.effective_total_bits());
        CollectingSink sink;
        vdb::index::ProbeStats stats;
        prober.Probe(parsed, 0, view, std::numeric_limits<float>::infinity(),
                     -std::numeric_limits<float>::infinity(),
                     /*enable_address_decode_simd=*/true,
                     /*enable_fine_grained_timing=*/true,
                     /*enable_stage1_safein=*/false,
                     /*enable_stage2_collect_block_first=*/true,
                     /*enable_stage2_scatter_batch_classify=*/true, nullptr, nullptr, sink, stats);

        ASSERT_EQ(stats.num_stage2_candidates, kRecords);
        EXPECT_EQ(stats.stage2_decode_blocks, 0u);
        ASSERT_EQ(sink.batches.size(), 1u);
        const auto& batch = sink.batches[0];
        ASSERT_EQ(batch.count, kRecords);

        for (uint32_t lane = 0; lane < kRecords; ++lane) {
            const float normalized_ip = vdb::simd::OfficialRaBitQCombineNormalizedIP(
                stage1_eval.ip_x0_qr[lane], ip_ex[lane], pq.sum_q, parsed.rabitq_ex_bits);
            const float expected = vdb::simd::OfficialRaBitQEstimateDistance(
                pq.norm_qc_sq, block_view.official_factor_adds[lane],
                block_view.official_factor_rescales[lane], pq.norm_qc * normalized_ip);
            EXPECT_EQ(batch.global_idx[lane], lane);
            EXPECT_EQ(batch.cls[lane], vdb::index::CandidateClass::Uncertain);
            EXPECT_NEAR(batch.est_dist[lane], expected, 1e-3f)
                << "layout=" << std::string(vdb::RaBitQExDataLayoutName(layout))
                << " lane=" << lane;
        }

        std::filesystem::remove_all(test_dir);
    }
}
