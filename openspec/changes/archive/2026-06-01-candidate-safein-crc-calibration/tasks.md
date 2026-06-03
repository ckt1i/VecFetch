## 1. Configuration and Metadata

- [x] 1.1 Add an explicit SafeIn threshold source for candidate-level CRC, distinct from legacy exact-L2 and RabitQ kth calibration.
- [x] 1.2 Extend persisted metadata or sidecar artifact fields to record candidate CRC threshold value, `beta`, split sizes, candidate domain, nprobe, top-k, bits, and validation falseSafeIn rate.
- [x] 1.3 Preserve fallback behavior for indexes that do not contain candidate CRC threshold metadata.
- [x] 1.4 Update benchmark/config logging so every run reports the active SafeIn threshold source and threshold value.

## 2. Candidate Replay Calibration

- [x] 2.1 Define calibration records containing query id, candidate id, stage, `U_i = d_hat_i + e_i`, and exact top-k membership.
- [x] 2.2 Generate exact full-vector top-k labels for calibration/validation queries without using exact `r_k(q)` as the candidate CRC threshold source.
- [x] 2.3 Replay the serving candidate domain using the configured nprobe and the same `d_hat_i` / `e_i` calculation as the online SafeIn path.
- [x] 2.4 Implement threshold search over sorted candidate `U_i` breakpoints, selecting the threshold with maximum SafeIn count subject to `falseSafeIn / SafeIn <= beta`.
- [x] 2.5 Handle the no-legal-threshold case by emitting a conservative threshold that produces no SafeIn and an explicit diagnostic flag.
- [x] 2.6 Split query samples into calibration and held-out validation sets using a recorded seed.

## 3. Runtime Integration

- [x] 3.1 Load candidate CRC SafeIn threshold into the existing ConANN/SafeIn threshold path when the threshold source is `candidate_crc`.
- [x] 3.2 Keep online SafeIn comparison as `d_hat_i + e_i < T` for both Stage1 and Stage2.
- [x] 3.3 Ensure Stage1 FastScan masks continue to use the algebraically equivalent threshold form `d_hat_i < T - e_i`.
- [x] 3.4 Ensure this change does not alter SafeOut frontier logic or RaBitQ/FastScan distance estimation kernels.

## 4. Benchmark and Validation Output

- [x] 4.1 Add CLI/config support for running candidate CRC calibration with a target `beta`.
- [x] 4.2 Emit calibration and validation JSON fields for SafeIn count, falseSafeIn count, falseSafeIn rate, Uncertain count, and Stage1/Stage2 breakdown.
- [x] 4.3 Support loading a precomputed candidate CRC threshold artifact for repeatable benchmark runs.
- [x] 4.4 Run COCO100k validation with `nlist=2048`, `nprobe=64`, and report candidate-level `falseSafeIn / SafeIn` under the new threshold source.
- [x] 4.5 Compare the new candidate CRC threshold against the previous exact/RabitQ kth SafeIn threshold on SafeIn count, falseSafeIn rate, recall, and latency.

## 5. Tests and OpenSpec Validation

- [x] 5.1 Add unit tests for candidate threshold search, including tie handling and no-legal-threshold behavior.
- [x] 5.2 Add metadata load/save tests covering candidate CRC source and legacy fallback.
- [x] 5.3 Add runtime classification tests proving candidate CRC mode uses `U_i < T` and does not use exact distance during online comparison.
- [x] 5.4 Build affected benchmarks and tests.
- [x] 5.5 Run `openspec validate candidate-safein-crc-calibration --strict` and fix any spec/task validation issues.
