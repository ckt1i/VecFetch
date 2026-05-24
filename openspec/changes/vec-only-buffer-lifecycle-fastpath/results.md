# Validation Results

Date: `2026-05-22`

Index:
- `/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator`

Ground truth:
- `/home/zcq/VDB/test/msmarco_fht_kac_adapter/gt_top10.npy`

Mode:
- `full_e2e`
- `crc=1`
- `clu_read_mode=full_preload`
- `use_resident_clusters=1`
- `queries=1000`
- `io_queue_depth=64`
- `hotpath_detailed_timing=0`

Sweep outputs:
- `fixed_vec_buffer_count=0`: `/home/zcq/VDB/test/vec_only_buffer_lifecycle_sweep/c0/msmarco_fht_kac_adapter_20260522T190445`
- `fixed_vec_buffer_count=128`: `/home/zcq/VDB/test/vec_only_buffer_lifecycle_sweep/c128/msmarco_fht_kac_adapter_20260522T190525`
- `fixed_vec_buffer_count=256`: `/home/zcq/VDB/test/vec_only_buffer_lifecycle_sweep/c256/msmarco_fht_kac_adapter_20260522T190604`
- `fixed_vec_buffer_count=512`: `/home/zcq/VDB/test/vec_only_buffer_lifecycle_sweep/c512/msmarco_fht_kac_adapter_20260522T190644`
- `fixed_vec_buffer_count=1024`: `/home/zcq/VDB/test/vec_only_buffer_lifecycle_sweep/c1024/msmarco_fht_kac_adapter_20260522T190723`

Key metrics:

| fixed_vec_buffer_count | avg_query_ms | probe_submit_ms | probe_submit_vec_only_emit_ms | fixed_hit | fixed_miss | recall@10 | io_wait_ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 3.281 | 0.986 | 0.943 | 0.2 | 771.4 | 0.1993 | 0.002 |
| 128 | 3.291 | 1.016 | 0.973 | 0.4 | 771.2 | 0.1993 | 0.002 |
| 256 | 3.260 | 1.007 | 0.964 | 0.8 | 770.9 | 0.1993 | 0.002 |
| 512 | 3.254 | 0.999 | 0.956 | 1.3 | 770.3 | 0.1993 | 0.002 |
| 1024 | 3.292 | 1.023 | 0.979 | 2.9 | 768.7 | 0.1993 | 0.002 |

Recommendation:
- Keep implementation default as `fixed_vec_buffer_count=0` for compatibility.
- Recommend `fixed_vec_buffer_count=512` for this `fht_kac_rotator` resident/full-preload setup.
- Rationale: it produced the best end-to-end latency and lowest submit cost in the sweep while preserving the same recall and near-zero I/O wait.

Perf outputs:
- Broad profile: `/home/zcq/VDB/VectorRetrival/profile_output/vec_only_buffer_lifecycle_c512.perf.data`
- Query-heavy profile: `/home/zcq/VDB/VectorRetrival/profile_output/vec_only_buffer_lifecycle_c512_query.perf.data`

Perf summary:
- `BufferPool::Acquire` dropped to `0.02%` children in the query-heavy profile.
- `CleanupPendingSlots()` dropped to `0.02%`.
- `DispatchCompletion` dropped to `0.33%`.
- `EmitPendingDataRequests` dropped to `0.37%`.
- Residual costs are now dominated by broader allocation/free activity, sorting, and cluster probing rather than vec-only fallback buffer management.
