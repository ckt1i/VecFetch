# Claims From Results

时间：2026-06-30T17:24:00+08:00

状态：`partial` `[pending Codex review]`

## 评估对象

Intended claim：一个最大 bits 的物理 RaBitQ 索引可以在启动时按内存预算选择较低 resident bits，无需为每个 bits 单独构建索引；该能力可作为格式贡献的一部分。

## 实验证据

- 数据集：Amazon ESCI
- 索引：同一个 bits=4 物理索引
- 查询设置：`query-count=1000`，`topk=10`，`nprobe=256`，two-level coarse routing 开启，`budget_factor=16`
- 结果目录：`/home/zcq/VDB/test/selective_preload_amazon_esci_20260630_nprobe256/`

| 配置 | cluster mem MB | avg ms | QPS | recall@10 |
| --- | ---: | ---: | ---: | ---: |
| active bits=4, resident bits=4 | 727.1 | 1.4146 | 706.9 | 0.8917 |
| active bits=3, resident bits=4 | 727.1 | 1.3975 | 715.6 | 0.7842 |
| active bits=3, resident bits=3 | 561.0 | 1.3790 | 725.2 | 0.7842 |
| active bits=2, resident bits=4 | 727.1 | 1.4034 | 712.6 | 0.7361 |
| active bits=2, resident bits=2 | 394.8 | 1.3790 | 725.2 | 0.7361 |

## Claim Gate

- `claim_supported`: partial
- `what_results_support`: 当前实现和实验支持“同一个 bits=4 物理索引可以启动期选择 resident bits=3 或 bits=2，并显著降低 resident memory”。在 Amazon ESCI 上，resident bits=3 降低约 22.8%，resident bits=2 降低约 45.7%，且对应 recall 与同 active bits、full resident 的结果一致。`nprobe=256` 下 bits=4 的 recall@10 达到 0.8917，比 `nprobe=64` 的 0.7708 更适合作为该消融设置。
- `what_results_dont_support`: 结果不支持“selective preload 稳定提升查询速度”或“该能力在所有数据集上均成立”。当前只验证了 Amazon ESCI 一个数据集和 `nprobe=256` 一个点。
- `missing_evidence`: 若论文把该点作为强 claim，建议至少在正文或附录保留这个同物理索引消融；若要泛化到多数据集，应追加 COCO100k 或 MSMARCO 的同口径点。
- `suggested_claim_revision`: 将 claim 写成“单物理索引支持按内存预算选择运行精度，降低部署和 resident memory 成本”，不要写成查询加速 claim。
- `next_experiments_needed`: 可选补充一个 COCO100k 同口径表；不是当前 Amazon ESCI 最小验证的阻塞项。
- `confidence`: medium
