# Deep1M_synth Experiment Tracker

| Run ID | Milestone | Purpose | Dataset | System / Variant | Backend | Key Params | Metrics | Priority | Status | Notes |
|---|---|---|---|---|---|---|---|---|---|---|
| D1M-000 | D0 | asset shape check | deep1m_synth | raw/formal assets | all | base=1M x 96, query=10K x 96, GT top10/20/100 | readiness | MUST | DONE | validated by `deep1m_synth_asset_manifest.json` |
| D1M-001 | D0 | query/GT split contract | deep1m_synth | split_v1 | all | query_count=1000, sampled from 10000 | split checksum, GT rows | MUST | DONE | formal split and GT top10/20/100 persisted |
| D1M-002 | D0 | synthetic payload source check | deep1m_synth | default payload | cleaned parquet | bucket_mixture_v1,seed=20260510,min=256B,mean~=4KB,max=64KB | row count,bucket counts,min/mean/max,checksum | MUST | DONE | cleaned payload regenerated from deterministic synthetic source |
| D1M-002A | D0 | FlatStor backend derivation | deep1m_synth | FlatStor default | flatstor | rows=1,000,000,derived from cleaned payload | row count,bytes,random fetch | MUST | DONE | FlatStor bytes validated against cleaned payload samples |
| D1M-002B | D0 | Parquet backend derivation | deep1m_synth | Parquet default | parquet | rows=1,000,000,derived from cleaned payload | row count,payload_size,random fetch | MUST | DONE | payload_size retained in derived Parquet backend |
| D1M-003 | D0 | canonical artifact check | deep1m_synth | VecFetch/RaBitQ/PQ | all | nlist=4096,bits=4,single assignment | manifest, cache meta | MUST | DONE | canonical artifact built under `outputs/index_build/deep1m_synth/.../nlist4096_bits4_single` |
| D1M-004 | D0 | smoke run | deep1m_synth | VecFetch + four baselines | flatstor+lance | nprobe=64,topk=10 | recall@10,e2e,p99 | MUST | DONE | final smoke passed; earlier PQ/lancedb setup failures were corrected with `m=16` and labnew Python |
| D1M-005 | D0 | Lance asset gate | deep1m_synth | Lance payload/index | lance | rows=1,000,000,same doc ids and payload bytes as cleaned source | row count, bytes, random fetch | MUST | DONE | Lance backend required and used for formal baseline runs |
| D1M-010 | D1 | main sweep | deep1m_synth | VecFetch / BoundFetch-Guarded | integrated | nprobe=16,32,64,128,256,512;topk=10 | recall@10,e2e,p95,p99,triage | MUST | DONE | one VecFetch warmup before sweep; formal rows written to `deep1m_synth_main_sweep_top10.csv` |
| D1M-011 | D1 | main sweep | deep1m_synth | IVF+RaBitQ | flatstor | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | DONE | completed as strongest FlatStor baseline |
| D1M-012 | D1 | main sweep | deep1m_synth | IVF+PQ | flatstor | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | DONE | completed with `m=16, nbits=8` |
| D1M-013 | D1 | main sweep | deep1m_synth | IVF+RaBitQ | lance | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | DONE | completed with labnew Python Lance runtime |
| D1M-014 | D1 | main sweep | deep1m_synth | IVF+PQ | lance | nprobe=16,32,64,128,256,512;topk=10;budget=100 | recall@10,e2e,p95,p99 | MUST | DONE | completed with `m=16, nbits=8` and labnew Python Lance runtime |
| D1M-020 | D1 | matched-quality selection | deep1m_synth | retained systems | flatstor+lance | topk=10 | Q*, representative point | MUST | DONE | common-threshold rule succeeded at R@10 >= 0.950; no narrow-band fallback needed |
| D1M-030 | D2 | topk=20 supplement | deep1m_synth | VecFetch | integrated | nprobe=32,64,128 or 64,128,256;topk=20 | recall@20,e2e,p99 | SHOULD | DONE | ran nprobe=32,64,128 after recall@20 smoke check |
| D1M-031 | D2 | topk=20 supplement | deep1m_synth | IVF+RaBitQ | flatstor | same/adjoining nprobe;budget=150 then 200 if needed | recall@20,e2e,p99 | SHOULD | DONE | candidate_budget=150 sufficient; no 200 fallback needed |
| D1M-032 | D2 | topk=20 supplement | deep1m_synth | IVF+PQ | flatstor | same/adjoining nprobe;budget=150 then 200 if needed | recall@20,e2e,p99 | SHOULD | DONE | candidate_budget=150 sufficient; no 200 fallback needed |
| D1M-033 | D2 | topk=20 supplement | deep1m_synth | IVF+RaBitQ | lance | same/adjoining nprobe;budget=150 then 200 if needed | recall@20,e2e,p99 | SHOULD | DONE | candidate_budget=150 sufficient; no 200 fallback needed |
| D1M-034 | D2 | topk=20 supplement | deep1m_synth | IVF+PQ | lance | same/adjoining nprobe;budget=150 then 200 if needed | recall@20,e2e,p99 | SHOULD | DONE | candidate_budget=150 sufficient; no 200 fallback needed |
| D1M-040 | D3 | cleanup repeats | deep1m_synth | final top10 points | all | 1 warmup + repeat3 | mean,median,std,min,best | MUST | SUPERSEDED | current OpenSpec task 8.1 uses topk=10 sweep measurement directly; no repeat cleanup |
| D1M-041 | D3 | cleanup repeats | deep1m_synth | final top20 points | all | 1 warmup + repeat3 | mean,median,std,min,best | SHOULD | SUPERSEDED | current OpenSpec task 8.2 uses topk=20 supplement measurement directly; no repeat cleanup |
| D1M-050 | D4 | reporting | deep1m_synth | all retained | all | top10/top20 summaries | final CSV + summary | MUST | DONE | generated final CSVs and `DEEP1M_SYNTH_DECISION_SUMMARY.md` |
