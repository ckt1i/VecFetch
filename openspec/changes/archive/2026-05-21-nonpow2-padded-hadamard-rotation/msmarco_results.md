# MSMARCO Padded-Hadamard Comparison

## Operating Point

- Dataset: `msmarco_passage_adapter_full`
- `nlist=16384`
- `nprobe=256`
- `bits=4`
- `topk=10`
- `queries=1000`
- `crc=1`
- `clu-read-mode=full_preload`
- `use-resident-clusters=1`

## Compared Runs

### Baseline

- Result file:
  - `/home/zcq/VDB/test/nonpow2_existing_index_fulle2e/msmarco_passage_adapter_full_20260424T145820/results.json`
- Index:
  - `/home/zcq/VDB/test/data/msmarco_passage_adapter_full_20260423T215947/index`
- Metadata:
  - `logical_dim=768`
  - `effective_dim=768`
  - `padding_mode=none`
  - `rotation_mode=random_matrix`

### Padded-Hadamard

- Result file:
  - `/home/zcq/VDB/test/msmarco_hadamard_e2e_from_preclustered/msmarco_passage_adapter_full_20260424T173234/results.json`
- Index:
  - `/home/zcq/VDB/test/msmarco_hadamard_e2e_from_preclustered/msmarco_passage_adapter_full_20260424T173234/index`
- Metadata:
  - `logical_dim=768`
  - `effective_dim=1024`
  - `padding_mode=zero_pad_to_pow2`
  - `rotation_mode=hadamard_padded`

## Summary Table

| Metric | Baseline | Padded-Hadamard | Delta |
|---|---:|---:|---:|
| `recall@10` | `0.9615` | `0.9615` | `0.0000` |
| `avg_query_ms` | `14.1218` | `8.8845` | `-37.1%` |
| `avg_probe_prepare_ms` | `7.0315` | `0.4502` | `-93.6%` |
| `avg_probe_prepare_rotation_ms` | `0.0000` | `0.0023` | `+0.0023 ms` |
| `avg_probe_stage1_ms` | `1.6541` | `1.8415` | `+11.3%` |
| `avg_probe_stage2_ms` | `0.9915` | `1.0151` | `+2.4%` |
| `avg_probe_submit_ms` | `1.6186` | `2.2480` | `+38.9%` |
| `index_total_bytes` | `45072245474` | `84236556169` | `+86.9%` |

## Microbench Gate Result

The prepare-focused gate is satisfied.

- The candidate keeps `recall@10` unchanged.
- `avg_probe_prepare_ms` drops from `7.0315 ms` to `0.4502 ms`.
- The prepare/rotation hotspot identified on MSMARCO is therefore materially reduced before full E2E accounting.

Decision:

- `6.3`: padded-Hadamard is worth full E2E comparison.

## Full E2E Result

The full E2E comparison also favors padded-Hadamard at this operating point.

- `avg_query_ms` improves from `14.1218 ms` to `8.8845 ms`.
- The gain from removing the random-rotation prepare hotspot is larger than the added linear cost from `768 -> 1024`.
- The main tradeoff is footprint: total index bytes rise from `45.07 GB` to `84.24 GB`.

Decision:

- `6.4`: padded-Hadamard delivers a net serving benefit on MSMARCO for this operating point when latency is prioritized over footprint.

## Interpretation

- The previous MSMARCO perf result correctly identified `PrepareQueryInto` / random-rotation prepare as the dominant query-side hotspot.
- Replacing the `768d` random-matrix path with `1024d` padded Hadamard effectively removes that hotspot.
- Stage1, Stage2, and submit costs do increase, but not enough to offset the prepare reduction.
- This makes padded-Hadamard a credible next-step serving path for non-power-of-two dimensions, with the main caveat being the substantial index-size increase.
