# Round3：tile_lane_bitmajor 的 tile 内指针提升

日期：2026-06-28

## 背景

Round2 后，`tile_lane_bitmajor` 在 Amazon 50-query smoke 中已经把 Stage2 时间从
`0.0625 ms` 降到 `0.0597 ms`，但总查询时间仍略慢于 `vector_bitplanes`：

| layout | avg ms | Stage2 ms | recall@10 |
| --- | ---: | ---: | ---: |
| vector_bitplanes | 0.6319 | 0.0625 | 0.8960 |
| tile_lane_bitmajor | 0.6367 | 0.0597 | 0.8960 |

这说明新的 batch tile 布局对 Stage2 有收益，但 hot path 仍有额外循环开销。

## 修改方案

当前 `tile_lane_bitmajor` 的 AVX512 kernel 在每个 `sub-slice` 内，对每个 requested lane
重复计算：

```text
plane_base + lane * plane_bytes + sub * 2
```

Round3 将在每个 tile 开始时，为 requested lanes 预先计算当前 tile 的 bit-plane 指针：

```text
plane_ptr[bit][requested_lane]
```

随后内层 `sub-slice` 循环只做：

```text
load mask from plane_ptr[bit][li] + sub * 2
masked add query slice
```

## 预期

- 不改变磁盘格式、内存格式、recall 或 active/stored ex_bits 语义。
- 对稀疏 lane_mask 和 full lane_mask 都应降低 Stage2 内层地址计算开销。
- 若总查询时间仍未超过 official-like `vector_bitplanes`，说明主要差距已不在 Stage2 kernel 本身，而在外围 submit/probe 调度噪声或当前 batch 布局对 sparse mask 的局部性收益不足。

## 验证

1. 跑 `test_ip_exrabitq`、`test_cluster_store`、`test_cluster_prober`。
2. 使用既有 Amazon 索引跑同口径 50-query smoke：
   - `topk=10`
   - `nprobe=64`
   - `stored_ex_bits=3`
   - `active_ex_bits=3`
   - 对比 `vector_bitplanes` 与 `tile_lane_bitmajor`
3. 若 smoke 有收益，再进入 Amazon/MSMARCO 1000-query formal sweep。

## 结果

相关单测通过：

- `test_ip_exrabitq`
- `test_cluster_store`
- `test_cluster_prober`

Amazon 50-query smoke 结果：

| layout | avg ms | Stage1 ms | Stage2 ms | submit ms | recall@10 |
| --- | ---: | ---: | ---: | ---: | ---: |
| vector_bitplanes | 0.6313 | 0.1167 | 0.0622 | 0.1901 | 0.8960 |
| tile_lane_bitmajor + pointer hoist | 0.6411 | 0.1207 | 0.0627 | 0.1974 | 0.8960 |

## 结论

该优化为负收益，未进入最终代码。可能原因是：

- 原先的 `lane * plane_bytes` 地址计算已经被编译器较好优化；
- 新增的临时指针数组增加了寄存器压力和栈访问；
- 当前 Stage2 lane mask 较稀疏，指针提升没有形成稳定的数据复用优势。

后续继续保留 Round2 的 fused bit-plane kernel 作为 `tile_lane_bitmajor` 当前实现。
