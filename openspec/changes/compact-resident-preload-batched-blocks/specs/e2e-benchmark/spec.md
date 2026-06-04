## ADDED Requirements

### Requirement: E2E benchmark SHALL report compact resident memory component breakdown
E2E/online benchmark 输出 SHALL 报告 compact resident preload 所需的内存组件拆分，使实验能够区分 `.clu` 文件大小、实际 resident index、地址表、parallel view 和 query-time scratch/RSS。

#### Scenario: Resident component fields are exported
- **WHEN** benchmark 在 full-file preload 或 compact resident preload 模式下完成
- **THEN** JSON 输出 SHALL 包含 `resident_file_buffer_bytes`
- **AND** SHALL 包含 `resident_code_storage_bytes`
- **AND** SHALL 包含 `resident_decoded_address_bytes`
- **AND** SHALL 包含 `resident_raw_address_bytes`
- **AND** SHALL 包含 `resident_parallel_view_bytes`
- **AND** SHALL 包含 `resident_parsed_address_duplicate_bytes`
- **AND** SHALL 包含 `resident_cluster_mem_bytes`

#### Scenario: Compact mode exposes address and file-buffer savings
- **WHEN** benchmark 使用 compact resident preload
- **THEN** `resident_file_buffer_bytes` SHALL 为 0
- **AND** `resident_raw_address_bytes` SHALL 为 0
- **AND** `resident_parsed_address_duplicate_bytes` SHALL 为 0

### Requirement: E2E benchmark SHALL report compact preload build cost
E2E/online benchmark 输出 SHALL 报告 compact resident preload 的构建成本，包括 block read/parse/address decode/code materialize/parallel view build 合计时间。该成本 SHALL 与 query latency 分开报告，避免混入在线 query steady-state 延迟。

#### Scenario: Preload timing is separated from query timing
- **WHEN** benchmark 使用 compact resident preload
- **THEN** 输出 SHALL 包含 `resident_preload_time_ms`
- **AND** 输出 SHALL 包含 `resident_parallel_view_build_ms`
- **AND** query latency 字段 SHALL 仍只统计 measured query batch 的在线执行时间

#### Scenario: Batch read metadata is exported
- **WHEN** compact resident preload 使用 16-block batch read
- **THEN** 输出 SHALL 包含 batch size 或等价 metadata
- **AND** 输出 SHALL 能区分 compact batched preload 与旧 full-file preload

### Requirement: COCO100k compact preload evaluation SHALL compare time and memory under one protocol
本 change 的验证 SHALL 在 COCO100k 数据集下用同一索引和同一查询参数比较旧 full-file preload 与 compact batched preload。比较 SHALL 同时覆盖构建/preload 成本、RSS、resident component bytes、recall 和查询延迟。

#### Scenario: COCO100k comparison uses fixed operating point
- **WHEN** compact resident preload 实现完成
- **THEN** 验证 SHALL 在 COCO100k 上至少运行 `topk=10`、`nprobe=64`、`query-count=1000`
- **AND** full-file preload 与 compact batched preload SHALL 使用同一索引、同一 query 文件和同一 ground-truth 文件

#### Scenario: COCO100k comparison records required metrics
- **WHEN** COCO100k 对比运行完成
- **THEN** 结果 SHALL 记录 recall@10
- **AND** SHALL 记录 avg/p95/p99 latency
- **AND** SHALL 记录 preload/build resident view wall time
- **AND** SHALL 记录 after-preload RSS 和 query peak RSS delta
- **AND** SHALL 记录 resident component bytes breakdown

#### Scenario: Results are stored under the test directory
- **WHEN** COCO100k 对比测试产出结果
- **THEN** 结果 SHALL 写入 `/home/zcq/VDB/test/compact_resident_preload_batched_blocks/` 或该目录下的子目录
- **AND** 该目录 SHALL 包含可复现命令或简要结果说明
