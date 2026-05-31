## Validation Summary

Baseline command family: MSMARCO adapter, `fht_kac_rotator` index, real GT, `nprobe=256`, resident full-preload, CRC on, SafeIn d_k p97, SafeIn epsilon override `0.087117`, SafeOut p95.

| Case | SafeOut epsilon | Valid errors | Recall@10 | Avg query ms | Stage2 ms | S2 SafeOut | S2 uncertain |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| default SafeOut epsilon | 0.0939 | n/a | 0.9485 | 5.8192 | 0.9878 | 1975.175 | 772.235 |
| global_pair K=1000 seed=42 | 0.0644 | 1000 | 0.9485 | 4.6675 | 0.3195 | 401.915 | 762.660 |
| global_pair K=5000 seed=42 | 0.0650 | 5000 | 0.9485 | 4.7656 | 0.3289 | 418.740 | 762.850 |
| global_pair K=10000 seed=42 | 0.0646 | 10000 | 0.9485 | 4.6580 | 0.3227 | 408.130 | 762.710 |
| global_pair K=1000 seed=43 | 0.0643 | 1000 | 0.9485 | 4.6424 | 0.3203 | 400.660 | 762.635 |
| global_pair K=1000 seed=44 | 0.0683 | 1000 | 0.9485 | 4.7527 | 0.3643 | 505.880 | 763.770 |

K=1000 seed sweep: SafeOut epsilon mean `0.0657`, population stdev `0.00186`; recall@10 was unchanged across all three K=1000 seed runs.

Recommendation:
- Use `global_pair K=1000` for quick SafeOut p95 sweeps.
- Use `global_pair K=5000` or `K=10000` when reporting a stronger baseline.
- Keep `legacy_per_cluster` available for final high-confidence calibration checks, but do not use it for routine parameter sweeps on MSMARCO-scale indexes.
