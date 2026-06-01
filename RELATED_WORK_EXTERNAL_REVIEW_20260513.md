# Related Work External Review: BBC and AquaPipe

Date: 2026-05-13
Reviewer route: Codex subagent, `gpt-5.5`, xHigh
Reviewer agent id: `019e1fab-a3c0-7940-a215-c0137cbe4098`

## Question

Assess whether two newly noticed works should be incorporated into the current VecFetch thesis:

- BBC: Improving Large-k Approximate Nearest Neighbor Search with a Bucket-based Result Collector
- AquaPipe: A Quality-Aware Pipeline for Knowledge Retrieval and Large Language Models

If they should be incorporated, identify how to position them without weakening the thesis narrative.

## External Review Conclusion

BBC must be added. It directly overlaps with bounded quantization query execution: for IVF+RaBitQ-style bounded methods, BBC uses distance bounds to skip candidates that are guaranteed inside or outside the current top-k boundary and reranks only uncertain candidates. Omitting it would make the related-work section look incomplete and would undermine any wording that suggests candidate distinction from bounds is absent in prior work.

AquaPipe should be added. It is adjacent rather than competing: AquaPipe pipelines disk-based ANNS and LLM prefill in RAG, using recall-aware early return and correction strategies. It operates at the RAG system level, while VecFetch operates inside a single ANNS query and maps candidate uncertainty to raw-vector, full-record, and payload reads.

## Impact on VecFetch Positioning

The novelty claim should not be "using RaBitQ bounds to distinguish candidates" by itself. BBC already demonstrates that bounded quantization can classify candidates into inside/outside/uncertain groups for reranking.

The safer and stronger positioning is:

> VecFetch uses candidate uncertainty signals from bounded quantization to control record-return execution: whether to read no raw record, read only the original vector, prefetch a full record under restrictions, or fetch payload after final top-k materialization.

This keeps the contribution centered on original-record access semantics and storage-action mapping, not on bounded rerank pruning alone.

## Recommended Thesis Placement

1. `chapters/chapter2.tex`, Section 2.2: add BBC after the RaBitQ / ADSampling discussion. Explicitly acknowledge bounded quantization rerank pruning.

2. `chapters/chapter2.tex`, Section 2.3: revise the final paragraph so it no longer says prior work lacks candidate-level distinction. Instead, state that prior bounded rerank work optimizes collector / rerank / CPU-cache behavior, while VecFetch models raw-vector, payload, and full-record access semantics.

3. `chapters/chapter2.tex`, Section 2.5: add AquaPipe as cross-stage RAG pipeline related work.

4. `chapters/chapter2.tex`, Section 2.6: add a positioning sentence that RaBitQ and BBC show the value of distance bounds for rerank pruning, while VecFetch connects these signals to record-return access actions.

5. `chapters/chapter1.tex`, contribution 2: rewrite the contribution around access actions, not just "error-bound-driven candidate screening".

6. `chapters/chapter4.tex`, after the candidate state definition: add a short statement that the state split is related to bounded rerank pruning but has a different execution semantics in VecFetch.

7. `misc/1_conclusion.tex`: optionally add future work on combining VecFetch with large-k collectors and RAG-level pipelines.

## Suggested Insertions

Section 2.2:

```tex
新近的 BBC 工作进一步表明，在 bounded quantization 方法中，距离上下界不仅可用于传统阈值剪枝，也可用于减少真实距离重排对象。BBC 面向 large-$k$ ANN 查询设计 bucket-based result collector，并针对 IVF+RaBitQ 等 bounded methods 利用距离 bounds 跳过可判定为 top-$k$ 内侧或外侧的对象，只对边界附近的 uncertain 对象执行重排\cite{yin2026bbc}。这说明基于距离区间区分候选重排必要性已经成为 bounded quantization 查询执行中的一个重要方向。与此不同，本文关注的是当检索系统需要返回完整记录时，类似区间信号如何进一步映射为原始向量读取、完整记录预取和最终 payload 补读等访问动作。
```

Section 2.5:

```tex
除单个 ANNS 查询内部的 I/O 调度外，RAG 系统还可以在检索阶段和生成阶段之间进行跨阶段流水线。AquaPipe 将 disk-based ANNS 执行与 LLM prefill 阶段重叠，通过 recall-aware prefetching 先返回质量可接受的部分文本，使 prefill 可在完整检索结果返回前启动，并根据 LLM 代价模型选择 remove-after-prefill 或 re-prefill 修正策略\cite{yu2025aquapipe}。这类方法优化的是 RAG 端到端响应时间，调度对象是检索结果文本块和 LLM prefill；本文的粒度更低，发生在单次 ANNS 查询内部，关注每个候选是否访问原始向量、payload 或完整记录。二者可以组合，但不能互相替代。
```

Section 2.6:

```tex
需要强调的是，本文并不将 bounded quantization 下的 inside/outside/uncertain 区分本身作为唯一贡献；RaBitQ 及 BBC 等工作已经展示了距离 bounds 对重排剪枝的价值\cite{gao2024rabitq,yin2026bbc}。本文的定位是把这种候选不确定性信号接入记录返回型查询执行路径，使其控制原始记录的读取范围和读取时机。
```

## Literature Search Notes

Sources checked: local paper library in `VectorRetrival` (no local PDFs found), arXiv API, web search, arXiv/DBLP primary metadata pages.

Entries appended to `/home/zcq/VDB/paper/thesis/reference/main.bib`:

- `yin2026bbc`: direct competitor/positioning paper for bounded quantization reranking and large-k result collection.
- `yu2025aquapipe`: adjacent RAG-level pipeline and recall-aware prefetch paper.
- `shi2026gpuivfrabitq`: adjacent RaBitQ system extension on GPU-native IVF-RaBitQ.
- `li2026iooptimizations`: adjacent disk-resident ANN I/O design-space paper.
- `yao2024cacheblend`: adjacent RAG prefill / cached knowledge fusion paper.
- `agarwal2025cachecraft`: adjacent RAG chunk-cache management paper.

Search result interpretation: only BBC and AquaPipe should be foregrounded in the current thesis narrative. The other added entries are useful citation reserves for extended related work or future-work discussion, but should not distract from the core VecFetch positioning.
