# Stage2 Layout Optimization Final Audit

时间：2026-06-07

## 目标拆解

原始目标包含以下要求：

1. 使用 research-lit / official implementation 参考 Stage2 存储和计算方案。
2. 在实现前记录多个方案：
   - 逐向量、按维度和 bits 并行；
   - 适配所有 `ex_bits` 的通用方案；
   - 针对 `ex_bits=2` 和 `ex_bits=4` 的专门方案；
   - 降低向量打包数量的方案。
3. 具体实现并对比这些方案。
4. 相同 bits 下保持 recall、平均存储大小和内存用量不降，并争取查询速度加快。
5. 对 `ex_bits=4` 做专门无损压缩，目标是接近 `total_bits=4/ex_bits=3` 的大小。
6. 文档和实验结果放在本轮目录：
   `/home/zcq/VDB/test/rabitq_code_zip_20260606/`
   以及 `rabitq_code_zip/`。
7. 通过 auto-review-loop 形式迭代记录。

## 证据

### 参考与方案记录

- 方案文档：
  `rabitq_code_zip/stage2_vector_layout_review.md`
- official RaBitQ-Library 参考结论：
  - Stage1 使用 32-vector FastScan batch；
  - Stage2 是逐向量 compact ExData record；
  - SIMD 主要沿维度方向并行，而不是把 8 个候选向量作为 Stage2 SIMD lane。
- 已记录路线：
  - A1 `vector_bitplanes`
  - A2 `vector_bitplanes_prefetch`
  - A3 `vector_bitplanes_microbatch`
  - B1 `small_lane4_bitplanes`
  - B2 `small_lane2_bitplanes`
  - C1 `vector_2bit`
  - C2 `vector_nibble4`
  - D1 `vector4_highplane_elide`
  - D2 `vector4_block_entropy`

### 实现覆盖

核心代码改动：

- `include/vdb/common/types.h`
- `benchmarks/bench_e2e.cpp`
- `include/vdb/simd/ip_exrabitq.h`
- `src/simd/ip_exrabitq.cpp`
- `src/storage/cluster_store.cpp`
- `src/index/cluster_prober.cpp`
- `tests/common/types_test.cpp`
- `tests/simd/ip_exrabitq_test.cpp`
- `tests/storage/cluster_store_test.cpp`
- `tests/index/cluster_prober_test.cpp`

已实现 layout：

- `vector_bitplanes`
- `vector_bitplanes_prefetch`
- `vector_bitplanes_microbatch`
- `small_lane4_bitplanes`
- `small_lane2_bitplanes`
- `vector_2bit`
- `vector_nibble4`

默认构建策略已落地：

- official `ex_bits=1,2,3` 默认 `vector_bitplanes`
- official `ex_bits=4` 默认 `vector_nibble4`
- 显式 `generic_packed` 保持旧语义
- `selected_direct` 解析为当前推荐 direct layout

smoke 证据：

- `total_bits=4/ex_bits=3` 未显式 layout 生成：
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/default_layout_smoke_outputs/default_layout_smoke_dataset_20260607T033821/index_official_1_plus_n_total4_ex3_vector_bitplanes`
- `total_bits=5/ex_bits=4` 未显式 layout 生成：
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/default_layout_smoke_outputs/default_layout_smoke_dataset_20260607T033822/index_official_1_plus_n_total5_ex4_vector_nibble4`

### 性能结论

COCO100k `total_bits=4/ex_bits=3`，10 次关键候选合并：

| layout | R@10 | avg ms | QPS | Stage2 ms | resident code bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| `vector_bitplanes_prefetch` | 0.9504 | 0.337183 | 2966.0 | 0.068106 | 29,282,040 |
| `vector_bitplanes` | 0.9504 | 0.337431 | 2964.0 | 0.068673 | 29,282,040 |
| `split3_trimmed` | 0.9504 | 0.342518 | 2919.9 | 0.071546 | 29,282,040 |
| `small_lane4` | 0.9504 | 0.343722 | 2909.6 | 0.072457 | 29,282,040 |

最终选择：

- `ex_bits=1,2,3`: 默认 `vector_bitplanes`
- `ex_bits=4`: 默认 `vector_nibble4`
- `vector_bitplanes_prefetch`: 保留为可选变体
- `small_lane4/small_lane2/microbatch`: 负结果，不进入默认 hot path
- `vector_2bit`: 负结果，不进入默认 hot path

### ex_bits=4 压缩结论

稀疏压缩：

| dataset | ex4 payload bytes | high-plane elide bytes | saving |
| --- | ---: | ---: | ---: |
| COCO100k | 25,600,000 | 25,705,528 | -0.412% |
| voxceleb2 | 14,400,000 | 14,458,947 | -0.409% |

块级熵压缩：

| dataset | entropy bit/dim | best compressed bytes | ratio to ex3 target |
| --- | ---: | ---: | ---: |
| COCO100k | 3.984887 | 25,604,614 | 1.3336x |
| voxceleb2 | 3.989137 | 14,403,968 | 1.3337x |

结论：

- `ex_bits=4` 码值熵接近满 4 bit/dim。
- 接近 `ex_bits=3` 空间需要接近 3 bit/dim，无损压缩理论下界不支持。
- sparse/elide 和 zlib/lzma/lz4/zstd 块压缩均为负结果。
- 因此 `ex_bits=4` 空间压缩目标不能作为正 claim；应作为边界分析或负结果报告。

### 验证命令

已通过：

```text
git diff --check
python3 -m py_compile rabitq_code_zip/analyze_vector_bitplanes.py
jq empty REVIEW_STATE.json
jq empty /home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_20260607T0328_summary.json
jq empty /home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_interleaved_20260607T0330_summary.json
jq empty /home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_combined_20260607T0330_summary.json
cmake --build build --target test_types bench_build_index -j$(nproc)
./build/test_types
cmake --build build --target test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)
./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober
```

## Completion Judgment

完成。

该目标中的正向优化部分已经实现并设为默认构建策略；负向部分也已经用分布、熵下界和实际 codec 实验说明不可成立。当前不再存在必须继续实现的 Stage2 layout 或 ex4 lossless compression 路线。

`mcp__codex` 外部 reviewer 工具在当前环境不可用，因此 auto-review-loop 以本地 `AUTO_REVIEW.md` 和 `REVIEW_STATE.json` 方式完成并保留审计轨迹。
