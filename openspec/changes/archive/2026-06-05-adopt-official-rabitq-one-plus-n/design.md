## Context

当前实现已经完成了几个关键工程优化：Stage1 FastScan 使用 packed block，Stage2 支持 compact blocked layout，Stage2 magnitude 已经可以按实际 bit-width bit-pack，resident preload 也已经能够只保留 query hot path 所需的 code/address 状态。

但当前 RaBitQ bit 语义仍然不是官方 `1+n`：

```text
当前 legacy signed-magnitude:
  Stage1: 1-bit sign/MSB path
  Stage2: bits-bit magnitude
  Stage2: separate packed sign

官方 1+n:
  Stage1: 1-bit BinData
  Stage2: ex_bits = total_bits - 1 的 sign-folded ExData
  Stage2 score: 2^ex_bits * ip_x0_qr + ip_ex + factors
```

因此当前 `bits=4` 实际比官方 `total_bits=4` 存储了更多 Stage2 信息。若继续沿用当前命名，实验报告中的 bit-width、内存和 recall/QPS Pareto 都会与官方 RaBitQ 不公平。

本设计引入新一版 rebuild-required RaBitQ 格式，使新格式遵循官方 `1+n` 数学语义，同时保留本项目已有的 resident compact preload、Stage2 block scheduling、SafeIn/SafeOut pipeline 和 raw-vector rerank 架构。

## Goals / Non-Goals

**Goals:**

- 将新格式的 bit 语义改为 `total_bits = 1 + ex_bits`。
- Stage2 持久化 sign-folded `ex_code`，不再持久化 `ex_sign_packed`。
- 查询路径使用 Stage1 产生的 `ip_x0_qr` 中间项和 Stage2 `ip_ex` 组合官方 ExData 估计值。
- 新增显式存储版本，避免旧 v10/v11/v12 signed-magnitude 格式被误解析。
- 支持 `total_bits=1/3/4/5` 对应 `ex_bits=0/2/3/4`，其中 `total_bits=4` 是主实验需要的官方 `1+3` 点。
- benchmark 和 metadata 同时报告 `storage_format`、`total_bits`、`ex_bits` 和 legacy/official estimator mode。
- 保留读取旧格式索引的能力，便于对拍和回滚。

**Non-Goals:**

- 不在本 change 中删除旧 v10/v11/v12 reader 或旧实验结果。
- 不要求旧 binary 能读取新格式索引。
- 不改变 `data.dat` 原始向量 rerank 语义。
- 不引入 PQ 或其他新的向量量化方法。
- 不重新设计 coarse routing、payload store、DiskANN baseline 或 parquet baseline。
- 不保证新格式在所有数据集上立即替代旧 Pareto 点；需要实验验证后再合并主表。

## Decisions

### Decision 1: 采用官方 sign-folded ExData，而不是本地 sign-dedup 变体

新格式 SHALL 在编码时对负维度的 ExData code 做 complement，使 sign 信息折叠进 `ex_code`。Stage2 查询不再读取 `ex_sign_packed`，而是计算 `ip_ex = dot(query, ex_code)`，再与 Stage1 的 `ip_x0_qr` 组合。

理由：

- 数学语义与官方 RaBitQ 对齐，论文中可直接报告 `total_bits=1+n`。
- 内存上比当前格式少一个 sign bit/维；同名 `total_bits` 下还少一个 Stage2 magnitude bit/维。
- 查询路径不需要从 Stage1 FastScan block 临时解码 sign，避免把节省的常驻内存换成额外热路径 CPU。

备选方案是只删除 Stage2 sign，查询时从 Stage1 sign 临时解码并继续使用 signed-magnitude kernel。该方案实现较小，但仍不是官方 estimator，且会引入 touched block 上的重复 sign decode，因此不作为主路径。

### Decision 2: 新格式使用独立 `cluster.clu` 版本

新格式定义为当前 compact packed-magnitude 格式之后的新版本，本文称为 `v13`。`v13` Region2 仍保持 batch-major blocked layout，但每个 Stage2 batch block 存：

```text
valid_count
for each dim_block:
  for each lane:
    packed sign-folded ex_code[ceil(dim_block * ex_bits / 8)]
ex_factor arrays or per-lane factor scalars needed by official estimator
```

对于 `ex_bits=0`，Stage2 ExData region SHALL 为空，查询只使用 Stage1。

理由：

- 新格式与 v12 的 `packed magnitude + packed sign + xipnorm` 语义不同，必须通过版本显式区分。
- 继续沿用 batch-major blocked layout 可复用当前 resident preload、block scheduling 和 batch decode 框架。
- format version 明确 rebuild/rollback 边界，避免 silent misparse。

### Decision 3: 配置和 metadata 拆分 `total_bits` 与 `ex_bits`

新索引 metadata SHALL 记录：

- `rabitq_total_bits`
- `rabitq_ex_bits`
- `rabitq_estimator_mode = official_1_plus_n`
- `cluster_clu_version`
- legacy 兼容字段 `bits` 的解释

runtime 内部使用 `total_bits` 计算 ConANN epsilon、SafeIn/SafeOut margin 和实验标签；Stage2 ExData packing/kernel 使用 `ex_bits`。

理由：

- 当前 `cfg.bits` 同时被 build、storage、margin、ConANN 和 benchmark 使用，继续复用会造成误用。
- `total_bits=4` 和 `ex_bits=3` 必须同时可见，否则 3-bit SIMD 支持和论文标签容易混淆。

### Decision 4: Stage1 需要输出可复用的 `ip_x0_qr`

Stage1 FastScan 当前主要输出 Stage1 estimated distance 和 margin 分类所需数据。官方 Stage2 需要每个 Stage2 candidate 的 `ip_x0_qr`。因此 Stage1 prober SHALL 在候选进入 Stage2 时保存或可重构该中间项，并随 lane metadata 传入 Stage2 scatter/classify。

理由：

- 官方 estimator 的核心项是 `2^ex_bits * ip_x0_qr + ip_ex + kbxsumq`。
- 重新对 Stage2 candidate 单独扫描 BinData 计算 `ip_x0_qr` 会增加 hot path 工作，应尽量从 Stage1 raw accumulator 推导并缓存。

### Decision 5: 3-bit ExData 是一等支持对象

`total_bits=4` 对应 `ex_bits=3`，是主实验最关键点。实现 SHALL 支持 3-bit ExData pack/unpack 和 SIMD dot path。布局可以采用官方思路，将 3-bit code 拆成低 2-bit compact payload 和高 1-bit payload。

理由：

- 只支持 2/4-bit 会迫使官方 `total_bits=4` 缺失，无法完成和官方 RaBitQ 语义一致的主实验。
- 3-bit unpack 比 4-bit 更复杂，但官方库已有成熟 layout 思路，可作为实现参考。

### Decision 6: 新旧格式并存，benchmark 明确标注

旧格式继续称为 `legacy_signed_magnitude`，新格式称为 `official_1_plus_n`。benchmark 输出和结果汇总 SHALL 明确标注两者，避免把 legacy `bits=4` 与 official `total_bits=4` 混为同一点。

理由：

- 旧结果仍有对比价值，特别是说明更高 Stage2 信息量带来的 recall/QPS 上界。
- 主结果表应使用 official-compatible 点；legacy 点可作为 ablation 或 engineering comparison。

## Risks / Trade-offs

- [Risk] 新 estimator 改动会影响 SafeIn/SafeOut 判定，可能导致 recall 回退。  
  Mitigation: 先做 score parity / monotonic sanity tests，再在 COCO100k 上跑 recall/QPS/memory probe，不达标则不替换主表。

- [Risk] Stage1 `ip_x0_qr` 推导与当前 distance formula 不一致会造成 Stage2 分数偏移。  
  Mitigation: 增加小样本 reference path，对比官方公式、当前 scalar reference 和 SIMD kernel 输出。

- [Risk] 3-bit ExData SIMD kernel 复杂度较高，可能拖慢 `total_bits=4` 查询。  
  Mitigation: 先实现 scalar/reference + AVX2/AVX512 microbench，再接入 E2E；必要时保留 decode-to-scratch 后通用 dot 的过渡路径。

- [Risk] v13 metadata 与旧 `bits` 字段并存时容易误读。  
  Mitigation: 新格式读入时要求 `total_bits/ex_bits/estimator_mode` 完整；旧格式缺字段时明确降级为 legacy mode。

- [Risk] 新格式需要 rebuild，会增加实验准备成本。  
  Mitigation: 不原地覆盖旧索引；新索引写入独立目录，并在 benchmark metadata 中记录源数据、nlist、nprobe、format version。

## Migration Plan

1. 增加 `v13 official_1_plus_n` writer/reader，同时保留 v10/v11/v12 reader。
2. 在 encoder/config 中引入 `total_bits/ex_bits`，并在新格式 build 中生成 sign-folded ExData。
3. 为 Stage1 candidate metadata 增加 `ip_x0_qr` 或等价可复用中间项。
4. 实现 ExData pack/unpack 与 dot kernel，覆盖 `ex_bits=2/3/4` 和 `ex_bits=0`。
5. 接入 Stage2 classifier、SafeIn/SafeOut margin、ConANN epsilon 和 benchmark metadata。
6. 在 COCO100k 上构建新索引并运行 correctness、memory、latency、recall 对拍。
7. 验证通过后再扩展到 Amazon ESCI、MSMARCO、ImageNet1K、VoxCeleb2。
8. 若需要回滚，继续使用旧格式索引和 legacy query path；不承诺旧 binary 读取 v13。

## Open Questions

- `total_bits=2` 是否需要作为正式实验点，还是仅保留 `total_bits=1/3/4/5`。
- v13 是否将 official factors 完全按官方 `f_add_ex/f_rescale_ex` 命名存储，还是复用当前 `xipnorm` 字段但更改语义。
- 3-bit ExData 首版是否必须有手写 AVX512 dot kernel，还是允许先 decode-to-scratch 后用通用 SIMD dot 验证端到端。
