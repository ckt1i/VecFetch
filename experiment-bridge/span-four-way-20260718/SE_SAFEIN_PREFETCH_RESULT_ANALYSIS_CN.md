# SE 与传统 SafeIn Prefetch 消融结果

日期：2026-07-18

> **已被严格 NoReuse 重跑替代。** 本实验两侧均开启
> `vec_span_payload_reuse=1`，因此比较的是 eager SafeIn 与 SE span cofetch 的组合
> 差异，不是此前 SafeIn 口径下的严格 prefetch on/off。论文不得继续引用本文件的
> 数值；应改用 `SAFEIN_PREFETCH_NOREUSE_RESULT_ANALYSIS_CN.md`。

## 结论

在 `SE,rho=0.1,alpha=3/2,tail=0,topk=100,nprobe=96` 下，真正启用 eager
SafeIn full-record prefetch 后，四个数据集的 QPS paired median 均低于关闭该路径、
仅保留 SE span cofetch 的配置。因此当前实现不支持“独立 SafeIn prefetch 是 SE
的有效补充”；更合理的默认是关闭 eager prefetch，以 span cofetch 作为唯一的
SafeIn-aware 读取机制。

## 五次同-rep配对

下表为 `SafeInPrefetch / NoSafeInPrefetch - 1`；QPS 正值更好。

| 数据集 | QPS median | QPS mean±std | 正向 rep | p99 delta | vector request delta | total byte delta |
|---|---:|---:|---:|---:|---:|---:|
| ESCI | -2.36% | -2.31%±0.14% | 0/5 | +1.15% | +39.31% | -1.42% |
| COCO | -0.81% | -0.43%±0.56% | 2/5 | +0.73% | +6.52% | +2.68% |
| MSMARCO | -1.10% | -1.12%±0.21% | 0/5 | +1.88% | +14.91% | +0.69% |
| VoxCeleb2 | -1.85% | -2.15%±1.01% | 0/5 | -4.73% | 0.00% | +12.50% |

这里的 vector request 包含 VEC_ONLY/VEC_SPAN 与 VEC_ALL。开启 eager prefetch
会把 SafeIn 候选从可合并的 VEC_ONLY span 中移出：ESCI/MSM/COCO 分别增加约
156/122/73 个 VEC_ALL 请求/query；Vox 增加约 60 个/query。相应地，开启 eager
prefetch 时 planner credit 为 0，关闭时 ESCI/MSM/COCO 分别约为
79.7/36.8/60.6 KiB/query。

## 最低 NoSafeIn 固定基线口径

按用户指定的第二口径，对每个数据集取五次 NoSafeInPrefetch 中最低 QPS，再与
全部 SafeInPrefetch repetition 比较：

| 数据集 | 最低 NoSafeIn QPS | SafeIn QPS median delta | mean delta | 正向 rep |
|---|---:|---:|---:|---:|
| ESCI | 250.73 | -2.15% | -2.13% | 0/5 |
| COCO | 359.26 | +0.33% | +0.33% | 5/5 |
| MSMARCO | 206.09 | -0.85% | -0.83% | 0/5 |
| VoxCeleb2 | 201.83 | -1.21% | -1.41% | 0/5 |

最低基线仍只有 COCO 一个正向数据集，没有达到“同一 nprobe 至少两个数据集有效”
的目标。该口径只作补充展示，不覆盖正式配对结论。

## 语义审计

- `NoSafeInPrefetch`：`materialization_mode=late`、`safein_as_vec_only=true`、
  `avg_all_read_requests=0`。
- `SafeInPrefetch`：`materialization_mode=eager`、`safein_as_vec_only=false`，四个
  数据集均有非零 VEC_ALL 请求。
- 所有 paired cells 的 recall、probed、reranked 与 unique-fetch coverage 一致；
  planner fallback 为 0。

原始结果：

- `/home/zcq/VDB/test/recordgate_span_se_safein_prefetch_ablation_20260718`
- `/home/zcq/VDB/test/recordgate_span_se_safein_prefetch_eager_smoke_20260718`
