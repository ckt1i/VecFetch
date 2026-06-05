## Context

当前主搜索路径已经删除 window cluster read，正式运行环境收敛到 resident full-preload。代码中仍保留三类历史实验模式：

- coarse routing 层：exact、two-level、HNSW 三套入口，其中 HNSW 是 query-time over centroids 的临时图索引。
- build/serving 层：single、redundant top-2 naive、RAIR、recall-aware/overlap-aware 等多归属分支。
- rotation 层：普通 Hadamard、padded Hadamard、blocked Hadamard permuted、FHT-Kac、random matrix 等多分支。

论文后续只保留 resident 主路径、single assignment、以及非 2 的幂维度下的 FHT-Kac rotator。继续保留旧分支会让 hot path 优化需要持续维护已经放弃的条件判断、配置输出和测试矩阵。

## Goals / Non-Goals

**Goals:**

- 删除 HNSW coarse routing 的正式 CLI、配置、执行路径和 benchmark 输出字段。
- 将新构建索引收敛到 single assignment，删除冗余 top-2、RAIR、recall-aware secondary assignment、overlap-aware refinement 的正式入口。
- 删除 padded Hadamard 和 blocked Hadamard permuted 的正式构建/benchmark 入口；非 2 的幂维度使用 FHT-Kac，2 的幂维度使用普通 Hadamard。
- 清理 benchmark scripts、test_config、JSON/CSV 输出中的历史模式维度。
- 保留已有 single/FHT-Kac 索引复用能力，不要求因本 change 重建现有主线索引。

**Non-Goals:**

- 不删除 two-level coarse routing。
- 不改变 SafeIn/SafeOut/Uncertain 判定公式。
- 不修改 FastScan 单 bit 优化目标。
- 不重写 FlatBuffers schema 或强制执行索引文件格式迁移。
- 不删除第三方 FAISS 中的 HNSW 实现。
- 不保证旧 redundant、blocked Hadamard、padded Hadamard 索引仍可作为正式 serving 目标。

## Decisions

### Decision 1: 对运行时模式采用 hard-delete，而不是保留 no-op 开关

`bench_e2e`、`bench_vector_search` 和主搜索配置不再接受 HNSW、RAIR/redundant、padded/blocked Hadamard 作为正式控制参数。若为了短期脚本兼容保留参数解析，也必须输出明确错误或 warning，并且不能改变实际运行路径。

理由：

- no-op 参数会继续污染结果解释，用户无法判断某个历史开关是否真的生效。
- hard-delete 能让编译错误暴露仍依赖旧分支的代码。
- 当前优化目标是降低主路径维护成本，而不是继续保留实验矩阵。

备选方案是保留参数但强制默认值。该方案迁移短期成本低，但会继续在 JSON/CSV、脚本和测试中传播无效维度，本轮不采用。

### Decision 2: 新构建只支持 single assignment，旧 redundant 索引显式 unsupported

`IvfBuilderConfig` 的正式构建语义固定为 single assignment：`assignment_factor = 1`，`assignment_mode = single`。实现上应删除或内部固定 secondary assignment 派生、secondary assignment 保存、RAIR 参数和重复 posting 构建逻辑。

对于旧索引：

- 如果 metadata 显示 `assignment_mode=SINGLE` 且 `assignment_factor=1`，继续可打开和查询。
- 如果 metadata 显示 redundant/RAIR 或 `assignment_factor != 1`，正式路径应拒绝并给出清晰错误，或仅在明确 legacy diagnostic 工具中读取。

理由：

- resident single-assignment 是后续 hot path 优化的前提。
- 静默接受 redundant 索引会让轻量提交路径和候选去重假设失效。
- 不改 schema 能避免破坏旧文件读取器和 FlatBuffers 生成代码。

### Decision 3: 非 2 的幂维度固定使用 FHT-Kac

构建路径按维度选择：

```text
if IsPowerOf2(dim):
    rotation = hadamard
else:
    rotation = fht_kac_rotator
```

删除 `pad_non_power_of_two_to_pow2`、`use_blocked_hadamard_permuted` 和对应 CLI。FHT-Kac 可以保留内部配置字段，也可以直接成为非 2 的幂维度默认行为；外部不再需要用户通过 `--fht-kac-rotator 1` 才能进入正确路径。

旧 blocked/padded 索引策略：

- 如果 reopen 仅用于离线检查，可读取 metadata 并报告 legacy rotation mode。
- 正式 query path 不再要求支持 `blocked_hadamard_permuted` 或 `hadamard_padded`。
- 若旧索引被用于正式 benchmark，应失败并提示重建 FHT-Kac 索引。

### Decision 4: HNSW coarse routing 只删运行时实现，不迁移 schema

当前 `--hnsw-coarse-routing` 对应的是 query-time 基于已加载 centroids 构建 `faiss::IndexHNSWFlat`，并不依赖持久化的 `segment_meta.fbs::HnswParams`。本轮删除 `SetHnswCoarseRouting()`、`FindNearestClustersHnsw()`、HNSW stats 和 benchmark 字段即可。

`segment_meta.fbs` 中历史 `HnswParams` 字段先保留为 legacy schema 字段，不作为本轮格式迁移目标。

理由：

- 避免为了删除未持久化运行时分支而引入 schema 兼容风险。
- 如果后续要做 schema 清理，应单独开 change 管理 FlatBuffers 版本和测试。

### Decision 5: Benchmark 输出只保留主线可调参数

`bench_e2e` 和相关脚本输出不再包含：

- HNSW enable/M/efConstruction/efSearch 和 HNSW stats。
- assignment mode/factor、RAIR lambda、RAIR strict、secondary assignment path。
- pad-to-pow2、blocked Hadamard、blocked/padded 对照矩阵字段。

继续保留：

- dataset、index-dir、nlist、nprobe、topk、bits、metric。
- rotation_mode，但正式结果只应出现 `hadamard` 或 `fht_kac_rotator`。
- exact/two-level coarse routing 字段。
- latency、recall、SafeIn/SafeOut/Uncertain、preload、resident memory、query breakdown。

### Decision 6: 验证以同参主线 benchmark 为准

实现后至少执行：

- COCO100k 主线 `bench_e2e` 或当前 test_config 对应工作点。
- MS MARCO FHT-Kac 主线 `bench_e2e` 或当前 test_config 对应工作点。
- `bench_vector_search` 的 COCO100k vector-only 验证，确认删除 `--pad-to-pow2` 后当前 COCO/FHT-Kac 无回归。
- 单元测试覆盖 build/open/search/rotation 的新边界。

结果必须记录是否使用 legacy 参数、索引 rotation mode、assignment mode、coarse routing mode，以及 SafeIn/SafeOut/Uncertain 指标。

## Risks / Trade-offs

- [Risk] 旧实验脚本仍传入删除的参数导致运行失败。  
  Mitigation: 在 tasks 中先清理 `test_configs.txt` 和 benchmark scripts；必要时提供清晰错误信息。

- [Risk] 旧 redundant/blocked/padded 索引被误用于主线 benchmark。  
  Mitigation: open/search 阶段检查 metadata，报出具体 unsupported mode 和重建建议。

- [Risk] 删除 blocked/padded 后非 2 的幂维度意外回退到 random matrix。  
  Mitigation: builder 选择逻辑加单测，要求非 2 的幂维度默认产生 `rotation_mode=fht_kac_rotator`。

- [Risk] HNSW 统计字段删除影响结果聚合脚本。  
  Mitigation: 更新 JSON/CSV 解析脚本和结果 schema，聚合逻辑不得再要求 HNSW 字段存在。

- [Risk] 不改 schema 会留下历史字段。  
  Mitigation: 文档明确这是 legacy storage compatibility，不是正式 runtime capability；后续如要清理 schema 单独提案。

## Migration Plan

1. 先删除 benchmark CLI 和配置输出中的历史模式字段，让编译暴露实际依赖。
2. 删除 HNSW coarse routing runtime 实现和 stats。
3. 固定 builder 为 single assignment，删除 secondary assignment 派生和 RAIR 参数。
4. 固定非 2 的幂维度为 FHT-Kac，删除 pad/blocked 入口和正式测试。
5. 更新 OpenSpec specs、benchmark scripts、test_config 和结果聚合。
6. 运行单元测试和 COCO100k/MS MARCO 主线 benchmark。

## Open Questions

- None. 本 change 采用 hard-delete runtime modes + 保留 legacy schema 字段的策略。
