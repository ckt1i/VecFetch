# Round 1 Experiment Tracker

**日期**: 2026-05-09  
**状态说明**: 本文件是补充实验执行清单；当前全部为 TODO，尚未启动新实验。

| ID | 优先级 | 实验 | 数据集 | 系统/变体 | 固定点 | 重复 | 状态 | 产物 |
|---|---|---|---|---|---|---|---|---|
| R1-E1 | P0 | COCO 主结果 matched-quality 对齐 | COCO100K | VecFetch, IVF+RaBitQ FlatStor | top10, nprobe 64/128/256；必要时 96/160/192 | 每候选点 3 次 | TODO | `coco_main_alignment_round1.csv` |
| R1-E2 | P0 | SafeIn 稳定性重复 | COCO100K | Full, SafeIn-off | top10, nprobe=64 | 5-10 次 | TODO | `safein_repeat_coco_round1.csv` |
| R1-E3 | P1 | SafeIn 稳定性确认 | MS MARCO | Full, SafeIn-off | top10, nprobe=128 | 3-5 次 | TODO | `safein_repeat_msmarco_round1.csv` |
| R1-E4 | P1 | 主表基线协议清理 | COCO/MS MARCO | IVF+RaBitQ FlatStor | 最终主表点 | 3 次 | TODO | `baseline_measurement_cleanup_round1.csv` |
| R1-E5 | P2 | SafeIn 适用边界压力实验 | COCO100K / MS MARCO | Full, SafeIn-off, threshold sweep | top20/top50 或 threshold sweep | 3 次 | TODO | `safein_boundary_stress_round1.csv` |

## 统一记录字段

每个运行至少记录：

- dataset
- system / variant
- top-k
- nprobe
- queries
- protocol
- repeat id
- recall@10 或 recall@20
- avg/p50/p95/p99 latency
- QPS
- SafeOut / Uncertain / SafeIn count
- reranked candidates
- SafeIn prefetch count
- final original-data fetch count
- bytes read
- submit calls
- io wait / remaining fetch time
- config hash 或公开配置摘要

## 论文写入规则

- 主表只采用通过新选点规则筛选后的代表点。
- SafeIn 相关结论必须基于 repeat distribution，而不是单次运行。
- 如果实验结果与第一版数字冲突，优先使用新 measurement 结果，旧结果移到历史记录或弃用。
- 任何本机路径、run id、私有输出目录只留在实验内部记录，不写入论文正文或公开附录。

