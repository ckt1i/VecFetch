# Auto Review Loop: Dynamic SafeIn Quality Gate

Started: 2026-06-02T11:12:45Z

## Context

The reviewed method is the current Dynamic SafeIn prefetch policy:

```text
frontier_blend
lambda = 0.0
defer_initial_clusters = 4
defer_until_ready = 1
stable_probes = 1
```

The user asked whether two additional ideas are already covered and whether
they are worth implementing:

1. A sustained uncertain candidate pool that can later upgrade to SafeIn or
   drop as SafeOut.
2. A quality-aware early-start gate based on query convergence rather than a
   fixed probe count.

## Round 1

### Assessment Summary

- Score: current coverage of the two ideas was assessed as partial.
- Suggestion 1 coverage: 5/10.
- Suggestion 2 coverage: 4/10.
- Continue improvement necessity: do not implement full sustained pool; test a
  minimal quality-aware gate ablation.

### Reviewer Raw Response

<details>
<summary>Click to expand full reviewer response</summary>

Findings:

1. 当前实现不是“持续 candidate pool”，只是“一次性 warmup defer + flush”。 `DeferredSafeInPlan` 只在 defer 窗口里缓存，随后统一转成 `VEC_ALL` 或 `VEC_ONLY`，flush 后就清空；已经进入 `VEC_ONLY` 的候选不会再升级，也不会later drop。

2. 在当前 frontier 定义下，“ready 之后再把 Uncertain 升成 SafeIn”这件事本身价值很小。`F_lower` `F_upper` 都是 online `kth_smallest`，代码只会在新值更小时替换堆顶，因此二者单调不增；`T_q = F_lower +
lambda(F_upper-F_lower)` 也随之单调不增。也就是说，threshold 只会越来越严，不会 later 变松，所以 post-ready 持续 pool 很难产生新的 late SafeIn upgrade。

3. 建议 2 只被“半实现”。代码里确实有 query-level ready/stability gate，但当前 winning 配置仍然硬性要求前 4 个 probe defer，而且 `stable_probes=1` 基本等于“frontier 首次可用就 ready”。这不是质量感知early-start，只是“固定 warmup + 很弱的动态 gate”。

4. 现有结果已经足以说明“frontier_blend + warmup defer”有效，但还不足以证明“没必要再做任何 gate 改进”。COCO100k 上 `blend0_defer4` 已满足 coverage / false-rate 目标，MSMARCO 200-query 也保住了 recall 和读行为；但这更多证明当前 heuristic 可用，不证明 fixed defer4 是最优或最稳健。

Recommended next step: do not implement the full sustained pool. Run a minimal
quality-aware early-start gate ablation with a frontier-gap condition.

</details>

### Actions Taken

Implemented an experimental frontier-gap gate:

```text
gap = F_upper - F_lower
gap_ready =
    gap <= dynamic_safein_gap_abs_tol
    or gap / max(abs(F_lower), eps) <= dynamic_safein_gap_rel_tol
```

Added:

- `SearchConfig.dynamic_safein_gap_rel_tol`
- `SearchConfig.dynamic_safein_gap_abs_tol`
- gap stats in `SearchStats`
- CLI in `bench_vector_search` and `bench_e2e`
- unit tests for gap blocking and allowing Dynamic SafeIn readiness

Verification:

```text
cmake --build build --target test_overlap_scheduler bench_vector_search bench_e2e -j4
ctest --test-dir build -R test_overlap_scheduler --output-on-failure
```

Both passed.

### Results

See [quality_gate_check/summary.md](../safeIn_dk_review_logs/quality_gate_check/summary.md).

Key results:

| scheme | COCO vector coverage | COCO vector false | COCO e2e avg | MSMARCO avg | disposition |
|---|---:|---:|---:|---:|---|
| `blend000_defer4` | 47.41% | 11.53% | 0.780 ms | 62.715 ms | default |
| `defer0_stable4` | 55.96% | 18.22% | 0.961 ms | 64.392 ms | not default |
| `defer0_stable5` | 53.87% | 12.16% | 1.127 ms | 64.851 ms | not default |
| `defer4_gap_rel_0.013` | 35.85% | 9.15% | 1.009 ms | 64.884 ms | experimental only |

## Round 2

### Assessment Summary

- Score: 8/10.
- Verdict: ready.
- Recommended disposition:
  - Default: `frontier_blend`, `lambda=0`, `defer_initial_clusters=4`,
    `defer_until_ready=1`.
  - Experimental only: gap gate.
  - Do not pursue sustained uncertain pool now.

### Reviewer Raw Response

<details>
<summary>Click to expand full reviewer response</summary>

Findings:

1. I do not see a blocking implementation bug in the new gap gate. The code
does what you described: it adds `gap_ready` to `FrontierBlend/Stable/Scale`
readiness, wires the CLI through both benches, records stats, and has focused
unit coverage.

2. The failure mode is heuristic robustness, not correctness. `gap_ready` is
driven by `gap / |F_lower|` when `rel_tol` is used, so its trigger is
scale-sensitive across datasets. That matches your result where MSMARCO never
became ready (`0/51000` ready samples) and effectively disabled dynamic SafeIn.

3. The case for a sustained uncertain pool is still weak. Under the current
frontier policy, both `F_lower` and `F_upper` only tighten as better candidates
arrive, so the blended threshold does not get looser later. That means a
persistent pool has little chance to create new late SafeIn upgrades; it mostly
adds rescanning overhead and more aggressive exact-read suppression risk.

With your ablations, `blend000_defer4` is the best default among tested
variants: it keeps COCO purity within target, beats `stable4/5` on COCO e2e and
MSMARCO latency, and avoids the MSMARCO collapse seen with `defer4 + gap_rel`.

Answers:

1. Yes. Keep the gap gate as experimental only, not as a default recommendation.
Score for “default-worthy”: `2/10`.

2. Yes. The correct conclusion is that a full sustained uncertain pool is not
worth implementing now. Implementation priority: `1/10`.

3. Yes. The current evidence is enough to stop here and retain
`blend000_defer4` as the default recommendation. Confidence in that decision:
`8/10`.

Final score and verdict:

Overall decision quality: `8/10`
Verdict: ready

</details>

## Method Description

The final recommended SafeIn prefetch method uses online query-level frontier
statistics to avoid relying on a global static SafeIn threshold. During probing,
the scheduler maintains `F_lower = kth_smallest(L_i)` and
`F_upper = kth_smallest(U_i)` over emitted candidates. The recommended
threshold is the conservative blend with `lambda=0`, so `T_q = F_lower`.
For the first four probed clusters, candidates are held in a deferred buffer
instead of immediately committing to `VEC_ALL` or `VEC_ONLY`; once the frontier
is ready, the deferred candidates are replayed and those with
`safein_upper_bound < T_q` are prefetched as `VEC_ALL`.

The sustained uncertain-pool idea is not adopted. Because the maintained
frontiers tighten over time, the SafeIn threshold normally becomes stricter
rather than looser, so late SafeIn upgrades are unlikely. A gap-based
quality-aware gate was implemented as an experimental knob, but it is not a
default because it improves COCO vector-level purity while degrading COCO e2e
and MSMARCO behavior.

## Final Status

Stop iteration. Retain `blend000_defer4` as the default recommendation and keep
the gap gate only for future experiments.

