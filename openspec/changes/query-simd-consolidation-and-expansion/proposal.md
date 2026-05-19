## Why

当前查询热路径已经完成一轮 `submit-batch` 和 Stage2 collect/scatter 精修，但仍有几处明显的串行 CPU 骨架留在 query 阶段，包括地址解码、rerank 精确距离、coarse top-n 选择以及部分 Stage2 后处理。同时，部分 SIMD 逻辑直接写在功能文件里，导致 `src/index` / `src/query` 中混入 ISA 相关实现，后续继续扩展 SIMD 时会进一步破坏目录边界。现在需要同时解决“还能继续做 SIMD 的地方”和“SIMD 代码应集中管理”的问题。

## What Changes

- 将现有散落在 `src/index`、`src/query` 等目录中的 SIMD 逻辑一次性收拢到 `src/simd` 与 `include/vdb/simd`，业务层只保留调用和 feature gate，不再直接承载 ISA 细节。
- 将新增的 query SIMD 能力拆成两阶段推进：
  - Phase 1: 统一接口、目录收口、保守实现与 fallback 接通
  - Phase 2: 在已收口的接口上追加更激进的 SIMD specialization
- Phase 1 覆盖地址解码、rerank batched exact distance、coarse top-n 小规模选择，以及现有 Stage2 classify/writeback 的进一步整理；Phase 2 再覆盖这些能力各自的更强 AVX2/AVX-512 或 small-topn 专门化实现。
- 为新的 SIMD 路径补齐统一的接口、fallback 约束和 benchmark 验证口径，确保 query-only 与 full E2E 下的结果语义一致且可回退，并且 Phase 2 只在 Phase 1 correctness 与基线验证完成后进入。
- 统一后续 SIMD 扩展的组织方式，要求新增 SIMD 代码默认进入 `simd` 目录，避免继续在业务代码中内嵌 AVX/ISA 分支。

## Capabilities

### New Capabilities
- `query-simd-runtime`: 规定查询路径中可独立启用/关闭的 SIMD 优化能力，包括地址解码、rerank batched distance、coarse select 小 top-n，以及可继续拆分的 Stage2 辅助路径，并要求新增能力按 Phase 1/Phase 2 两阶段演进。
- `simd-code-organization`: 规定 SIMD 代码的目录归属、接口边界、fallback 责任和业务层调用方式，要求 SIMD 实现集中在 `src/simd` 与 `include/vdb/simd`。

### Modified Capabilities
- `e2e-benchmark`: 扩展 benchmark 需求，使 query-only / full E2E 能覆盖 SIMD on/off、fallback 路径与关键阶段指标对比。

## Impact

- Affected code: `src/index/cluster_prober.cpp`, `src/query/rerank_consumer.cpp`, `src/index/ivf_index.cpp`, `include/vdb/query/parsed_cluster.h`, `src/query/overlap_scheduler.cpp`, `src/simd/*`, `include/vdb/simd/*`, `benchmarks/bench_e2e.cpp`, `benchmarks/scripts/run_hotpath_experiments.py`
- Affected systems: query hot path, benchmark/diagnostic pipeline, SIMD code organization conventions
- No intended index format change and no new resident-layout prerequisite

## Delivery Strategy

```text
Track 1: SIMD Consolidation
  - 一次性完成

Track 2: New SIMD Runtime Paths
  - Phase 1: interface + conservative implementation
  - Phase 2: specialization + stronger SIMD path
```

- `Stage2ClassifyBatch`、`Stage1SafeInMask` 和同类已有 SIMD helper 属于 Track 1，一次性迁移并清理目录。
- `address_decode`、`rerank_distance`、`coarse_select` 属于 Track 2，必须分两阶段推进：
  - Phase 1 先保证 `simd` 目录收口、fallback 可用、benchmark 可测
  - Phase 2 再根据 Phase 1 的 correctness 和性能结果决定是否继续加更激进的 SIMD specialization

## Implementation Priorities

```text
Priority 1
  - Stage2ClassifyBatch 搬迁
  - Stage1SafeInMask 搬迁

Priority 2
  - address_decode Phase 1
  - rerank_distance Phase 1

Priority 3
  - coarse_select Phase 1

Priority 4
  - coarse_select Phase 2
  - address_decode / rerank_distance Phase 2（暂缓）
```

- `Priority 1` 先完成已有 SIMD helper 的收口，因为这部分风险最低，而且能立即建立“业务层只调用 simd::*”的目录边界。
- `Priority 2` 优先做 `address_decode` 与 `rerank_distance` 的 Phase 1，因为这两项最有可能带来稳定收益，同时不需要先改更大范围的 query pipeline 结构。
- `Priority 3` 再做 `coarse_select` 的 Phase 1，因为它是固定成本优化，但收益更依赖具体 `nprobe` 工作点和 tie-breaking 语义。
- `Priority 4` 中只有 `coarse_select` 的 Phase 2 进入当前主线；`address_decode` 与 `rerank_distance` 的 Phase 2 仍然暂缓，保留 Phase 1 作为默认实现。`coarse_select` 的 Phase 2 已实现，并在低负载重测中复核，但 benchmark 仍未证明默认开启有稳定收益，因此继续保持默认关闭。
