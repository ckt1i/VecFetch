# Research Review: Why Candidate Pool and Quality-Aware Gate Did Not Win

Timestamp: 2026-06-02T11:38:18Z

Reviewer agent: `019e881c-08bd-7763-b0fb-05cd3cacc10e`

## Question

We reviewed why two proposed extensions did not reach or exceed expectations
in the latest Dynamic SafeIn experiments:

1. A sustained candidate pool where SafeIn is prefetched immediately, SafeOut is
   dropped, and Uncertain candidates stay in a pool for later reclassification.
2. A quality-aware early-start gate where SafeIn starts based on query
   convergence signals rather than fixed `defer_initial_clusters=4`.

## Evidence Base

Main result files:

- `safeIn_dk_review_logs/blend0_check/summary.md`
- `safeIn_dk_review_logs/quality_gate_check/summary.md`
- `review-stage/AUTO_REVIEW.md`

Current default:

```text
--dynamic-safein frontier_blend
--dynamic-safein-scale 0.0
--dynamic-safein-stable-probes 1
--dynamic-safein-defer-initial-clusters 4
--dynamic-safein-defer-until-ready 1
```

Key numbers:

| scheme | COCO coverage | COCO false | COCO e2e avg | MSMARCO avg | outcome |
|---|---:|---:|---:|---:|---|
| `blend000_defer4` | 47.41% | 11.53% | 0.780 ms | 62.715 ms | default |
| `defer0_stable4` | 55.96% | 18.22% | 0.961 ms | 64.392 ms | worse e2e |
| `defer0_stable5` | 53.87% | 12.16% | 1.127 ms | 64.851 ms | worse e2e |
| `defer4_gap_rel_0.013` | 35.85% | 9.15% | 1.009 ms | 64.884 ms | not robust |

## Why Sustained Candidate Pool Did Not Meet Expectations

The sustained pool idea assumes that an Uncertain candidate can become SafeIn
later as the query progresses. Under the current frontier definition, that
assumption is mostly false.

The scheduler maintains online kth-smallest frontiers:

```text
F_lower = kth_smallest(L_i)
F_upper = kth_smallest(U_i)
T_q = F_lower + lambda * (F_upper - F_lower)
```

For the recommended `lambda=0`, this becomes:

```text
T_q = F_lower
```

Because the frontier heaps only replace their kth element when a smaller bound
arrives, `F_lower` and `F_upper` normally tighten over time. Consequently,
`T_q` also tightens rather than loosens.

That has a direct implication:

```text
If U_i >= T_t at probe t,
and T_{t+1} <= T_t,
then U_i < T_{t+1} is even less likely.
```

So the main hoped-for benefit, late SafeIn promotion, is structurally weak.
The candidate's `safein_upper_bound` is already fixed when emitted; there is no
second estimate that improves the candidate itself later.

What remains is later SafeOut/drop. But that changes the problem from payload
prefetch timing into exact-vector suppression. That is much riskier because it
can affect recall, and it introduces a persistent pool scan, pool age policy,
dedup/upgrade state, and submit-order complexity.

Conclusion: sustained pool is not blocked by implementation difficulty alone;
it is low expected reward under the current monotone frontier semantics.

## Why Quality-Aware Early-Start Gate Did Not Meet Expectations

The quality-aware gate failed because the tested convergence signals do not
directly measure the real error source.

The original static-threshold diagnosis showed that false SafeIn mostly happens
when:

```text
T_q > R_q
```

where `R_q` is the true query top-k radius.

The tested gates measure different proxy signals:

```text
stable gate: frontier changed little across probes
gap gate: F_upper - F_lower is small
```

These signals measure whether the frontier is internally stable or tight. They
do not guarantee that the frontier is close to `R_q`. A frontier can be stable
but still too high; it can also have a narrow gap while both bounds remain
miscalibrated for that query or dataset.

This explains the observed pattern:

- `defer0_stable1/2`: starts too early, false rate explodes.
- `defer0_stable4/5`: becomes pure enough, but mostly by starting later; this
  is delayed gating, not genuinely smarter early-start.
- `gap-only`: has a cliff behavior. Too tight means almost no SafeIn; slightly
  looser means false rate jumps.
- `defer4 + gap`: improves COCO vector-level purity but suppresses too much
  useful prefetch, hurting e2e and failing to transfer to MSMARCO.

The MSMARCO result is the clearest robustness warning:

```text
defer4_gap_rel_0.013:
  gap ready = 0 / 51000 samples
  all reads = 0
  final payload = 2000
```

That is not a better SafeIn policy; it is effectively disabling SafeIn prefetch.

## Why COCO Vector Metrics Looked Better but E2E Did Not

The local metrics are not the system objective.

Vector-level metrics reward different behaviors:

- `coverage` rewards aggressive prefetch.
- `false rate` rewards conservative prefetch.

End-to-end latency depends on a timing balance:

1. Too many `VEC_ALL` reads occupy the high-priority vector read path and can
   interfere with rerank/probe overlap.
2. Too few `VEC_ALL` reads defer payload to final top-k payload fetch, which
   sits on the tail critical path.

Therefore:

- `stable4/5` can look good in vector stats, but they issue more all-read work
  or shift work in a way that hurts e2e.
- `gap_rel=0.013` reduces false prefetch on COCO, but it does so by suppressing
  prefetch; final payload fetch rises and latency worsens.

The key distinction is:

```text
The gate controls whether to read.
The system objective depends on when the read happens and whether it overlaps.
```

## Research Narrative Recommendation

These should be reported as meaningful negative results, not as failed tuning.

Recommended wording:

1. Sustained pool:

> Under a monotone query-frontier design, the SafeIn threshold tightens as more
> candidates are observed. As a result, a persistent Uncertain pool rarely
> produces late SafeIn promotions; its remaining benefit would come from later
> SafeOut pruning, which changes the problem into recall-risky exact-vector
> suppression.

2. Quality-aware gates:

> Frontier stability and frontier gap are useful diagnostics, but they are not
> direct estimates of whether the query-level threshold has fallen below the
> true top-k radius. In our experiments, these gates improved local purity
> proxies in some cases but did not robustly improve end-to-end latency or
> transfer to MSMARCO.

3. Default method:

> The fixed four-probe warmup is not claimed to be globally optimal. It is the
> most robust operating point among tested variants because it directly handles
> the unreliable early-frontier phase while preserving enough prefetch to hide
> payload I/O.

## Claims Boundary

Supported:

- `blend000_defer4` is the best default among the tested variants.
- The two proposed adaptive refinements did not improve the full system
  objective under the current frontier semantics.
- The negative results support the design choice of using a short fixed warmup
  plus conservative lower-frontier SafeIn.

Not supported:

- `defer4` is globally optimal.
- Quality-aware gating is generally useless.
- Sustained pools are impossible to make useful under a different frontier or
  exact-vector suppression objective.

## Final Conclusion

The two ideas did not exceed expectations because they optimize the wrong layer
of the system.

The sustained pool assumes thresholds may loosen later, but the current
frontier only tightens. The early-start gates assume frontier stability or gap
tracks top-k correctness, but the false-SafeIn failure mode is really threshold
miscalibration relative to `R_q`. Both ideas can improve a local proxy, but they
do not improve the full overlap pipeline reliably.

This negative result strengthens the current recommendation:

```text
frontier_blend, lambda=0, defer4, defer_until_ready=1
```

It is not the most adaptive-looking design, but it is the most robust tested
operating point for the current payload-prefetch objective.

