# SafeIn d_k Review Logs

本目录记录 SafeIn `d_k` 静态阈值问题的 review 结论、验证分解与原始输出，便于后续接手。

## 文件说明

- `static_dk_review.md`：上一轮关于“全局静态 `safein_d_k` 是否因 query-level top-k 半径异质性而失效”的 review 结论。
- `validation_p090/`：`safein_dk_percentile=0.90` 的验证输出，包含候选级分解 CSV。
- `validation_p095/`：`safein_dk_percentile=0.95` 的验证输出。
- `validation_p097/`：`safein_dk_percentile=0.97` 的验证输出。
- `validation_report.md`：验证完成后的汇总报告。
- `dynamic_dk_design.md`：historical 查询期间动态 SafeIn `d_k` / prefetch 探索设计，包含已删除的 lower/upper frontier、stable/delay、payload-only 等候选方案。
- `run_dynamic_safein_experiments.py`：COCO100k 当前受支持 `static/frontier` Dynamic SafeIn 实验脚本；旧 exploratory modes 已不再可运行。
- `dynamic_prefetch_smoke/`：动态 SafeIn 小规模 smoke run 输出。
- `dynamic_prefetch_runs/`：完整 COCO100k 实验输出，包含每个 scheme 的 `cmd.txt`、`run.log`、`results.json`、`online_per_query.csv`，以及 `summary.csv/json`。
- `dynamic_experiment_report.md`：historical 动态方案实验结果，记录已删除 exploratory modes。
- `aggressive_dynamic_safein_design.md`：当前正式 `frontier + defer4`、lower frontier 阈值、deferred flush 重判与 prefetch 统计口径的算法设计说明。
- `run_aggressive_safein_experiments.py`：当前受支持 `static/frontier` SafeIn COCO100k topk=10/50 实验矩阵脚本。
- `aggressive_dynamic_prefetch_runs/`：historical COCO100k topk=10/50 完整实验输出，可能包含已删除 mode 名称。
- `aggressive_e2e_coco/`：本轮 COCO100k `bench_e2e` 端到端读取对比输出。
- `msmarco_passage_adapter_for_aggressive_safein/`：为 MSMARCO 验证生成的 bench adapter，复用 formal baseline embedding 与外部 GT。
- `aggressive_vector_msmarco_200_skip_false/`：MSMARCO 200-query 大规模 index 验证输出，使用 `--skip-false-stats 1` 避免恢复 8.8M cluster members。
- `aggressive_dynamic_experiment_report.md`：historical aggressive 方案、COCO 达标结果、e2e 对比、MSMARCO 验证限制与推荐迁移依据。
- `blend0_check/`：补跑旧 `frontier_blend --dynamic-safein-scale 0.0` 的 COCO100k 与 MSMARCO 结果；该语义现在迁移为 `--dynamic-safein frontier`。
- `quality_gate_check/`：对持续 candidate pool 与质量感知 early-start gate 的审查、historical gap gate 实验和最终结论。
- `frontier_apply_validation/`：OpenSpec apply 后 `static/frontier` smoke 验证输出。

## 口径

- 数据集：COCO100k。
- Index：`/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`。
- Base/query：`/home/zcq/VDB/data/coco_100k/image_embeddings.fvecs` 与 `query_embeddings.fvecs`。
- `nlist=2048`、`nprobe=64`、`topk=10`、`bits=4`、`queries=1000`。
- 验证工具主要使用 `bench_dk_space_compare` 的离线 S2 replay：对 nprobe 候选计算 `U_i = d_hat_i + e_i`，比较全局 `T=safein_d_k` 与 per-query oracle `R_q=exact_d_k(q)`。
- 当前 Dynamic SafeIn 正式实现主要使用 `bench_vector_search` 和 `bench_e2e` 的生产 `OverlapScheduler` 路径：`--dynamic-safein frontier` 在查询期间维护 lower-bound top-k frontier，并用 `T_q=F_lower` 做 SafeIn classification / prefetch threshold。
- 当前推荐命令为 `--dynamic-safein frontier --dynamic-safein-stable-probes 1 --dynamic-safein-defer-initial-clusters 4 --dynamic-safein-defer-until-ready 1`。旧 `frontier_blend`、scale、gap 和 payload-only flags 已删除。
