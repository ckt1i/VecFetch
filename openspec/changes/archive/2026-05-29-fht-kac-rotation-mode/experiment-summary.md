# FhtKac Fixed-Parameter MSMARCO Comparison

Run date: `2026-05-21 17:05:52 CST`

Fixed evaluation contract:
- dataset: `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings`
- adapter: `/home/zcq/VDB/test/msmarco_fht_kac_adapter`
- `nlist = 16384`
- `nprobe = 256`
- `topk = 10`
- `queries = 1000`
- `bits = 4`
- `clu_read_mode = full_preload`
- `use_resident_clusters = 1`
- `early_stop = 0`

## Results

| mode | logical/effective | avg_query_time_ms | recall@10 | cluster.clu bytes | rotated_centroids bytes | rotation.bin bytes | total index bytes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `hadamard_padded` | `768 / 1024` | `9.6888` | `0.9615` | `11664814080` | `67108864` | `4195333` | `48028829564` |
| `blocked_hadamard_permuted` | `768 / 768` | `5.8158` | `0.9615` | `8793952256` | `50331648` | `2363161` | `45122581194` |
| `fht_kac_rotator` | `768 / 768` | `5.9409` | `0.9615` | `8793952256` | `50331648` | `2362393` | `45122580495` |

## Takeaways

- On this hardware and at this fixed operating point, `fht_kac_rotator` matches the recall of both baselines: `recall@10 = 0.9615`.
- `fht_kac_rotator` is much faster than `hadamard_padded` at the same recall: `5.9409 ms` vs `9.6888 ms`.
- `fht_kac_rotator` is slightly slower than `blocked_hadamard_permuted` at the same recall: `5.9409 ms` vs `5.8158 ms`.
- `fht_kac_rotator` and `blocked_hadamard_permuted` have effectively the same storage footprint at `768` dimensions because both keep `effective_dim = 768`.
- Compared with `hadamard_padded`, both non-padding modes reduce `.clu` size by about `2.79 GB` and total index size by about `2.91 GB`.

## Result Files

- padded: `/home/zcq/VDB/test/msmarco_fht_kac_fixed_eval_20260521/padded_reuse2/msmarco_fht_kac_adapter_20260521T165210/results.json`
- blocked: `/home/zcq/VDB/test/msmarco_fht_kac_fixed_eval_20260521/blocked_reuse/msmarco_fht_kac_adapter_20260521T164846/results.json`
- fht_kac: `/home/zcq/VDB/test/msmarco_fht_kac_fixed_eval_20260521/fht_kac_full/msmarco_fht_kac_adapter_20260521T165315/results.json`
