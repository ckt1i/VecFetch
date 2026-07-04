# 技术冻结建议：功能、分支与选项取舍

日期：2026-07-01

## 总体判断

建议冻结为一条简洁主线：

```text
tile_lane_bitmajor RaBitQ 格式
+ fixed bits 查询
+ selective resident preload
+ compact_batched resident preload
+ two-level coarse routing warmup
+ SafeIn/SafeOut + pipeline overlap
```

不要把 progressive bit pruning、Stage1 envelope、full-file hugepage、address sorting、budgeted early-submit 放入最终默认路径。它们都有明确负结果或收益不稳定，继续保留在主线会削弱论文叙事，也会增加复现实验和调参负担。

冻结实现口径：`remove-redundant-query-features` 清理后，progressive pruning、Stage1 block envelope、vector-read address sort、budgeted early-submit 已从主线查询配置、benchmark CLI 和正式输出字段中删除；历史优化记录只作为负结果和设计取舍依据，不再作为可运行推荐。

## 建议保留并进入默认主线

| 功能 | 建议 | 依据 |
| --- | --- | --- |
| `tile_lane_bitmajor` 格式 | 保留为我们方法的冻结格式 | 支持同一 bits=4 物理索引在启动期选择 resident bits=3/2；Stage2 component 有稳定小收益，但不要 claim 端到端稳定加速。 |
| fixed 查询方案 | 默认使用 | progressive pruning 多轮实验未稳定跑赢 fixed。 |
| `--rabitq-active-bits` | 保留为用户可见接口 | 用 total bits 表述，不再对外暴露 stage1/stage2 ex bits。 |
| `--rabitq-resident-bits` | 保留为 selective preload 的核心接口 | Amazon ESCI nprobe=256 下 resident bits=3/2 分别降低 resident memory 约 22.8%/45.7%，recall 与同 active bits 的 full resident 一致。 |
| `VDB_RESIDENT_PRELOAD_MODE=compact` / `compact_batched` | 默认 resident preload 路径 | 稳定、简单，支持 selective resident preload。 |
| two-level coarse routing + warmup | 默认开启，`budget_factor=16` | warmup 前移后避免 hierarchy build 摊入查询；此前用户也要求默认开启 two-level。 |
| SafeIn/SafeOut + pipeline overlap | 保留为主方法核心 | 对应论文算法和系统贡献，不应为了精简而移除。 |
| `non_safeout_candidate_budget=400` | 主实验建议固定使用；selective preload 消融可单独说明口径 | 历史主实验多采用该 budget；selective preload 最新表主要证明内存与同口径 recall，不作为主 Pareto。 |

## 建议保留但默认关闭

| 功能/选项 | 建议 | 原因 |
| --- | --- | --- |
| `compact_code_mmap_2mb` hugepage 模式 | 保留为部署实验选项，默认关闭 | MSMARCO bits=4 上 code-only slab 相比 compact 有约 4.7%-5% 查询收益，但 preload 更慢，且依赖 THP materialize。 |
| `VDB_RESIDENT_HUGEPAGE=1` / `VDB_RESIDENT_HUGEPAGE_COLLAPSE=1` | 保留为高级部署开关，默认关闭 | 环境敏感；不能作为论文默认优化。 |
| fine-grained / hotpath timing | 保留为 profiling 开关，默认关闭 | 对定位瓶颈有用，但会干扰性能。 |
| `vector_bitplanes` official-like layout | 保留为 baseline / compatibility | 用于和 official-like RaBitQ 对照，不作为我们方法默认格式。 |
| 增量 bit-plane kernel | 可保留在底层 SIMD/test 中 | 正确性已验证，可支持未来研究，但不暴露为主方法。 |

## 建议从最终主线去掉或隐藏

| 功能/分支/选项 | 建议 | 证据 |
| --- | --- | --- |
| progressive pruning：`--rabitq-progressive-*` | 已从主线 benchmark CLI / 查询配置删除；底层 SIMD 正确性代码暂保守保留 | Conservative bound 剪不掉 lane；scale=0 也不能稳定赢 fixed，MSMARCO full Stage2 SafeOut oracle 上限仅约 2.3%。 |
| Stage1 block envelope：`--stage1-block-skip-envelope` | 已从主线 benchmark CLI / 查询配置 / resident preload 统计删除 | runtime 版本 ESCI 1.8ms -> 10.9ms；precompute 版本 skip rate 仅 0.47%/0.28%，明显负优化。 |
| `VDB_STAGE1_PRECOMPUTE_ENVELOPE` | 已从 resident preload 路径删除 | 额外内存 22.7 MiB / 106.2 MiB，但总耗时大幅变慢。 |
| `--vec-read-address-sort` / sort window | 已从主线 benchmark CLI / scheduler 分支删除 | 地址排序降低少量 final drain，但排序/emit 开销更高，总体变慢。 |
| `--budgeted-early-submit` 及 interval/count/max | 已从主线 benchmark CLI / scheduler early-submit 分支删除；最终 `non_safeout_candidate_budget` 保留 | submit/drain 降低但总耗时变慢；当前 early-submit 判定不够稳定。 |
| `full_file_mmap_2mb` | 不作为默认；建议隐藏到 legacy/debug | MSMARCO bits=4 final_drain 明显倒退，full-file 冷数据常驻污染热路径。 |
| full-file resident preload | 不建议用于最终 online-query 主路径 | 内存口径更大，也和 selective resident preload 不兼容。 |
| `small_lane4` / `small_lane2` / `microbatch` | 从最终候选删除 | 复核后收益不稳定或负收益。 |
| `vector_2bit` 专用 direct kernel | 不作为默认 hot path | COCO total bits=3 下略慢于 `vector_bitplanes`。 |
| ex4 无损压缩路线 | 删除正向 claim，只保留负结果记录 | 熵接近满 4 bit/dim，无法接近 3-bit 空间目标。 |
| legacy signed-magnitude / generic packed 路线 | 只保留读旧索引或调试需要 | 论文和最终实验不应再提。 |
| `--rabitq-active-ex-bits` 兼容别名 | 仅内部保留；用户文档和实验脚本全部使用 `--rabitq-active-bits` | 用户侧术语应统一 total bits，避免 stage1/stage2 ex bits 混淆。 |

## 建议冻结的实验/运行配置

### 主方法默认

```bash
VDB_RESIDENT_PRELOAD_MODE=compact
bench_e2e \
  --rabitq-active-bits 4 \
  --rabitq-resident-bits 4 \
  --two-level-coarse-routing 1 \
  --two-level-coarse-budget-factor 16 \
  --dynamic-safeout 1 \
  --dynamic-safein frontier
```

### selective preload 消融

```bash
--rabitq-active-bits 4 --rabitq-resident-bits 4
--rabitq-active-bits 3 --rabitq-resident-bits 3
--rabitq-active-bits 2 --rabitq-resident-bits 2
```

必要时保留 full resident 对照：

```bash
--rabitq-active-bits 3 --rabitq-resident-bits 4
--rabitq-active-bits 2 --rabitq-resident-bits 4
```

Amazon ESCI 最新建议口径使用 `nprobe=256`，因为 `nprobe=64` 下 recall@10 偏低。

## 论文表述边界

可以写：

- 我们的格式支持一个最大 bits 物理索引在启动时按内存预算选择 resident precision。
- 在 Amazon ESCI `nprobe=256` 上，resident bits=3/2 相比 full resident 分别降低约 22.8%/45.7% resident memory，且与同 active bits 的 full resident recall 一致。
- `tile_lane_bitmajor` 降低了 Stage2 code-scan 组件成本，并支持 active bits / resident bits 解耦。

不要写：

- progressive bit pruning 稳定加速。
- 新 Stage2 格式稳定带来 2% 以上端到端加速。
- hugepage 是默认优化或跨数据集稳定收益。
- ex4 无损压缩能接近 ex3 空间。

## 最终建议

技术冻结时，主线应尽可能少：

1. 保留 `tile_lane_bitmajor + fixed query + selective preload`。
2. 保留 two-level warmup 和现有 SafeIn/SafeOut pipeline。
3. 所有负结果优化从最终实验脚本移除；代码层若暂不删除，也要标为 experimental/diagnostic。
4. 用户文档、命令和结果表统一使用 `bits=2/3/4`，不再使用 `ex_bits=1/2/3` 表述。
