## Why

MSMARCO `768` 维查询的 perf 显示，当前 query-side 最大热点已经从 Stage2 kernel 转移到 `PrepareQueryInto` 和 `RotationMatrix::Apply`。按现有实现，非 2 的幂维度无法使用 Hadamard 快路径，只能退回随机旋转的标量 `O(D^2)` matvec，这让 prepare/rotation 在大规模数据集上成为主要瓶颈。

## What Changes

- Introduce an experimental padded-Hadamard rotation path for non-power-of-two dimensions by zero-padding `logical_dim` to `effective_dim = next_power_of_two(logical_dim)`.
- Extend the IVF-RaBitQ build path to record both logical and effective dimensions, build padded rotated centroids, and preserve reopening compatibility.
- Extend the query pipeline to consume padded-Hadamard index metadata, pad query vectors once, rotate them once, and reuse the pre-rotated query path during probing.
- Add a dedicated experiment contract that compares:
  - baseline `logical_dim` random-rotation query preparation
  - padded `effective_dim` Hadamard query preparation
  under the same MSMARCO operating point.
- Add benchmark reporting for padding/rotation mode and prepare-substep attribution so the tradeoff between faster rotation and larger code width remains measurable.

## Capabilities

### New Capabilities
- `nonpow2-padded-hadamard-rotation`: experimental support for zero-padding non-power-of-two vectors to the next power of two so they can use Hadamard-based rotation while preserving the original logical dimension contract.
- `prepare-rotation-experiment`: a benchmark and experiment workflow that compares baseline random rotation against padded Hadamard under both microbench and full E2E serving settings.

### Modified Capabilities
- `ivf-rabitq-baseline-build`: build metadata and build outputs must record logical/effective dimension and allow padded rotated-centroid artifacts.
- `query-pipeline`: query preparation must recognize padded-Hadamard metadata and route eligible searches through query-once padded rotation plus pre-rotated probe preparation.
- `e2e-benchmark`: benchmark output must expose logical/effective dimension, padding mode, rotation mode, and prepare/rotation attribution fields for fair comparison.

## Impact

- Affected code: IVF builder/index metadata, query prepare/rotation path, benchmark CLI/output, MSMARCO experiment scripts.
- Affected artifacts: index metadata, rotated centroid artifacts, benchmark result schema, experiment manifests.
- Main risk: `effective_dim` grows from `768` to `1024`, increasing code size, memory footprint, and some linear scan costs even if rotation gets faster.
