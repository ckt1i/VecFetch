# Round1：跨 lane 的 bit-major tile 布局

日期：2026-06-28

## 目标

在 `stored_ex_bits=3` 的索引上支持查询时 `active_ex_bits=1/2/3`，同时降低 Stage2 计算中重复加载 query tile 的开销。当前 `vector_bitmajor_tiles` 已经能跳过高 bit-plane，但布局是：

```text
[lane0: tile0 bit0 bit1 bit2, tile1 bit0 bit1 bit2, ...]
[lane1: tile0 bit0 bit1 bit2, tile1 bit0 bit1 bit2, ...]
...
```

它适合单 lane 计算，但同一个 query tile 会被每个 requested lane 重复加载。

## 新布局

新增 layout：`tile_lane_bitmajor`。

每个 batch block 最多 8 个 lane，payload 按下面顺序存储：

```text
for tile in dim_tiles:
  for bit in stored_ex_bits:
    for lane in valid_lanes:
      append bit-plane(tile, lane, bit)
append factor_add[valid_lanes]
append factor_rescale[valid_lanes]
```

其中 tile 维度仍沿用当前策略：

- 剩余维度 `>256`：tile 为 512 维。
- 剩余维度 `>128`：tile 为 256 维。
- 剩余维度 `>64`：tile 为 128 维。
- 否则 tile 为 64 维。

## 预期收益

- 对 `active_ex_bits < stored_ex_bits`：按 tile 跳过高 bit-plane，不需要访问高 bit 数据。
- 对 `active_ex_bits=3`：一个 query tile 可以服务多个 requested lane，减少 query 向量重复 load。
- 对候选 lane mask 稀疏的场景：仍只处理 requested lane，不改变 SafeOut/SafeIn 语义。

## 正确性口径

`stored_ex_bits` 仍表示索引内保存的最高 extra bits；`active_ex_bits` 表示查询本轮实际计算的低位 bit 数。`active_ex_bits < stored_ex_bits` 不是重新量化出的低 bit RaBitQ，而是对 stored code 的低位部分求近似：

```text
code_active[i] = code_stored[i] & ((1 << active_ex_bits) - 1)
ip_ex_active = sum_i query[i] * code_active[i]
```

因此该模式用于动态低成本剪枝/近似 probing，不直接替代 full active bits 的最终质量口径。

## 实现范围

1. 增加 `RaBitQExDataLayout::kTileLaneBitMajor` 和 CLI 解析名 `tile_lane_bitmajor`。
2. 在 `cluster_store` v15 variable ExData writer 中按新布局打包。
3. 在 `LoadCodes` debug/测试路径补对应 unpack。
4. 增加 SIMD pack/unpack 和 masked batch kernel。
5. 在 prober 中对该 layout dispatch 到新 kernel。
6. 加 SIMD 与 cluster store/prober 单测。

## 实验口径

先跑 smoke：

- dataset：`amazon_esci`
- `nlist=8192`, `nprobe=64`, `topk=10`, `queries=50`
- `stored_ex_bits=3`, `active_ex_bits in {1,3}`
- two-level coarse routing 开启，`budget_factor=16`

若 smoke 通过，再构建 Amazon/MSMARCO 的 formal 对比索引并跑 1000-query sweep。
