# SafeIn Prefetch Budget Refinement

Date: 2026-06-02

## Problem Anchor

- Bottom-line problem: keep the accepted `frontier_blend + defer4` SafeIn mechanism, but prevent one query from issuing too many SafeIn `VEC_ALL` prefetches when the dataset payload size or local I/O path makes aggressive prefetch counterproductive.
- Must-solve bottleneck: current SafeIn has a per-record guard (`safein_all_threshold = 256KB`) but no per-query or per-window prefetch budget. A difficult query or a large-payload dataset can consume I/O queue slots with speculative payload reads even when payload-tail savings are small.
- Non-goals: do not change recall semantics, do not train a new ANN model, do not replace `blend000_defer4`, and do not make SafeIn depend on ground truth.
- Constraints: implementation should fit the existing `OverlapScheduler` read plan path, use online statistics available during search, and preserve `VEC_ONLY` rerank correctness when a SafeIn candidate is not admitted for payload prefetch.
- Success condition: same recall as `blend000_defer4`, lower p95/p99 or lower I/O pressure on large-payload / high-latency datasets, and no meaningful slowdown on MS MARCO where current average SafeIn prefetch is already modest.

## Current Evidence

MS MARCO e2e, 200 queries, no runtime static calibration:

- `static_loaded`: recall@10 0.9425, avg 11.497 ms, payload reads 9.905/query.
- `blend000_defer4`: recall@10 0.9430, avg 11.437 ms, SafeIn `VEC_ALL` 8.875/query, payload reads 3.260/query.
- `blend010_defer4_rerun`: recall@10 0.9430, avg 11.638 ms, SafeIn `VEC_ALL` 9.260/query.
- `gap_rel013_defer4`: recall@10 0.9425, avg 12.196 ms, SafeIn blocked.

MS MARCO record size estimate from the loaded index:

- `data.dat = 36,224,487,424 bytes`
- `N = 8,841,823`
- average record size is about `4097 bytes`
- vector bytes are `768 * 4 = 3072`
- average payload bytes are about `1025`

Interpretation: `blend000_defer4` saves about 6.6 final payload reads/query, but the payload tail is only about 0.010 ms/query in the resident-cluster e2e run. The budget mechanism is therefore mainly a stability and cross-dataset guard, not a major MS MARCO speed lever.

## Literature Grounding

| Work | Mechanism | Relation to Our SafeIn Budget |
|---|---|---|
| DiskANN, NeurIPS 2019 | On-disk graph search with SSD-friendly beam search and selected in-memory caching. | Uses beam width and cached nodes to control disk traversal, but not query-level payload prefetch admission for already-classified SafeIn candidates. |
| SPANN, NeurIPS 2021 | Memory-disk inverted index, balanced posting lists, closure replication, and query-aware dynamic pruning. | Closest conceptual precedent for bounding per-query CPU/I/O work. It prunes posting lists/machines, not SafeIn payload prefetches after candidate classification. |
| DiskANN++, arXiv 2023 | Query-sensitive entry vertex and page-oriented asynchronous search to reduce routing I/O. | Query-aware I/O reduction, but at graph traversal/layout level rather than payload prefetch budget. |
| Starling, SIGMOD 2024 | Segment-local disk-resident graph layout plus block search to reduce disk I/O. | Optimizes disk layout and block search granularity; orthogonal to deciding how many SafeIn payloads to speculatively fetch. |
| PipeANN, OSDI 2025 | Aligns best-first graph search with SSD by avoiding strict compute-I/O order and improving pipeline utilization. | Strong precedent for hardware-aware asynchronous scheduling. Our proposal uses this spirit at the payload-prefetch admission layer. |
| GoVector, arXiv 2025 | Static and dynamic cache for disk-based graph indices, with locality-aware disk reordering. | Relevant dynamic caching work. It caches graph/vector nodes based on locality, not query-local SafeIn admission based on frontier confidence and byte budget. |
| PageANN, arXiv 2025 | Page-node graph aligned to SSD pages plus coordinated memory-disk allocation. | Storage-layout and page granularity optimization; complementary to per-query prefetch caps. |
| DistVS, NSDI 2026 | Three-tier precision layout; exact vectors are coldest and only read for promising candidates; includes async execution and batching. | Very relevant principle: progressively prune accesses as data becomes more expensive. Our SafeIn budget is a local version for payload/full-record reads. |
| QVCache, arXiv 2026 | Backend-agnostic query-level cache exploiting semantic query repetition with learned thresholds. | Query-level and memory-bounded, but caches full ANN results across queries. Our proposal controls within-query speculative prefetch. |
| DiskSeen, USENIX 2007 | Disk-level prefetching with accuracy, eagerness, aggressiveness, and adaptive split between prefetching/caching space. | Good systems precedent for adaptive prefetch aggressiveness; not vector-search-specific. |
| AquaPipe, SIGMOD/PACMMOD 2025 | Recall-aware early retrieval for overlapping disk ANNS with LLM prefill. | Similar quality-aware early pipeline idea, but targets RAG prefill overlap, not per-query SafeIn payload I/O budget. |

Conclusion: there is strong related work on I/O-aware ANN, query-aware pruning, caching, and adaptive prefetching. I did not find a direct prior that combines (1) online SafeIn frontier confidence, (2) deferred candidate reclassification, and (3) per-query byte/count admission for payload/full-record prefetch. The closest prior is SPANN's query-aware dynamic pruning, but the control point is different.

## Recommended Route: Budgeted SafeIn Admission

Keep the accepted default:

```text
--dynamic-safein frontier_blend
--dynamic-safein-scale 0.0
--dynamic-safein-stable-probes 1
--dynamic-safein-defer-initial-clusters 4
--dynamic-safein-defer-until-ready 1
```

Add a lightweight admission layer immediately before a candidate becomes `VEC_ALL`.

### New Config Knobs

```cpp
uint32_t dynamic_safein_prefetch_max_count = 0;   // 0 = unlimited
uint32_t dynamic_safein_prefetch_max_bytes = 0;   // 0 = unlimited
uint32_t dynamic_safein_prefetch_window = 0;      // 0 = whole query
float dynamic_safein_budget_slack_alpha = 1.0f;  // optional controller
```

For the first implementation, use whole-query count/byte caps only. Leave the controller optional.

### Admission Rule

For each candidate that satisfies the existing SafeIn condition:

```text
U_i < T_q
addr.size <= safein_all_threshold
```

it is admitted as `VEC_ALL` only if:

```text
safein_prefetch_count + 1 <= max_count
safein_prefetch_bytes + addr.size <= max_bytes
```

Otherwise, it is emitted as `VEC_ONLY`, preserving rerank correctness.

### Priority Rule

When candidates are available in a deferred flush, do not admit in arbitrary discovery order. Sort by confidence density:

```text
margin_i = T_q - U_i
score_i  = margin_i / max(addr.size, 1)
```

Admit the highest `score_i` candidates under the count/byte budget. This gives priority to candidates that are both safer and cheaper.

For non-deferred immediate candidates, use a small pending SafeIn heap until the next submit boundary. If that feels too invasive, start with discovery order and add priority sorting only for deferred flushes.

### Conservative Defaults

Use dataset-derived defaults only when the user enables auto-budgeting:

```text
max_count = min(2 * top_k, io_queue_depth / 4)
max_bytes = max_count * avg_record_bytes
```

For the current MS MARCO index:

```text
avg_record_bytes ~= 4097
top_k = 10
io_queue_depth = 64
max_count = 16
max_bytes ~= 64KB
```

This should not hurt current `blend000_defer4`, which only uses about 8.875 `VEC_ALL` reads/query on average. It mainly caps pathological queries and larger-payload datasets.

## Optional Route 2: I/O-Aware Adaptive Budget

Static caps are robust but may underuse fast devices and overuse slow devices. Add a global EWMA controller per index/search worker:

```text
pressure = EWMA(io_wait_ms + submit_emit_ms)
tail_need = EWMA(remaining_payload_fetch_ms)
```

Update budget every epoch, e.g. every 256 queries:

```text
if pressure > pressure_target:
    budget_bytes *= 0.8
elif tail_need > tail_target and pressure < pressure_target / 2:
    budget_bytes += avg_record_bytes
```

Clamp:

```text
budget_bytes in [top_k * avg_record_bytes / 2, 4 * top_k * avg_record_bytes]
budget_count in [top_k / 2, 4 * top_k]
```

This is an AIMD-style controller. It is simple, online, and does not require ground truth. It adapts to local SSD, page cache state, payload size, and concurrent load.

Recommended use: not as the first patch. Implement static budget first, then add adaptive budget once we have per-query budget stats.

## Optional Route 3: Global Token Bucket Across Concurrent Queries

If the benchmark moves from one-query-at-a-time to concurrent serving, per-query caps are insufficient. Add a shared token bucket for speculative `VEC_ALL` bytes:

```text
global_tokens_bytes += refill_rate_bytes_per_ms * elapsed_ms
if query wants VEC_ALL:
    admit only if query_budget_ok && global_tokens_bytes >= addr.size
```

This prevents many simultaneous easy queries from overfilling the data I/O queue with payload prefetch. It also lets unused budget from light queries be consumed by heavier queries.

Recommended use: only after single-query budget is validated.

## Minimal Validation Plan

### Experiment 1: No-regression on Current Datasets

Compare:

```text
blend000_defer4
blend000_defer4 + cap_count=16
blend000_defer4 + cap_count=8
blend000_defer4 + cap_bytes=64KB
blend000_defer4 + cap_bytes=32KB
```

Datasets:

- COCO100k e2e/vector
- MS MARCO e2e/vector

Metrics:

- recall@10
- avg/p50/p95/p99 latency
- all reads/query
- vec-only reads/query
- payload reads/query
- remaining payload fetch ms
- dynamic SafeIn budget admitted/rejected counts

Expected:

- `cap_count=16` and `cap_bytes=64KB` should match current MS MARCO speed and recall.
- Smaller caps may increase final payload reads but can reduce p99 on payload-heavy datasets.

### Experiment 2: Payload-Heavy Stress Test

Use an existing dataset adapter or synthetic payload inflation:

```text
payload_size_multiplier in {1, 4, 16, 64}
```

Expected:

- uncapped SafeIn becomes worse as payload grows.
- byte budget prevents p95/p99 blowup.
- priority-by-margin/byte beats simple discovery-order cap.

### Experiment 3: Device / Load Sensitivity

Run under:

- resident cluster + normal page cache
- cold-ish data path if practical
- background I/O load using a controlled reader/writer

Expected:

- adaptive budget decreases under high `io_wait`.
- static budget remains predictable but less optimal.

## Implementation Notes

Where to attach:

- `FlushDeferredSafeInPlans`: apply sorted budget admission before submitting `VEC_ALL`.
- Immediate SafeIn path: track per-query `safein_prefetch_count` and `safein_prefetch_bytes`; reject over-budget candidates to `VEC_ONLY`.
- `SearchStats`: add admitted/rejected count and bytes.
- Bench flags: expose `--dynamic-safein-prefetch-max-count`, `--dynamic-safein-prefetch-max-bytes`, and optionally `--dynamic-safein-prefetch-auto-budget`.

Correctness invariant:

```text
Budget rejection must never drop a candidate.
It only changes VEC_ALL -> VEC_ONLY.
```

This means recall should be unchanged; only latency and payload-fetch timing change.

## Recommended Next Step

Implement Route 1 first:

```text
Budgeted SafeIn Admission with count + byte caps,
priority sorting for deferred flush,
and stats for admitted/rejected/bytes.
```

Initial defaults for experiments:

```text
cap_count in {8, 16}
cap_bytes in {32KB, 64KB, 128KB}
```

Do not enable adaptive control as default until the static cap experiment shows which pressure signals are stable.
