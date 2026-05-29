## 1. Rotation Core

- [x] 1.1 Extend `RotationMatrix` with a distinct `fht_kac_rotator` kind and mode-specific metadata.
- [x] 1.2 Implement deterministic FhtKac forward and inverse rotation fast paths, including the fixed-round sign-flip, alternating FFHT, and Kac mixing sequence.
- [x] 1.3 Extend rotation persistence and reload tests so `fht_kac_rotator` can be reopened reproducibly.

## 2. Build And Query Integration

- [x] 2.1 Add builder configuration and selection logic to build IVF-RaBitQ indexes with `fht_kac_rotator`.
- [x] 2.2 Generate and load FhtKac rotated centroids, and keep the query-once rotated-centroid fast path available after reopen.
- [x] 2.3 Emit build metadata for `rotation_mode`, logical/effective dimension, and FhtKac-specific rotation settings.

## 3. Validation And Fixed Evaluation

- [x] 3.1 Add unit and integration coverage for FhtKac dimension handling, determinism, reopen behavior, and rotated-query semantics.
- [x] 3.2 Run one fixed-parameter MSMARCO comparison against `hadamard_padded` and `blocked_hadamard_permuted`.
- [x] 3.3 Record the resulting latency, recall, and storage comparison in a reusable experiment summary.
