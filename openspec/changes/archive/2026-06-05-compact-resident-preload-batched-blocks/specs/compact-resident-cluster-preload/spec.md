## ADDED Requirements

### Requirement: Compact resident preload SHALL retain only query-required cluster state
系统 SHALL 支持 compact resident preload 模式，在预加载完成后只保留在线查询所需的 cluster-side resident index 状态。该状态至少包括 FastScan code region、ExRaBitQ Stage2 code region、已解码地址表，以及必要时为 Stage2 SIMD 查询构建的 parallel view；系统 MUST NOT 依赖完整 `cluster.clu` 文件 buffer 来支撑后续 resident query。

#### Scenario: Compact preload releases non-query cluster bytes
- **WHEN** compact resident preload 完成
- **THEN** resident 状态 SHALL 不再保留完整 `.clu` file buffer
- **AND** resident 状态 SHALL 不再保留 header、lookup table、mini-trailer 或已经解码完成的 address payload bytes
- **AND** 后续 resident query SHALL 仍可访问 probed cluster 的 quantized codes 和地址信息

#### Scenario: Compact resident pointers reference owned resident storage
- **WHEN** compact resident preload 为一个 cluster 建立 resident view
- **THEN** `fastscan_blocks`、`exrabitq_entries` 和 `exrabitq_batch_blocks` 等 code 指针 SHALL 指向该 cluster 自己持有的 resident code storage
- **AND** 这些指针 MUST NOT 指向已释放的 batch read buffer

### Requirement: Compact resident preload SHALL decode addresses once during preload
compact resident preload SHALL 在 preload 阶段完成 raw/packed address payload 到 `AddressEntry` 的解码，并在 resident query 阶段直接使用 decoded address。resident query MUST NOT 对同一个 resident address payload 重复执行 raw-address decode。

#### Scenario: Resident query uses decoded addresses directly
- **WHEN** compact resident preload 已经完成并且查询路径请求一个 resident cluster 的 candidate addresses
- **THEN** `AddressAt()` 和 `DecodeAddressBatch()` SHALL 从 decoded address view 或 decoded address storage 直接读取地址
- **AND** 它们 MUST NOT 在 resident 模式下从 raw address table 再次解码地址

#### Scenario: Lazy block path keeps raw-address compatibility
- **WHEN** 查询不使用 compact resident preload，而是使用 lazy 或 async cluster block 解析路径
- **THEN** 系统 SHALL 保持现有 raw address table 即时 decode 兼容性
- **AND** compact resident 的地址去重逻辑 MUST NOT 改变非 resident 模式的正确性

### Requirement: Resident parsed clusters SHALL avoid duplicated decoded-address ownership
在 compact resident preload 模式下，resident parsed cluster SHALL 使用非 owning decoded-address view 指向 `ResidentClusterView` 持有的 decoded address storage。系统 MUST NOT 同时在 resident view 和 resident parsed cluster 中各自持有一份等价的 decoded address vector。

#### Scenario: Parsed cluster uses non-owning address view
- **WHEN** compact resident preload 为一个 cluster 生成 `ParsedCluster`
- **THEN** `ParsedCluster` SHALL 可以通过非 owning pointer/count view 访问 decoded addresses
- **AND** `ParsedCluster::decoded_addresses` owning vector SHALL 不复制同一份 resident decoded address

#### Scenario: Address view lifetime is stable after preload
- **WHEN** resident preload 完成并进入多 query steady state
- **THEN** resident parsed cluster 的 decoded-address view SHALL 在 `ClusterStoreReader` 生命周期内保持有效
- **AND** reader close 或重新 preload 前 SHALL 不释放被 view 引用的 resident address storage

### Requirement: Compact resident preload SHALL batch-read cluster blocks in groups of sixteen
compact resident preload SHALL 按 block offset 对 cluster blocks 进行批量读取，每批目标大小为 16 个 cluster blocks。每批 SHALL 通过一次连续 span read 读取批内 block 所覆盖的字节范围，然后在批内逐 block 解析并拷贝必要 resident state。

#### Scenario: Sixteen cluster blocks are read as one batch when layout is dense
- **WHEN** lookup table 中连续 16 个 cluster blocks 的 offset span 没有异常空洞
- **THEN** compact resident preload SHALL 用一次 batch read 读取这 16 个 blocks 覆盖的连续 span
- **AND** SHALL 在该 batch buffer 中按每个 block 的相对 offset 解析 cluster

#### Scenario: Sparse spans are split to avoid excessive over-read
- **WHEN** 一个 16-block batch 的连续 span 明显大于这些 block size 的合计
- **THEN** 系统 MAY 将该 batch 拆成更小 batch
- **AND** 拆分后 SHALL 保持每个 cluster block 被完整解析且 resident query 语义不变

#### Scenario: Batch buffer is temporary
- **WHEN** 一个 batch 中所有 cluster blocks 解析并 materialize 完成
- **THEN** batch read buffer SHALL 可以被释放
- **AND** resident state MUST NOT 保存指向该 batch buffer 的 raw pointer

### Requirement: Compact resident preload SHALL preserve search semantics
compact resident preload SHALL 只改变 `.clu` 预加载后的内存保留形态和 preload 读取组织方式，不得改变同一索引和同一查询参数下的搜索语义、候选有效性、rerank 输入或最终 top-k 定义。

#### Scenario: Compact and full-file resident modes produce equivalent result semantics
- **WHEN** 使用同一 COCO100k 索引、同一 query 集、同一 `nprobe`、同一 `topk` 和同一 SafeOut/rerank 参数分别运行旧 full-file preload 与 compact resident preload
- **THEN** 两者 SHALL 使用相同 recall 口径
- **AND** 结果差异 MUST 只能来自同等语义下的执行路径变化，而不是参数或候选定义变化
