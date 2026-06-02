# Research Review Trace: SafeIn Negative Results

Timestamp: 2026-06-02T11:38:18Z

Reviewer route: Codex sub-agent, `gpt-5.4`, xhigh reasoning.

Agent id: `019e881c-08bd-7763-b0fb-05cd3cacc10e`

Prompt summary:

- Asked reviewer to explain why sustained candidate pool and quality-aware
  early-start gate did not outperform `blend000_defer4`.
- Provided current method, COCO vector results, COCO e2e results, MSMARCO
  skip-false results, and gap/stable ablations.

Reviewer decision:

- Sustained pool is structurally low value under monotone frontier semantics.
- Stable/gap gates optimize proxy signals rather than direct `T_q <= R_q`
  correctness.
- COCO vector-level improvements do not transfer to e2e/MSMARCO because the
  system objective depends on read timing and overlap, not only prefetch purity.
- Negative results support retaining `blend000_defer4` as the current default.

Output document:

- `review-stage/RESEARCH_REVIEW_SAFEIN_NEGATIVE_RESULTS.md`

