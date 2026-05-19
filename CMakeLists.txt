cmake_minimum_required(VERSION 3.24)
project(vectordb VERSION 0.1.0 LANGUAGES CXX)

# =============================================================================
# Build Options
# =============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VDB_BUILD_TESTS "Build tests" ON)
option(VDB_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(VDB_BUILD_TOOLS "Build tools" OFF)
option(VDB_USE_AVX2 "Enable AVX2 SIMD" ON)
option(VDB_USE_AVX512 "Enable AVX-512 SIMD" OFF)
option(VDB_USE_MKL "Enable Intel MKL for BLAS acceleration" OFF)

# =============================================================================
# Compiler Flags
# =============================================================================
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG -funroll-loops -fno-math-errno")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

set(VDB_AVX2_COMPILE_OPTIONS -mavx2 -mfma)
set(VDB_AVX512_COMPILE_OPTIONS
    -mavx512f -mavx512dq -mavx512bw -mavx512vl -mavx512cd -mavx512vpopcntdq
    -mavx2 -mfma)

function(vdb_enable_target_avx2 target_name)
    target_compile_options(${target_name} PRIVATE ${VDB_AVX2_COMPILE_OPTIONS})
    target_compile_definitions(${target_name} PRIVATE VDB_USE_AVX2=1)
endfunction()

function(vdb_enable_target_avx512 target_name)
    if(VDB_USE_AVX512)
        target_compile_options(${target_name} PRIVATE ${VDB_AVX512_COMPILE_OPTIONS})
        target_compile_definitions(${target_name} PRIVATE VDB_USE_AVX512=1 VDB_USE_AVX2=1)
    elseif(VDB_USE_AVX2)
        vdb_enable_target_avx2(${target_name})
    endif()
endfunction()

# Link-time optimization for cross-TU inlining (L2Sqr, AccumulateBlock, etc.)
option(VDB_USE_LTO "Enable link-time optimization" ON)
if(VDB_USE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT lto_supported OUTPUT lto_error)
    if(lto_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        message(STATUS "LTO: enabled for Release builds")
    else()
        message(WARNING "LTO: not supported (${lto_error})")
    endif()
endif()

# =============================================================================
# Dependencies (FetchContent)
# =============================================================================
include(FetchContent)
find_package(Threads REQUIRED)
find_package(GSL REQUIRED)

# SuperKMeans — fast clustering library (header-only INTERFACE)
set(SKMEANS_COMPILE_TESTS OFF CACHE BOOL "" FORCE)
set(SKMEANS_COMPILE_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SKMEANS_COMPILE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKMEANS_SKIP_FFTW ON CACHE BOOL "" FORCE)
add_subdirectory(thrid-party/SuperKMeans)

# Intel MKL (optional, for BLAS-accelerated centroid distance)
if(VDB_USE_MKL)
    find_path(MKL_INCLUDE_DIR mkl.h PATHS /usr/include/mkl)
    find_library(MKL_LP64 mkl_intel_lp64 PATHS /usr/lib/x86_64-linux-gnu)
    find_library(MKL_SEQ mkl_sequential PATHS /usr/lib/x86_64-linux-gnu)
    find_library(MKL_CORE mkl_core PATHS /usr/lib/x86_64-linux-gnu)
    if(MKL_INCLUDE_DIR AND MKL_LP64 AND MKL_SEQ AND MKL_CORE)
        add_library(mkl INTERFACE)
        target_include_directories(mkl INTERFACE ${MKL_INCLUDE_DIR})
        target_link_libraries(mkl INTERFACE ${MKL_LP64} ${MKL_SEQ} ${MKL_CORE} pthread m dl)
        add_compile_definitions(VDB_USE_MKL=1)
        message(STATUS "MKL: found (sequential, lp64)")
    else()
        message(FATAL_ERROR "MKL: not found. Install with: sudo apt install libmkl-dev")
    endif()
endif()

# OpenMP (optional, for parallelising calibration loops in benchmarks)
find_package(OpenMP)

# FlatBuffers - build from source to get flatc compiler
FetchContent_Declare(
    flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG v24.3.25
)
set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flatbuffers)

# Google Test (for tests)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# =============================================================================
# FlatBuffers Code Generation
# =============================================================================
set(FLATBUFFERS_SCHEMA_DIR ${CMAKE_SOURCE_DIR}/schema)
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)

file(MAKE_DIRECTORY ${FLATBUFFERS_GENERATED_DIR})

# Schema files
set(FBS_FILES
    ${FLATBUFFERS_SCHEMA_DIR}/columns.fbs
    ${FLATBUFFERS_SCHEMA_DIR}/segment_meta.fbs
    ${FLATBUFFERS_SCHEMA_DIR}/record.fbs
)

# Generate headers using the flatc we just built
foreach(FBS_FILE ${FBS_FILES})
    get_filename_component(FBS_NAME ${FBS_FILE} NAME_WE)
    set(GENERATED_HEADER ${FLATBUFFERS_GENERATED_DIR}/${FBS_NAME}_generated.h)
    
    add_custom_command(
        OUTPUT ${GENERATED_HEADER}
        COMMAND flatc --cpp --scoped-enums -o ${FLATBUFFERS_GENERATED_DIR} ${FBS_FILE}
        DEPENDS ${FBS_FILE} flatc
        COMMENT "Generating FlatBuffers header for ${FBS_NAME}"
    )
    list(APPEND GENERATED_HEADERS ${GENERATED_HEADER})
endforeach()

add_custom_target(flatbuffers_generated DEPENDS ${GENERATED_HEADERS})

# =============================================================================
# VDB Common Library (header-only for now)
# =============================================================================
add_library(vdb_common INTERFACE)
target_include_directories(vdb_common INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# =============================================================================
# VDB Schema Library (FlatBuffers generated headers)
# =============================================================================
add_library(vdb_schema INTERFACE)
target_include_directories(vdb_schema INTERFACE
    $<BUILD_INTERFACE:${FLATBUFFERS_GENERATED_DIR}>
)
target_link_libraries(vdb_schema INTERFACE flatbuffers)
add_dependencies(vdb_schema flatbuffers_generated)

# =============================================================================
# VDB Columns Library (Phase 2)
# =============================================================================
add_library(vdb_columns STATIC
    src/columns/column_chunk_writer.cpp
    src/columns/column_chunk_reader.cpp
    src/columns/column_store.cpp
)
target_include_directories(vdb_columns PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_columns PUBLIC vdb_common Threads::Threads)
vdb_enable_target_avx512(vdb_columns)

# =============================================================================
# VDB SIMD Library (Phase 3)
# AVX-512, AVX2, and scalar paths coexist; runtime selection is compile-time
# through VDB_USE_AVX512 / VDB_USE_AVX2.
# =============================================================================
add_library(vdb_simd STATIC
    src/simd/distance_l2.cpp
    src/simd/bit_unpack.cpp
    src/simd/prefix_sum.cpp
    src/simd/popcount.cpp
    src/simd/transpose.cpp
    src/simd/ip_exrabitq.cpp
    src/simd/fastscan.cpp
    src/simd/prepare_query.cpp
    src/simd/hadamard.cpp
    src/simd/signed_dot.cpp
    src/simd/stage2_classify.cpp
    src/simd/address_decode.cpp
    src/simd/rerank_distance.cpp
    src/simd/coarse_select.cpp
)
target_include_directories(vdb_simd PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_simd PUBLIC vdb_common)
vdb_enable_target_avx512(vdb_simd)

# =============================================================================
# VDB Codec Library (Phase 3)
# =============================================================================
add_library(vdb_codec STATIC
    src/codec/bitpack_codec.cpp
    src/codec/dict_codec.cpp
)
target_include_directories(vdb_codec PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_codec PUBLIC vdb_simd)
vdb_enable_target_avx512(vdb_codec)

# =============================================================================
# VDB RaBitQ Library (Phase 4)
# 1-bit RaBitQ quantization: rotation, encoder, estimator.
# See RabitQemplementation.md for algorithm details.
# =============================================================================
add_library(vdb_rabitq STATIC
    src/rabitq/rabitq_rotation.cpp
    src/rabitq/rabitq_encoder.cpp
    src/rabitq/rabitq_estimator.cpp
)
target_include_directories(vdb_rabitq PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_rabitq PUBLIC vdb_simd)
vdb_enable_target_avx512(vdb_rabitq)

# =============================================================================
# VDB Storage Library (Phase 5)
# Provides AddressColumn, DataFileWriter, DataFileReader, ClusterStore, Segment.
# =============================================================================
add_library(vdb_storage STATIC
    src/storage/address_column.cpp
    src/storage/data_file_writer.cpp
    src/storage/data_file_reader.cpp
    src/storage/cluster_store.cpp
    src/storage/pack_codes.cpp
    src/storage/segment.cpp
)
target_include_directories(vdb_storage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_storage PUBLIC vdb_columns vdb_codec vdb_rabitq)
vdb_enable_target_avx512(vdb_storage)

# =============================================================================
# Vendored Faiss (CPU-only coarse-builder dependency for vdb_index only)
# Keep the root project on C++17 and isolate Faiss's C++20 requirement to the
# subproject + the local adapter that links it.
# Dependency boundary:
# - OpenMP is resolved inside Faiss and used privately by vdb_index.
# - BLAS/LAPACK are consumed through the Faiss target only.
# - MKL/GPU/Python/extras/C API/testing stay disabled in this integration.
# =============================================================================
set(FAISS_ENABLE_GPU OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_EXTRAS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(FAISS_ENABLE_MKL OFF CACHE BOOL "" FORCE)
add_subdirectory(thrid-party/faiss EXCLUDE_FROM_ALL)
vdb_enable_target_avx2(faiss)

# Restore the parent standard after entering Faiss's C++20 subproject so later
# VDB targets continue to build with the repository default.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(vdb_faiss_adapter STATIC
    src/index/faiss_coarse_builder.cpp
)
target_include_directories(vdb_faiss_adapter PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(vdb_faiss_adapter PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
target_link_libraries(vdb_faiss_adapter PRIVATE faiss)
vdb_enable_target_avx2(vdb_faiss_adapter)

# =============================================================================
# VDB Index Library (Phase 6)
# IVF index: ConANN 3-class classifier, IvfIndex, IvfBuilder.
# =============================================================================
add_library(vdb_index STATIC
    src/index/conann.cpp
    src/index/cluster_prober.cpp
    src/index/ivf_index.cpp
    src/index/ivf_builder.cpp
)
target_include_directories(vdb_index PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${FLATBUFFERS_GENERATED_DIR}>
    $<INSTALL_INTERFACE:include>
)
set_target_properties(vdb_index PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_link_libraries(vdb_index PUBLIC vdb_storage vdb_rabitq vdb_simd vdb_schema vdb_io superkmeans)
target_link_libraries(vdb_index PRIVATE vdb_faiss_adapter)
# IvfBuilder uses CrcCalibrator for offline CRC score precomputation.
# GSL dependency stays PRIVATE within vdb_crc, not exposed to vdb_index consumers.
target_link_libraries(vdb_index PRIVATE vdb_crc)
if(OpenMP_CXX_FOUND)
    target_link_libraries(vdb_index PRIVATE OpenMP::OpenMP_CXX)
endif()
if(VDB_USE_MKL)
    target_link_libraries(vdb_index PRIVATE mkl)
endif()
vdb_enable_target_avx512(vdb_index)

# =============================================================================
# VDB CRC Library (ConANN CRC calibration — offline only, depends on GSL)
# Separated from vdb_index so online query path does not link GSL.
# =============================================================================
add_library(vdb_crc STATIC
    src/index/crc_calibrator.cpp
)
target_include_directories(vdb_crc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_crc PUBLIC vdb_simd vdb_common vdb_rabitq)
target_link_libraries(vdb_crc PRIVATE GSL::gsl GSL::gslcblas)
if(OpenMP_CXX_FOUND)
    target_link_libraries(vdb_crc PRIVATE OpenMP::OpenMP_CXX)
endif()
vdb_enable_target_avx512(vdb_crc)

# =============================================================================
# VDB Query Library (Phase 7)
# Single-threaded two-phase query pipeline: Probe+Submit → Drain+Rerank.
# =============================================================================
find_library(URING_LIB uring)
if(NOT URING_LIB)
    message(FATAL_ERROR "liburing not found. Install with: sudo apt install liburing-dev")
endif()

add_library(vdb_query STATIC
    src/common/distance.cpp
    src/query/pread_fallback_reader.cpp
    src/query/io_uring_reader.cpp
    src/query/result_collector.cpp
    src/query/rerank_consumer.cpp
    src/query/overlap_scheduler.cpp
    src/query/buffer_pool.cpp
)
target_include_directories(vdb_query PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_query PUBLIC vdb_index vdb_storage vdb_simd vdb_rabitq)
target_link_libraries(vdb_query PRIVATE ${URING_LIB})
vdb_enable_target_avx512(vdb_query)

# =============================================================================
# VDB IO Library (Phase 8)
# Npy/Jsonl file loaders for dataset ingestion.
# =============================================================================
add_library(vdb_io STATIC
    src/io/npy_reader.cpp
    src/io/jsonl_reader.cpp
    src/io/vecs_reader.cpp
)
target_include_directories(vdb_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(vdb_io PUBLIC vdb_common)
vdb_enable_target_avx512(vdb_io)

# =============================================================================
# Tests
# =============================================================================
if(VDB_BUILD_TESTS)
    enable_testing()
    
    # -------------------------------------------------------------------------
    # Common module tests (Phase 0)
    # -------------------------------------------------------------------------
    add_executable(test_types tests/common/types_test.cpp)
    target_link_libraries(test_types PRIVATE vdb_common GTest::gtest GTest::gtest_main)
    add_test(NAME test_types COMMAND test_types)
    
    add_executable(test_status tests/common/status_test.cpp)
    target_link_libraries(test_status PRIVATE vdb_common GTest::gtest GTest::gtest_main)
    add_test(NAME test_status COMMAND test_status)
    
    add_executable(test_aligned_alloc tests/common/aligned_alloc_test.cpp)
    target_link_libraries(test_aligned_alloc PRIVATE vdb_common GTest::gtest GTest::gtest_main)
    add_test(NAME test_aligned_alloc COMMAND test_aligned_alloc)
    
    # -------------------------------------------------------------------------
    # Schema tests (Phase 1)
    # -------------------------------------------------------------------------
    add_executable(test_schema tests/schema/schema_test.cpp)
    target_link_libraries(test_schema PRIVATE vdb_common vdb_schema GTest::gtest GTest::gtest_main)
    add_dependencies(test_schema flatbuffers_generated)
    add_test(NAME test_schema COMMAND test_schema)
    
    add_executable(test_record tests/schema/record_test.cpp)
    target_link_libraries(test_record PRIVATE vdb_common vdb_schema GTest::gtest GTest::gtest_main)
    add_dependencies(test_record flatbuffers_generated)
    add_test(NAME test_record COMMAND test_record)
    
    # -------------------------------------------------------------------------
    # Column store tests (Phase 2)
    # -------------------------------------------------------------------------
    add_executable(test_column_chunk tests/columns/column_chunk_test.cpp)
    target_link_libraries(test_column_chunk PRIVATE vdb_columns GTest::gtest GTest::gtest_main)
    add_test(NAME test_column_chunk COMMAND test_column_chunk)
    
    add_executable(test_column_store tests/columns/column_store_test.cpp)
    target_link_libraries(test_column_store PRIVATE vdb_columns GTest::gtest GTest::gtest_main)
    add_test(NAME test_column_store COMMAND test_column_store)

    # -------------------------------------------------------------------------
    # SIMD tests (Phase 3)
    # -------------------------------------------------------------------------
    add_executable(test_distance_l2 tests/simd/distance_l2_test.cpp)
    target_link_libraries(test_distance_l2 PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_distance_l2 COMMAND test_distance_l2)

    add_executable(test_bit_unpack tests/simd/bit_unpack_test.cpp)
    target_link_libraries(test_bit_unpack PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_bit_unpack COMMAND test_bit_unpack)

    add_executable(test_prefix_sum tests/simd/prefix_sum_test.cpp)
    target_link_libraries(test_prefix_sum PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_prefix_sum COMMAND test_prefix_sum)

    add_executable(test_transpose tests/simd/transpose_test.cpp)
    target_link_libraries(test_transpose PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_transpose COMMAND test_transpose)

    add_executable(test_prepare_query tests/simd/prepare_query_test.cpp)
    target_link_libraries(test_prepare_query PRIVATE vdb_simd vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_prepare_query COMMAND test_prepare_query)

    add_executable(test_ip_exrabitq tests/simd/ip_exrabitq_test.cpp)
    target_link_libraries(test_ip_exrabitq PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_ip_exrabitq COMMAND test_ip_exrabitq)

    # -------------------------------------------------------------------------
    # Codec tests (Phase 3)
    # -------------------------------------------------------------------------
    add_executable(test_bitpack_codec tests/codec/bitpack_codec_test.cpp)
    target_link_libraries(test_bitpack_codec PRIVATE vdb_codec GTest::gtest GTest::gtest_main)
    add_test(NAME test_bitpack_codec COMMAND test_bitpack_codec)

    add_executable(test_dict_codec tests/codec/dict_codec_test.cpp)
    target_link_libraries(test_dict_codec PRIVATE vdb_codec GTest::gtest GTest::gtest_main)
    add_test(NAME test_dict_codec COMMAND test_dict_codec)

    # -------------------------------------------------------------------------
    # SIMD popcount test (Phase 4 prerequisite)
    # -------------------------------------------------------------------------
    add_executable(test_popcount tests/simd/popcount_test.cpp)
    target_link_libraries(test_popcount PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_popcount COMMAND test_popcount)

    add_executable(test_signed_dot tests/simd/signed_dot_test.cpp)
    target_link_libraries(test_signed_dot PRIVATE vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_signed_dot COMMAND test_signed_dot)

    # -------------------------------------------------------------------------
    # RaBitQ tests (Phase 4)
    # -------------------------------------------------------------------------
    add_executable(test_rabitq_rotation tests/rabitq/rabitq_rotation_test.cpp)
    target_link_libraries(test_rabitq_rotation PRIVATE vdb_rabitq GTest::gtest GTest::gtest_main)
    add_test(NAME test_rabitq_rotation COMMAND test_rabitq_rotation)

    add_executable(test_rabitq_encoder tests/rabitq/rabitq_encoder_test.cpp)
    target_link_libraries(test_rabitq_encoder PRIVATE vdb_rabitq GTest::gtest GTest::gtest_main)
    add_test(NAME test_rabitq_encoder COMMAND test_rabitq_encoder)

    add_executable(test_rabitq_estimator tests/rabitq/rabitq_estimator_test.cpp)
    target_link_libraries(test_rabitq_estimator PRIVATE vdb_rabitq vdb_simd GTest::gtest GTest::gtest_main)
    add_test(NAME test_rabitq_estimator COMMAND test_rabitq_estimator)

    # -------------------------------------------------------------------------
    # Storage tests (Phase 5)
    # -------------------------------------------------------------------------
    add_executable(test_address_column tests/storage/address_column_test.cpp)
    target_link_libraries(test_address_column PRIVATE vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_address_column COMMAND test_address_column)

    add_executable(test_data_file tests/storage/data_file_test.cpp)
    target_link_libraries(test_data_file PRIVATE vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_data_file COMMAND test_data_file)

    add_executable(test_cluster_store tests/storage/cluster_store_test.cpp)
    target_link_libraries(test_cluster_store PRIVATE vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_cluster_store COMMAND test_cluster_store)

    add_executable(test_segment tests/storage/segment_test.cpp)
    target_link_libraries(test_segment PRIVATE vdb_storage GTest::gtest GTest::gtest_main)
    add_test(NAME test_segment COMMAND test_segment)

    # -------------------------------------------------------------------------
    # Index tests (Phase 6)
    # -------------------------------------------------------------------------
    add_executable(test_conann tests/index/conann_test.cpp)
    target_link_libraries(test_conann PRIVATE vdb_index GTest::gtest GTest::gtest_main)
    add_test(NAME test_conann COMMAND test_conann)

    add_executable(test_crc_stopper tests/index/crc_stopper_test.cpp)
    target_link_libraries(test_crc_stopper PRIVATE vdb_common GTest::gtest GTest::gtest_main)
    add_test(NAME test_crc_stopper COMMAND test_crc_stopper)

    add_executable(test_crc_calibrator tests/index/crc_calibrator_test.cpp)
    target_link_libraries(test_crc_calibrator PRIVATE vdb_crc GTest::gtest GTest::gtest_main)
    add_test(NAME test_crc_calibrator COMMAND test_crc_calibrator)

    add_executable(test_ivf_index tests/index/ivf_index_test.cpp)
    target_link_libraries(test_ivf_index PRIVATE vdb_index GTest::gtest GTest::gtest_main)
    add_test(NAME test_ivf_index COMMAND test_ivf_index)

    add_executable(test_ivf_builder tests/index/ivf_builder_test.cpp)
    target_link_libraries(test_ivf_builder PRIVATE vdb_index GTest::gtest GTest::gtest_main)
    add_test(NAME test_ivf_builder COMMAND test_ivf_builder)

    add_executable(test_payload_pipeline tests/index/payload_pipeline_test.cpp)
    target_link_libraries(test_payload_pipeline PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_payload_pipeline COMMAND test_payload_pipeline)

    # -------------------------------------------------------------------------
    # Query Pipeline tests (Phase 7)
    # -------------------------------------------------------------------------
    add_executable(test_distance tests/common/distance_test.cpp)
    target_link_libraries(test_distance PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_distance COMMAND test_distance)

    add_executable(test_pread_fallback_reader tests/query/pread_fallback_reader_test.cpp)
    target_link_libraries(test_pread_fallback_reader PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_pread_fallback_reader COMMAND test_pread_fallback_reader)

    add_executable(test_io_uring_reader tests/query/io_uring_reader_test.cpp)
    target_link_libraries(test_io_uring_reader PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_io_uring_reader COMMAND test_io_uring_reader)

    add_executable(test_result_collector tests/query/result_collector_test.cpp)
    target_link_libraries(test_result_collector PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_result_collector COMMAND test_result_collector)

    add_executable(test_buffer_pool tests/query/buffer_pool_test.cpp)
    target_link_libraries(test_buffer_pool PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_buffer_pool COMMAND test_buffer_pool)

    add_executable(test_rerank_consumer tests/query/rerank_consumer_test.cpp)
    target_link_libraries(test_rerank_consumer PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_rerank_consumer COMMAND test_rerank_consumer)

    add_executable(test_overlap_scheduler tests/query/overlap_scheduler_test.cpp)
    target_link_libraries(test_overlap_scheduler PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_overlap_scheduler COMMAND test_overlap_scheduler)

    add_executable(test_early_stop tests/query/early_stop_test.cpp)
    target_link_libraries(test_early_stop PRIVATE vdb_query GTest::gtest GTest::gtest_main)
    add_test(NAME test_early_stop COMMAND test_early_stop)

    # -------------------------------------------------------------------------
    # IO tests (Phase 8)
    # -------------------------------------------------------------------------
    add_executable(test_npy_reader tests/io/npy_reader_test.cpp)
    target_link_libraries(test_npy_reader PRIVATE vdb_io GTest::gtest GTest::gtest_main)
    add_test(NAME test_npy_reader COMMAND test_npy_reader)

    add_executable(test_jsonl_reader tests/io/jsonl_reader_test.cpp)
    target_link_libraries(test_jsonl_reader PRIVATE vdb_io GTest::gtest GTest::gtest_main)
    add_test(NAME test_jsonl_reader COMMAND test_jsonl_reader)

    add_executable(test_vecs_reader tests/io/vecs_reader_test.cpp)
    target_link_libraries(test_vecs_reader PRIVATE vdb_io GTest::gtest GTest::gtest_main)
    add_test(NAME test_vecs_reader COMMAND test_vecs_reader)
endif()

# =============================================================================
# Benchmarks (separate from tests, controlled by VDB_BUILD_BENCHMARKS)
# =============================================================================
add_subdirectory(benchmarks)

# =============================================================================
# Summary
# =============================================================================
message(STATUS "")
message(STATUS "VectorDB Configuration Summary:")
message(STATUS "  Version:      ${PROJECT_VERSION}")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  AVX2:         ${VDB_USE_AVX2}")
message(STATUS "  AVX-512:      ${VDB_USE_AVX512}")
message(STATUS "  Tests:        ${VDB_BUILD_TESTS}")
message(STATUS "  Benchmarks:   ${VDB_BUILD_BENCHMARKS}")
message(STATUS "  Tools:        ${VDB_BUILD_TOOLS}")
message(STATUS "  MKL:          ${VDB_USE_MKL}")
message(STATUS "")
