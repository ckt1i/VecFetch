# Span Four-Way Experiment Bridge Execution Plan

Date: 2026-07-18

## Scope

Implement GV/SV/GE/SE, complete correctness gates, perform at least three
profile-driven CPU optimization rounds, then run the four-way screen and the
conditional GE/SE ablation requested by the user.

## Frozen implementation contract

- Planner invocation scope: one address-sorted, same-tile run visible to the
  current async/serial flush.
- Objective for GE/SE: lexicographic `(physical requests, physical bytes)`.
- Admission:

  `B(i,j) <= alpha * (V(i,j) + rho * S_internal(i,j))`

- `alpha=3/2`; supported `rho={0,1/2,1}` use exact rational arithmetic.
- GV/GE force `rho=0`; SV/SE use configured rho.
- SafeIn credit is the actual reusable inline payload/prefix bytes of internal
  members only. Endpoint, descriptor, padding, cold/external payload,
  vector-sidecar, metadata miss, disabled reuse, and invalid metadata receive
  zero credit.
- Async and serial execution consume the same pure planner output.
- Exact uses one-dimensional endpoint dominance in `O(n log n)` and must match
  an independent `O(n^2)` oracle on randomized small runs.

## Gates

1. B0 correctness and differential tests.
2. Query sanity with semantic parity and complete JSON provenance.
3. Three CPU optimization rounds, each retaining B0 and query parity.
4. Four-way screen on the preregistered core cells.
5. If SV/GE/SE all fail to beat GV, run the preregistered secondary screen on
   MSMARCO, ESCI, and VoxCeleb2:
   - GE matched baseline: GV.
   - SE matched baselines: SV for exactness and GE for the SafeIn factor.
   - At least two of three datasets must satisfy paired QPS non-inferiority
     (`>=-1%`), p99 regression `<=3%`, issued-byte increase `<=5%`, request
     count not higher than the matched greedy planner, correctness, and the
     physical-amplification gate.
   - The original four core cells must not contain a hard-gate failure.
6. A secondary-screen winner is labelled `theory-preferred, empirically
   non-inferior default`, not a performance winner. Formal main results can
   still force fallback to GV.

## Required optimization rounds

- Round 1: shared planner, reusable workspace, one-time credit resolution,
  phase-separated telemetry.
- Round 2: perf-guided removal of map/key/Fenwick/branch overhead.
- Round 3: evaluate SIMD/data-layout preprocessing; retain only changes that
  reduce measured cycles without E2E regression.

## Completed outcome

- Auto-review Round 3: 9.1/10, READY.
- CPU rounds retained the scratch high-water guard and bounded direct-DP fast
  path for `m<=8`; manual SIMD was rejected after compiler diagnostics.
- Full CTest: 43/43.
- Five q500 repetitions on MSMARCO, ESCI, and VoxCeleb2 completed with
  Latin-rotated mode order.
- At nprobe=192, GE-GV and SE-SV each pass the secondary gate on 2/3
  datasets. The five-repetition nprobe=96 main experiment separates them:
  GE-GV passes both core datasets, while SE-SV fails both by the -1% QPS gate.
- Rho=1/2 improves SE-GE but SE still regresses versus GV by about 1.2%--1.3%.
- Selected default: GE, labelled `theory-preferred, empirically non-inferior`.
  GV remains the absolute-latency and simplicity baseline; SV/SE remain
  supplementary SafeIn-credit ablations.
- Current-binary NoSpan anchors pass, and representative NoCombine/NoPipeline
  drift checks preserve the prior claim directions.
- The selected planner model is tail-free. All winner confirmation, main, and
  drift-check runs use `vec_span_safein_tail_count=0`. The legacy endpoint
  tail is not restored unless a separate post-partition model is introduced.

