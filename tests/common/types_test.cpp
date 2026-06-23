#include <gtest/gtest.h>
#include "vdb/common/types.h"

namespace vdb {
namespace {

TEST(TypesTest, InvalidSentinelValues) {
  EXPECT_EQ(kInvalidVecID, std::numeric_limits<VecID>::max());
  EXPECT_EQ(kInvalidRowID, std::numeric_limits<RowID>::max());
  EXPECT_EQ(kInvalidSegmentID, std::numeric_limits<SegmentID>::max());
  EXPECT_EQ(kInvalidListID, std::numeric_limits<ListID>::max());
}

TEST(TypesTest, MetricTypeName) {
  EXPECT_EQ(MetricTypeName(MetricType::L2), "L2");
  EXPECT_EQ(MetricTypeName(MetricType::InnerProduct), "InnerProduct");
  EXPECT_EQ(MetricTypeName(MetricType::COSINE), "COSINE");
}

TEST(TypesTest, DTypeSize) {
  // Fixed-width types
  EXPECT_EQ(DTypeSize(DType::INT8), 1);
  EXPECT_EQ(DTypeSize(DType::INT16), 2);
  EXPECT_EQ(DTypeSize(DType::INT32), 4);
  EXPECT_EQ(DTypeSize(DType::INT64), 8);
  EXPECT_EQ(DTypeSize(DType::UINT8), 1);
  EXPECT_EQ(DTypeSize(DType::UINT16), 2);
  EXPECT_EQ(DTypeSize(DType::UINT32), 4);
  EXPECT_EQ(DTypeSize(DType::UINT64), 8);
  EXPECT_EQ(DTypeSize(DType::FLOAT16), 2);
  EXPECT_EQ(DTypeSize(DType::FLOAT32), 4);
  EXPECT_EQ(DTypeSize(DType::FLOAT64), 8);
  EXPECT_EQ(DTypeSize(DType::BOOL), 1);
  EXPECT_EQ(DTypeSize(DType::TIMESTAMP), 8);
  
  // Variable-width types return 0
  EXPECT_EQ(DTypeSize(DType::STRING), 0);
  EXPECT_EQ(DTypeSize(DType::BYTES), 0);
  EXPECT_EQ(DTypeSize(DType::VECTOR_FLOAT32), 0);
}

TEST(TypesTest, DTypeIsFixedWidth) {
  EXPECT_TRUE(DTypeIsFixedWidth(DType::INT32));
  EXPECT_TRUE(DTypeIsFixedWidth(DType::FLOAT32));
  EXPECT_FALSE(DTypeIsFixedWidth(DType::STRING));
  EXPECT_FALSE(DTypeIsFixedWidth(DType::BYTES));
  EXPECT_FALSE(DTypeIsFixedWidth(DType::VECTOR_FLOAT32));
}

TEST(TypesTest, DTypeIsVector) {
  EXPECT_TRUE(DTypeIsVector(DType::VECTOR_FLOAT32));
  EXPECT_TRUE(DTypeIsVector(DType::VECTOR_FLOAT16));
  EXPECT_TRUE(DTypeIsVector(DType::VECTOR_INT8));
  EXPECT_TRUE(DTypeIsVector(DType::VECTOR_UINT8));
  
  EXPECT_FALSE(DTypeIsVector(DType::INT32));
  EXPECT_FALSE(DTypeIsVector(DType::FLOAT32));
  EXPECT_FALSE(DTypeIsVector(DType::STRING));
}

TEST(TypesTest, DTypeName) {
  EXPECT_EQ(DTypeName(DType::INT32), "INT32");
  EXPECT_EQ(DTypeName(DType::FLOAT32), "FLOAT32");
  EXPECT_EQ(DTypeName(DType::STRING), "STRING");
  EXPECT_EQ(DTypeName(DType::VECTOR_FLOAT32), "VECTOR_FLOAT32");
}

TEST(TypesTest, SearchResultComparison) {
  SearchResult a{1, 0.5f, {}};
  SearchResult b{2, 1.0f, {}};
  
  EXPECT_TRUE(a < b);
  EXPECT_FALSE(b < a);
  EXPECT_TRUE(b > a);
  EXPECT_FALSE(a > b);
}

TEST(TypesTest, SearchResultWithPayload) {
  SearchResult result;
  result.id = 42;
  result.distance = 1.5f;
  result.payload[0] = Datum::Int64(100);
  result.payload[1] = Datum::String("hello");
  
  EXPECT_EQ(result.id, 42);
  EXPECT_EQ(result.payload.size(), 2);
  EXPECT_EQ(result.payload[0].dtype, DType::INT64);
  EXPECT_EQ(result.payload[0].fixed.i64, 100);
  EXPECT_EQ(result.payload[1].dtype, DType::STRING);
  EXPECT_EQ(result.payload[1].var_data, "hello");
}

TEST(TypesTest, Constants) {
  EXPECT_EQ(kDefaultBlockSize, 4096);
  EXPECT_EQ(kDefaultPageSize, 4096);
  EXPECT_EQ(kCacheLineSize, 64);
  EXPECT_EQ(kSimdWidth, 32);
  EXPECT_EQ(kSimd512Width, 64);
  EXPECT_EQ(kInlineThreshold, 4096);
}

// ============================================================================
// PayloadMode tests
// ============================================================================

TEST(TypesTest, PayloadModeName) {
  EXPECT_EQ(PayloadModeName(PayloadMode::kInline), "Inline");
  EXPECT_EQ(PayloadModeName(PayloadMode::kExtern), "Extern");
}

TEST(TypesTest, PayloadModeValues) {
  EXPECT_EQ(static_cast<uint8_t>(PayloadMode::kInline), 0);
  EXPECT_EQ(static_cast<uint8_t>(PayloadMode::kExtern), 1);
}

// ============================================================================
// ExternRef tests
// ============================================================================

TEST(TypesTest, ExternRefEquality) {
  ExternRef a{1, 4096, 2048};
  ExternRef b{1, 4096, 2048};
  ExternRef c{2, 4096, 2048};

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(TypesTest, ExternRefFields) {
  ExternRef ref{42, 1024000, 8192};
  EXPECT_EQ(ref.file_id, 42);
  EXPECT_EQ(ref.offset, 1024000);
  EXPECT_EQ(ref.length, 8192);
}

// ============================================================================
// RecordLocator tests
// ============================================================================

TEST(TypesTest, RecordLocatorEquality) {
  RecordLocator a{0, 512, PayloadMode::kInline};
  RecordLocator b{0, 512, PayloadMode::kInline};
  RecordLocator c{0, 512, PayloadMode::kExtern};

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(TypesTest, RecordLocatorFields) {
  RecordLocator loc{65536, 4000, PayloadMode::kInline};
  EXPECT_EQ(loc.offset, 65536);
  EXPECT_EQ(loc.length, 4000);
  EXPECT_EQ(loc.mode, PayloadMode::kInline);
}

TEST(TypesTest, InlineThresholdDecision) {
  // Payloads < kInlineThreshold => inline
  EXPECT_TRUE(100 < kInlineThreshold);
  EXPECT_TRUE(4095 < kInlineThreshold);
  // Payloads >= kInlineThreshold => extern
  EXPECT_FALSE(4096 < kInlineThreshold);
  EXPECT_FALSE(10000 < kInlineThreshold);
}

// ============================================================================
// Phase 2.5: New type aliases and sentinel values
// ============================================================================

TEST(TypesTest, ClusterIDAlias) {
  ClusterID cid = 42;
  EXPECT_EQ(cid, 42u);
  EXPECT_EQ(kInvalidClusterID, std::numeric_limits<ClusterID>::max());
}

TEST(TypesTest, FileIDAlias) {
  FileID fid = 1;
  EXPECT_EQ(fid, 1u);
  EXPECT_EQ(kInvalidFileID, std::numeric_limits<FileID>::max());
  // FileID is uint16_t
  EXPECT_EQ(sizeof(FileID), 2);
}

// ============================================================================
// Phase 2.5: ResultClass enum
// ============================================================================

TEST(TypesTest, ResultClassValues) {
  EXPECT_EQ(static_cast<uint8_t>(ResultClass::SafeIn), 0);
  EXPECT_EQ(static_cast<uint8_t>(ResultClass::SafeOut), 1);
  EXPECT_EQ(static_cast<uint8_t>(ResultClass::Uncertain), 2);
}

TEST(TypesTest, ResultClassName) {
  EXPECT_EQ(ResultClassName(ResultClass::SafeIn), "SafeIn");
  EXPECT_EQ(ResultClassName(ResultClass::SafeOut), "SafeOut");
  EXPECT_EQ(ResultClassName(ResultClass::Uncertain), "Uncertain");
}

// ============================================================================
// Phase 2.5: AddressEntry
// ============================================================================

TEST(TypesTest, AddressEntryEquality) {
  AddressEntry a{1024, 256};
  AddressEntry b{1024, 256};
  AddressEntry c{2048, 256};
  
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(TypesTest, AddressEntryFields) {
  AddressEntry addr{65536, 4096};
  EXPECT_EQ(addr.offset, 65536u);
  EXPECT_EQ(addr.size, 4096u);
}

// ============================================================================
// Phase 2.5: ReadTaskType enum
// ============================================================================

TEST(TypesTest, ReadTaskTypeValues) {
  EXPECT_EQ(static_cast<uint8_t>(ReadTaskType::VEC_ONLY), 0);
  EXPECT_EQ(static_cast<uint8_t>(ReadTaskType::ALL), 1);
  EXPECT_EQ(static_cast<uint8_t>(ReadTaskType::PAYLOAD), 2);
}

TEST(TypesTest, ReadTaskTypeName) {
  EXPECT_EQ(ReadTaskTypeName(ReadTaskType::VEC_ONLY), "VEC_ONLY");
  EXPECT_EQ(ReadTaskTypeName(ReadTaskType::ALL), "ALL");
  EXPECT_EQ(ReadTaskTypeName(ReadTaskType::PAYLOAD), "PAYLOAD");
}

// ============================================================================
// Phase 2.5: Candidate
// ============================================================================

TEST(TypesTest, CandidateOrdering) {
  // Candidate uses reverse ordering for max-heap (larger dist = "less than")
  Candidate near{0.5f, ResultClass::SafeIn, 0, 0};
  Candidate far{2.0f, ResultClass::Uncertain, 1, 5};

  // near has smaller dist → larger in max-heap sense
  EXPECT_TRUE(far < near);   // far.approx_dist > near.approx_dist → far < near
  EXPECT_TRUE(near > far);
}

TEST(TypesTest, CandidateFields) {
  Candidate cand{1.5f, ResultClass::SafeIn, 42, 7};
  EXPECT_FLOAT_EQ(cand.approx_dist, 1.5f);
  EXPECT_EQ(cand.result_class, ResultClass::SafeIn);
  EXPECT_EQ(cand.cluster_id, 42u);
  EXPECT_EQ(cand.local_idx, 7u);
}

// ============================================================================
// Phase 2.5: RaBitQConfig
// ============================================================================

TEST(TypesTest, RaBitQConfigDefaults) {
  RaBitQConfig config;
  EXPECT_EQ(config.bits, 1);
  EXPECT_EQ(config.block_size, 64u);
  EXPECT_FLOAT_EQ(config.c_factor, 5.75f);
  EXPECT_EQ(config.effective_total_bits(), 1u);
  EXPECT_EQ(config.stage2_payload_bits(), 0u);
  EXPECT_EQ(config.active_code_bits(), 1u);
  EXPECT_EQ(config.estimator_mode, RaBitQEstimatorMode::kLegacySignedMagnitude);
  EXPECT_EQ(config.exdata_layout, RaBitQExDataLayout::kGenericPacked);
}

TEST(TypesTest, RaBitQConfigEquality) {
  RaBitQConfig a{1, 64, 5.75f};
  RaBitQConfig b{1, 64, 5.75f};
  RaBitQConfig c{2, 64, 5.75f};
  
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST(TypesTest, RaBitQConfigCustom) {
  RaBitQConfig config{4, 128, 3.0f};
  EXPECT_EQ(config.bits, 4);
  EXPECT_EQ(config.block_size, 128u);
  EXPECT_FLOAT_EQ(config.c_factor, 3.0f);
  EXPECT_EQ(config.effective_total_bits(), 4u);
  EXPECT_EQ(config.stage2_payload_bits(), 4u);
  EXPECT_EQ(config.active_code_bits(), 4u);
}

TEST(TypesTest, RaBitQConfigOfficialBitSemantics) {
  RaBitQConfig config;
  config.total_bits = 4;
  config.ex_bits = 3;
  config.estimator_mode = RaBitQEstimatorMode::kOfficial1PlusN;

  EXPECT_TRUE(config.uses_official_1_plus_n());
  EXPECT_EQ(config.effective_total_bits(), 4u);
  EXPECT_EQ(config.stage2_payload_bits(), 3u);
  EXPECT_EQ(config.active_code_bits(), 3u);
  EXPECT_TRUE(config.official_bits_valid());
  EXPECT_EQ(RaBitQEstimatorModeName(config.estimator_mode), "official_1_plus_n");
  EXPECT_EQ(RaBitQFormatKey(config), "official_1_plus_n_total4_ex3");

  config.exdata_layout = RaBitQExDataLayout::kSplit3TwoPlusOne;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kSplit3TwoPlusOne);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total4_ex3_split3_2plus1");

  config.exdata_layout = RaBitQExDataLayout::kSplit3Bitplanes;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total4_ex3_split3_bitplanes");

  config.ex_bits = 2;
  config.total_bits = 3;
  EXPECT_FALSE(config.exdata_layout_valid());

  config.exdata_layout = RaBitQExDataLayout::kSplit2Bitplanes;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kSplit2Bitplanes);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total3_ex2_split2_bitplanes");

  config.ex_bits = 1;
  config.total_bits = 2;
  config.exdata_layout = RaBitQExDataLayout::kSplit1Bitplane;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kSplit1Bitplane);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total2_ex1_split1_bitplane");

  config.exdata_layout = RaBitQExDataLayout::kSelectedDirect;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total2_ex1_vector_bitmajor_tiles");

  config.ex_bits = 3;
  config.total_bits = 4;
  config.exdata_layout = RaBitQExDataLayout::kSplit3TrimmedBitplanes;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total4_ex3_split3_trimmed_bitplanes");

  config.exdata_layout = RaBitQExDataLayout::kSplit3ZeroPlaneElide;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total4_ex3_split3_zero_plane_elide");

  config.ex_bits = 4;
  config.total_bits = 5;
  config.exdata_layout = RaBitQExDataLayout::kVectorBitplanes;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kVectorBitplanes);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total5_ex4_vector_bitplanes");

  config.exdata_layout = RaBitQExDataLayout::kVectorBitplanesPrefetch;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(),
            RaBitQExDataLayout::kVectorBitplanesPrefetch);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total5_ex4_vector_bitplanes_prefetch");

  config.exdata_layout = RaBitQExDataLayout::kVectorBitplanesMicroBatch;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(),
            RaBitQExDataLayout::kVectorBitplanesMicroBatch);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total5_ex4_vector_bitplanes_microbatch");

  config.ex_bits = 3;
  config.total_bits = 4;
  config.exdata_layout = RaBitQExDataLayout::kVectorBitMajorTiles;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(),
            RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total4_ex3_vector_bitmajor_tiles");

  config.ex_bits = 4;
  config.total_bits = 5;
  config.exdata_layout = RaBitQExDataLayout::kSmallLane4Bitplanes;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kSmallLane4Bitplanes);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total5_ex4_small_lane4_bitplanes");

  config.exdata_layout = RaBitQExDataLayout::kSmallLane2Bitplanes;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kSmallLane2Bitplanes);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total5_ex4_small_lane2_bitplanes");

  config.exdata_layout = RaBitQExDataLayout::kVectorNibble4;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kVectorNibble4);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total5_ex4_vector_nibble4");

  config.ex_bits = 2;
  config.total_bits = 3;
  config.exdata_layout = RaBitQExDataLayout::kVector2Bit;
  EXPECT_TRUE(config.exdata_layout_valid());
  EXPECT_EQ(config.effective_exdata_layout(), RaBitQExDataLayout::kVector2Bit);
  EXPECT_EQ(RaBitQFormatKey(config),
            "official_1_plus_n_total3_ex2_vector_2bit");
}

TEST(TypesTest, RaBitQEstimatorModeParser) {
  RaBitQEstimatorMode mode = RaBitQEstimatorMode::kLegacySignedMagnitude;
  EXPECT_TRUE(ParseRaBitQEstimatorMode("official", &mode));
  EXPECT_EQ(mode, RaBitQEstimatorMode::kOfficial1PlusN);
  EXPECT_TRUE(ParseRaBitQEstimatorMode("legacy_signed_magnitude", &mode));
  EXPECT_EQ(mode, RaBitQEstimatorMode::kLegacySignedMagnitude);
  EXPECT_FALSE(ParseRaBitQEstimatorMode("auto", &mode));
}

TEST(TypesTest, RaBitQExDataLayoutParser) {
  RaBitQExDataLayout layout = RaBitQExDataLayout::kGenericPacked;
  EXPECT_TRUE(ParseRaBitQExDataLayout("split3_2plus1", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSplit3TwoPlusOne);
  EXPECT_TRUE(ParseRaBitQExDataLayout("1plus1plus1", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSplit3Bitplanes);
  EXPECT_TRUE(ParseRaBitQExDataLayout("1plus1", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSplit2Bitplanes);
  EXPECT_TRUE(ParseRaBitQExDataLayout("split1_bitplane", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSplit1Bitplane);
  EXPECT_TRUE(ParseRaBitQExDataLayout("trim_valid_lanes", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSplit3TrimmedBitplanes);
  EXPECT_TRUE(ParseRaBitQExDataLayout("zero_plane_elide", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSplit3ZeroPlaneElide);
  EXPECT_TRUE(ParseRaBitQExDataLayout("vector_compact", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kVectorBitplanes);
  EXPECT_TRUE(ParseRaBitQExDataLayout("vector_compact_prefetch", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kVectorBitplanesPrefetch);
  EXPECT_TRUE(ParseRaBitQExDataLayout("vector_microbatch", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kVectorBitplanesMicroBatch);
  EXPECT_TRUE(ParseRaBitQExDataLayout("cacheline_tiles", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_TRUE(ParseRaBitQExDataLayout("official_4bit_nibble", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kVectorNibble4);
  EXPECT_TRUE(ParseRaBitQExDataLayout("official_2bit", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kVector2Bit);
  EXPECT_TRUE(ParseRaBitQExDataLayout("small_lane4", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSmallLane4Bitplanes);
  EXPECT_TRUE(ParseRaBitQExDataLayout("small_lane2", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSmallLane2Bitplanes);
  EXPECT_TRUE(ParseRaBitQExDataLayout("selected_direct", &layout));
  EXPECT_EQ(layout, RaBitQExDataLayout::kSelectedDirect);
  EXPECT_EQ(RaBitQResolveSelectedExDataLayout(layout),
            RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQResolveSelectedExDataLayoutForBits(layout, 1),
            RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQResolveSelectedExDataLayoutForBits(layout, 2),
            RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQResolveSelectedExDataLayoutForBits(layout, 3),
            RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQResolveSelectedExDataLayoutForBits(layout, 4),
            RaBitQExDataLayout::kVectorNibble4);
  EXPECT_EQ(RaBitQDefaultOfficialExDataLayoutForBits(0),
            RaBitQExDataLayout::kGenericPacked);
  EXPECT_EQ(RaBitQDefaultOfficialExDataLayoutForBits(1),
            RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQDefaultOfficialExDataLayoutForBits(2),
            RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQDefaultOfficialExDataLayoutForBits(3),
            RaBitQExDataLayout::kVectorBitMajorTiles);
  EXPECT_EQ(RaBitQDefaultOfficialExDataLayoutForBits(4),
            RaBitQExDataLayout::kVectorNibble4);
  EXPECT_FALSE(ParseRaBitQExDataLayout("packed3", &layout));
}

}  // namespace
}  // namespace vdb
