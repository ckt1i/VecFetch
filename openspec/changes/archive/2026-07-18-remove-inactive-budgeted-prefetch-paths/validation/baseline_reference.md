# Pre-cleanup no-cap reference

Date: 2026-07-17

## Canonical source

- Runner: `/home/zcq/VDB/test/recordgate_p0_p1_cpu_memory_autoreview_20260717/scripts/run_formal_sweep.sh`
- Frozen binary: `/home/zcq/VDB/test/recordgate_p0_p1_cpu_memory_autoreview_20260717/bin/bench_e2e_after`
- Representative result: `/home/zcq/VDB/test/recordgate_p0_p1_cpu_memory_autoreview_20260717/runs/formal_sweep/msmarco_passage/k100/np96/SafeIn/rep1/results.json`
- Output family: `/home/zcq/VDB/test/recordgate_p0_p1_cpu_memory_autoreview_20260717/runs/formal_sweep/`

## Frozen operating point

- Dataset/index: MSMARCO passage, `/home/zcq/VDB/test/recordgate_vec_span_stage1_20260715/stores/msmarco_passage/compact_g4_prefix0_align16`
- Query/GT: 500 holdout queries from `recordgate_safein_confidence_bextra_20260712/inputs/msmarco_passage/holdout`
- `topk=100`, `nprobe=96`, active/resident bits `4/4`
- two-level coarse routing enabled, threshold `4096`, factor `16`, cap `12288`
- payload cache mode `drop-before-queries`; each formal repetition has a 500-query warmup run
- shared submission, queue depth `64`, fixed vector buffers `1024`, submit batch `32`
- vector span `64KiB/1.5x`, payload reuse enabled, SafeIn tail count `8`
- `materialization=late`, dynamic SafeOut enabled, Dynamic SafeIn frontier
- pre-cleanup deleted controls: candidate budget `0`, budgeted prefetch `0`, optional early-submit `0`

## Representative pre-cleanup metrics

| Metric | Value |
|---|---:|
| Recall@100 (`recall_at_k`) | 0.868660 |
| avg query time | 2.723287 ms |
| QPS derived as `1000/avg_ms` | 367.203 |
| avg total probed | 61,324.478 |
| avg candidates reranked | 1,022.126 |
| avg vector physical requests | 326.582 |
| avg vector read bytes | 3,882,349.824 |
| avg span requests | 84.450 |
| avg span bytes | 3,138,520.320 |
| avg payload requests | 53.252 |
| avg payload bytes | 54,947.840 |
| avg span reuse hits | 46.748 |
| avg SafeIn tails extended | 6.116 |
| candidate-budget/speculative counters | all 0 |
| optional early-submit calls/requests | 0 / 0 |

This is an identified pre-cleanup reference rather than a newly executed run. It was produced by the frozen binary before the removal and explicitly records all three removed controls as zero.

## Preservation boundary

The cleanup must retain SafeOut; mandatory `VEC_ONLY`; amplification-bounded `VEC_SPAN`; span payload views/reuse; SafeIn tail and external-prefix policy; normal optional-I/O admission; final missing-payload materialization; fixed submit/tail flush/final drain; and `SerialNoOverlap` as a benchmark-only execution mode.

