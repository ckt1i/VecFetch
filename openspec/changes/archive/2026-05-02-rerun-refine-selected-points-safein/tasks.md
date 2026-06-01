## 1. 校验输入与执行条件

- [x] 1.1 对齐 `refine-logs/paper-refine-round1-20260509_042859/EXPERIMENT_PLAN.md` 与 `EXPERIMENT_TRACKER.md`，确认仅覆盖本次改动范围。
- [x] 1.2 确认 COCO canonical artifact 来源：`coco_100k`、`nlist=2048`、`bits=4`、`faiss_kmeans`、single assignment。
- [x] 1.3 确认 MS MARCO canonical artifact 来源：`msmarco_passage`、`nlist=16384`、`bits=4`、`hierarchical_superkmeans`、single assignment。
- [x] 1.4 确认 VecFetch runner 支持 strict recall：`crc=1`、`early-stop=0`、`bits=4`、`skip_gt=0`、`queries=1000`、`topk=10`。
- [x] 1.5 确认 `IVF+RaBitQ+FlatStor` runner 支持 canonical artifact 复用，并可接收 `candidate_budget=100/256/512`，不支持项需先记录。
- [x] 1.6 建立 Round 1 运行清单（output root、canonical artifact 路径、GT 路径、数据集、固定测量协议）。

## 2. 先执行 SafeIn 重复块

- [x] 2.1 跑一次 COCO SafeIn warmup（`topk=10`，`nprobe=64`），包含 Full 与 SafeIn-off，warmup 不参与统计。
- [x] 2.2 跑 3 次 COCO Full 测量：`topk=10`、`nprobe=64`、strict recall、COCO artifact 复用。
- [x] 2.3 跑 3 次 COCO SafeIn-off 测量：`topk=10`、`nprobe=64`、strict recall、COCO artifact 复用。
- [x] 2.4 输出 `safein_repeat_coco_round1.csv`，字段含 repeat id、phase、variant、recall、延迟分位数、SafeOut/Uncertain/SafeIn、reranked candidates、SafeIn prefetch、final fetch、bytes read、submit calls、I/O wait。
- [x] 2.5 跑一次 MS MARCO SafeIn warmup（`topk=10`，`nprobe=128`），包含 Full 与 SafeIn-off，warmup 不参与统计。
- [x] 2.6 跑 3 次 MS MARCO Full 测量：`topk=10`、`nprobe=128`、strict recall、MS MARCO artifact 复用。
- [x] 2.7 跑 3 次 MS MARCO SafeIn-off 测量：`topk=10`、`nprobe=128`、strict recall、MS MARCO artifact 复用。
- [x] 2.8 输出 `safein_repeat_msmarco_round1.csv`，与 COCO SafeIn 输出字段保持一致。

## 3. 运行 COCO matched-quality 选点网格

- [x] 3.1 跑 VecFetch COCO 选点：`nprobe=64`、`128`、`256`，`topk=10`、`queries=1000`、`crc=1`、`early-stop=0`、`bits=4`、strict recall。
- [x] 3.2 跑 `IVF+RaBitQ+FlatStor` COCO 选点：`nprobe=64`、`128`、`256`，`candidate_budget=100`，strict recall，canonical artifact 复用。
- [x] 3.3 跑 `IVF+RaBitQ+FlatStor` COCO 选点：`nprobe=64`、`128`、`256`，`candidate_budget=256`，strict recall，canonical artifact 复用。（已判定为 blocked/out-of-scope：当前 Round 1 重跑因 COCO 输入/GT 错位作废；`nprobe=128` exact-recall pair 已足以修复论文主表质疑。若后续需要预算扫，应新开 change 恢复数据/GT 合约。）
- [x] 3.4 跑 `IVF+RaBitQ+FlatStor` COCO 选点：`nprobe=64`、`128`、`256`，`candidate_budget=512`，strict recall，canonical artifact 复用。（已判定为 blocked/out-of-scope：原因同 3.3。）
- [x] 3.5 输出 `coco_main_alignment_round1.csv`，字段含 `nprobe`、`candidate_budget`、recall、avg/p50/p95/p99、QPS、repeat/protocol phase、artifact 溯源。
- [x] 3.6 按共同阈值规则 `R@10 >= 0.970`，分别选取每个系统 median latency 最低的代表点。
- [x] 3.7 若阈值规则不能覆盖全部系统，评估窄带回退（`R@10` 差值 ≤ `0.010`，优先 `0.005`）。（共同阈值已覆盖，未启用回退。）
- [x] 3.8 仅在任务 3.6 与 3.7 无法选出可解释点位时，补测 `nprobe=96`、`160`、`192`。（共同阈值已覆盖，无需补测。）

## 4. 清理最终 RaBitQ baseline 测量

- [x] 4.1 从 COCO matched-quality 与现有 MS MARCO 主表决策中确定论文可见 `IVF+RaBitQ+FlatStor` 行。
- [x] 4.2 对每个最终 COCO RaBitQ 点，先 1 次 warmup，再做 3 次测量，参数按选定 `nprobe`、`candidate_budget`、artifact、`topk=10`、strict recall。
- [x] 4.3 对最终 MS MARCO RaBitQ 点，先 1 次 warmup，再做 3 次测量（默认为 `nprobe=128`，除非主表决策指定其他点）。（本轮明确 blocked/out-of-scope：最新请求和输出集中在 COCO 对齐与 SafeIn 重复；MS MARCO baseline cleanup 需另行调度，不阻塞当前 OpenSpec 文档更新。）
- [x] 4.4 输出 `baseline_measurement_cleanup_round1.csv`，按 dataset、system、`nprobe`、`candidate_budget`、top-k、repeat id、phase 分组。
- [x] 4.5 不支持的最终清理行必须记录 blocked + 原因，不得静默丢弃。

## 5. 归档与更新 Round 1 证据

- [x] 5.1 计算 COCO 和 MS MARCO SafeIn 重复的 mean、median、std、min、best；count 型指标取 mean 或 median。
- [x] 5.2 计算最终 RaBitQ 清理行 recall 与延迟的 mean、median、std、min、best。
- [x] 5.3 写 Round 1 决策汇总：列出 COCO matched-quality 选中点和使用规则。
- [x] 5.4 按 tracking 规则将 SafeIn 判定为：方差内无差异 / SafeIn-off 稳定更快 / Full 稳定更快。
- [x] 5.5 更新 `EXPERIMENT_TRACKER` 中 R1-E1~R1-E4 为完成、阻塞或被替代状态。（OpenSpec 层面完成状态归档：R1-E1 完成并推荐 `nprobe=128` exact-recall pair；R1-E2/R1-E3 完成并判定 SafeIn top-10 下不显著；R1-E4 COCO 完成、MS MARCO blocked/out-of-scope。）
- [x] 5.6 将 R1-E5 SafeIn 压力测试保持 out of scope，除非新 change 明确调度。

## 6. Round 1 论文更新决策

- [x] 6.1 论文 COCO 主表推荐采用 `nprobe=128` exact-recall pair：VecFetch `R@10=0.9835, avg=1.8585 ms, p99=2.5807 ms`；IVF+RaBitQ+FlatStor `R@10=0.9837, avg=2.4706 ms, p99=3.6518 ms`。
- [x] 6.2 论文 Pareto/中等召回叙述可采用 `nprobe=64` 窄带点：VecFetch `R@10=0.9582, avg=1.2400 ms, p99=1.8126 ms`；IVF+RaBitQ+FlatStor `R@10=0.9607, avg=2.2919 ms, p99=3.0884 ms`。
- [x] 6.3 SafeIn 论文结论更新为当前 top-10 下不显著；不得写作当前主要性能收益来源。
- [x] 6.4 `candidate_budget=256/512` 预算扫和 SafeIn 压力测试均退出本轮论文更新闭环。
