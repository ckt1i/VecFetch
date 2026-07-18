# =============================================================================
# Benchmarks — controlled by VDB_BUILD_BENCHMARKS
# =============================================================================
if(NOT VDB_BUILD_BENCHMARKS)
    return()
endif()

# ----- IVF Quality Benchmark -----
add_executable(bench_ivf_quality bench_ivf_quality.cpp)
target_link_libraries(bench_ivf_quality PRIVATE vdb_index)
vdb_enable_target_avx512(bench_ivf_quality)

# ----- Legacy build/full-flow benchmark -----
add_executable(bench_build_index bench_e2e.cpp rabitq_bench_calibration.cpp)
target_link_libraries(bench_build_index PRIVATE vdb_query vdb_io)
vdb_enable_target_avx512(bench_build_index)

# ----- Online E2E query benchmark -----
add_executable(bench_e2e bench_online_query.cpp rabitq_bench_calibration.cpp)
target_link_libraries(bench_e2e PRIVATE vdb_query vdb_io)
vdb_enable_target_avx512(bench_e2e)

if(VDB_BUILD_TESTS)
    add_test(
        NAME test_bench_e2e_rejects_deleted_prefetch_flags
        COMMAND ${CMAKE_COMMAND}
            -DBENCH_E2E=$<TARGET_FILE:bench_e2e>
            -DBENCH_BUILD_INDEX=$<TARGET_FILE:bench_build_index>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/reject_deleted_prefetch_flags.cmake)
endif()

# ----- No Combine Store Materializer -----
add_executable(bench_materialize_no_combine_store bench_materialize_no_combine_store.cpp)
target_link_libraries(bench_materialize_no_combine_store PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_materialize_no_combine_store)

# ----- Hot/Cold Record Store Materializer -----
add_executable(bench_materialize_hotcold_store bench_materialize_no_combine_store.cpp)
target_link_libraries(bench_materialize_hotcold_store PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_materialize_hotcold_store)

# ----- Inline Hot Record Store Materializer -----
add_executable(bench_materialize_inline_hot_record_store bench_materialize_inline_hot_record_store.cpp)
target_link_libraries(bench_materialize_inline_hot_record_store PRIVATE vdb_index vdb_io vdb_storage)
vdb_enable_target_avx512(bench_materialize_inline_hot_record_store)

# ----- Inline Hot Record Store Verifier -----
add_executable(bench_verify_inline_hot_record_store bench_verify_inline_hot_record_store.cpp)
target_link_libraries(bench_verify_inline_hot_record_store PRIVATE vdb_index vdb_io vdb_storage)
vdb_enable_target_avx512(bench_verify_inline_hot_record_store)

# ----- Offline SafeIn d_k samples generator -----
add_executable(bench_safein_dk_samples bench_safein_dk_samples.cpp rabitq_bench_calibration.cpp)
target_link_libraries(bench_safein_dk_samples PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_safein_dk_samples)

# ----- ExRaBitQ Payload Compare Utility -----
add_executable(bench_compare_exrabitq_payloads bench_compare_exrabitq_payloads.cpp)
target_link_libraries(bench_compare_exrabitq_payloads PRIVATE vdb_storage)
vdb_enable_target_avx512(bench_compare_exrabitq_payloads)

# ----- Stage1/Stage2 Path Compare Utility -----
add_executable(bench_compare_stage_paths bench_compare_stage_paths.cpp)
target_link_libraries(bench_compare_stage_paths PRIVATE vdb_query vdb_io)
vdb_enable_target_avx512(bench_compare_stage_paths)

# ----- Official 3-bit direct compact Stage2 kernel microbench -----
add_executable(bench_direct3_kernel bench_direct3_kernel.cpp)
target_link_libraries(bench_direct3_kernel PRIVATE vdb_simd)
vdb_enable_target_avx512(bench_direct3_kernel)

# ----- Ordered span planner CPU microbenchmark -----
add_executable(bench_span_planner_cpu bench_span_planner_cpu.cpp)
target_link_libraries(bench_span_planner_cpu PRIVATE vdb_query)

# ----- Index Geometry Utility -----
add_executable(bench_index_geometry bench_index_geometry.cpp)
target_link_libraries(bench_index_geometry PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_index_geometry)

# ----- IVF Clustering Export Utility -----
add_executable(export_ivf_clustering export_ivf_clustering.cpp)
target_link_libraries(export_ivf_clustering PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(export_ivf_clustering)

# ----- Cluster .clu Migration Utility (legacy byte-sign -> packed-sign) -----
add_executable(migrate_cluster_clu_packedsign migrate_cluster_clu_packedsign.cpp)
target_link_libraries(migrate_cluster_clu_packedsign PRIVATE vdb_storage)
vdb_enable_target_avx512(migrate_cluster_clu_packedsign)

# ----- RaBitQ Accuracy Benchmark -----
add_executable(bench_rabitq_accuracy bench_rabitq_accuracy.cpp)
target_link_libraries(bench_rabitq_accuracy PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_rabitq_accuracy)

# ----- ConANN Recall Benchmark -----
add_executable(bench_conann_recall bench_conann_recall.cpp)
target_link_libraries(bench_conann_recall PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_conann_recall)

# ----- RaBitQ Diagnostic Benchmark -----
add_executable(bench_rabitq_diagnostic bench_rabitq_diagnostic.cpp)
target_link_libraries(bench_rabitq_diagnostic PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_rabitq_diagnostic)

# ----- Precision-conditioned RaBitQ interval auditor -----
add_executable(bench_rabitq_interval_audit bench_rabitq_interval_audit.cpp)
target_link_libraries(bench_rabitq_interval_audit PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_rabitq_interval_audit)

# ----- SafeIn Replay Benchmark -----
add_executable(bench_safein_replay bench_safein_replay.cpp)
vdb_enable_target_avx512(bench_safein_replay)

# ----- Sparse-vs-dense raw-vector I/O replay -----
add_executable(bench_layout_io_replay bench_layout_io_replay.cpp)
target_link_libraries(bench_layout_io_replay PRIVATE vdb_query)
vdb_enable_target_avx512(bench_layout_io_replay)

# ----- d_k Distance Distribution Analysis Benchmark -----
add_executable(bench_dk_distance_analysis bench_dk_distance_analysis.cpp)
vdb_enable_target_avx512(bench_dk_distance_analysis)

# ----- d_k Calibration Space Compare Benchmark -----
add_executable(bench_dk_space_compare bench_dk_space_compare.cpp)
target_link_libraries(bench_dk_space_compare PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_dk_space_compare)

# ----- Phase A Grid Replay Benchmark -----
add_executable(bench_phase_a_grid bench_phase_a_grid.cpp)
target_link_libraries(bench_phase_a_grid PRIVATE vdb_index vdb_io)
vdb_enable_target_avx512(bench_phase_a_grid)

# ----- Vector Search Benchmark -----
add_executable(bench_vector_search bench_vector_search.cpp rabitq_bench_calibration.cpp)
target_link_libraries(bench_vector_search PRIVATE vdb_query vdb_index vdb_io)
vdb_enable_target_avx512(bench_vector_search)
if(VDB_USE_MKL)
    target_link_libraries(bench_vector_search PRIVATE mkl)
endif()
if(OpenMP_CXX_FOUND)
    target_link_libraries(bench_vector_search PRIVATE OpenMP::OpenMP_CXX)
endif()
