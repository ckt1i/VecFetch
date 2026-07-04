# Round 0 设计：stored_ex_bits / active_ex_bits 拆分

## 背景

现有实现中 `RaBitQConfig::ex_bits` 同时承担两个含义：

1. 索引构建时写入多少 Stage2 extra bits。
2. 查询时 kernel 按多少 extra bits 读取和计算。

这会限制一个重要场景：索引已经保存 `ex_bits=3`，但查询时希望只计算 `active_ex_bits=1` 或 `2`，用更低成本的 Stage2 估计先筛掉一批候选。

## 本轮语义调整

保持索引格式向后兼容：

- 现有 `RaBitQConfig::ex_bits` 在索引与文件元数据中继续表示 `stored_ex_bits`。
- 查询路径新增 `active_ex_bits`，默认等于 `stored_ex_bits`。
- CLI 新增 `--rabitq-active-ex-bits N`。未设置时保持旧行为。
- 只允许 `0 <= active_ex_bits <= stored_ex_bits`。
- `active_ex_bits=0` 表示查询不执行 Stage2 extra-bit boost，只保留 Stage1/1-bit 路径。

## 为什么当前 bit-major/tile 布局适合动态降级

当前 `vector_bitmajor_tiles` 在同一个向量内按 tile 存储 bit-plane：

```text
vector:
  tile0: bit0_plane | bit1_plane | bit2_plane
  tile1: bit0_plane | bit1_plane | bit2_plane
  ...
```

因此从同一个 `stored_ex_bits=3` code 中只计算前 `active_ex_bits=1/2` 个 plane，不需要重建索引。相比之下，RaBitQ-Library official compact code 更偏向“每种 bit width 一种专门布局”，适合固定 bit 宽度高效解包，但不天然面向同一高 bit 索引的在线降级。

## 第一阶段实现范围

1. `SearchConfig` 增加 `rabitq_active_ex_bits`。
2. `bench_e2e` 增加 `--rabitq-active-ex-bits` 参数，并在结果 JSON 中记录 `stored_ex_bits` 与 `active_ex_bits`。
3. Stage2 dispatch 时：
   - block stride 和物理偏移继续使用 `stored_ex_bits`。
   - kernel 计算 plane 数使用 `active_ex_bits`。
4. 对 `vector_bitmajor_tiles` 和 `vector_bitplanes` 都支持 partial active bits；但本轮主优化以 `vector_bitmajor_tiles` 为目标。

## 后续三轮格式优化候选

Round 1：保守实现 active bits

- 不改磁盘格式，只改 kernel 参数。
- 目标：证明 `stored_ex_bits=3, active_ex_bits=1/2/3` 的查询可运行且 recall 单调增加。

Round 2：bit-major tile hot path 优化

- 避免每个 candidate 重复计算 tile bytes 和 active plane 分支。
- 为 `active_ex_bits=1/2/3` 增加固定模板 dispatch。
- 目标：`active_ex_bits=3` 相比当前 bitmajor 不退化。

Round 3：dynamic partial Stage2

- 引入策略：先计算高收益 plane 子集，再根据当前 frontier 判断是否继续。
- 输出 `stage2_active_bits_avg`、`stage2_partial_pruned`、`stage2_plane_passes`。
- 目标：在 recall 允许范围内减少 Stage2 计算。

## 剪枝策略初稿

候选在 Stage2 中按 bit-plane 分步：

1. 计算第一组 active planes，得到 partial score。
2. 根据剩余 plane 的最大可能贡献给出 conservative upper bound。
3. 若 `partial_score + remaining_upper < safeout_frontier`，则停止计算剩余 planes。
4. 否则继续计算下一组 plane。

第一版先实现保守统计和可开关策略，避免破坏已有 SafeOut/SafeIn 语义。

## 索引保留策略

本轮 test 目录中可以临时生成多个格式索引，但最终只保留：

- `indexes/baseline_official_vector_bitplanes/{amazon_esci,msmarco_passage}`：原始 official-like baseline 的引用或软链接。
- `indexes/best/{amazon_esci,msmarco_passage}`：最终最优方法索引。

其他中间索引目录在结果汇总后删除；删除前必须把对应实验结果 CSV/JSON 和路径记录写入本目录。

