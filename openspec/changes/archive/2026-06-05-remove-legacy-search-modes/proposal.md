## Why

`remove-window-cluster-read` 已经把 cluster 读路径收敛到 resident full-preload，但主搜索代码仍保留多组论文后续不再采用的实验分支：HNSW coarse routing、冗余/RAIR/overlap assignment、以及非 2 的幂维度下的 padded/blocked Hadamard 旋转。继续保留这些开关会增加 window cluster 读路径优化后的主线复杂度，并干扰后续 window cluster resident read path 与 FastScan 优化的验证口径。

## What Changes

- **BREAKING** 删除 `--hnsw-coarse-routing` 及其参数，主搜索 coarse routing 只保留 exact centroid scoring 和当前仍采用的 two-level coarse routing。
- **BREAKING** 删除冗余 assignment 与 RAIR/overlap 相关构建模式，新构建索引只支持 `single` assignment；`assignment_factor` 固定为 `1`，不再生成或保存 secondary assignments。
- **BREAKING** 删除 `--assignment-mode`、`--assignment-factor`、`--rair-lambda`、`--rair-strict-second-choice`、`--save-secondary-assignments` 等 benchmark/build 控制入口。
- **BREAKING** 删除 `--pad-to-pow2` 和 `--blocked-hadamard-permuted`，非 2 的幂维度正式构建路径只保留 `fht_kac_rotator`；2 的幂维度继续使用普通 Hadamard fast path。
- 清理 `bench_e2e`、`bench_vector_search`、benchmark 脚本、测试配置和 JSON/CSV 输出中已废弃的 HNSW、冗余 assignment、RAIR、padded/blocked Hadamard 字段。
- 保留旧索引文件格式兼容边界：不强制修改 FlatBuffers schema，不要求重建已经可用的 single/FHT-Kac 索引；对旧的 redundant/blocked/padded 索引应明确报错或标记为 legacy unsupported，而不是静默走错误路径。
- 实现后重跑 COCO100k 和 MS MARCO 主搜索工作点，验证速度、recall、SafeIn/SafeOut/Uncertain、coarse routing 统计和旋转模式输出。

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `hnsw-coarse-centroid-routing`: 移除 HNSW centroid routing 作为正式 query backend。
- `rair-secondary-assignment`: 移除 RAIR secondary assignment 和 redundant top-2 serving 作为正式 build/query 能力。
- `recall-aware-secondary-assignment`: 移除 recall-aware secondary assignment 及 overlap policy 对照要求。
- `overlap-aware-centroid-refinement`: 移除 overlap membership refinement 作为当前正式构建能力。
- `coarse-cover-diagnostics`: 诊断输出不再要求跨 `single`、`redundant_top2_naive`、`redundant_top2_rair` 对照。
- `blocked-hadamard-rotation`: 移除 `blocked_hadamard_permuted` 作为正式非 2 的幂旋转模式。
- `blocked-hadamard-evaluation`: 删除 blocked/padded/random Hadamard 对照实验合同。
- `nonpow2-padded-hadamard-rotation`: 移除 padded Hadamard 实验模式和 `--pad-to-pow2` 构建路径。
- `fht-kac-rotation`: 将 FHT-Kac 从可选非 2 的幂模式提升为唯一正式非 2 的幂旋转路径。
- `fht-kac-fixed-evaluation`: 不再要求与 padded/blocked Hadamard 做固定三方对照，改为验证 FHT-Kac 主路径。
- `query-pipeline`: 查询主路径不再承担 HNSW routing、冗余 assignment、padded/blocked Hadamard 的正式分支语义。
- `e2e-benchmark`: benchmark CLI、配置记录和结果输出删除上述历史模式参数与字段。
- `benchmark-infra`: 脚本、测试矩阵和结果聚合删除历史模式维度，只保留当前主线工作点。

## Impact

- 查询与索引：`include/vdb/index/ivf_index.h`、`src/index/ivf_index.cpp`、`include/vdb/query/search_context.h`、`src/query/overlap_scheduler.cpp`。
- 构建：`include/vdb/index/ivf_builder.h`、`src/index/ivf_builder.cpp`、`include/vdb/index/ivf_metadata.h`、`schema/segment_meta.fbs` 的兼容策略。
- 旋转：`include/vdb/rabitq/rabitq_rotation.h`、`src/rabitq/rabitq_rotation.cpp` 中 blocked/padded 相关测试和持久化兼容判断。
- Benchmark：`benchmarks/bench_e2e.cpp`、`benchmarks/bench_vector_search.cpp`、`benchmarks/scripts/run_blocked_hadamard_msmarco.py`、`benchmarks/scripts/run_fht_kac_msmarco.py`、`test_configs.txt`。
- 测试：`tests/index/ivf_builder_test.cpp`、`tests/index/ivf_index_test.cpp`、`tests/query/overlap_scheduler_test.cpp`、`tests/rabitq/rabitq_rotation_test.cpp`、`tests/rabitq/rabitq_estimator_test.cpp`。
- 依赖：删除运行时 HNSW coarse routing 后，项目代码不应再直接依赖 `faiss::IndexHNSWFlat` 作为 centroid routing backend；第三方 FAISS 自身保留不受影响。
