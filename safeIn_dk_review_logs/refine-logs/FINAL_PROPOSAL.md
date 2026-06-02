# Research Proposal: Budgeted SafeIn Admission

The clean current proposal is available in:

`refine-logs/safein_prefetch_budget_20260602.md`

Summary:

- Keep `frontier_blend + defer4` as the accepted default.
- Add a per-query SafeIn `VEC_ALL` count/byte budget.
- Over-budget SafeIn candidates fall back to `VEC_ONLY`, preserving recall.
- During deferred flush, admit candidates by `score = (T_q - U_i) / addr.size`.
- Start with static caps, then evaluate an I/O-aware adaptive controller.
