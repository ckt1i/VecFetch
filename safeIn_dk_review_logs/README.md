# SafeIn d_k Review Logs

本目录记录 SafeIn `d_k` 静态阈值问题的 review 结论、验证分解与原始输出，便于后续接手。

## 文件说明

- `static_dk_review.md`：上一轮关于“全局静态 `safein_d_k` 是否因 query-level top-k 半径异质性而失效”的 review 结论。
- `validation_p090/`：`safein_dk_percentile=0.90` 的验证输出，包含候选级分解 CSV。
- `validation_p095/`：`safein_dk_percentile=0.95` 的验证输出。
- `validation_p097/`：`safein_dk_percentile=0.97` 的验证输出。
- `validation_report.md`：验证完成后的汇总报告。

## 口径

- 数据集：COCO100k。
- Index：`/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`。
- Base/query：`/home/zcq/VDB/data/coco_100k/image_embeddings.fvecs` 与 `query_embeddings.fvecs`。
- `nlist=2048`、`nprobe=64`、`topk=10`、`bits=4`、`queries=1000`。
- 验证工具主要使用 `bench_dk_space_compare` 的离线 S2 replay：对 nprobe 候选计算 `U_i = d_hat_i + e_i`，比较全局 `T=safein_d_k` 与 per-query oracle `R_q=exact_d_k(q)`。

