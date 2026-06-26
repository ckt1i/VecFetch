# 2MB 对齐 mmap Resident Buffer 与 HugePage 验证结果

时间：2026-06-25

## 实现内容

本轮在不重建索引的前提下，只改变查询进程中 `cluster.clu` resident preload 的内存分配方式。

新增可选 resident 模式：

```bash
VDB_RESIDENT_PRELOAD_MODE=full_file_mmap_2mb
VDB_RESIDENT_HUGEPAGE=1
VDB_RESIDENT_HUGEPAGE_COLLAPSE=1
```

实现路径：

- `ClusterStoreReader::PreloadAllClusters()` 在 full-file resident 模式下可使用匿名 `mmap()` 分配大块 buffer。
- 分配大小为 `cluster.clu` 文件大小加 2MB 对齐余量。
- resident 数据起始地址按 2MB 对齐。
- 对 resident 数据区调用 `madvise(MADV_HUGEPAGE)`。
- 若设置 `VDB_RESIDENT_HUGEPAGE_COLLAPSE=1`，额外调用 `madvise(MADV_COLLAPSE)`。

该模式复用已有索引文件，不改变 `cluster.clu`/`data.dat` 格式。

## mTHP 小页说明

当前机器暴露了 multi-size THP 入口：

```text
hugepages-16kB/enabled
hugepages-32kB/enabled
hugepages-64kB/enabled
...
hugepages-2048kB/enabled
```

但小于 2MB 的 mTHP 当前均为 `never`。由于没有 sudo 权限，本轮没有修改 sysfs，也没有测试 16KB/32KB/64KB 等小页。用户态只能通过 `madvise(MADV_HUGEPAGE)` 给出提示，不能稳定指定某个 mTHP 尺寸。

## ESCI ex_bits=3 完整 benchmark 验证

设置：

- 数据集：`amazon_esci`
- 索引：`new_vector_bitmajor_tiles`, `total_bits=4`, `ex_bits=3`
- `topk=100`, `nprobe=256`, `queries=1000`
- two-level coarse routing enabled, budget factor 12
- 每个模式 3 次；最佳模式额外补到 8 次 paired 验证

结果文件：

- `/home/zcq/VDB/test/hugepage_mmap2mb_20260625/results/esci_ex3_mmap2mb_all_summary.csv`
- `/home/zcq/VDB/test/hugepage_mmap2mb_20260625/results/esci_ex3_mmap2mb_summary.md`

汇总：

| 模式 | n | avg ms | speedup vs compact | recall@100 | preload ms | AnonHugePages KiB | Stage1 ms | Stage2 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| compact_default | 8 | 1.7471 | 1.0000 | 0.9217 | 353.22 | 0.0 | 0.4694 | 0.1871 |
| full_file_mmap2mb + HP + collapse | 8 | 1.6813 | 1.0391 | 0.9217 | 460.31 | 0.0 | 0.4371 | 0.1770 |

paired common reps 的平均降幅为 3.76%。但完整 benchmark 下 `AnonHugePages` 仍为 0，因此这一组只能说明 2MB 对齐 mmap + collapse 路径有稳定降延迟现象，不能证明 Linux THP 已经实际 materialize。

## Online Query 扩展验证

为避免 MSMARCO 全量 base embeddings 加载干扰，本轮给 `bench_online_query` 增加了 `smaps_rollup` 指标，并使用 online-query 口径扩展到两个数据集和三个 bits。

设置：

- `topk=100`, `nprobe=256`, `queries=1000`
- `dynamic-safeout=1`
- `dynamic-safein=frontier`
- `non-safeout-candidate-budget=400`
- 对比 `compact_default` 与 `full_file_mmap2mb + HP + collapse`
- 每点 3 次

结果文件：

- `/home/zcq/VDB/test/hugepage_mmap2mb_20260625/results/online_extension_raw.csv`
- `/home/zcq/VDB/test/hugepage_mmap2mb_20260625/results/online_extension_summary.csv`
- `/home/zcq/VDB/test/hugepage_mmap2mb_20260625/results/online_extension_summary.md`

汇总：

| 数据集 | total_bits | compact ms | mmap2mb+collapse ms | speedup | recall delta | AnonHugePages KiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | 2 | 2.2305 | 2.1573 | 1.0339 | 0.00000 | 430080.0 |
| amazon_esci | 3 | 2.2175 | 2.1330 | 1.0396 | 0.00000 | 600064.0 |
| amazon_esci | 4 | 2.2227 | 2.1805 | 1.0194 | 0.00000 | 770048.0 |
| msmarco_passage | 2 | 4.9135 | 4.5566 | 1.0783 | 0.00000 | 1945600.0 |
| msmarco_passage | 3 | 4.8774 | 4.5381 | 1.0748 | 0.00000 | 2775040.0 |
| msmarco_passage | 4 | 4.1820 | 4.5306 | 0.9230 | 0.00000 | 3604480.0 |

online-query 口径下，`AnonHugePages` 明确非零，说明 2MB 对齐 mmap + `MADV_HUGEPAGE/MADV_COLLAPSE` 在该路径下实际 materialize 了 huge pages。

## 结论

1. 2MB 对齐 mmap 是比上一轮 `std::vector + madvise()` 更有效的 HugePage 验证路径。
2. online-query 下已经能观察到非零 `AnonHugePages`，并且 ESCI bits=2/3/4 与 MSMARCO bits=2/3 有收益。
3. MSMARCO bits=4 出现总延迟倒退，虽然 Stage1/Stage2/submit 单项仍变快。这说明 full-file 大块 resident 与 huge page 并非单调收益，可能受到更大 resident footprint、内存压力、page collapse 成本或其他非 probe 阶段影响。
4. 该优化可以作为可选系统优化继续保留，但目前不应无条件替代默认 compact resident。更合理的策略是按数据集/bit 配置启用，或后续做更细粒度的 per-region mmap slab。

## 后续建议

- 保留 `full_file_mmap_2mb` 作为实验开关。
- 若要进入默认路径，需要增加自动判定：
  - `cluster.clu` 足够大；
  - `AnonHugePages` 能实际 materialize；
  - warmup 后查询延迟稳定下降；
  - bits=4/MSMARCO 这类倒退点不会被误启用。
- 如果后续有 sudo 权限，再测试 mTHP 16KB/64KB/256KB；当前无权限条件下不建议继续尝试小页。
