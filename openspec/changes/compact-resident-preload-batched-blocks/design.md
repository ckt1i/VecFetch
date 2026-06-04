## Context

当前 `ClusterStoreReader::PreloadAllClusters()` 会把整个 `cluster.clu` 读入 `resident_file_buffer_`，再基于该 buffer 为每个 cluster 构造 `ResidentClusterView` 和 `resident_parsed_clusters_`。这一路径在功能上可行，但在线内存口径偏厚：

- `.clu` 文件本体在 preload 后仍整文件常驻，包括 header、lookup table、address payload、trailer 等 query hot path 不再需要的字节。
- V9+ raw address table 在 preload 时已经解码成 `AddressEntry`，但 resident view 仍保留 raw address 指针，`ParsedCluster::DecodeAddressBatch()` 也会优先从 raw table 再解码。
- `ResidentClusterView::decoded_addresses` 和 `ResidentClusterView::ToParsedCluster()` 生成的 `ParsedCluster::decoded_addresses` 会形成等价地址表的重复持有。

本次设计针对 resident/full-preload 的在线查询模式，目标是在 preload 后只保留在线 probe 和 rerank submit 必需的 resident index 状态，并用每批 16 个 cluster block 的批量读取降低 compact preload 的构建成本。

## Goals / Non-Goals

**Goals:**

- compact resident preload 后不再保留完整 `resident_file_buffer_`。
- preload 阶段完成 address 解码，并在 resident 查询中直接使用 decoded address。
- resident parsed cluster 不再复制 decoded address。
- preload 读取从逐 cluster `pread` 改为每批最多 16 个 block 的 batch `pread`。
- benchmark 输出足够区分构建/preload 成本、resident index component bytes 和 query-time RSS。
- 在 COCO100k 下比较旧 full-file preload 与新 compact batched preload 的构建成本和内存开销。

**Non-Goals:**

- 不改变 `cluster.clu` 的磁盘格式。
- 不改变 Stage1/Stage2 搜索语义、SafeOut 判定、rerank 语义或 `data.dat` 原始向量读取方式。
- 不在本 change 中压缩 `data.dat`、缓存原始向量，或改变 payload store 后端。
- 不强制移除所有 centroid/rotation 侧内存；本 change 只报告这些内存，是否进一步裁剪留给后续 change。

## Decisions

### Decision 1: compact resident layout 只保留 code region、decoded address 和必要 parallel view

`ParseClusterBlockView()` 继续负责解析一个完整 cluster block，但需要额外暴露 code/address 布局元数据，例如 `code_region_bytes`、`fastscan_region_offset`、`exrabitq_region_offset`、`address_payload_offset` 和 `address_payload_bytes`。compact preload 解析完临时 block 后：

- 将 `[block_ptr, block_ptr + code_region_bytes)` 拷贝到 `ResidentClusterView::code_storage`。
- 将 `fastscan_blocks`、`exrabitq_entries`、`exrabitq_batch_blocks` 等指针重新定位到 `code_storage.data()`。
- 将 decoded address 移入 resident view。
- 不保留 raw address payload、mini-trailer、header、lookup-table bytes 或整文件 buffer。
- `BuildResidentParallelStage2View()` 仍可从 parsed code view 构建 parallel view；parallel view 作为必要加速结构继续计入 resident footprint。

备选方案是继续整文件 preload，仅在统计中扣除 file buffer。这不能降低真实 RSS，也无法解决 raw address 重复 decode，因此不采用。

### Decision 2: resident parsed cluster 使用非 owning decoded-address view

保留 `ParsedCluster::decoded_addresses` 作为 lazy/async block path 的 owning 容器，同时为 resident path 增加 `const AddressEntry* decoded_addresses_data` 和 `uint32_t decoded_address_count` 这类非 owning view 字段。`AddressAt()` 和 `DecodeAddressBatch()` 的优先级调整为：

1. 如果存在 decoded-address view，则直接拷贝/索引 decoded address。
2. 否则如果 `decoded_addresses` owning vector 非空，则使用 owning vector。
3. 最后才对 lazy path 中的 raw address table 做即时 decode。

`ResidentClusterView::ToParsedCluster()` 在 compact resident path 中只设置 decoded-address view，不复制 vector。这样 resident view 是 decoded address 的唯一 owner，`resident_parsed_clusters_` 只保存指针和元数据。

备选方案是删除 `resident_parsed_clusters_`，每次 query 从 `ResidentClusterView` 构造临时 `ParsedCluster`。这会减少常驻对象，但会增加 hot path 组装成本，并影响现有 `GetResidentParsedCluster()` 调用点，因此本轮先保留 parsed map，但使其 non-owning。

### Decision 3: address 在 preload 解码后清掉 raw-address 语义

compact resident view 中 `raw_addresses` SHALL 为 `nullptr`，`addresses_are_raw_v2` SHALL 为 `false`。这使 resident query 不会再次从 raw table 解码地址，也让内存统计中 `resident_raw_address_bytes` 可稳定为 0。

lazy/async path 仍保持现有 raw decode 行为，避免改变非 resident 模式。

### Decision 4: 16-block batch read 以 offset 有序批处理实现

compact preload 按 `block_offset` 对 lookup entries 排序，每批最多 16 个 cluster block。每批读取从 `first.block_offset` 到 `max(block_offset + block_size)` 的连续 span 到临时 batch buffer，然后对批内每个 block 用相对 offset 调用解析逻辑。

实现约束：

- 默认 batch size 为 16，可先作为内部常量；若后续需要实验 sweep，再暴露 CLI/env 配置。
- 如果批内 span 明显大于 block size 总和，允许拆分批次，避免因为 sparse block 布局读入大量空洞。
- 每批解析完成后释放 batch buffer；resident 状态只能引用自身 `code_storage`、decoded address 和 parallel view，不能引用 batch buffer。

备选方案是逐 cluster `pread`，实现简单但 syscall 数与调度成本较高；用户明确希望批量读取，因此不采用。

### Decision 5: benchmark 输出组件化内存和构建成本

`bench_e2e`/`bench_online_query` 的 JSON 输出应继续报告现有 RSS 字段，并新增或补齐以下 component bytes：

- `resident_preload_bytes`：读取过的 `.clu` 文件字节或 compact 模式下读过的 block span 字节。
- `resident_file_buffer_bytes`：preload 后仍持有的整文件 buffer 字节；compact 模式应为 0。
- `resident_code_storage_bytes`：resident code region 实际持有字节。
- `resident_decoded_address_bytes`：resident decoded address 表字节。
- `resident_raw_address_bytes`：resident raw address payload 持有字节；compact 模式应为 0。
- `resident_parallel_view_bytes`：Stage2 parallel view 字节。
- `resident_parsed_address_duplicate_bytes`：parsed cluster 重复持有 decoded address 的字节；compact 模式应为 0。
- `resident_cluster_mem_bytes`：以上 resident cluster 组件的合计，不含 benchmark query/GT。
- `resident_preload_time_ms` 和 `resident_parallel_view_build_ms`。

COCO100k 对比测试至少固定 `topk=10`、`nprobe=64`、`query-count=1000`，分别运行旧 full-file preload 和 compact batched preload，并记录 recall、avg/p95/p99 latency、preload/build resident view wall time、after-preload RSS 和 query peak delta。

## Risks / Trade-offs

- [Risk] 指针重新定位出错会造成 resident query 读到已释放的 batch buffer。  
  Mitigation: 在测试中增加 `PreloadAllClusters()` 后释放临时 buffer 的场景，并比较 resident query/parse 地址与 code pointer 均落在 owner storage 内。

- [Risk] non-owning decoded-address view 生命周期依赖 `ResidentClusterView`，移动 map 或重新赋值可能悬空。  
  Mitigation: 先构造 owning resident view，插入 `resident_clusters_` 后再生成 `resident_parsed_clusters_`；`ToParsedCluster()` 从最终 map 节点地址生成指针。

- [Risk] batch span 读取在稀疏 block 布局下可能多读大量字节。  
  Mitigation: 以 16 个 block 为目标批大小，同时加 span/used-bytes 比例保护，超过阈值就拆批。

- [Risk] compact preload 会比整文件 preload 多一次 code region 拷贝，构建/preload wall time 可能上升。  
  Mitigation: 用 16-block batch read 减少 syscall，并在 COCO100k 报告 preload time；若时间退化过大，再考虑 mmap 或一次性 span read 后批量 memcpy 优化。

- [Risk] 旧 benchmark 结果中 `resident_preload_bytes` 语义是 `.clu` file size，新 compact 模式可能代表 bytes read。  
  Mitigation: 新增 `resident_file_size_bytes` 或在 JSON 字段说明中保留 `resident_preload_bytes` 为实际读入字节，同时用 `resident_file_buffer_bytes` 表示 retained bytes。
