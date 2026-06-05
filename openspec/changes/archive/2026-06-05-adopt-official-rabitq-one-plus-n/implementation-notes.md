# Official RaBitQ 1+n Implementation Notes

## Baseline `RaBitQConfig.bits` Audit

Current `bits` is a legacy signed-magnitude Stage2 width, not an official
`total_bits` value. The current use sites fall into these groups:

- Public config and equality: `include/vdb/common/types.h`.
- ConANN error-bound semantics: `include/vdb/index/conann.h`,
  `src/index/conann.cpp`, and `tests/index/conann_test.cpp`.
- Build, encoding, epsilon calibration, SafeIn d_k calibration, FlatBuffer
  metadata, and build JSON metadata: `include/vdb/index/ivf_builder.h` and
  `src/index/ivf_builder.cpp`.
- Cluster storage version selection, `.clu` header serialization, Region2
  layout sizing, reader parsing, resident views, and `LoadCodes` round trips:
  `include/vdb/storage/cluster_store.h`,
  `src/storage/cluster_store.cpp`, and
  `tests/storage/cluster_store_test.cpp`.
- Parsed query views and Stage2 Region2 accessors:
  `include/vdb/query/parsed_cluster.h`.
- Stage2 classification and query scheduling:
  `include/vdb/index/cluster_prober.h`,
  `src/index/cluster_prober.cpp`,
  `include/vdb/query/overlap_scheduler.h`, and
  `src/query/overlap_scheduler.cpp`.
- RaBitQ encoder/estimator semantics:
  `include/vdb/rabitq/rabitq_encoder.h`,
  `src/rabitq/rabitq_encoder.cpp`,
  `include/vdb/rabitq/rabitq_estimator.h`, and
  `src/rabitq/rabitq_estimator.cpp`.
- SIMD Stage2 helpers and tests:
  `include/vdb/simd/ip_exrabitq.h`,
  `src/simd/ip_exrabitq.cpp`,
  `include/vdb/simd/stage2_classify.h`,
  `src/simd/stage2_classify.cpp`,
  `tests/simd/ip_exrabitq_test.cpp`, and
  `tests/simd/classify_masks_test.cpp`.
- Reader/query metadata and benchmark output:
  `benchmarks/bench_e2e.cpp`, `benchmarks/bench_online_query.cpp`,
  `benchmarks/bench_compare_stage_paths.cpp`,
  `benchmarks/bench_index_geometry.cpp`,
  `benchmarks/bench_safein_dk_samples.cpp`,
  `benchmarks/bench_dk_space_compare.cpp`,
  `benchmarks/bench_phase_a_grid.cpp`, and
  `benchmarks/bench_rabitq_diagnostic.cpp`.
- Diagnostic payload comparison:
  `benchmarks/bench_compare_exrabitq_payloads.cpp`.
- Segment/schema tests and readers:
  `schema/segment_meta.fbs`, `src/index/ivf_index.cpp`,
  `include/vdb/index/ivf_index.h`, `tests/schema/schema_test.cpp`,
  `tests/common/types_test.cpp`, and `tests/storage/segment_test.cpp`.

## Legacy Signed-Magnitude Fields

The current v10/v11/v12 ExRaBitQ path stores:

- Stage1 FastScan packed sign codes in Region1, generated from the MSB/sign
  plane and used for Stage1 candidate generation.
- Stage2 magnitude payload in Region2 as `ex_code`. v10 stores per-vector
  entries. v11 stores batch-major blocks with byte magnitudes. v12 stores
  batch-major blocks with packed magnitudes for supported bit widths.
- `ex_sign_packed`, a separate packed sign stream for Stage2. v10 stores it
  per vector. v11/v12 store it as batch-major sign blocks.
- `xipnorm`, the legacy signed-magnitude correction factor. v10 stores one
  scalar per vector. v11/v12 store one scalar per lane in each batch block.
- Metadata currently exposes `bits`, `safein_dk_bits`, and `.clu` storage
  version. It does not distinguish legacy Stage2 payload bits from official
  RaBitQ `total_bits`.

## Legacy To Official Storage Mapping

- v10: legacy signed-magnitude per-vector Region2 entries
  (`ex_code + ex_sign_packed + xipnorm`).
- v11: legacy signed-magnitude batch-major Region2 blocks with byte
  magnitudes, separate sign blocks, and lane `xipnorm` factors.
- v12: legacy signed-magnitude batch-major Region2 blocks with packed
  magnitudes, separate sign blocks, and lane `xipnorm` factors.
- v13: official `1+n` sign-folded ExData Region2 blocks. `total_bits` is the
  reported RaBitQ precision, `ex_bits = total_bits - 1` is the Stage2 payload
  width, negative residual dimensions are represented by complemented ExData
  codes, and no independent `ex_sign_packed` payload is persisted.

## COCO100k Official `total_bits=4` Validation

Build command:

```bash
./build/benchmarks/bench_build_index \
  --dataset /home/zcq/VDB/data/coco_100k \
  --nlist 2048 --nprobe 64 --topk 10 --bits 4 \
  --rabitq-estimator-mode official_1_plus_n \
  --rabitq-total-bits 4 --rabitq-ex-bits 3 \
  --epsilon-percentile 0.90 \
  --centroids /home/zcq/VDB/data/coco_100k/coco_centroid_2048.fvecs \
  --assignments /home/zcq/VDB/data/coco_100k/coco_cluster_id_2048.ivecs \
  --calibration-samples 1000 --epsilon-samples 100 --max-iter 20 --seed 42
```

Output index:
`/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_eps0.90`

Index size summary:

| index | total | cluster.clu | resident file bytes | resident code bytes |
| --- | ---: | ---: | ---: | ---: |
| official_1_plus_n_total4_ex3 | 439M | 40,296,448 | 40,296,448 | 30,680,572 |
| legacy_signed_magnitude_bits4 | 649M | 80,171,008 | 80,171,008 | 71,196,152 |

The official index writes `.clu` version 13 and reports
`rabitq_total_bits=4`, `rabitq_ex_bits=3`,
`rabitq_estimator_mode=official_1_plus_n`.

Query probe commands used 20 COCO100k queries with `nprobe=64`. The bundled
`groundtruth_top10.npy` stores row ids, so it was converted to image ids at
`/home/zcq/VDB/test/data/COCO100k/groundtruth_top10_image_ids.npy` before
recall measurement.

| index | recall@10 | avg ms | p95 ms | peak RSS KiB | avg SafeOut | avg rerank |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| official_1_plus_n_total4_ex3 | 0.0500 | 0.9470 | 1.3869 | 81,516 | 2,614.05 | 94.75 |
| legacy_signed_magnitude_bits4 | 0.0500 | 0.4781 | 0.7388 | 181,456 | 2,593.05 | 100.20 |

The short 20-query probe shows the expected memory reduction from official
`1+3` storage. Recall is equal on this small slice; the official Stage2 path is
currently slower than legacy Stage2 in this probe, so larger-query latency runs
should be used before claiming a QPS improvement.

The conditional extension to Amazon ESCI and the remaining Pareto datasets was
not run in this change because the COCO100k probe did not establish a recall/QPS
target suitable for promotion beyond the COCO validation gate.
