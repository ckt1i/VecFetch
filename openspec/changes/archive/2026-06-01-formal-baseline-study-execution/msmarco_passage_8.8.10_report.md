# 8.8.10 MS MARCO Passage 结果报告（去重后）

## 去重说明
- 本次针对 `msmarco_passage_topk10/50/100_summary.csv` 按字段去重：
  - `dataset, system, backend, variant, param, topk, candidate_budget, protocol`
- 去重规则：同组重复保留 `e2e_ms` 最小的一条（更优延迟），其余版本丢弃为历史重复。
- 去重结果：
  - `msmarco_passage_topk10_summary.csv`：`33 -> 32`
  - `msmarco_passage_topk50_summary.csv`：`28 -> 14`
  - `msmarco_passage_topk100_summary.csv`：`21 -> 14`
- 说明：`topk=10` 仍保留 32 条，是因为存在多个协议版本（`corrected_rebaseline_topk10`、`warm_coupled`、`warm_coupled_a1`、`warm_coupled_a2`），其中 `corrected` 与 `a1/a2` 仅 2 行每协议各一套系统组合，不属于 `warm_coupled` 主实验重复。

## `msmarco_passage × warm_coupled` 主实验汇总（`topk=10/50/100`）

### Top-k = 10（主协议）
- `faiss_ivfpq_refine`：`nprobe` 增大时 recall 单调上升且 e2e 变慢；  
  - 最快点：`nprobe=32, recall@10=0.6843, e2e_ms=28.2119`  
  - 最高召回：`nprobe=1024, recall@10=0.7412, e2e_ms=72.5161`
- `ivf_rabitq_rerank`：`nprobe` 提升带来更高 recall，但到 1024 时代价增加；  
  - 最快点：`nprobe=64, recall@10=0.8935, e2e_ms=30.8343`  
  - 最高召回：`nprobe=1024, recall@10=0.9895, e2e_ms=43.6137`

### Top-k = 50（主协议）
- `faiss_ivfpq_refine`：  
  - 最快点：`nprobe=16, recall@10=0.6773, e2e_ms=29.8544`  
  - 最高召回：`nprobe=1024, recall@10=0.8167, e2e_ms=56.8759`
- `ivf_rabitq_rerank`：  
  - 最快点：`nprobe=16, recall@10=0.7668, e2e_ms=27.9579`  
  - 最高召回：`nprobe=1024, recall@10=0.9895, e2e_ms=38.5482`

### Top-k = 100（主协议）
- `faiss_ivfpq_refine`：  
  - 最快点：`nprobe=32, recall@10=0.7625, e2e_ms=30.3761`  
  - 最高召回：`nprobe=1024, recall@10=0.8659, e2e_ms=58.8266`
- `ivf_rabitq_rerank`：  
  - 最快点：`nprobe=16, recall@10=0.7668, e2e_ms=29.7037`  
  - 最高召回：`nprobe=1024, recall@10=0.9895, e2e_ms=40.7046`

## 结论（按当前参数与主协议）
1. 去重后文件不再出现重复 `key` 行，主协议（`warm_coupled`）每个 `(system,nprobe)` 组合仅保留一条可复用结果。  
2. 在相同召回层级下，`ivf_rabitq_rerank` 在 `msmarco_passage` 上仍显著优于 `faiss_ivfpq_refine`，且速度优势在 topk=50/100 尤为明显；topk=10 时二者最小延迟均接近。  
3. 下一步可直接使用本报告作为 `8.8.10` 的最终交付文本，并继续推进 8.12/8.13 选择与复测链路。
