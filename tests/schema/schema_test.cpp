// =============================================================================
// FlatBuffers Schema Tests
// =============================================================================
// 验证 columns.fbs 和 segment_meta.fbs 的序列化/反序列化
// =============================================================================

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include <string>

// FlatBuffers generated headers
#include "columns_generated.h"
#include "segment_meta_generated.h"

// Note: We don't include vdb/common/types.h here to avoid MetricType conflict
// The schema defines its own MetricType in vdb::schema namespace

namespace vdb {
namespace test {

// Namespace alias for cleaner code
namespace schema = vdb::schema;

// =============================================================================
// columns.fbs Tests
// =============================================================================

class ColumnsSchemaTest : public ::testing::Test {
protected:
    flatbuffers::FlatBufferBuilder builder_{1024};
};

TEST_F(ColumnsSchemaTest, CreateChunkMeta) {
    // Create encoding params (Raw for simplicity)
    auto raw_params = schema::CreateRawEncodingParams(builder_);
    
    // Create ChunkMeta
    auto chunk = schema::CreateChunkMeta(
        builder_,
        0,                              // chunk_id
        0,                              // row_start
        1000,                           // row_count
        1000,                           // num_records
        8192,                           // offset_table_offset
        8000,                           // offset_table_length
        0,                              // data_offset
        4000,                           // data_length
        4000,                           // compressed_length (no compression)
        schema::EncodingType::RAW,      // encoding
        schema::EncodingParams::RawEncodingParams,
        raw_params.Union(),
        schema::CompressionType::NONE,  // compression
        0,                              // local_index (none)
        0,                              // aux_offset
        0,                              // aux_length
        0,                              // null_count
        100                             // distinct_count (estimate)
    );
    
    builder_.Finish(chunk);
    
    // Read back - use flatbuffers::GetRoot for non-root types
    auto* read_chunk = flatbuffers::GetRoot<schema::ChunkMeta>(builder_.GetBufferPointer());
    ASSERT_NE(read_chunk, nullptr);
    EXPECT_EQ(read_chunk->chunk_id(), 0);
    EXPECT_EQ(read_chunk->row_count(), 1000);
    EXPECT_EQ(read_chunk->num_records(), 1000);
    EXPECT_EQ(read_chunk->offset_table_offset(), 8192);
    EXPECT_EQ(read_chunk->offset_table_length(), 8000);
    EXPECT_EQ(read_chunk->data_length(), 4000);
    EXPECT_EQ(read_chunk->encoding(), schema::EncodingType::RAW);
    EXPECT_EQ(read_chunk->compression(), schema::CompressionType::NONE);
}

TEST_F(ColumnsSchemaTest, CreateColumnMeta) {
    // Create column name
    auto name = builder_.CreateString("user_id");
    
    // Create empty chunks vector for now
    std::vector<flatbuffers::Offset<schema::ChunkMeta>> chunks_vec;
    auto chunks = builder_.CreateVector(chunks_vec);
    
    // Create empty indexes vector
    std::vector<flatbuffers::Offset<schema::IndexMeta>> indexes_vec;
    auto indexes = builder_.CreateVector(indexes_vec);
    
    // Create ColumnMeta
    auto column = schema::CreateColumnMeta(
        builder_,
        0,                              // column_id
        name,                           // name
        schema::DataType::INT64,        // data_type
        false,                          // nullable
        true,                           // sorted
        schema::DataType::UNKNOWN,      // nested_type (not nested)
        chunks,                         // chunks
        indexes,                        // indexes
        1000000,                        // total_rows
        0,                              // null_count
        500000,                         // distinct_count
        schema::EncodingType::VARINT,   // default_encoding
        schema::CompressionType::LZ4,   // default_compression
        65536,                          // target_chunk_bytes
        10000,                          // max_chunk_rows
        1                               // sort_order (primary key)
    );
    
    builder_.Finish(column);
    
    // Read back
    auto* read_col = flatbuffers::GetRoot<schema::ColumnMeta>(builder_.GetBufferPointer());
    ASSERT_NE(read_col, nullptr);
    EXPECT_EQ(read_col->column_id(), 0);
    EXPECT_STREQ(read_col->name()->c_str(), "user_id");
    EXPECT_EQ(read_col->data_type(), schema::DataType::INT64);
    EXPECT_EQ(read_col->total_rows(), 1000000);
    EXPECT_EQ(read_col->sorted(), true);
    EXPECT_EQ(read_col->sort_order(), 1);
}

TEST_F(ColumnsSchemaTest, CreateColumnsFileHeader) {
    // Create column directory entries
    std::vector<flatbuffers::Offset<schema::ColumnDirectoryEntry>> dir_vec;
    dir_vec.push_back(schema::CreateColumnDirectoryEntry(
        builder_,
        0,          // column_id
        1024,       // col_meta_offset
        2048,       // chunk_index_offset
        4096,       // data_region_start
        1048576,    // data_region_size (1MB)
        0           // flags
    ));
    auto directory = builder_.CreateVector(dir_vec);
    
    // Create empty columns vector
    std::vector<flatbuffers::Offset<schema::ColumnMeta>> cols_vec;
    auto columns = builder_.CreateVector(cols_vec);
    
    // Create file header
    auto header = schema::CreateColumnsFileHeader(
        builder_,
        0x434F4C53,     // magic ("COLS")
        1,              // version
        1,              // num_columns
        1000000,        // total_rows
        512,            // directory_offset
        4096,           // data_region_offset
        1048576,        // metadata_region_offset
        directory,      // directory
        columns,        // columns
        10485760,       // uncompressed_size (10MB)
        5242880,        // compressed_size (5MB)
        1699999999,     // create_time
        0               // checksum
    );
    
    builder_.Finish(header, "COLS");
    
    // Verify buffer
    EXPECT_TRUE(flatbuffers::BufferHasIdentifier(
        builder_.GetBufferPointer(), "COLS"));
    
    // Read back - use GetColumnsFileHeader for root type
    auto* read_header = schema::GetColumnsFileHeader(builder_.GetBufferPointer());
    ASSERT_NE(read_header, nullptr);
    EXPECT_EQ(read_header->magic(), 0x434F4C53);
    EXPECT_EQ(read_header->version(), 1);
    EXPECT_EQ(read_header->num_columns(), 1);
    EXPECT_EQ(read_header->total_rows(), 1000000);
    EXPECT_EQ(read_header->directory()->size(), 1);
}

TEST_F(ColumnsSchemaTest, DataTypeEnumValues) {
    // Verify enum values are as expected
    EXPECT_EQ(static_cast<int>(schema::DataType::UNKNOWN), 0);
    EXPECT_EQ(static_cast<int>(schema::DataType::INT8), 1);
    EXPECT_EQ(static_cast<int>(schema::DataType::INT16), 2);
    EXPECT_EQ(static_cast<int>(schema::DataType::INT32), 3);
    EXPECT_EQ(static_cast<int>(schema::DataType::INT64), 4);
    EXPECT_EQ(static_cast<int>(schema::DataType::FLOAT32), 9);
    EXPECT_EQ(static_cast<int>(schema::DataType::STRING), 11);
}

TEST_F(ColumnsSchemaTest, EncodingTypeEnumValues) {
    EXPECT_EQ(static_cast<int>(schema::EncodingType::RAW), 0);
    EXPECT_EQ(static_cast<int>(schema::EncodingType::BITPACK), 1);
    EXPECT_EQ(static_cast<int>(schema::EncodingType::DICT), 2);
    EXPECT_EQ(static_cast<int>(schema::EncodingType::RLE), 3);
    EXPECT_EQ(static_cast<int>(schema::EncodingType::VARINT), 4);
}

TEST_F(ColumnsSchemaTest, ZonemapParams) {
    // Create ZoneMap with min/max values
    std::vector<int8_t> min_val = {0, 0, 0, 0};  // int32 = 0
    std::vector<int8_t> max_val = {-1, -1, -1, 127};  // int32 = INT_MAX
    
    auto min_vec = builder_.CreateVector(min_val);
    auto max_vec = builder_.CreateVector(max_val);
    
    auto zonemap = schema::CreateZonemapParams(
        builder_,
        min_vec,    // min_value
        max_vec,    // max_value
        10,         // null_count
        true        // has_null
    );
    
    builder_.Finish(zonemap);
    
    auto* read_zm = flatbuffers::GetRoot<schema::ZonemapParams>(builder_.GetBufferPointer());
    ASSERT_NE(read_zm, nullptr);
    EXPECT_EQ(read_zm->null_count(), 10);
    EXPECT_EQ(read_zm->has_null(), true);
    EXPECT_EQ(read_zm->min_value()->size(), 4);
}

// =============================================================================
// segment_meta.fbs Tests
// =============================================================================

class SegmentMetaSchemaTest : public ::testing::Test {
protected:
    flatbuffers::FlatBufferBuilder builder_{4096};
};

TEST_F(SegmentMetaSchemaTest, CreateIvfParams) {
    auto ivf = schema::CreateIvfParams(
        builder_,
        1024,       // nlist
        16,         // nprobe
        0,          // centroids_offset
        4194304,    // centroids_length (4MB)
        100000,     // training_vectors
        20,         // kmeans_iterations
        100,        // min_list_size
        2000,       // max_list_size
        975.5f      // avg_list_size
    );
    
    builder_.Finish(ivf);
    
    auto* read_ivf = flatbuffers::GetRoot<schema::IvfParams>(builder_.GetBufferPointer());
    ASSERT_NE(read_ivf, nullptr);
    EXPECT_EQ(read_ivf->nlist(), 1024);
    EXPECT_EQ(read_ivf->nprobe(), 16);
    EXPECT_EQ(read_ivf->training_vectors(), 100000);
    EXPECT_NEAR(read_ivf->avg_list_size(), 975.5f, 0.01f);
}

TEST_F(SegmentMetaSchemaTest, CreatePqParams) {
    auto pq = schema::CreatePqParams(
        builder_,
        16,         // m (subspaces)
        256,        // ks (clusters per subspace)
        8,          // nbits
        0,          // codebook_offset
        262144,     // codebook_length (256KB)
        50000,      // training_vectors
        8           // subvector_dim (128/16)
    );
    
    builder_.Finish(pq);
    
    auto* read_pq = flatbuffers::GetRoot<schema::PqParams>(builder_.GetBufferPointer());
    ASSERT_NE(read_pq, nullptr);
    EXPECT_EQ(read_pq->m(), 16);
    EXPECT_EQ(read_pq->ks(), 256);
    EXPECT_EQ(read_pq->nbits(), 8);
    EXPECT_EQ(read_pq->subvector_dim(), 8);
}

TEST_F(SegmentMetaSchemaTest, CreateInvertedListMeta) {
    auto ivl = schema::CreateInvertedListMeta(
        builder_,
        42,         // list_id
        1500,       // size
        0,          // ids_offset
        6000,       // ids_length (1500 * 4)
        6000,       // codes_offset
        24000,      // codes_length (1500 * 16 for M=16)
        0,          // raw_offset (not stored)
        0,          // raw_length
        131072,     // record_offsets_offset
        12000,      // record_offsets_length (1500 * 8)
        0           // checksum
    );
    
    builder_.Finish(ivl);
    
    auto* read_ivl = flatbuffers::GetRoot<schema::InvertedListMeta>(builder_.GetBufferPointer());
    ASSERT_NE(read_ivl, nullptr);
    EXPECT_EQ(read_ivl->list_id(), 42);
    EXPECT_EQ(read_ivl->size(), 1500);
    EXPECT_EQ(read_ivl->codes_length(), 24000);
    EXPECT_EQ(read_ivl->record_offsets_offset(), 131072);
    EXPECT_EQ(read_ivl->record_offsets_length(), 12000);
}

TEST_F(SegmentMetaSchemaTest, CreateSegmentMeta) {
    // Create IVF params
    auto ivf = schema::CreateIvfParams(builder_, 1024, 16, 0, 4194304, 100000, 20, 100, 2000, 975.5f);
    
    // Create PQ params
    auto pq = schema::CreatePqParams(builder_, 16, 256, 8, 0, 262144, 50000, 8);
    
    // Create empty inverted lists vector
    std::vector<flatbuffers::Offset<schema::InvertedListMeta>> ivl_vec;
    auto ivl_list = builder_.CreateVector(ivl_vec);
    
    // Create empty files vector
    std::vector<flatbuffers::Offset<schema::FileRef>> files_vec;
    auto files = builder_.CreateVector(files_vec);
    
    // Create stats
    auto stats = schema::CreateSegmentStats(
        builder_,
        1000000,    // total_vectors
        5000,       // deleted_vectors
        995000,     // active_vectors
        512000000,  // raw_vector_bytes (512MB)
        67108864,   // index_bytes (64MB)
        134217728,  // column_bytes (128MB)
        713326592,  // total_bytes
        0.72f,      // compression_ratio
        0, 0, 0     // query stats (runtime)
    );
    
    // Create LSN info
    auto lsn = schema::CreateLsnInfo(builder_, 0, 100000, 99000, 90000);
    
    // Create delete bitmap meta
    auto del_bitmap = schema::CreateDeleteBitmapMeta(builder_, 5000, 0, 125000, 1699999999);
    
    // Create empty parent segments
    std::vector<uint64_t> parents_vec;
    auto parents = builder_.CreateVector(parents_vec);
    
    // Create SegmentMeta
    auto segment = schema::CreateSegmentMeta(
        builder_,
        1,                              // segment_id
        1,                              // version
        schema::SegmentState::ACTIVE,   // state
        128,                            // dimension
        schema::MetricType::L2,         // metric_type
        schema::VectorDType::FLOAT32,   // vector_dtype
        ivf,                            // ivf_params
        pq,                             // pq_params
        0,                              // opq_params (none)
        0,                              // hnsw_params (none)
        0,                              // rabitq_params (none for legacy test)
        0,                              // conann_params (none for legacy test)
        0,                              // crc_params (none for legacy test)
        0,                              // clusters (none for legacy test)
        ivl_list,                       // inverted_lists
        files,                          // files
        del_bitmap,                     // delete_bitmap
        stats,                          // stats
        lsn,                            // lsn_info
        1699999000,                     // create_time
        1699999999,                     // last_update_time
        0,                              // last_compact_time
        0,                              // level
        parents,                        // parent_segments
        0                               // checksum
    );
    
    builder_.Finish(segment, "VSEG");
    
    // Verify buffer
    EXPECT_TRUE(flatbuffers::BufferHasIdentifier(
        builder_.GetBufferPointer(), "VSEG"));
    
    // Read back - use GetSegmentMeta for root type
    auto* read_seg = schema::GetSegmentMeta(builder_.GetBufferPointer());
    ASSERT_NE(read_seg, nullptr);
    EXPECT_EQ(read_seg->segment_id(), 1);
    EXPECT_EQ(read_seg->state(), schema::SegmentState::ACTIVE);
    EXPECT_EQ(read_seg->dimension(), 128);
    EXPECT_EQ(read_seg->metric_type(), schema::MetricType::L2);
    EXPECT_EQ(read_seg->vector_dtype(), schema::VectorDType::FLOAT32);
    
    // Check nested structs
    ASSERT_NE(read_seg->ivf_params(), nullptr);
    EXPECT_EQ(read_seg->ivf_params()->nlist(), 1024);
    
    ASSERT_NE(read_seg->pq_params(), nullptr);
    EXPECT_EQ(read_seg->pq_params()->m(), 16);
    
    ASSERT_NE(read_seg->stats(), nullptr);
    EXPECT_EQ(read_seg->stats()->total_vectors(), 1000000);
    EXPECT_EQ(read_seg->stats()->active_vectors(), 995000);
}

TEST_F(SegmentMetaSchemaTest, CreateSegmentDirectory) {
    // Create directory entries
    std::vector<flatbuffers::Offset<schema::SegmentDirectoryEntry>> entries;
    
    auto meta_file1 = builder_.CreateString("segment_1.vseg");
    entries.push_back(schema::CreateSegmentDirectoryEntry(
        builder_, 1, schema::SegmentState::ACTIVE, 0, 500000, meta_file1, 1699999000));
    
    auto meta_file2 = builder_.CreateString("segment_2.vseg");
    entries.push_back(schema::CreateSegmentDirectoryEntry(
        builder_, 2, schema::SegmentState::ACTIVE, 0, 500000, meta_file2, 1699999500));
    
    auto segments = builder_.CreateVector(entries);
    
    // Create directory
    auto dir = schema::CreateSegmentDirectory(
        builder_,
        0x56444253,             // magic ("VDBS")
        1,                      // version
        segments,               // segments
        128,                    // dimension
        schema::MetricType::L2, // metric_type
        2,                      // active_segment_id
        0,                      // last_compact_time
        0,                      // compact_generation
        1000000,                // total_vectors
        2,                      // total_segments
        100000,                 // global_lsn
        0                       // checksum
    );
    
    builder_.Finish(dir);
    
    auto* read_dir = flatbuffers::GetRoot<schema::SegmentDirectory>(builder_.GetBufferPointer());
    ASSERT_NE(read_dir, nullptr);
    EXPECT_EQ(read_dir->magic(), 0x56444253);
    EXPECT_EQ(read_dir->total_segments(), 2);
    EXPECT_EQ(read_dir->total_vectors(), 1000000);
    EXPECT_EQ(read_dir->segments()->size(), 2);
    
    // Check first entry
    auto* entry0 = read_dir->segments()->Get(0);
    EXPECT_EQ(entry0->segment_id(), 1);
    EXPECT_EQ(entry0->vector_count(), 500000);
}

TEST_F(SegmentMetaSchemaTest, FileRefTypes) {
    auto filename = builder_.CreateString("segment_1/data.cols");
    
    auto file_ref = schema::CreateFileRef(
        builder_,
        schema::FileType::COLUMNS,  // file_type
        filename,                   // file_name
        134217728,                  // file_size (128MB)
        0xDEADBEEF,                 // checksum
        1699999000                  // create_time
    );
    
    builder_.Finish(file_ref);
    
    auto* read_ref = flatbuffers::GetRoot<schema::FileRef>(builder_.GetBufferPointer());
    ASSERT_NE(read_ref, nullptr);
    EXPECT_EQ(read_ref->file_type(), schema::FileType::COLUMNS);
    EXPECT_STREQ(read_ref->file_name()->c_str(), "segment_1/data.cols");
    EXPECT_EQ(read_ref->file_size(), 134217728);
}

TEST_F(SegmentMetaSchemaTest, SegmentStateEnum) {
    EXPECT_EQ(static_cast<int>(schema::SegmentState::BUILDING), 0);
    EXPECT_EQ(static_cast<int>(schema::SegmentState::ACTIVE), 1);
    EXPECT_EQ(static_cast<int>(schema::SegmentState::MERGING), 2);
    EXPECT_EQ(static_cast<int>(schema::SegmentState::SEALED), 3);
    EXPECT_EQ(static_cast<int>(schema::SegmentState::DELETED), 4);
}

TEST_F(SegmentMetaSchemaTest, MetricTypeEnum) {
    // Using schema::MetricType to avoid conflict with vdb::MetricType
    EXPECT_EQ(static_cast<int>(schema::MetricType::L2), 0);
    EXPECT_EQ(static_cast<int>(schema::MetricType::IP), 1);
    EXPECT_EQ(static_cast<int>(schema::MetricType::COSINE), 2);
}

// =============================================================================
// Cross-schema Integration Test
// =============================================================================

TEST(SchemaIntegrationTest, SegmentWithColumnsReference) {
    flatbuffers::FlatBufferBuilder builder(4096);
    
    // Create a FileRef pointing to a columns file
    auto cols_filename = builder.CreateString("segment_1/payload.cols");
    auto cols_ref = schema::CreateFileRef(
        builder,
        schema::FileType::COLUMNS,
        cols_filename,
        268435456,      // 256MB
        0x12345678,
        1699999000
    );
    
    // Create a FileRef for vector index
    auto idx_filename = builder.CreateString("segment_1/vectors.vidx");
    auto idx_ref = schema::CreateFileRef(
        builder,
        schema::FileType::VECTOR_INDEX,
        idx_filename,
        134217728,      // 128MB
        0x87654321,
        1699999000
    );
    
    std::vector<flatbuffers::Offset<schema::FileRef>> files_vec = {cols_ref, idx_ref};
    auto files = builder.CreateVector(files_vec);
    
    // Create minimal segment meta with files
    auto ivf = schema::CreateIvfParams(builder, 256, 8, 0, 1048576, 50000, 10, 50, 1000, 195.3f);
    auto pq = schema::CreatePqParams(builder, 8, 256, 8, 0, 65536, 25000, 16);
    
    std::vector<flatbuffers::Offset<schema::InvertedListMeta>> ivl_vec;
    auto ivl_list = builder.CreateVector(ivl_vec);
    
    auto stats = schema::CreateSegmentStats(builder, 50000, 0, 50000, 
                                     25600000, 16777216, 268435456, 310812672, 0.85f, 0, 0, 0);
    auto lsn = schema::CreateLsnInfo(builder, 0, 50000, 50000, 50000);
    auto del_bitmap = schema::CreateDeleteBitmapMeta(builder, 0, 0, 0, 0);
    
    std::vector<uint64_t> parents_vec;
    auto parents = builder.CreateVector(parents_vec);
    
    auto segment = schema::CreateSegmentMeta(
        builder,
        1, 1, schema::SegmentState::ACTIVE,
        128, schema::MetricType::L2, schema::VectorDType::FLOAT32,
        ivf, pq, 0, 0,
        0, 0, 0, 0,                 // rabitq, conann, crc, clusters (legacy test)
        ivl_list, files, del_bitmap, stats, lsn,
        1699999000, 1699999000, 0, 0, parents, 0
    );
    
    builder.Finish(segment, "VSEG");
    
    // Verify we can access files
    auto* read_seg = schema::GetSegmentMeta(builder.GetBufferPointer());
    ASSERT_NE(read_seg, nullptr);
    ASSERT_NE(read_seg->files(), nullptr);
    EXPECT_EQ(read_seg->files()->size(), 2);
    
    // Find columns file
    const schema::FileRef* cols_file = nullptr;
    for (auto* f : *read_seg->files()) {
        if (f->file_type() == schema::FileType::COLUMNS) {
            cols_file = f;
            break;
        }
    }
    
    ASSERT_NE(cols_file, nullptr);
    EXPECT_STREQ(cols_file->file_name()->c_str(), "segment_1/payload.cols");
}

// =============================================================================
// Phase 2.5: New Schema Tests (RaBitQ, ConANN, ClusterMeta, AddressColumn)
// =============================================================================

class Phase25SchemaTest : public ::testing::Test {
protected:
    flatbuffers::FlatBufferBuilder builder_{4096};
};

TEST_F(Phase25SchemaTest, CreateRaBitQParams) {
    auto rabitq = schema::CreateRaBitQParams(
        builder_,
        1,          // bits (1-bit quantization)
        64,         // block_size
        5.75f,      // c_factor
        0,          // codebook_offset
        0           // codebook_length
    );
    
    builder_.Finish(rabitq);
    
    auto* read = flatbuffers::GetRoot<schema::RaBitQParams>(builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->bits(), 1);
    EXPECT_EQ(read->block_size(), 64);
    EXPECT_NEAR(read->c_factor(), 5.75f, 0.001f);
}

TEST_F(Phase25SchemaTest, CreateConANNParams) {
    auto safein_mode = builder_.CreateString("official_1_plus_n");
    auto conann = schema::CreateConANNParams(
        builder_,
        0.8f,       // tau_in_factor
        1.2f,       // tau_out_factor
        0.0f,       // epsilon
        0.0f,       // d_k
        0,          // calibration_samples
        0,          // calibration_topk
        0.0f,       // calibration_percentile
        0.0f,       // eps_ip_fs
        false,      // has_rotated_centroids
        0.0f,       // safein_d_k
        1,          // safein_dk_space
        0.99f,      // safein_dk_percentile
        12,         // safein_dk_calibration_samples
        1,          // safein_dk_search_scope
        4,          // safein_dk_nprobe
        4,          // safein_dk_bits
        safein_mode
    );
    
    builder_.Finish(conann);
    
    auto* read = flatbuffers::GetRoot<schema::ConANNParams>(builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_NEAR(read->tau_in_factor(), 0.8f, 0.001f);
    EXPECT_NEAR(read->tau_out_factor(), 1.2f, 0.001f);
    ASSERT_NE(read->safein_dk_estimator_mode(), nullptr);
    EXPECT_STREQ(read->safein_dk_estimator_mode()->c_str(),
                 "official_1_plus_n");
}

TEST_F(Phase25SchemaTest, CreateAddressBlockMeta) {
    auto block = schema::CreateAddressBlockMeta(
        builder_,
        1024,       // base_offset
        8,          // bit_width (8 bits per size value)
        2048,       // data_offset (in .clu file)
        512         // data_length
    );
    
    builder_.Finish(block);
    
    auto* read = flatbuffers::GetRoot<schema::AddressBlockMeta>(builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->base_offset(), 1024);
    EXPECT_EQ(read->bit_width(), 8);
    EXPECT_EQ(read->data_offset(), 2048);
    EXPECT_EQ(read->data_length(), 512);
}

TEST_F(Phase25SchemaTest, CreateAddressColumnMeta) {
    // Create some address blocks
    std::vector<flatbuffers::Offset<schema::AddressBlockMeta>> blocks_vec;
    blocks_vec.push_back(schema::CreateAddressBlockMeta(builder_, 0, 8, 1024, 64));
    blocks_vec.push_back(schema::CreateAddressBlockMeta(builder_, 65536, 10, 1088, 80));
    blocks_vec.push_back(schema::CreateAddressBlockMeta(builder_, 131072, 12, 1168, 96));
    auto blocks = builder_.CreateVector(blocks_vec);
    
    auto addr_col = schema::CreateAddressColumnMeta(
        builder_,
        64,         // block_granularity (64 records per block)
        3,          // num_blocks
        blocks      // blocks
    );
    
    builder_.Finish(addr_col);
    
    auto* read = flatbuffers::GetRoot<schema::AddressColumnMeta>(builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->block_granularity(), 64);
    EXPECT_EQ(read->num_blocks(), 3);
    ASSERT_NE(read->blocks(), nullptr);
    EXPECT_EQ(read->blocks()->size(), 3);
    
    auto* blk0 = read->blocks()->Get(0);
    EXPECT_EQ(blk0->base_offset(), 0);
    EXPECT_EQ(blk0->bit_width(), 8);
    
    auto* blk2 = read->blocks()->Get(2);
    EXPECT_EQ(blk2->base_offset(), 131072);
    EXPECT_EQ(blk2->bit_width(), 12);
}

TEST_F(Phase25SchemaTest, CreateClusterMeta) {
    // Build address column
    std::vector<flatbuffers::Offset<schema::AddressBlockMeta>> blocks_vec;
    blocks_vec.push_back(schema::CreateAddressBlockMeta(builder_, 0, 8, 4096, 64));
    auto blocks = builder_.CreateVector(blocks_vec);
    auto addr_col = schema::CreateAddressColumnMeta(builder_, 64, 1, blocks);
    
    auto data_file = builder_.CreateString("cluster_0042.dat");
    
    auto cluster = schema::CreateClusterMeta(
        builder_,
        42,         // cluster_id
        1500,       // size (records)
        0,          // centroid_offset
        512,        // centroid_length (128 dim * 4 bytes)
        512,        // rabitq_data_offset
        24000,      // rabitq_data_length
        addr_col,   // address_column
        data_file,  // data_file_path
        0           // checksum
    );
    
    builder_.Finish(cluster);
    
    auto* read = flatbuffers::GetRoot<schema::ClusterMeta>(builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->cluster_id(), 42);
    EXPECT_EQ(read->size(), 1500);
    EXPECT_EQ(read->centroid_length(), 512);
    EXPECT_EQ(read->rabitq_data_length(), 24000);
    EXPECT_STREQ(read->data_file_path()->c_str(), "cluster_0042.dat");
    
    // Check nested address column
    ASSERT_NE(read->address_column(), nullptr);
    EXPECT_EQ(read->address_column()->block_granularity(), 64);
    EXPECT_EQ(read->address_column()->num_blocks(), 1);
}

TEST_F(Phase25SchemaTest, CreateSegmentMetaWithClusters) {
    // Create IVF params
    auto ivf = schema::CreateIvfParams(builder_, 256, 8, 0, 1048576, 50000, 10, 50, 1000, 195.3f);
    
    // Create RaBitQ params (new)
    auto rabitq = schema::CreateRaBitQParams(builder_, 1, 64, 5.75f, 0, 0);
    
    // Create ConANN params (new)
    auto conann = schema::CreateConANNParams(builder_, 0.8f, 1.2f);
    
    // Create cluster list (new)
    std::vector<flatbuffers::Offset<schema::ClusterMeta>> clusters_vec;
    for (uint32_t i = 0; i < 3; ++i) {
        std::vector<flatbuffers::Offset<schema::AddressBlockMeta>> blks;
        blks.push_back(schema::CreateAddressBlockMeta(builder_, i * 65536, 8, i * 100, 64));
        auto blks_v = builder_.CreateVector(blks);
        auto addr = schema::CreateAddressColumnMeta(builder_, 64, 1, blks_v);
        
        auto path = builder_.CreateString("cluster_" + std::to_string(i) + ".dat");
        clusters_vec.push_back(schema::CreateClusterMeta(
            builder_, i, 500, 0, 512, 512, 8000, addr, path, 0));
    }
    auto clusters = builder_.CreateVector(clusters_vec);
    
    // Empty vectors for legacy fields
    std::vector<flatbuffers::Offset<schema::InvertedListMeta>> ivl_vec;
    auto ivl_list = builder_.CreateVector(ivl_vec);
    std::vector<flatbuffers::Offset<schema::FileRef>> files_vec;
    auto files = builder_.CreateVector(files_vec);
    
    auto stats = schema::CreateSegmentStats(builder_, 1500, 0, 1500,
                                      768000, 32768, 0, 800768, 0.9f, 0, 0, 0);
    auto lsn = schema::CreateLsnInfo(builder_, 0, 1500, 1500, 1500);
    auto del_bitmap = schema::CreateDeleteBitmapMeta(builder_, 0, 0, 0, 0);
    
    std::vector<uint64_t> parents_vec;
    auto parents = builder_.CreateVector(parents_vec);
    
    auto segment = schema::CreateSegmentMeta(
        builder_,
        100,                            // segment_id
        2,                              // version (Phase 2.5)
        schema::SegmentState::ACTIVE,
        128,                            // dimension
        schema::MetricType::L2,
        schema::VectorDType::FLOAT32,
        ivf,                            // ivf_params
        0,                              // pq_params (legacy, not used)
        0,                              // opq_params (legacy, not used)
        0,                              // hnsw_params
        rabitq,                         // rabitq_params (new)
        conann,                         // conann_params (new)
        0,                              // crc_params (new, not set)
        clusters,                       // clusters (new)
        ivl_list,                       // inverted_lists (legacy, empty)
        files,
        del_bitmap,
        stats,
        lsn,
        1699999000, 1699999999, 0, 0, parents, 0
    );
    
    builder_.Finish(segment, "VSEG");
    
    auto* read_seg = schema::GetSegmentMeta(builder_.GetBufferPointer());
    ASSERT_NE(read_seg, nullptr);
    EXPECT_EQ(read_seg->segment_id(), 100);
    EXPECT_EQ(read_seg->version(), 2);
    
    // Verify RaBitQ params
    ASSERT_NE(read_seg->rabitq_params(), nullptr);
    EXPECT_EQ(read_seg->rabitq_params()->bits(), 1);
    EXPECT_EQ(read_seg->rabitq_params()->block_size(), 64);
    EXPECT_NEAR(read_seg->rabitq_params()->c_factor(), 5.75f, 0.001f);
    
    // Verify ConANN params
    ASSERT_NE(read_seg->conann_params(), nullptr);
    EXPECT_NEAR(read_seg->conann_params()->tau_in_factor(), 0.8f, 0.001f);
    EXPECT_NEAR(read_seg->conann_params()->tau_out_factor(), 1.2f, 0.001f);
    
    // Verify clusters
    ASSERT_NE(read_seg->clusters(), nullptr);
    EXPECT_EQ(read_seg->clusters()->size(), 3);
    
    auto* c0 = read_seg->clusters()->Get(0);
    EXPECT_EQ(c0->cluster_id(), 0);
    EXPECT_EQ(c0->size(), 500);
    EXPECT_STREQ(c0->data_file_path()->c_str(), "cluster_0.dat");
    ASSERT_NE(c0->address_column(), nullptr);
    EXPECT_EQ(c0->address_column()->block_granularity(), 64);
    
    auto* c2 = read_seg->clusters()->Get(2);
    EXPECT_EQ(c2->cluster_id(), 2);
    EXPECT_STREQ(c2->data_file_path()->c_str(), "cluster_2.dat");
    
    // Legacy fields should be empty/null
    EXPECT_EQ(read_seg->pq_params(), nullptr);
    EXPECT_EQ(read_seg->inverted_lists()->size(), 0);
}

TEST_F(Phase25SchemaTest, RaBitQEncodingInColumns) {
    // Test the new RaBitQ encoding type in columns.fbs
    EXPECT_EQ(static_cast<int>(schema::EncodingType::RABITQ), 8);
    
    // Create a RaBitQ encoding params
    auto rabitq_params = schema::CreateRaBitQEncodingParams(builder_, 1, 64);
    
    // Create a ChunkMeta with RaBitQ encoding
    auto chunk = schema::CreateChunkMeta(
        builder_,
        0,                                  // chunk_id
        0,                                  // row_start
        1000,                               // row_count
        1000,                               // num_records
        0, 0,                               // offset_table_offset/length
        0,                                  // data_offset
        16000,                              // data_length (1000 * 128 bits / 8)
        16000,                              // compressed_length
        schema::EncodingType::RABITQ,       // encoding (new!)
        schema::EncodingParams::RaBitQEncodingParams,
        rabitq_params.Union(),
        schema::CompressionType::NONE,
        0, 0, 0, 0, 0                      // local_index, aux, null_count, distinct, checksum
    );
    
    builder_.Finish(chunk);
    
    auto* read = flatbuffers::GetRoot<schema::ChunkMeta>(builder_.GetBufferPointer());
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->encoding(), schema::EncodingType::RABITQ);
    EXPECT_EQ(read->encoding_params_type(), schema::EncodingParams::RaBitQEncodingParams);
    
    auto* rq = read->encoding_params_as_RaBitQEncodingParams();
    ASSERT_NE(rq, nullptr);
    EXPECT_EQ(rq->bits(), 1);
    EXPECT_EQ(rq->block_size(), 64);
}

}  // namespace test
}  // namespace vdb
