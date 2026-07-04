# Round2：tile_lane_bitmajor 的 bit-plane 融合计算

日期：2026-06-28

## 背景

Round1 的 `tile_lane_bitmajor` 在 Amazon smoke 中相对 `vector_bitmajor_tiles` 更快，但在同口径 `vector_bitplanes` official-like baseline 下，50-query 自检结果略慢：

| layout | avg ms | stage2 ms | recall@10 |
| --- | ---: | ---: | ---: |
| vector_bitplanes | 0.6354 | 0.0626 | 0.8960 |
| tile_lane_bitmajor | 0.6502 | 0.0666 | 0.8960 |

初步判断：Round1 kernel 虽然让同一 tile 下多个 lane 连续存放，但计算内层按 `bit` 循环，每个 bit-plane 都重新 load query slice，并且用 runtime weight 乘法累加。它没有充分兑现“query tile 复用”的设计收益。

## 修改方案

将 `tile_lane_bitmajor` 的 AVX512 hot path 从：

```text
for tile:
  for bit:
    for sub-slice:
      load q
      for lane:
        dot += masked(q * bit_weight)
```

改为：

```text
for tile:
  for sub-slice:
    load q once
    for lane:
      dot0 += masked(q, plane0)
      dot1 += masked(q, plane1)
      dot2 += masked(q, plane2)
```

最后统一做：

```text
ip = sum(dot0) + 2 * sum(dot1) + 4 * sum(dot2)
```

## 预期

- `active_ex_bits=3`：减少 query slice 重复 load 和 runtime multiply，Stage2 应下降。
- `active_ex_bits=1`：路径接近 Round1，不应明显退化。
- recall 不应变化。

## 验证

先跑：

- `test_ip_exrabitq`
- Amazon 50-query：`topk=10,nprobe=64,active=3`

若有效，再进入 1000-query sweep。
