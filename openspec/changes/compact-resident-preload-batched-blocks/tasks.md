## 1. 数据结构与解析元数据

- [x] 1.1 在 `ParsedCluster` 中增加 code/address 布局元数据字段，用于标识 code region、address payload 和 trailer 的边界
- [x] 1.2 在 `ParsedCluster` 中增加 non-owning decoded-address view，并调整 `AddressAt()` / `DecodeAddressBatch()` 的读取优先级
- [x] 1.3 保留 lazy/async block path 的 owning `decoded_addresses` 与 raw-address 即时 decode 兼容性
- [x] 1.4 扩展 `ResidentClusterView`，增加 resident `code_storage` 和内存组件统计所需字段

## 2. Compact Resident Preload 实现

- [x] 2.1 重构 `ParseClusterBlockView()`，在不改变磁盘格式的前提下输出 code region bytes、address payload bytes 和 decoded address
- [x] 2.2 实现 compact resident materialize helper：从临时 parsed block 拷贝 code region，重定位 FastScan/ExRaBitQ 指针，移动 decoded address
- [x] 2.3 在 compact resident path 中清除 raw address resident 语义，确保 `raw_addresses=nullptr` 且 `addresses_are_raw_v2=false`
- [x] 2.4 修复 `ResidentClusterView::ToParsedCluster()`，使 resident parsed cluster 只持有 decoded-address view，不复制 decoded address vector
- [x] 2.5 保证 `BuildResidentParallelStage2View()` 使用 compact code view 后仍能构建 Stage2 parallel view

## 3. 16-Block Batch Read

- [x] 3.1 按 `block_offset` 对 lookup entries 建立 compact preload 读取顺序
- [x] 3.2 实现每批最多 16 个 cluster block 的连续 span `pread`，并按相对 offset 解析批内 cluster
- [x] 3.3 增加 sparse span 保护：当 span/used-bytes 过大时拆分 batch，避免过度读取
- [x] 3.4 确认 batch buffer 只在 preload 批次内存活，resident 指针均落在 per-cluster owner storage 中

## 4. 内存统计与 Benchmark 输出

- [x] 4.1 为 `ClusterStoreReader` 增加 `resident_file_buffer_bytes`、`resident_code_storage_bytes`、`resident_decoded_address_bytes`、`resident_raw_address_bytes`、`resident_parsed_address_duplicate_bytes` 等 getter
- [x] 4.2 调整 `resident_cluster_mem_bytes` 的含义，使其反映 compact preload 后实际 retained cluster component bytes
- [x] 4.3 在 `bench_online_query.cpp` 输出新增 resident memory component fields、batch size metadata 和 preload timing
- [x] 4.4 如旧 `bench_e2e.cpp` 仍输出 preload/RSS 结果，同步补齐相同字段，保证 build-index 与 online-query 口径可对齐

## 5. 测试

- [x] 5.1 更新 `tests/storage/cluster_store_test.cpp`，覆盖 compact preload 后不保留完整 file buffer、地址 view 不重复 owning、raw address 不被 resident query 重解码
- [x] 5.2 增加指针生命周期测试：preload 后 resident FastScan/ExRaBitQ/code 指针必须指向 `ResidentClusterView` owner storage
- [x] 5.3 运行 `cmake --build build --target bench_e2e bench_build_index test_cluster_store -j 8`
- [x] 5.4 运行 `./build/test_cluster_store`
- [x] 5.5 运行已有 Stage2/overlap 相关测试，确认 compact code view 未破坏查询路径

## 6. COCO100k 对比实验

- [x] 6.1 在 `/home/zcq/VDB/test/compact_resident_preload_batched_blocks/` 下创建结果目录和运行记录
- [x] 6.2 使用 COCO100k 既有可复用索引，固定 `topk=10`、`nprobe=64`、`query-count=1000` 跑旧 full-file preload 对照
- [x] 6.3 使用同一索引和参数跑 compact batched preload
- [x] 6.4 对比并记录 recall@10、avg/p95/p99 latency、preload/build resident view wall time、after-preload RSS、query peak RSS delta
- [x] 6.5 对比并记录 `resident_file_buffer_bytes`、`resident_code_storage_bytes`、`resident_decoded_address_bytes`、`resident_raw_address_bytes`、`resident_parallel_view_bytes`、`resident_parsed_address_duplicate_bytes` 和 `resident_cluster_mem_bytes`
- [x] 6.6 写出中文结果说明，判断 compact preload 的内存收益和 16-block batch read 的构建成本变化是否满足后续论文实验使用
