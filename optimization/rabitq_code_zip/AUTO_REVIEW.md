# Auto Review Loop

Context: RaBitQ Stage2 storage/query optimization for sparse survivor lanes.

## Round 1 - 2026-06-07

### Reviewer Findings

- The user concern is valid, but the current implementation already avoids most all-lane compute waste through `lane_mask`.
- The remaining issue is mainly memory layout: current direct Stage2 layouts are dim-block major (`[dim_block][lane]`), so a single survivor vector jumps across dim blocks and touches cache lines containing unused lanes.
- A full on-disk 2/4-lane batch-size change is riskier than first proving the locality upper bound.
- Recommended first two experiments:
  - Keep on-disk format mostly intact and add a vector-contiguous ExData layout or resident/read-time vector-contiguous prototype.
  - Add bits-specialized direct kernels, especially for `ex_bits=2/4`.
- For `ex_bits=4`, lossless compression can only approach `ex_bits=3` storage if the fourth bitplane is sparse or predictable; otherwise this is information-theoretically unrealistic.

### Implemented This Round

- Added `vector_bitplanes` v15 layout with lane-major Stage2 payload.
- Extended official direct bitplane pack/unpack and dot to `ex_bits=4`.
- Added vector-contiguous masked Stage2 kernel and ClusterProber dispatch.
- Added tests for layout parsing, SIMD correctness, storage roundtrip, resident preload, and ClusterProber scoring.

### Verification

- `cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober`
- `./build/test_types`
- `./build/test_ip_exrabitq`
- `./build/test_cluster_store`
- `./build/test_cluster_prober`
- `git diff --check`

All passed.

### COCO100k Probe

Results are documented in `rabitq_code_zip/stage2_vector_layout_results.md`.

Key result: same `total_bits=4/ex_bits=3` recall and storage as `split3_trimmed_bitplanes`, with avg latency changing from `0.3388 ms` to `0.3378 ms` and p95 from `0.4341 ms` to `0.4287 ms`.

The `ex_bits=4` distribution check shows the fourth bitplane is not sparse:
per-plane zero 64-bit word rates are all `0.00029`; a high-plane mask+raw
elision estimate increases payload size by about `0.41%`. This blocks the
lossless "near ex_bits=3 size" target for COCO100k unless the method changes
semantics.

### Remaining Work

- Add a second implementation route for comparison, preferably `vector_bitplanes_prefetch` or a small-lane/specialized dispatch path. `vector_bitplanes_prefetch` has now been implemented and measured; it is functionally correct but does not produce a meaningful speedup on COCO100k.
- Repeat the `ex_bits=4` sparsity analysis on other datasets before ruling out
  dataset-specific lossless compression globally.
- Run repeated trials to distinguish real speedup from benchmark noise.

## Round 1 Follow-up - 2026-06-07

### Implemented

- Added `vector_bitplanes_prefetch` as a second implementation route.
- Added parser/format-key, SIMD, storage round-trip, and ClusterProber coverage for `ex_bits=1..4`.
- Rebuilt COCO100k prefetch indexes for:
  - `total_bits=4/ex_bits=3`
  - `total_bits=5/ex_bits=4`

### Verification

- `cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)`
- `./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober`
- `git diff --check`

All passed.

### COCO100k Result

- `ex_bits=3`: `vector_bitplanes_prefetch` avg `0.347924 ms`, R@10 `0.9504`; `vector_bitplanes` avg `0.348227 ms`, R@10 `0.9504`; `split3_trimmed_bitplanes` avg `0.347850 ms`, R@10 `0.9504`.
- `ex_bits=4`: `vector_bitplanes_prefetch` avg `0.329941 ms`, R@10 `0.9486`; `vector_bitplanes` avg `0.330380 ms`, R@10 `0.9486`.
- Storage and resident code bytes are unchanged versus corresponding non-prefetch vector layouts.

### Review Conclusion

The prefetch route is safe and test-covered, but the speed difference is within
benchmark noise. It should remain as an experimental layout rather than the
default. The next useful implementation route should change the Stage2 compute
shape more substantially, e.g. `ex_bits=2` direct compact IP, `ex_bits=4` nibble
direct compact IP, or survivor micro-batching.

## Round 1 Follow-up 2 - 2026-06-07

### Implemented

- Added `vector_nibble4` as the `ex_bits=4` specialized route.
- Storage remains v15 variable ExData with vector-contiguous layout:
  `[lane][dim_block][official 4-bit nibble groups]`.
- Added official nibble4 pack/unpack and masked vector Stage2 kernel.
- Added parser/format-key, SIMD, storage round-trip, LoadCodes, and ClusterProber coverage.

### Verification

- `cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)`
- `./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober`
- `git diff --check`

All passed.

### COCO100k Result

For `total_bits=5/ex_bits=4`, `nlist=2048`, `nprobe=64`, `topk=10`,
`query_count=1000`, `non_safeout_candidate_budget=400`:

- `vector_bitplanes`: R@10 `0.9486`, avg `0.330380 ms`, p95 `0.416571 ms`.
- `vector_bitplanes_prefetch`: R@10 `0.9486`, avg `0.329941 ms`, p95 `0.415100 ms`.
- `vector_nibble4`: R@10 `0.9486`, avg `0.312664 ms`, p95 `0.383165 ms`.
- `cluster.clu`, resident code bytes, and resident cluster memory are unchanged:
  `45,244,416`, `35,682,040`, and `37,282,040` bytes respectively.

### Review Conclusion

The `ex_bits=4` specialized official nibble route is a meaningful improvement:
it keeps recall and storage/memory unchanged while improving avg latency by
about `5.4%` and p95 by about `8.0%` over vector bitplanes on COCO100k. The
remaining open items are `ex_bits=2` specialization/small-lane variants and the
broader `ex_bits=4` lossless compression target.

## Round 1 Follow-up 3 - 2026-06-07

### Implemented

- Added `vector_2bit` as the `ex_bits=2` specialized route.
- Storage remains v15 variable ExData with vector-contiguous layout:
  `[lane][dim_block][official 2-bit compact block]`.
- Added official 2-bit pack/unpack and masked vector Stage2 kernel.
- Added parser/format-key, SIMD, storage round-trip, LoadCodes, and ClusterProber coverage.

### Verification

- `cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)`
- `./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober`
- `git diff --check`

All passed.

### COCO100k Result

For `total_bits=3/ex_bits=2`, `nlist=2048`, `nprobe=64`, `topk=10`,
`query_count=1000`, `non_safeout_candidate_budget=400`:

- `vector_bitplanes`: R@10 `0.9532`, avg `0.371782 ms`, p95 `0.496264 ms`.
- `vector_2bit`: R@10 `0.9532`, avg `0.376670 ms`, p95 `0.499544 ms`.
- `cluster.clu`, resident code bytes, and resident cluster memory are unchanged:
  `32,251,904`, `22,882,040`, and `24,482,040` bytes respectively.
- Diagnostic timing with `fine-grained-timing=1` shows both layouts rerank the
  same number of vectors, `137.738` on average per query.

### Review Conclusion

The official 2-bit compact route is functionally correct and storage-neutral,
but it does not improve the low-overhead end-to-end latency on COCO100k. The
likely reason is that two bitplane mask-add operations are already cheaper than
2-bit unpack plus integer-to-float FMA in this workload. Keep `vector_2bit` as an
experimental negative control; the next `ex_bits=2` optimization should target
small-lane or survivor micro-batching while retaining bitplane arithmetic.

## Round 1 Follow-up 4 - 2026-06-07

### Implemented

- Added `small_lane4_bitplanes` as the lane-count reduction route.
- The outer Stage2 block remains 8-vector compatible, but payload is split into
  two 4-lane subgroups: `[subgroup4][dim_block][local_lane][bitplanes]`.
- Added parser/format-key, SIMD, storage round-trip, LoadCodes, and ClusterProber coverage.

### Verification

- `cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)`
- `./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober`
- `git diff --check`

All passed.

### COCO100k Result

For `total_bits=4/ex_bits=3`, `nlist=2048`, `nprobe=64`, `topk=10`,
`query_count=1000`, `non_safeout_candidate_budget=400`:

- `vector_bitplanes`: R@10 `0.9504`, avg `0.348227 ms`, p95 `0.446099 ms`.
- `small_lane4_bitplanes`: R@10 `0.9504`, avg `0.338932 ms`, p95 `0.431809 ms`.
- `cluster.clu`, resident code bytes, and resident cluster memory are unchanged:
  `38,658,048`, `29,282,040`, and `30,882,040` bytes respectively.

For `total_bits=3/ex_bits=2`:

- `vector_bitplanes`: R@10 `0.9532`, avg `0.371782 ms`, p95 `0.496264 ms`.
- `small_lane4_bitplanes`: R@10 `0.9532`, avg `0.385253 ms`, p95 `0.524674 ms`.
- Storage and resident memory are unchanged.

### Review Conclusion

The 4-lane route is a meaningful improvement for `ex_bits=3`, reducing avg
latency by about `2.7%` without recall or storage/memory regression. It is a
negative result for `ex_bits=2`, where the extra subgroup traversal does not pay
off. The next lane-count experiment should either try `small_lane2_bitplanes` or
survivor micro-batching, prioritizing `ex_bits=3`; `ex_bits=2` should stay on
`vector_bitplanes` for now.

## Round 1 Follow-up 5 - 2026-06-07

### Implemented

- Added `small_lane2_bitplanes` as the second lane-count reduction route.
- The outer Stage2 block remains 8-vector compatible, but payload is split into
  2-lane subgroups: `[subgroup2][dim_block][local_lane][bitplanes]`.
- Added parser/format-key, SIMD, storage round-trip, LoadCodes, and ClusterProber coverage.

### Verification

- `cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)`
- `./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober`
- `git diff --check`

All passed.

### COCO100k Result

For `total_bits=4/ex_bits=3`, `nlist=2048`, `nprobe=64`, `topk=10`,
`query_count=1000`, `non_safeout_candidate_budget=400`:

- `vector_bitplanes`: R@10 `0.9504`, avg `0.348227 ms`, p95 `0.446099 ms`.
- `small_lane4_bitplanes`: R@10 `0.9504`, avg `0.338932 ms`, p95 `0.431809 ms`.
- `small_lane2_bitplanes`: R@10 `0.9504`, avg `0.342380 ms`, p95 `0.439149 ms`.
- `cluster.clu`, resident code bytes, and resident cluster memory are unchanged:
  `38,658,048`, `29,282,040`, and `30,882,040` bytes respectively.

### Review Conclusion

The 2-lane route is correct and storage-neutral, and it improves over
`vector_bitplanes` by about `1.7%`, but it is worse than the 4-lane route by
about `1.0%`. The current best lane-count route for `ex_bits=3` is
`small_lane4_bitplanes`; further lane reduction is not worth making the default
without a different survivor micro-batching design.

## Round 1 Follow-up 6 - 2026-06-07

### Investigated

- Rechecked the official RaBitQ-Library Stage2 design:
  - Stage1 uses a 32-vector FastScan batch layout.
  - Stage2 `ex_data` is a per-vector compact record:
    `padded_dim * ex_bits / 8 + 2 * sizeof(float)`.
  - `packing_2bit_excode`, `packing_3bit_excode`, and `packing_4bit_excode`
    pack dimensions within one vector; their `vec_00_to_15` naming refers to
    dimensions, not candidate vectors.
  - Stage2 query computes each survivor vector independently with SIMD along
    the dimension axis.
- Re-ran the `ex_bits=4` distribution analyzer on both COCO100k
  `vector_bitplanes` and `vector_nibble4` official indexes.

### Evidence

- Updated design notes in `rabitq_code_zip/stage2_vector_layout_review.md`.
- Updated results in `rabitq_code_zip/stage2_vector_layout_results.md`.
- Distribution files:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/vector_bitplanes_ex4_distribution_v2.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/vector_nibble4_ex4_distribution.json`

### COCO100k Result

Both layouts decode to the same `ex_bits=4` distribution:

- Records: `100,000`
- Payload bytes: `25,600,000`
- Per-plane zero-word rates: `[0.00029, 0.00029, 0.00029, 0.00029]`
- Estimated high-plane mask+raw elide bytes: `25,705,528`
- Estimated saving: `-0.41%`

### Review Conclusion

The official library confirms the high-level direction: Stage2 should be
optimized around survivor-vector locality, with SIMD applied within each
vector's dimensions. The current best implemented candidates remain
`small_lane4_bitplanes` for `ex_bits=3` and `vector_nibble4` for `ex_bits=4`.
For COCO100k, the `ex_bits=4` lossless compression target is not supported by
the observed code distribution; the next useful compression evidence requires
official `total_bits=5/ex_bits=4` indexes on additional datasets.

## Round 1 Follow-up 7 - 2026-06-07

### Implemented

- Added `vector_bitplanes_microbatch` as a survivor micro-batch query route.
- The on-disk and resident layout is identical to `vector_bitplanes`; only the
  Stage2 IP kernel changes.
- The kernel collects requested survivor lanes, processes up to 4 lanes per
  micro-batch, reuses query loads across those lanes, and still applies SIMD
  along the dimension axis.

### Verification

- `git diff --check`
- `python3 -m py_compile rabitq_code_zip/analyze_vector_bitplanes.py`
- `cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)`
- `./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober`
- `cmake --build build --target bench_build_index bench_e2e -j$(nproc)`

All passed.

### COCO100k Result

For `total_bits=4/ex_bits=3`, `nlist=2048`, `nprobe=64`, `topk=10`,
`query_count=1000`, `non_safeout_candidate_budget=400`:

- `vector_bitplanes`: R@10 `0.9504`, avg `0.337759 ms`, p95 `0.428651 ms`,
  Stage2 `0.067374 ms`, avg reranked `99.845`.
- `small_lane4_bitplanes`: R@10 `0.9504`, avg `0.338932 ms`,
  p95 `0.431809 ms`, Stage2 `0.070318 ms`, avg reranked `99.845`.
- `vector_bitplanes_microbatch`: R@10 `0.9504`, avg `0.342787 ms`,
  p95 `0.444292 ms`, Stage2 `0.073778 ms`, avg reranked `99.845`.
- `cluster.clu`, resident code bytes, and resident cluster memory are unchanged:
  `38,658,048`, `29,282,040`, and `30,882,040` bytes respectively.

### Review Conclusion

The survivor micro-batch route is correct and storage-neutral, but it is a
negative result on COCO100k `ex_bits=3`: avg latency is about `1.5%` slower than
`vector_bitplanes`, and Stage2 time increases by about `9.5%`. The likely cause
is that each survivor still reads its own compact code, so the saved query-load
work is smaller than the added lane collection, micro-batch scheduling, and
multi-accumulator loop overhead. Do not make this the default hot path; keep
`small_lane4_bitplanes` as the best current `ex_bits=3` lane-count route.

## Round 1 Follow-up 8 - 2026-06-07

### Experiment

- Built a non-COCO official `total_bits=5/ex_bits=4` index for
  `voxceleb2_ecapa_150k_split_v1`.
- Used the existing 2048-cluster centroids and assignments:
  `/home/zcq/VDB/data/bench_e2e/voxceleb2_ecapa_150k/clustering/`.
- The temporary symlink dataset is under
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/datasets/voxceleb2_ecapa_150k_split_v1`.

### Evidence

- Index:
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/build_outputs_vox_ex4/voxceleb2_ecapa_150k_split_v1_20260607T031834/index_official_1_plus_n_total5_ex4_vector_bitplanes`
- Build log:
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vox_vector_bitplanes_ex4.log`
- Distribution:
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_distribution.json`

### Result

- Records: `150,000`
- Dim: `192`, dim blocks: `3`
- `cluster.clu`: `27,643,904` bytes
- Stage2 ex4 payload bytes: `14,400,000`
- Per-plane zero-word rates: `[0.0, 0.0, 0.0, 0.0]`
- Estimated high-plane mask+raw elide bytes: `14,458,947`
- Estimated saving: `-0.409%`

### Review Conclusion

The COCO100k compression failure is not a dataset-specific artifact. On
voxceleb2, the ex4 bitplanes are even less sparse: no 64-dim plane word is all
zero in the analyzed payload. Sparse/elide-style lossless compression cannot
bring `ex_bits=4` close to `total_bits=4/ex_bits=3`; it slightly increases
payload size. If the ex4 space target remains mandatory, the only remaining
direction is block-level entropy compression with explicit random-access
decompression cost. Otherwise, the paper should report ex4 space compression as
a negative result and keep `vector_nibble4` only as a compute optimization.

## Round 1 Follow-up 9 - 2026-06-07

### Implemented

- Extended `rabitq_code_zip/analyze_vector_bitplanes.py` with a block-level
  compression prototype for ex4 Stage2 payloads.
- The analyzer now reports code-value entropy and can evaluate independent
  random-access blocks with `zlib`, `lzma`, `lz4`, and `zstd`.
- Compression accounting includes the block offset table, so the reported size
  is the size a random-access online query path would need to keep.

### Verification

- `python3 -m py_compile rabitq_code_zip/analyze_vector_bitplanes.py`
- COCO100k ex4 compression runs:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/coco_vector_bitplanes_ex4_block_compression.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/coco_vector_bitplanes_ex4_block_compression_zstd_lz4.json`
- voxceleb2 ex4 compression runs:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_block_compression.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_block_compression_zstd_lz4.json`

### Result

- COCO100k entropy: `3.984887 bit/dim`; ideal entropy lower bound is still
  `1.3283x` the ex3 payload target.
- voxceleb2 entropy: `3.989137 bit/dim`; ideal entropy lower bound is still
  `1.3297x` the ex3 payload target.
- COCO100k best measured block compression: `zlib6`, 64KB blocks,
  `25,604,614` bytes including offset table, `1.3336x` ex3 target, and
  `26.33 us` random-block decompression.
- COCO100k practical fast codec: `zstd1`, 64KB blocks, `25,607,046` bytes,
  `1.3337x` ex3 target, and `3.20 us` random-block decompression.
- voxceleb2 best measured block compression: `zstd1`, 64KB blocks,
  `14,403,968` bytes including offset table, `1.3337x` ex3 target, and
  `2.52 us` random-block decompression.

### Review Conclusion

Block-level entropy compression does not rescue the ex4 space target. The code
distribution is too close to full 4-bit entropy, and every tested independent
random-access codec slightly increases payload size after block headers/offsets.
This route should not be implemented in the online hot path. The defensible
paper position is: ex4 can use `vector_nibble4` for compute speed, but lossless
space compression close to ex3 is a negative result under the current RaBitQ
code distribution.

## Round 1 Follow-up 10 - 2026-06-07

### Experiment

- Re-ran COCO100k `total_bits=4/ex_bits=3` online query benchmarks using only
  existing indexes.
- First pass: 6 layouts, 5 repeats each.
- Second pass: 4 key layouts, 5 repeats each with interleaved order to reduce
  ordering and thermal bias.

### Evidence

- Sequential route-selection outputs:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/ex3_route_selection_20260607T0328/`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_20260607T0328_summary.json`
- Interleaved outputs:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/ex3_route_selection_interleaved_20260607T0330/`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_interleaved_20260607T0330_summary.json`
- Combined 10-run key-layout summary:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_combined_20260607T0330_summary.json`

### Result

Combined 10-run key-layout result:

- `vector_bitplanes_prefetch`: R@10 `0.9504`, avg `0.337183 ms`,
  p95 `0.431985 ms`, p99 `0.512925 ms`, Stage2 `0.068106 ms`.
- `vector_bitplanes`: R@10 `0.9504`, avg `0.337431 ms`,
  p95 `0.431544 ms`, p99 `0.511607 ms`, Stage2 `0.068673 ms`.
- `split3_trimmed`: R@10 `0.9504`, avg `0.342518 ms`,
  p95 `0.440187 ms`, p99 `0.527966 ms`, Stage2 `0.071546 ms`.
- `small_lane4`: R@10 `0.9504`, avg `0.343722 ms`,
  p95 `0.439989 ms`, p99 `0.527728 ms`, Stage2 `0.072457 ms`.
- All layouts have the same resident code bytes (`29,282,040`), resident
  cluster memory (`30,882,040`), and average reranked vectors (`99.845`).

### Review Conclusion

The earlier `small_lane4_bitplanes` improvement is not stable. After repeated
and interleaved runs, the final `ex_bits=3` default should be
`vector_bitplanes`; `vector_bitplanes_prefetch` can remain as an optional
variant because its avg latency is marginally lower, but the difference from
plain `vector_bitplanes` is only about `0.07%` and within run noise. Relative
to the old `split3_trimmed` path, `vector_bitplanes` preserves recall, storage,
resident memory, and rerank count while improving avg latency by about `1.49%`
and Stage2 time by about `4.0%`.

## Round 1 Follow-up 11 - 2026-06-07

### Implemented

- Moved the final route selection into default build behavior.
- `selected_direct` now resolves to the final direct layout:
  - `ex_bits=1,2,3` -> `vector_bitplanes`
  - `ex_bits=4` -> `vector_nibble4`
- Added `RaBitQDefaultOfficialExDataLayoutForBits`.
- `bench_build_index` / `bench_e2e` now use that default only when official
  1+n is enabled and the user did not explicitly pass `--rabitq-exdata-layout`.
- Explicit `generic_packed` remains available and keeps its old meaning, so old
  index reading semantics are not changed.

### Verification

- `cmake --build build --target test_types bench_build_index -j$(nproc)`
- `./build/test_types`
- `cmake --build build --target test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)`
- `./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober`

Smoke build without explicit `--rabitq-exdata-layout`:

- official `total_bits=4/ex_bits=3` produced:
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/default_layout_smoke_outputs/default_layout_smoke_dataset_20260607T033821/index_official_1_plus_n_total4_ex3_vector_bitplanes`
- official `total_bits=5/ex_bits=4` produced:
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/default_layout_smoke_outputs/default_layout_smoke_dataset_20260607T033822/index_official_1_plus_n_total5_ex4_vector_nibble4`

### Review Conclusion

The implementation now matches the final local route decision. Future official
1+n builds get the chosen Stage2 layout by default, while old `generic_packed`
semantics remain explicit and readable. This closes the gap where the
experiment-selected route existed but a default build could still silently use
the old generic format.
