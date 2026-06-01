可以。你现在的目标可以完整改写成一个 **FDP-UCB SafeIn 阈值标定方案**。它的核心不是控制“出现错误 SafeIn 的概率”，而是控制：

可以。你这里的“正则化”建议不要理解成训练模型里的 L2 regularization，而是：

> 对 SafeIn 的排序分数做一个额外惩罚项，把经验上更容易误划入的候选往后排，从而在相同 (\\beta) 约束下选入更多可靠 SafeIn。

你现在排序用的是：

[
U\_i=\\hat d\_i+e\_i
]

然后按 (U\_i) 从小到大选前缀。正则化后改成按：

[
V\_i = \\frac{U\_i}{s(q\_i)}+\\Omega\_i
]

排序。其中：

* (\\frac{U\_i}{s(q\_i)})：归一化后的距离上界；
* (\\Omega\_i)：正则化惩罚项；
* (V\_i)：最终用于标定和查询的 SafeIn score。

在线规则变成：

[
V\_i < \\tau\_{\\text{calibrated}}
\\Rightarrow \\mathrm{SafeIn}
]

而不是直接用：

[
U\_i<T
]

---

# 1. 最推荐的正则化形式：rank penalty

这是最接近 RAPS 思想的做法。RAPS 在分类 conformal prediction 中会对超过某个排名后的标签加入额外惩罚，用来避免预测集合过大；它的 score 形式中包含类似

[
\\lambda(k-k\_{\\mathrm{reg}})^+
]

的排名惩罚项。RAPS 的目标就是通过额外正则项减少过大的 prediction set，并且 (\\lambda)、(k\_{\\mathrm{reg}}) 这类超参数通常通过额外数据划分调节。([Mapie](https://mapie.readthedocs.io/en/latest/theoretical_description_classification.html "Theoretical Description Classification : contents — MAPIE 1.4.1.dev10+g106b4dfd8 documentation"))

迁移到你的 SafeIn 场景，可以定义：

# [

V\_i

\\frac{U\_i}{s(q\_i)}
+
\\lambda\\cdot (r\_i-r\_{\\mathrm{reg}})^+
]

其中：

* (U\_i=\\hat d\_i+e\_i)；
* (s(q\_i))：query 级归一化尺度；
* (r\_i)：candidate 在当前 query 内的 rank，比如按 (U\_i) 从小到大排序后的名次；
* (r\_{\\mathrm{reg}})：不惩罚的前多少个候选；
* ((x)^+=\\max(x,0))；
* (\\lambda)：惩罚强度。

直观上：

* 每个 query 中排名靠前的候选，不加惩罚；
* 排名越靠后的候选，越可能是 false SafeIn，因此分数变大；
* 排序时这些候选会往后移；
* 在相同 (\\beta) 下，前缀中 true candidate 的比例更高，SafeIn 数量可能增加。

---

# 2. Calibration 怎么做？

原来你是：

[
U\_i<T
\\Rightarrow \\mathrm{SafeIn}
]

现在改成：

[
V\_i(\\lambda,r\_{\\mathrm{reg}})<\\tau
\\Rightarrow \\mathrm{SafeIn}
]

完整流程是：

1. 收集 calibration records：

[
(U\_i,\\ s(q\_i),\\ r\_i,\\ is\_false\_i)
]

2. 枚举正则化参数：

[
\\lambda\\in{0,0.001,0.005,0.01,0.05,0.1}
]

[
r\_{\\mathrm{reg}}\\in{k,2k,4k,8k}
]

3. 对每组 ((\\lambda,r\_{\\mathrm{reg}}))，计算：

[
V\_i=
\\frac{U\_i}{s(q\_i)}
+
\\lambda(r\_i-r\_{\\mathrm{reg}})^+
]

4. 按 (V\_i) 从小到大排序；
5. 对前 (m) 个 candidate 计算：

[
S\_m=m
]

[
F\_m=\\sum\_{j=1}^m is\_false\_{(j)}
]

[
R\_m=F\_m/S\_m
]

6. 选择最大前缀：

# [

m^\*(\\lambda,r\_{\\mathrm{reg}})

\\arg\\max\_m S\_m
\\quad
\\text{s.t.}
\\quad
R\_m\\le \\beta\_{\\mathrm{calib}}
]

7. 得到该参数下的阈值：

[
\\tau(\\lambda,r\_{\\mathrm{reg}})=V\_{(m^\*)}
]

8. 在 held-out validation 上评估：

# [

\\mathrm{FDP}\_{valid}

F\_{valid}/S\_{valid}
]

9. 最终选择满足：

[
\\mathrm{FDP}\_{valid}\\le \\beta
]

且 SafeIn 数量最大的参数组合：

[
(\\lambda^*,r\_{\\mathrm{reg}}^*,\\tau^\*)
]

---

# 3. 在线查询怎么做？

正式查询时，对每个候选计算：

[
U\_i=\\hat d\_i+e\_i
]

[
s(q)=\\text{当前 query 的尺度}
]

[
r\_i=\\text{该候选在当前 query 内按 }U\_i\\text{ 排序的 rank}
]

然后：

[
V\_i=
\\frac{U\_i}{s(q)}
+
\\lambda^*(r\_i-r\_{\\mathrm{reg}}^*)^+
]

判断：

[
V\_i<\\tau^\*
\\Rightarrow \\mathrm{SafeIn}
]

否则进入 Uncertain 或 Stage2。

代码上就是：

```python
U = d_hat + e
V = U / scale + lambda_reg * max(rank - r_reg, 0)

if V < tau_calibrated:
    SafeIn
else:
    Uncertain_or_Stage2
```

---

# 4. Python 标定代码

```python
import numpy as np

def compute_regularized_score(U, scale, rank, lambda_reg, r_reg, eps=1e-12):
    """
    U: conservative upper bound, U = d_hat + e
    scale: query-level scale s(q)
    rank: within-query rank, starting from 1
    """
    return U / np.maximum(scale, eps) + lambda_reg * np.maximum(rank - r_reg, 0)


def prefix_calibrate(V, is_false, beta_calib):
    """
    Given regularized score V and false labels,
    find the largest prefix satisfying empirical FDP <= beta_calib.
    """
    order = np.argsort(V)
    V_sorted = V[order]
    y_sorted = is_false[order].astype(int)

    cum_false = np.cumsum(y_sorted)
    best_idx = None

    for idx in range(len(V_sorted)):
        S = idx + 1
        F = cum_false[idx]
        R = F / S

        if R <= beta_calib:
            best_idx = idx

    if best_idx is None:
        return None

    if best_idx + 1 < len(V_sorted):
        tau = 0.5 * (V_sorted[best_idx] + V_sorted[best_idx + 1])
    else:
        tau = np.nextafter(V_sorted[best_idx], np.inf)

    mask = V < tau
    return {
        "tau": float(tau),
        "S": int(mask.sum()),
        "F": int(is_false[mask].sum()),
        "fdp": float(is_false[mask].sum() / max(mask.sum(), 1)),
    }


def evaluate_threshold(V, is_false, tau):
    mask = V < tau
    S = int(mask.sum())
    F = int(is_false[mask].sum())
    return {
        "S": S,
        "F": F,
        "fdp": float(F / max(S, 1)),
    }


def tune_regularized_safein(
    train_records,
    valid_records,
    beta_target,
    beta_calib_grid=None,
    lambda_grid=None,
    r_reg_grid=None,
):
    """
    train_records / valid_records:
        dict with fields:
        U: np.ndarray
        scale: np.ndarray
        rank: np.ndarray
        is_false: np.ndarray
    """

    if beta_calib_grid is None:
        beta_calib_grid = np.linspace(beta_target, min(0.8, beta_target + 0.4), 21)

    if lambda_grid is None:
        lambda_grid = [0.0, 0.001, 0.003, 0.005, 0.01, 0.03, 0.05, 0.1]

    if r_reg_grid is None:
        r_reg_grid = [10, 20, 40, 80, 160]

    best = None

    for beta_calib in beta_calib_grid:
        for lambda_reg in lambda_grid:
            for r_reg in r_reg_grid:
                V_train = compute_regularized_score(
                    train_records["U"],
                    train_records["scale"],
                    train_records["rank"],
                    lambda_reg,
                    r_reg,
                )

                calib_result = prefix_calibrate(
                    V_train,
                    train_records["is_false"],
                    beta_calib,
                )

                if calib_result is None:
                    continue

                tau = calib_result["tau"]

                V_valid = compute_regularized_score(
                    valid_records["U"],
                    valid_records["scale"],
                    valid_records["rank"],
                    lambda_reg,
                    r_reg,
                )

                valid_result = evaluate_threshold(
                    V_valid,
                    valid_records["is_false"],
                    tau,
                )

                if valid_result["fdp"] <= beta_target:
                    if best is None or valid_result["S"] > best["valid_S"]:
                        best = {
                            "beta_calib": float(beta_calib),
                            "lambda_reg": float(lambda_reg),
                            "r_reg": int(r_reg),
                            "tau": float(tau),
                            "train_S": calib_result["S"],
                            "train_F": calib_result["F"],
                            "train_fdp": calib_result["fdp"],
                            "valid_S": valid_result["S"],
                            "valid_F": valid_result["F"],
                            "valid_fdp": valid_result["fdp"],
                        }

    return best
```

---

# 5. 如何构造 rank？

对每个 query，你需要先对候选按 (U\_i) 排序，然后赋 rank：

```python
def assign_query_rank(records_by_query):
    """
    records_by_query:
        list of dict:
        {
            "U": np.ndarray,
            "is_false": np.ndarray,
            "scale": float,
        }

    Return flattened arrays:
        U, scale, rank, is_false
    """
    all_U = []
    all_scale = []
    all_rank = []
    all_false = []

    for rec in records_by_query:
        U = rec["U"]
        y = rec["is_false"]
        scale = rec["scale"]

        order = np.argsort(U)
        rank = np.empty_like(order)
        rank[order] = np.arange(1, len(U) + 1)

        all_U.append(U)
        all_scale.append(np.full_like(U, scale, dtype=float))
        all_rank.append(rank.astype(float))
        all_false.append(y)

    return {
        "U": np.concatenate(all_U),
        "scale": np.concatenate(all_scale),
        "rank": np.concatenate(all_rank),
        "is_false": np.concatenate(all_false).astype(int),
    }
```

---

# 6. Stage-aware 正则化版本

如果 Stage1 和 Stage2 的误差分布不同，建议加一个 stage penalty：

[
V\_i=
\\frac{U\_i}{s(q\_i)}
+
\\lambda\_r(r\_i-r\_{\\mathrm{reg}})^+
+
\\lambda\_s\\cdot \\mathbf 1[\\mathrm{stage}\_i=1]
]

或者如果 Stage2 更可靠，也可以惩罚 Stage1：

[
\\lambda\_s>0
]

如果 Stage1 更可靠，则惩罚 Stage2。

更通用地写：

[
V\_i=
\\frac{U\_i}{s(q\_i)}
+
\\lambda\_r(r\_i-r\_{\\mathrm{reg}})^+
+
b\_{\\mathrm{stage}(i)}
]

其中 (b\_1,b\_2) 是 stage bias。

代码：

```python
def compute_stage_regularized_score(
    U,
    scale,
    rank,
    stage,
    lambda_rank,
    r_reg,
    stage_bias,
    eps=1e-12,
):
    """
    stage_bias: dict, for example {1: 0.0, 2: -0.02}
    Negative bias means this stage is encouraged.
    Positive bias means this stage is penalized.
    """
    base = U / np.maximum(scale, eps)
    rank_penalty = lambda_rank * np.maximum(rank - r_reg, 0)

    bias = np.array([stage_bias[int(s)] for s in stage], dtype=float)

    return base + rank_penalty + bias
```

如果你想让 Stage2 更容易 SafeIn，可以设：

```python
stage_bias = {
    1: 0.0,
    2: -0.02,
}
```

这样 Stage2 的 (V\_i) 会稍微变小，更容易进入 SafeIn。

---

# 7. Cluster-rank 正则化

如果 false SafeIn 主要来自远距离 cluster，可以用 cluster rank：

[
V\_i=
\\frac{U\_i}{s(q\_i)}
+
\\lambda\_c(c\_i-c\_{\\mathrm{reg}})^+
]

其中 (c\_i) 是该 candidate 所属 cluster 在 IVF coarse routing 中的排名。

这个非常适合你的系统，因为 IVF 聚类 rank 本身就是一个很强的可靠性信号。

最终可以组合成：

[
V\_i=
\\frac{U\_i}{s(q\_i)}
+
\\lambda\_r(r\_i-r\_{\\mathrm{reg}})^+
+
\\lambda\_c(c\_i-c\_{\\mathrm{reg}})^+
+
b\_{\\mathrm{stage}(i)}
]

---

# 8. 什么时候正则化会让 SafeIn 更激进？

正则化看起来是在“加惩罚”，为什么反而更激进？

因为你的约束是：

[
F/S\\le \\beta
]

当前 raw (U) 排序可能是：

```text
true, true, false, true, false, true, true, ...
```

早期 false 会很快把 (F/S) 拉高，导致阈值卡住。

正则化后，如果这些 false 通常有某些特征，比如：

* rank 靠后；
* 来自远 cluster；
* 来自 Stage1 粗估计；
* 误差上界不稳定；

它们会被往后排。排序可能变成：

```text
true, true, true, true, true, false, false, ...
```

这样在同样的 (\\beta) 下，前缀能包含更多 candidate。所以正则化不是单纯变保守，而是**改善排序质量**。

---

# 9. 推荐你先跑的最小版本

你先不要一次加太多项。建议先用：

[
V\_i=
\\frac{U\_i}{s(q\_i)}
+
\\lambda(r\_i-r\_{\\mathrm{reg}})^+
]

其中：

[
s(q\_i)=\\operatorname{kth\_smallest}{U\_{ij}}\_{j\\in q\_i}
]

参数网格：

```text
lambda_reg ∈ {0, 0.001, 0.003, 0.005, 0.01, 0.03, 0.05}
r_reg ∈ {k, 2k, 4k, 8k}
beta_calib ∈ {beta, beta+0.05, beta+0.10, ..., min(beta+0.4, 0.8)}
```

最后选择：

[
\\mathrm{valid\\ FDP}\\le \\beta
]

且：

[
\\mathrm{valid\\ SafeIn}
]

最大的配置。

---

# 10. 论文里可以怎么写？

可以这样写：

> Directly sorting candidates by the conservative distance upper bound (U=\\hat d+e) may be overly restrictive, since early false positives can dominate the global prefix constraint. Inspired by regularized conformal prediction, we introduce a regularized SafeIn score:
>
> [
> V(q,c)=\\frac{U(q,c)}{s(q)}+\\lambda(r(q,c)-r\_{\\mathrm{reg}})^+.
> ]
>
> Here (s(q)) normalizes the query-specific distance scale, while the rank penalty discourages low-confidence tail candidates. The regularization parameters are selected on a held-out validation split to maximize the number of SafeIn candidates under the target FDP constraint.

中文：

> 直接基于保守距离上界 (U=\\hat d+e) 排序会受到查询尺度差异和早期误划入候选的影响，导致 SafeIn 阈值过紧。受正则化保形预测思想启发，本文引入正则化 SafeIn 分数：
>
> [
> V(q,c)=\\frac{U(q,c)}{s(q)}+\\lambda(r(q,c)-r\_{\\mathrm{reg}})^+.
> ]
>
> 其中 (s(q)) 用于消除查询间距离尺度差异，排名惩罚项用于降低低置信尾部候选被提前划入 SafeIn 的概率。正则化参数通过独立验证集选择，以在满足目标 FDP 约束的前提下最大化 SafeIn 数量。

---

最终建议：**先实现 rank-regularized normalized score**：

[
\\boxed{
V\_i=
\\frac{\\hat d\_i+e\_i}{s(q\_i)}
+
\\lambda(r\_i-r\_{\\mathrm{reg}})^+
}
]

然后仍然用你现在的前缀 FDP 校准，只是排序变量从 (U\_i) 换成 (V\_i)。这一步改动最小，也最可能解决你现在 SafeIn 阈值偏紧的问题。

[
\frac{\text{false SafeIn 数量}}{\text{SafeIn 总数量}}
]

也就是希望在正式查询时，所有被划入 SafeIn 的候选里，错误比例接近或不超过给定的 (\beta)。

---

# 1. 方法目标

给定每个候选读取单元的保守距离上界：

[
U_i=\hat d_i+e_i
]

其中：

* (\hat d_i)：候选的量化距离估计；
* (e_i)：误差补偿项；
* (U_i)：用于 SafeIn 判断的保守上界。

定义阈值 (T) 下的 SafeIn 集合：

[
A_T={i:U_i<T}
]

定义：

[
S(T)=|A_T|
]

[
F(T)=\sum_{i\in A_T}\mathbf 1[i\text{ is false}]
]

则 SafeIn 的错误比例为：

[
\mathrm{FDP}(T)=\frac{F(T)}{\max(S(T),1)}
]

你的目标是：

[
\mathrm{FDP}(T)\lesssim \beta
]

例如 (\beta=0.2) 时，希望正式查询中所有 SafeIn 候选里大约不超过 20% 是误划入。

FDP 是 false discovery proportion，FDR 则是 FDP 的期望。经典 FDR 目标就是控制“被选中结果中错误发现所占比例的期望”。([Purdue Statistics][1])

---

# 2. 为什么不用直接经验比例？

你原来的标定条件是：

[
\frac{F(T)}{S(T)}\le \beta
]

这个条件容易过于乐观。比如校准集上某个阈值只产生了：

[
S(T)=20,\quad F(T)=0
]

经验 FDP 是 0，但这不代表真实查询时错误率就是 0。样本数太少时，真实错误比例仍可能明显大于 0。

因此 FDP-UCB 的思想是：不用经验比例本身，而是用错误比例的**上置信界**：

[
\mathrm{UCB}(F(T),S(T),\delta_T)
]

然后要求：

[
\mathrm{UCB}(F(T),S(T),\delta_T)\le \beta
]

其中 UCB 是 upper confidence bound。这里可以使用 Hoeffding 不等式或 Clopper-Pearson 二项比例上界。Hoeffding 不等式为有界随机变量均值提供尾界；Clopper-Pearson 方法是二项分布比例的精确置信区间。([JSTOR][2])

---

# 3. 推荐使用的 UCB 形式

对于你的任务，我更推荐 **Clopper-Pearson / Beta-UCB**，因为你的问题天然是二项比例估计：

[
F(T)\sim \mathrm{Binomial}(S(T),p_T)
]

其中 (p_T) 是阈值 (T) 下真实的 SafeIn 错误比例。

单侧上置信界为：

[
\mathrm {UCB}_ {\mathrm{CP}}(F,S,\delta)
========================================

\mathrm{BetaInv}(1-\delta;F+1,S-F)
]

其中 (\mathrm{BetaInv}) 是 Beta 分布分位数函数。

如果你更想简单实现，也可以用 Hoeffding-UCB：

[
\mathrm {UCB}_ {\mathrm{Hoeffding}}
===================================

\frac{F}{S}
+
\sqrt{
\frac{\log(1/\delta)}{2S}
}
]

但 Hoeffding 通常会更保守。

---

# 4. 标定阶段完整流程

校准阶段必须模拟正式查询中的完整两阶段流程。也就是说，校准时生成的 (U_i) 必须和正式查询时使用的 (U_i) 来源一致。

对每个 calibration query (q)：

1. 用完整向量 exact search 得到真实 top-(k)；
2. 运行和正式查询一致的聚类搜索、Stage1、Stage2；
3. 对每个可能被判定为 SafeIn 的候选单元 (i)，记录：

[
U_i=\hat d_i+e_i
]

4. 根据完整向量 top-(k) 标注：

[
\mathrm{is_false}_i=
\begin{cases}
1,& i\text{ 是错误 SafeIn 候选}
0,& i\text{ 是正确 SafeIn 候选}
\end{cases}
]

然后把所有 calibration records 合并：

[
\mathcal D_{\mathrm{cal}}={(U_i,\mathrm{is_false}*i)}*{i=1}^{N_{\mathrm{cal}}}
]

按 (U_i) 从小到大排序：

[
U_{(1)}\le U_{(2)}\le\cdots\le U_{(N)}
]

对每个前缀 (m)：

[
S_m=m
]

[
F_m=\sum_{j=1}^{m}\mathrm{is_false}_{(j)}
]

计算：

[
\mathrm{UCB}_m=
\mathrm{UCB}(F_m,S_m,\delta_m)
]

选择：

[
m^*
===

\arg\max_m S_m
\quad
\text{s.t.}
\quad
\mathrm{UCB}_m\le \beta
]

最后设置：

[
T_{\mathrm{calibrated}}=U_{(m^*)}
]

如果在线判断使用严格小于：

[
U_i<T_{\mathrm{calibrated}}
]

那么为了包含第 (m^*) 个边界样本，代码里可以使用：

```python
T_calibrated = np.nextafter(U_sorted[m_star], np.inf)
```

或者在线直接使用：

```python
U_i <= T_calibrated
```

两者选一个即可，论文中写成 (U_i<T) 更简洁。

---

# 5. 多阈值搜索时的 (\delta) 修正

因为你不是只测试一个阈值，而是在很多候选阈值中选择最好的一个，所以需要对置信水平做修正。

设候选阈值数量为 (J)，总失败概率为 (\delta)，则可以用 Bonferroni 修正：

[
\delta_m=\frac{\delta}{J}
]

这样可以减少由于扫描大量阈值导致的偶然乐观。

---

# 6. 查询阶段判断规则

正式查询时，流程非常简单。

对每个候选单元 (i)：

[
U_i=\hat d_i+e_i
]

如果：

[
U_i<T_{\mathrm{calibrated}}
]

则：

[
i\in\mathrm{SafeIn}
]

否则进入：

[
\mathrm{Uncertain}
]

或者继续 Stage2。

也就是说，在线阶段不需要重新计算 FDP，也不需要重新做统计，只需要使用离线标定好的 (T_{\mathrm{calibrated}})。

---

# 7. 两阶段 SafeIn 的推荐实现

如果你的两阶段流程是：

* Stage1：1-bit 估计；
* Stage2：multi-bit 复估；
* 最终 SafeIn 来自 Stage1 和 Stage2 的合并结果。

那么标定时最好直接针对**最终 SafeIn 集合**做标定。

对于每个候选 (i)，定义最终用于 SafeIn 判断的上界：

[
U_i^ {\mathrm{final}}
=====================

\begin{cases}
U_i^{(1)},& \text{若 Stage1 已可判断}
U_i^{(2)},& \text{若进入 Stage2 后判断}
\end{cases}
]

然后统一记录：

[
(U_i^{\mathrm{final}},\mathrm{is_false}_i)
]

再做 FDP-UCB 标定。

如果 Stage1 和 Stage2 共用一个阈值，那么最终规则就是：

[
U_i^{\mathrm{final}}<T_{\mathrm{calibrated}}
]

如果 Stage1 和 Stage2 想使用两个阈值 (T_1,T_2)，也可以做二维搜索，但论文和实现都会复杂很多。我建议当前版本先使用一个统一阈值 (T)，更容易解释，也更稳定。

---

# 8. Python 标定代码

```python
import numpy as np
from scipy.stats import beta as beta_dist


def clopper_pearson_ucb(F: int, S: int, delta: float) -> float:
    """
    One-sided Clopper-Pearson upper confidence bound
    for a binomial false rate.
    """
    if S <= 0:
        return 1.0

    if F >= S:
        return 1.0

    return float(beta_dist.ppf(1.0 - delta, F + 1, S - F))


def calibrate_safein_fdp_ucb(
    U: np.ndarray,
    is_false: np.ndarray,
    beta_target: float,
    delta: float = 0.05,
    use_unique_thresholds: bool = True,
):
    """
    Calibrate SafeIn threshold using FDP-UCB.

    Args:
        U:
            Conservative distance upper bounds.
            U_i = d_hat_i + e_i.

        is_false:
            Binary labels.
            1 means false SafeIn candidate;
            0 means true SafeIn candidate.

        beta_target:
            Target false/SafeIn ratio, e.g. 0.2.

        delta:
            Overall confidence failure probability.

        use_unique_thresholds:
            If True, evaluate only unique U thresholds.

    Returns:
        T_calibrated, info
    """

    U = np.asarray(U, dtype=float)
    is_false = np.asarray(is_false, dtype=int)

    assert U.shape == is_false.shape
    assert 0.0 < beta_target < 1.0
    assert 0.0 < delta < 1.0

    order = np.argsort(U)
    U_sorted = U[order]
    false_sorted = is_false[order]

    cum_false = np.cumsum(false_sorted)
    N = len(U_sorted)

    if N == 0:
        return None, {"message": "No calibration records."}

    if use_unique_thresholds:
        # Evaluate thresholds at the last index of each unique U value.
        unique_values, last_indices = np.unique(U_sorted, return_index=True)
        # np.unique returns first index; compute last index manually.
        last_indices = []
        start = 0
        while start < N:
            end = start
            while end + 1 < N and U_sorted[end + 1] == U_sorted[start]:
                end += 1
            last_indices.append(end)
            start = end + 1
        candidate_indices = np.array(last_indices, dtype=int)
    else:
        candidate_indices = np.arange(N)

    J = len(candidate_indices)
    delta_each = delta / max(J, 1)

    best_idx = None
    best_info = None

    for idx in candidate_indices:
        S_m = idx + 1
        F_m = int(cum_false[idx])

        emp_fdp = F_m / S_m
        ucb_fdp = clopper_pearson_ucb(F_m, S_m, delta_each)

        if ucb_fdp <= beta_target:
            best_idx = idx
            best_info = {
                "S": S_m,
                "F": F_m,
                "empirical_fdp": emp_fdp,
                "ucb_fdp": ucb_fdp,
                "beta_target": beta_target,
                "delta": delta,
                "delta_each": delta_each,
                "threshold_raw": float(U_sorted[idx]),
            }

    if best_idx is None:
        return None, {
            "message": "No threshold satisfies FDP-UCB constraint.",
            "beta_target": beta_target,
            "delta": delta,
            "num_records": N,
        }

    # If online rule is U < T, move threshold slightly above boundary.
    T_calibrated = float(np.nextafter(U_sorted[best_idx], np.inf))
    best_info["threshold"] = T_calibrated

    return T_calibrated, best_info
```

---

# 9. 在线查询代码

```python
def safein_decision(d_hat: float, error_bound: float, T_calibrated: float):
    """
    Online SafeIn decision.
    """
    U = d_hat + error_bound

    if U < T_calibrated:
        return "SafeIn"
    else:
        return "Uncertain"
```

如果是批量候选：

```python
def batch_safein_decision(d_hat, error_bound, T_calibrated):
    """
    Vectorized SafeIn decision.

    Args:
        d_hat: np.ndarray
        error_bound: np.ndarray
        T_calibrated: float

    Returns:
        safein_mask: np.ndarray[bool]
    """
    U = d_hat + error_bound
    safein_mask = U < T_calibrated
    return safein_mask
```

---

# 10. 方法章节可以这样写

你可以把这一节命名为：

> FDP-UCB-based SafeIn Calibration

中文可以叫：

> 基于 FDP-UCB 的 SafeIn 阈值标定

建议写法如下。

---

## 基于 FDP-UCB 的 SafeIn 阈值标定

本文的 SafeIn 判定旨在提前读取高置信候选，从而隐藏后续精排阶段的 I/O 开销。然而，过于激进的 SafeIn 判定会导致大量无效读取。因此，本文将 SafeIn 阈值标定建模为对错误发现比例的控制问题。给定候选读取单元 (i)，其量化距离估计为 (\hat d_i)，误差补偿项为 (e_i)，本文定义其保守距离上界为：

[
U_i=\hat d_i+e_i.
]

给定阈值 (T)，SafeIn 集合定义为：

[
A_T={i:U_i<T}.
]

设 (S(T)=|A_T|) 表示被划入 SafeIn 的候选数量，(F(T)) 表示其中实际不属于有效候选的数量，则 SafeIn 的错误发现比例为：

[
\mathrm{FDP}(T)=\frac{F(T)}{\max(S(T),1)}.
]

本文希望通过校准选择阈值 (T)，使得正式查询中 SafeIn 集合的错误比例接近或不超过给定目标 (\beta)。由于校准集有限，直接使用经验比例 (F(T)/S(T)) 容易产生过于乐观的阈值。为此，本文采用错误比例的单侧上置信界进行阈值选择。具体而言，对于每个候选阈值 (T)，本文统计校准集上的 (S(T)) 与 (F(T))，并将 (F(T)) 建模为二项比例观测。随后通过 Clopper-Pearson 上置信界估计该阈值下真实错误比例的保守上界：

[
\mathrm {UCB}(F,S,\delta_T)
===========================

\mathrm{BetaInv}(1-\delta_T;F+1,S-F).
]

若总置信失败概率为 (\delta)，候选阈值数量为 (J)，本文采用 Bonferroni 修正并令：

[
\delta_T=\frac{\delta}{J}.
]

最终，SafeIn 阈值由如下优化问题确定：

[
T^*
===

\max_T
\left{
T:
\mathrm{UCB}(F(T),S(T),\delta_T)
\le \beta
\right}.
]

在线查询时，系统仅需计算每个候选单元的保守距离上界 (U_i)，并根据：

[
U_i<T^*
]

判断其是否进入 SafeIn。若条件成立，则该候选对应的原始向量或原始数据被提前读取；否则进入 Uncertain 集合，并由后续多 bit 复估或精排阶段处理。

---

# 11. 论文中的算法描述

你可以写成下面这种算法。

```text
Algorithm: FDP-UCB Calibration for SafeIn Threshold

Input:
    Calibration queries Q_cal
    Target FDP level beta
    Confidence level delta
    Exact top-k labels
    Candidate upper-bound scores U_i = d_hat_i + e_i

Output:
    Calibrated SafeIn threshold T*

1. Run the same candidate-generation pipeline as online search
   on all calibration queries.

2. For each candidate unit i, record:
       U_i = d_hat_i + e_i
       z_i = 1 if i is a false SafeIn candidate else 0

3. Sort all records by U_i in ascending order.

4. For each candidate prefix m:
       S_m = m
       F_m = sum_{j=1}^{m} z_(j)
       delta_m = delta / J
       UCB_m = BetaInv(1 - delta_m; F_m + 1, S_m - F_m)

5. Select:
       m* = argmax_m S_m
            subject to UCB_m <= beta

6. Set:
       T* = U_(m*)

7. Return T*
```

---

# 12. 实验部分建议报告的指标

为了证明这个方案有效，你后续实验表格建议至少报告：


| 指标                | 含义                     |
| ------------------- | ------------------------ |
| (\beta)             | 目标错误比例             |
| (T^*)               | 标定得到的 SafeIn 阈值   |
| calib empirical FDP | 校准集经验错误比例       |
| calib UCB-FDP       | 校准集错误比例上置信界   |
| validation FDP      | 验证集 SafeIn 错误比例   |
| online FDP          | 正式查询 SafeIn 错误比例 |
| SafeIn count        | SafeIn 数量              |
| recall@k            | 检索召回率               |
| avg / p99 latency   | 平均与尾延迟             |

你现在最关心的是：

[
\text{online FDP}\approx \beta
]

所以表格中最好把 `online false/SafeIn` 改名为：

[
\mathrm{FDP}_{\mathrm{online}}
]

这样论文表述会更清楚。

---

# 13. 需要特别注意的实现细节

第一，`is_false` 的定义必须和你的读取单元一致。

如果 SafeIn 单元是**向量 candidate**，那么：

[
\mathrm{is_false}=1
]

可以定义为该向量不属于 exact top-(k)。

如果 SafeIn 单元是**聚类或 block**，那么：

[
\mathrm{is_false}=1
]

应该定义为该聚类或 block 中没有任何 exact top-(k) 向量，或者该读取不会被最终 top-(k) 使用。你需要在论文中固定一种定义。

第二，calibration 流程必须和 online 流程完全一致。

如果 online 会先 Stage1，再 Stage2，那么 calibration 也必须记录 Stage1 和 Stage2 后最终进入 SafeIn 判断的候选。否则标定得到的 (T) 会和真实在线分布不一致。

第三，FDP-UCB 控制的是 SafeIn 集合中的错误比例，而不是 recall。

如果 recall@10 低，主要问题可能来自：

* 聚类 probe 不够；
* candidate generation 漏掉真实近邻；
* Stage2 复估不充分；
* rerank 候选不足。

SafeIn 阈值只负责提前读取策略，不应该单独承担 recall 保证。

---

# 14. 最终可采用的论文口径

你可以把这套方法总结成：

> 本文将 SafeIn 判定视为一个错误发现比例控制问题。对于每个候选读取单元，系统根据量化距离估计和误差补偿得到保守上界 (U=\hat d+e)，并使用阈值 (T) 将满足 (U<T) 的候选划入 SafeIn。为了避免有限校准样本导致的经验错误比例低估，本文不直接约束校准集上的 (F/S)，而是基于二项比例上置信界估计真实 FDP 的保守上界。最终选择满足 (\mathrm{UCB}_{\mathrm{FDP}}\le\beta) 的最大阈值，从而在控制 SafeIn 错误比例的同时尽可能扩大提前读取范围。

简而言之：

[
\boxed{
\text{FDP 负责定义 SafeIn 中错误比例}
}
]

[
\boxed{
\text{UCB 负责修正有限校准样本的不确定性}
}
]

[
\boxed{
T^*=\max_T{T:\mathrm{UCB}_{\mathrm{FDP}}(T)\le\beta}
}
]

[
\boxed{
\text{在线查询： } U_i=\hat d_i+e_i<T^* \Rightarrow \mathrm{SafeIn}
}
]

[1]: https://www.stat.purdue.edu/~doerge/BIOINFORM.D/FALL06/Benjamini%20and%20Y%20FDR.pdf?utm_source=chatgpt.com
[2]: https://www.jstor.org/stable/2282952?utm_source=chatgpt.com
