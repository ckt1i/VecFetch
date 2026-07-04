# Amazon ESCI Selective Preload 最小实验（nprobe=256）
## 设置
- 数据集：Amazon ESCI，复用现有 bits=4 物理索引。
- 索引：`/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/amazon_esci/tile_lane_bitmajor_ex3/current_index_official_1_plus_n_total4_ex3_tile_lane_bitmajor`。
- 查询：`query-count=1000`，`topk=10`，`nprobe=256`，two-level coarse routing 开启，`budget_factor=16`，fixed 查询方案，关闭 progressive。
- 对比：同一 bits=4 物理索引下，比较 full resident 与 selective resident。
- 输出口径：用户可见结果统一使用 total bits；内部 extra-bit 计数不写入结果 JSON。

## 结果
| 配置 | resident模式 | code MB | cluster mem MB | preload ms | avg ms | QPS | recall@10 | avg probe ms | avg total probed | rerank vecs |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| active bits=4, resident bits=4 | compact_batched | 699.4 | 727.1 | 377.9 | 1.4146 | 706.9 | 0.8917 | 0.9839 | 60219.6 | 259.9 |
| active bits=3, resident bits=4 | compact_batched | 699.4 | 727.1 | 378.2 | 1.3975 | 715.6 | 0.7842 | 0.9716 | 60219.6 | 254.3 |
| active bits=3, resident bits=3 | compact_selective_bits3 | 533.3 | 561.0 | 346.3 | 1.3790 | 725.2 | 0.7842 | 0.9609 | 60219.6 | 254.3 |
| active bits=2, resident bits=4 | compact_batched | 699.4 | 727.1 | 378.0 | 1.4034 | 712.6 | 0.7361 | 0.9740 | 60219.6 | 253.4 |
| active bits=2, resident bits=2 | compact_selective_bits2 | 367.1 | 394.8 | 314.3 | 1.3790 | 725.2 | 0.7361 | 0.9552 | 60219.6 | 253.4 |

## 直接结论
- `nprobe=256` 下 bits=4 的 recall@10 为 0.8917，显著高于原 `nprobe=64` 的 0.7708。
- selective preload 的 recall 与同 active bits、full resident 的 recall 一致：bits=3 均为 0.7842，bits=2 均为 0.7361。
- 内存收益保持明确：resident bits=3 时 cluster resident memory 从 727.1 MB 降到 561.0 MB，约 -22.9%；resident bits=2 时降到 394.8 MB，约 -45.7%。
- 查询速度不是稳定主 claim；该实验主要支持“同一物理索引可按内存预算选择 resident precision”。
