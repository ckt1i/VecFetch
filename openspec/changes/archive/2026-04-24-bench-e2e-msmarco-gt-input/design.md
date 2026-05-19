## Context

`bench_e2e` is currently optimized around a COCO-style layout and computes recall ground truth internally unless the benchmark is run in a query-only mode. That works for small or medium datasets, but it becomes a bottleneck when the goal is to study query hot-path scaling on a larger MSMARCO-scale corpus with 768-dimensional embeddings.

The user wants a temporary path that reuses the existing benchmark rather than introducing a new benchmark family. The adapter therefore needs to be narrow: convert MSMARCO assets into the existing benchmark contract, while allowing precomputed ground truth to be injected from disk instead of recomputed every run.

## Goals / Non-Goals

**Goals:**
- Allow `bench_e2e` to consume an explicit precomputed ground-truth file.
- Support a temporary MSMARCO-to-`bench_e2e` adapter directory that matches the current benchmark's expected file names.
- Keep the COCO flow and current query execution logic unchanged.
- Reduce repetitive brute-force GT computation during large-dataset benchmarking.

**Non-Goals:**
- Do not redesign the full benchmark input system.
- Do not convert `bench_e2e` into a generic retrieval framework for all datasets.
- Do not change the query pipeline or ranking logic.
- Do not require the adapter to preserve MSMARCO-native semantics beyond the benchmark's recall contract.

## Decisions

### 1. Add an explicit GT input path instead of overloading the existing dataset directory
`bench_e2e` should gain a dedicated CLI argument for precomputed ground truth. The benchmark should prefer that file when provided and skip brute-force GT generation.

Rationale:
- The current dataset directory already carries embeddings, ids, and metadata-like assets.
- Overloading the dataset directory with GT would make the contract ambiguous and brittle.

Alternatives considered:
- Keep brute-force GT only.
- Infer GT from the dataset directory by convention.
- Both were rejected because they do not solve the repeated GT cost cleanly.

### 2. Keep the MSMARCO adapter as a temporary directory transformation
The adapter should create a COCO-style benchmark directory containing embeddings, ids, metadata, and an optional GT file. It should not alter MSMARCO source assets.

Rationale:
- This minimizes risk to the canonical formal-baseline assets.
- The adapter can be thrown away or replaced once a native MSMARCO runner exists.

Alternatives considered:
- Modify the original MSMARCO formal-baseline directories in place.
- Add a full MSMARCO-native benchmark path to `bench_e2e`.
- Both are heavier than needed for the current goal.

### 3. Keep metadata optional in future, but not part of the first adaptation slice
The first version should still produce the benchmark's expected metadata file so the existing code paths stay untouched.

Rationale:
- This avoids a second benchmark refactor while the main risk is GT input handling.
- If metadata is removed later, that can be a follow-up change after the adapter proves useful.

Alternatives considered:
- Remove metadata dependency immediately.
- Rejected because it expands the benchmark contract change beyond GT injection.

### 4. Use the benchmark's existing recall contract and keep GT format simple
The external GT file should match the benchmark's existing top-k recall expectations and remain easy to generate offline.

Rationale:
- The purpose is to benchmark query latency and recall on a larger dataset, not to introduce a new evaluation metric family.
- A simple file-based GT contract is easier to produce from MSMARCO than a deeper integration.

## Risks / Trade-offs

- [Risk] The adapter may become a one-off script with duplicated logic. → Mitigation: keep it intentionally small and isolated from the formal-baseline pipeline.
- [Risk] GT file format mismatches could produce silent recall errors. → Mitigation: define the file contract explicitly in the spec and validate shapes/counts at load time.
- [Risk] Metadata may still be a naming mismatch (`image_*` vs passages). → Mitigation: treat the adapter as a temporary compatibility layer and document the mapping clearly.
- [Risk] The benchmark may still be tied to COCO-style payload semantics. → Mitigation: scope this change to input compatibility only; payload semantics are out of scope.

## Migration Plan

1. Add CLI support for loading external ground truth.
2. Add a small MSMARCO adapter script that emits a COCO-style temp directory.
3. Run the benchmark against the adapter directory with precomputed GT.
4. Validate that recall and latency are stable against the current COCO benchmark behavior.
5. If this path proves useful, decide later whether to keep the adapter, the GT input, or both.

## Open Questions

- Should the GT file be `.npy`, `.ivecs`, or both?
- Should the first version support only top-10 GT, or top-10/top-20/top-100 together?
- Should metadata remain required for the first adapter version, or can it be optional once GT input is present?
