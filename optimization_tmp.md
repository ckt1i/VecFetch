这些名称是“功能配置标签”。箭头表示每一步通常只增加或改变一个机制，以便把性能差异归因到该机制。

## 一、PCVE 与验证控制

### Verify-All

不使用 PCVE 的 SafeOut 剪枝。

所有 IVF probe 后产生、需要精确验证的候选，都读取 raw vector 并计算精确距离。

```text
候选
  → 全部读取 raw vector
  → 全部精确 rerank
```

用途：衡量“不做验证剪枝”时的 raw-vector I/O 上界。

它不是 `Fixed-R`：Verify-All 没有候选数量预算，而 Fixed-R 只验证固定数量候选。

---

### PCVE Final-Only

先完成全部 cluster probing，再使用 PCVE 区间统一过滤候选。

```text
探测完所有 clusters
  → 获得完整候选集合
  → 构造最终 top-k upper frontier
  → PCVE SafeOut
  → 读取剩余 raw vectors
```

它使用：

\[
L_i=\widehat d_i-m_i^L,\qquad
U_i=\widehat d_i+m_i^U
\]

并计算：

\[
T_k^U=\operatorname{kthmin}_i U_i.
\]

如果：

\[
L_i>T_k^U,
\]

候选不需要读取 raw vector。

用途：证明 PCVE 区间控制本身是否有效，不涉及在线 frontier 更新时间。

---

### PCVE Cluster-Epoch

每处理完一个 IVF cluster，就更新一次当前 top-k upper frontier，并尽早执行 SafeOut。

```text
probe cluster 1
  → 更新 frontier
  → 过滤当前候选
probe cluster 2
  → 再次更新 frontier
  → 过滤当前候选
...
```

与 Final-Only 的唯一区别应当是 frontier 更新和过滤发生的时间。

用途：判断在线传播是否能在 raw-vector I/O 提交前更早排除候选。

因此：

```text
Verify-All
    ↓ 增加最终 PCVE 过滤
PCVE Final-Only
    ↓ 把相同过滤提前到 cluster epoch
PCVE Cluster-Epoch
```

分别测量：

- Verify-All → Final-Only：PCVE 总体收益；
- Final-Only → Cluster-Epoch：早期 frontier 的额外收益。

---

### Fixed-R

按照估计距离排序，只选择固定数量 \(R\) 个候选读取 raw vector。

```text
候选按估计距离排序
  → 取前 R 个
  → 精确 rerank
```

它是常见的静态 rerank-budget 基线。

为了公平，应当在 validation split 上选择 \(R\)，使其 Recall@10/50 与 PCVE 接近，然后在独立 test split 比较 I/O 和性能。

用途：回答 PCVE 是否真的优于“简单选择一个固定 rerank 数量”。

---

### Legacy Fixed/Inherited Margin

使用旧的固定误差 margin，或者把完整精度的 margin 缩放/继承到其他精度。

它不根据真实部署的 stage、active bits 和 query-level 最大误差重新校准。

用途：证明旧 margin 为什么不足。它通常作为弱基线或失败案例，不应作为生产配置。

---

### Pair-P99

把所有“查询—候选”误差混合在一起，取候选对级别的 99% 分位数。

它控制的是：

> 随机选取一个候选对时，误差超过阈值的概率。

它不控制：

> 一次查询的全部候选中，是否至少有一个候选越界。

用途：证明 pair-level 分位数不能代替 query-level PCVE。

---

### Symmetric PCVE

上下两个方向使用相同的误差宽度：

\[
[\widehat d-m,\widehat d+m].
\]

当前主配置采用 4-bit symmetric PCVE。

---

### A-PCVE

Asymmetric PCVE，即分别校准 lower-side 和 upper-side error：

\[
[\widehat d-m^L,\widehat d+m^U].
\]

它允许两个方向使用不同宽度。

当前定位：

- 在 3-bit 下作为同位宽优化；
- 不作为 4-bit 主配置；
- 用于内存—性能消融。

## 二、精度配置名称

### `a4r4`

```text
active bits = 4
resident bits = 4
```

查询使用 4-bit estimator，内存中也驻留全部 4-bit 数据。

这是当前主配置。

---

### `a3r4`

```text
active bits = 3
resident bits = 4
```

内存中保留 4 bits，但本次查询只使用其中 3 bits。

与 `a4r4` 对比，隔离 active precision 的影响。

---

### `a3r3`

```text
active bits = 3
resident bits = 3
```

查询和驻留都只需要 3 bits。

与 `a3r4` 对比，隔离 resident precision 和内存节省；两者的查询计算语义应保持一致。

## 三、record format

### Split / NoCombine

raw vector 与 payload 分开存储。

```text
vector store:  [raw vector]
payload store: [payload]
```

读取 vector span 不会自然覆盖 payload。

用途：传统分离布局基线。

---

### Combined

raw vector 和 payload 位于同一 record。

```text
[raw vector | descriptor | payload]
```

但 `Combined` 只说明共置，不说明是否对大 payload 做大小自适应处理。

---

### All-Inline

所有 payload 都完整放入 hot record：

```text
[raw vector | descriptor | complete payload]
```

优点是 payload cofetch 机会最大。

缺点是：

- record 可能很大；
- 相邻 vector 距离增大；
- span 读放大可能增加；
- hot-store footprint 和 retained buffer 增大。

用途：作为“最大程度共置”的上界或负面对照。

---

### Size-Adaptive / Adaptive

根据 payload 大小选择三种格式：

```text
小 payload:
[vector | descriptor | complete payload]

中等 payload:
[vector | descriptor | inline prefix] + external suffix

大 payload:
[vector | descriptor] + external payload
```

用途：在 payload reuse 与 hot footprint/read amplification 之间取得平衡。

---

### Inline

payload 完整存储在 vector 后面。

只要 span 完整覆盖对应 payload，就可以建立 reusable payload view。

---

### Prefix + External

只把 payload 前缀放入 hot record，其余部分外置。

适用于 payload 较大、但希望 span 至少提前覆盖一部分数据的情况。

---

### External / Cold Payload

payload body 位于独立 cold store。

vector span 即使读取 raw vector，也不会自动读取 external payload；最终需要时再单独读取。

## 四、span planner

### NoSpan

每个 mandatory raw vector 使用独立物理读取。

```text
vector A → read A
vector B → read B
vector C → read C
```

用途：span coalescing 的直接基线。

它仍然可以使用 async I/O；`NoSpan` 不等于同步执行。

---

### GV

Greedy Vector-only span planner。

候选按物理 offset 排序，在读放大限制内，从前向后贪心扩展 span：

```text
vector A + gap + vector B + gap + vector C
              ↓
       one continuous read
```

其约束近似为：

\[
B(i,j)\le \alpha V(i,j),
\]

其中：

- \(B(i,j)\)：连续 span 实际读取字节；
- \(V(i,j)\)：成员 raw vectors 的必要字节；
- \(\alpha\)：最大读放大倍率。

GV 的正确包装是：

> 线性单遍、低规划开销、保证输出满足放大约束的部署策略。

不能称为最优或近似最优算法。

---

### GE

Greedy/Global Exact oracle，指使用精确 DP 或等价 exact planner，求当前 span 模型下的最优分组。

它通常首先最小化请求数，再在请求数相同时最小化读取字节。

用途：

- 定义模型最优解；
- 测量 GV 距离最优解还有多大差距；
- 判断更复杂 planner 是否值得。

当前不作为生产默认配置。

## 五、payload reuse

### ReuseOff

span 仍然合并 raw-vector reads，但即使 span buffer 已覆盖 inline payload，也不将其用于最终 materialization。

```text
span 读取 vector + payload bytes
  → vector 用于 rerank
  → payload coverage 被忽略
  → final top-k 仍重新读 payload
```

用途：隔离“请求合并”本身的收益。

---

### ReuseOn

completion 检查 span 是否完整覆盖 descriptor 和 inline payload。若覆盖，则建立 payload view。

最终 top-k 如果需要该 payload：

```text
直接复用 span buffer 中的 payload
```

否则：

```text
执行 missing payload read
```

用途：隔离 payload reuse 的增量收益。

关键对比：

```text
GV + Async + ReuseOff
          ↓ 只打开 payload view/reuse
GV + Async + ReuseOn
```

只有以下指标才能证明 reuse 有效：

- consumed reuse hits/bytes；
- avoided final payload requests/bytes；
- missing payload reads 下降。

`covered bytes` 本身只能证明机会存在。

## 六、pipeline 执行模式

### Async / Async Overlap

在 cluster probing 过程中，已形成的 span 可以提交到异步读取队列；系统不等待所有 I/O 完成，而是继续 probe 后续 clusters。

```text
probe cluster t
  → form/submit spans
  → probe cluster t+1
       ↕
  poll completed spans
  → exact rerank
```

用途：同时利用：

- I/O batching；
- 多请求并行；
- probe computation 与 I/O overlap；
- completion-driven rerank。

---

### NoOverlapAsyncFinal

先完成全部 cluster probing，然后才提交 raw-vector/span reads。

但仍保留：

- 相同 GV groups；
- 相同 `io_uring`；
- 相同 batching；
- 相同 queue depth；
- 相同 completion mapping；
- 相同 payload reuse 和 final fetch。

```text
probe all clusters
  → form/submit spans
  → async batch reads
  → rerank/materialize
```

用途：公平隔离“probe 与 I/O overlap”。

因此：

```text
GV + Async + ReuseOn
        vs
GV + NoOverlapAsyncFinal + ReuseOn
```

唯一主要差异应该是 I/O 是否与后续 probing 重叠。

---

### SerialNoOverlap

完成 probing 后，以同步或近似逐请求方式读取向量和 payload。

它同时失去：

- probe/I/O overlap；
- async batching；
- 多请求并行；
- 部分 completion-driven 执行优势。

用途：诊断同步读取的总代价。

它不能单独用于证明 overlap，因为与 Async 相比改变的因素太多。

---

### NoPipeline / NewNoPipeline

历史诊断配置，通常同时关闭或改变：

- overlap；
- async batching；
- span reuse；
- SafeIn/tail；
- final payload execution。

因此它只能说明“完整读取路径很重要”，不能把全部性能差异归因于 pipeline overlap。

## 七、整套功能阶梯如何解释

当前 C3 的推荐阶梯是：

```text
NoSpan + Async
    ↓ 打开 GV span
GV + Async + ReuseOff
    ↓ 打开 payload reuse
GV + Async + ReuseOn
    ↓ 关闭 probe/I/O overlap，但保留 async batching
GV + NoOverlapAsyncFinal + ReuseOn
    ↓ 换成同步逐请求执行
GV + SerialNoOverlap + ReuseOn
```

分别回答：

| 对比 | 隔离的功能 |
|---|---|
| NoSpan → GV ReuseOff | span coalescing |
| ReuseOff → ReuseOn | payload reuse |
| Async → NoOverlapAsyncFinal | probe/I/O overlap |
| NoOverlapAsyncFinal → Serial | async batching/并行读取 |

而格式侧采用：

```text
Split
  vs
Size-Adaptive
  vs
All-Inline
```

回答“payload 应该如何与 raw vector 组织”。

因此三条贡献的关系是：

```text
C1：哪些候选需要读？
C2：需要读取的数据如何存？
C3：这些读取如何合并、执行和复用？
```