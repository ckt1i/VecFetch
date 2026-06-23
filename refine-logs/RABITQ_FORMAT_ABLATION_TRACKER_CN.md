# RaBitQ 内存/格式优化消融 Tracker

| Run ID | Milestone | Purpose | Dataset | Variant | TopK | Bits | Params | Metrics | Priority | Status | Notes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| RF-000 | M0 | official baseline 接入 smoke | amazon_esci | old / official / new | 10,100 | 4 | nprobe=64 | recall, probed, reranked, avg_ms | MUST | TODO | 先确认候选集合与 GT 一致 |
| RF-001 | M1 | ESCI 主消融 | amazon_esci | old / official / new | 10,100 | 2,3,4 | nprobe=64,128,256,512 | recall, QPS, p95/p99, Stage2 ms, RSS | MUST | TODO | 正文最关键数据集 |
| RF-002 | M1 | layout/kernel 二维消融 | amazon_esci | new-direct / new-decode-scratch / official | 10,100 | 4 | nprobe=64,256 | stage2 ms, perf, RSS | MUST | TODO | 隔离 layout 与 direct compact kernel |
| RF-003 | M2 | MSMARCO 主消融 | msmarco_passage | old / official / new | 10,100 | 2,3,4 | nprobe=64,256,512 | recall, QPS, Stage2 share, RSS | SHOULD | TODO | 主文第二数据集；解释 Amdahl |
| RF-004 | M3 | ESCI 因果闭环 | amazon_esci | old / official / new | 10,100 | 4 | selected nprobe=64,256 | paired delta, perf counters, timeline | MUST | TODO | 至少 5 次，关键点 10 次 |
| RF-005 | M3 | MSMARCO 因果闭环 | msmarco_passage | old / official / new | 10,100 | 4 | selected nprobe=64,256 | paired delta, perf counters, timeline | SHOULD | TODO | 验证 Stage2 省时为何不等于端到端省时 |
| RF-006 | M4 | COCO appendix negative | coco_100k | old / official / new | 10,100 | 2,3,4 | nprobe=64,128,256 | recall, QPS, Stage2 share | NICE | TODO | 只作边界/回归，不支撑主规模 claim |
| RF-007 | M4 | survivor density 关系图 | amazon_esci, msmarco_passage | new vs official | 10,100 | 4 | all selected nprobe | lane density, speedup, bytes touched | MUST | TODO | 用于判断是否可作为 second contribution 子机制 |
