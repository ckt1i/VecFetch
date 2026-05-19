#!/usr/bin/env python3
"""8.15 gate check for Appendix readiness.

Assumptions:
- main-experiment summaries are emitted under:
  /home/zcq/VDB/baselines/formal-study/outputs/summaries/main_experiment_topk{10,50,100}_summary.csv
- extended backend execution summaries are emitted under:
  /home/zcq/VDB/baselines/formal-study/outputs/summaries/extended_backend_summary.csv
- operating points are defined in:
  /home/zcq/VDB/baselines/formal-study/outputs/extended_backend/selected_operating_points.csv
"""

from pathlib import Path
from typing import List, Tuple

import pandas as pd


BASE = Path("/home/zcq/VDB/baselines/formal-study/outputs")
SUMMARY_DIR = BASE / "summaries"
EXT_DIR = BASE / "extended_backend"


def _load_csv(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise FileNotFoundError(f"Required file missing: {path}")
    return pd.read_csv(path)


def check_main_experiment_completeness() -> Tuple[bool, List[str]]:
    """Check that each main dataset has non-empty top-k blocks in flatstor.

    We check presence in the merged dataset-level top-k summary files and report
    per-dataset, per-topk, per-system counts.
    """
    requirements = {
        10: {
            "coco_100k",
            "msmarco_passage",
            "deep8m_synth",
            "amazon_esci",
        },
        50: {
            "coco_100k",
            "msmarco_passage",
            "deep8m_synth",
            "amazon_esci",
        },
        100: {
            "coco_100k",
            "msmarco_passage",
            "deep8m_synth",
            "amazon_esci",
        },
    }

    ok = True
    reports: List[str] = []
    for topk, expected_datasets in sorted(requirements.items()):
        path = SUMMARY_DIR / f"main_experiment_topk{topk}_summary.csv"
        df = _load_csv(path)
        if df.empty:
            reports.append(f"topk={topk}: summary empty ({path})")
            ok = False
            continue
        for dataset in sorted(expected_datasets):
            ds = df[df["dataset"] == dataset]
            if ds.empty:
                reports.append(f"topk={topk}: missing dataset {dataset}")
                ok = False
                continue
            systems = sorted(s for s in ds["system"].dropna().astype(str).unique().tolist())
            reports.append(
                f"topk={topk}, dataset={dataset}: systems={systems}, rows={len(ds)}"
            )
    return ok, reports


def check_extended_backend_completeness() -> Tuple[bool, List[str]]:
    """Check every selected operating point has at least one successful extended backend row."""
    selected = _load_csv(EXT_DIR / "selected_operating_points.csv")
    summary = _load_csv(SUMMARY_DIR / "extended_backend_summary.csv")

    # keep one row per unique selected operating point as defined by replay contract
    selected_points = (
        selected[
            [
                "dataset",
                "system",
                "variant",
                "selected_param",
                "backend",
            ]
        ]
        .drop_duplicates()
    )
    reports: List[str] = []

    if "success" in summary.columns:
        summary_ok = summary[
            summary["success"].astype(str).str.lower().isin({"1", "true", "yes", "y"})
        ]
    else:
        summary_ok = summary

    summary_keys = set(
        tuple(row)
        for row in summary_ok[
            ["dataset", "system", "variant", "selected_param", "backend"]
        ].drop_duplicates().itertuples(index=False, name=None)
    )
    selected_points_list = list(
        selected_points.itertuples(index=False, name=None)
    )

    missing = [
        point
        for point in selected_points_list
        if tuple(point) not in summary_keys
    ]

    ok = len(missing) == 0
    if ok:
        reports.append(
            f"extended backend: all {len(selected_points_list)} selected points found in summary"
        )
    else:
        reports.append(
            f"extended backend: {len(missing)} of {len(selected_points_list)} selected points missing"
        )
        for ds, sys, var, p, b in missing[:20]:
            reports.append(
                f"  MISSING dataset={ds}, system={sys}, variant={var}, backend={b}, param={p}"
            )
        if len(missing) > 20:
            reports.append(f"  ... and {len(missing)-20} more missing points")
    return ok, reports


def main() -> int:
    main_ok, main_logs = check_main_experiment_completeness()
    ext_ok, ext_logs = check_extended_backend_completeness()
    gate_ok = main_ok and ext_ok

    print("===== 8.15 Appendix Gate Check =====")
    print("8.15.1 main experiment dataset blocks complete:", "PASS" if main_ok else "FAIL")
    for line in main_logs:
        print(line)

    print("8.15.2 backend extension blocks complete:", "PASS" if ext_ok else "FAIL")
    for line in ext_logs:
        print(line)

    print("8.15.3 appendix gate:", "PASS" if gate_ok else "FAIL")
    return 0 if gate_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
