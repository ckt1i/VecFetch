# Span Planner CPU Optimization Log

Date: 2026-07-18

## Outcome

Three profile-driven rounds were completed without changing planner semantics.
The exact planner remains more expensive than greedy in isolation, but its
query-level cost is below 0.07 ms/query on the tested core datasets. All
targeted tests and the final 43-test CTest suite pass.

## Round 1: shared pure planner and reusable workspace

The planner was separated from async/serial execution, SafeIn credit was
resolved once, scratch storage was reused, and sort/planner time was measured
separately.

Pinned-core microbenchmark baseline (ns/run):

| mode | m=16 | m=21 |
|---|---:|---:|
| GV | 144.316 | 185.751 |
| SV | 130.278 | 167.734 |
| GE | 594.459 | 851.594 |
| SE | 604.048 | 870.131 |

For MSMARCO GE q100, planner time was 0.070156 ms/query. Whole-process perf
reported about 140.17B instructions across 10M GE microbenchmark cases; branch
misses were below 0.01%, pointing to fixed setup/instruction cost rather than
branch prediction.

## Round 2: reserve high-water mark

`SpanPlannerScratch::Impl::Reserve` now skips eight repeated `vector::reserve`
checks when the current run fits the existing high-water mark.

- GE instructions fell from about 140.17B to 137.69B on the same 10M-case
  workload (-1.8%).
- Repeated pinned-core measurements improved exact time by roughly 2%--7%,
  depending on mode and run size.
- `test_span_planner` and `test_overlap_scheduler` remained green.

## Round 3: bounded direct-DP fast path

For `m<=8`, exact planning uses a constant-bounded quadratic DP and avoids
coordinate compression and Fenwick setup. For larger runs it retains the
one-dimensional endpoint-dominance solver, so the asymptotic bound remains
`O(n log n)`. The fast path preserves the same overflow contract and
lexicographic objective.

Query-level effect:

| dataset | mode | planner ms/query | requests/query | bytes/query |
|---|---|---:|---:|---:|
| MSMARCO | GV | 0.022591 | 308.88 | 3,814,674.56 |
| MSMARCO | GE | 0.062879 | 308.35 | 3,817,422.08 |
| ESCI | GV | 0.023027 | 336.09 | 2,688,987.92 |
| ESCI | GE | 0.060121 | 332.14 | 2,707,205.36 |

MSMARCO GE planning fell from about 0.0702 to 0.0629 ms/query (-10.4%).
Short runs are handled directly; the observed Fenwick operations fell from
about 957 to 700/query while request optimality was unchanged.

## SIMD evaluation

GCC vectorization diagnostics report zero vectorized loops in `PlanSpanRun`.
The key loops are blocked by control flow, `unsigned __int128` checked
arithmetic, DP dependencies, and Fenwick dependencies. The compiler already
uses SLP vectorization for adjacent group/state stores. With observed
`m<=21` and a sub-0.07 ms/query planner cost, manual SIMD would add complexity
without a credible end-to-end return, so it was rejected.

## Winner confirmation microbenchmark

Compiled GE/SE sweeps for `m=1..32` are stored at:

- `/home/zcq/VDB/test/recordgate_span_exact_secondary_screen_20260718/cpu/GE_m1_32.csv`
- `/home/zcq/VDB/test/recordgate_span_exact_secondary_screen_20260718/cpu/SE_m1_32.csv`

At `m=1..8`, GE ranges from 39.3 to 264.1 ns/run and SE from 41.8 to 305.5
ns/run. At `m=32`, GE and SE are 1.27 and 1.26 us/run, respectively.

## Validation

- Planner tests: 10/10.
- Scheduler tests: 67/67.
- Full CTest: 43/43.
- Five-repetition secondary screen: zero planner fallbacks; planned/issued
  request and byte accounting matches in every result.
