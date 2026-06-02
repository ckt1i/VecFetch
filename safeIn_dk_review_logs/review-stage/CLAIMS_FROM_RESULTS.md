# Claims From Results

1. `frontier_blend` with `lambda=0` and `defer_initial_clusters=4` satisfies the
   COCO100k top-k prefetch target: 47.41% true top-10 coverage and 11.53% false
   prefetch rate.

2. Removing the fixed warmup and relying only on frontier stability can satisfy
   COCO vector-level purity at `stable_probes=4/5`, but it is worse on COCO e2e
   and MSMARCO 200-query validation than `blend000_defer4`.

3. A frontier-gap early-start gate is feasible as an experimental control, but
   it is not robust enough to recommend as the default: `gap_rel=0.013` improves
   COCO vector false rate to 9.15% but slows COCO e2e and disables SafeIn
   prefetch on the MSMARCO skip-false run.

4. A sustained uncertain candidate pool is not justified under the current
   monotone frontier design because late SafeIn upgrades are unlikely and later
   SafeOut dropping would move the optimization into recall-risky exact-vector
   suppression.

