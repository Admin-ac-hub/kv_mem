#!/usr/bin/env python3
"""Serial, paired group-commit benchmarks. Never deletes user database paths."""
import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import platform
import random
import shutil
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, **kwargs):
    return subprocess.run([str(x) for x in command], check=True, text=True, **kwargs)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def baseline_binaries(ref, directory):
    source = directory / "source"
    source.mkdir(parents=True)
    archive = directory / "baseline.tar"
    with archive.open("wb") as output:
        subprocess.run(["git", "archive", ref], cwd=ROOT, stdout=output, check=True)
    # git archive contains only repository paths; use the system tar extractor.
    run(["tar", "-xf", archive, "-C", source])
    build = directory / "build"
    run(["cmake", "-S", source, "-B", build, "-DCMAKE_BUILD_TYPE=Release",
         "-DBUILD_TESTING=OFF"])
    run(["cmake", "--build", build, "--parallel", "6", "--target", "kv_core"])
    command = [os.environ.get("CXX", "c++"), "-O3", "-std=c++17", "-DNDEBUG",
               "-DKV_GROUP_COMMIT_BASELINE=1", "-I", source / "include",
               ROOT / "bench/group_commit_bench.cpp"]
    command += [build / f"lib{library}.a" for library in ("kv_core", "kv_common")]
    run(command + ["-pthread", "-o", build / "group_commit_bench"])
    return build


def interval(values):
    generator = random.Random(20261014)
    samples = sorted(statistics.median(generator.choices(values, k=len(values)))
                     for _ in range(10000))
    return [samples[249], samples[9749]]


def summarize(rows):
    cases = {}
    for row in rows:
        if int(row["round"]) < 0:
            continue
        key = (row["engine"], int(row["threads"]), int(row["batch_size"]))
        cases.setdefault(key, {}).setdefault(row["implementation"], []).append(row)
    result = []
    for key, variants in cases.items():
        item = dict(zip(("engine", "threads", "batch_size"), key))
        item["variants"] = {}
        for variant, observations in variants.items():
            item["variants"][variant] = {field: statistics.median(float(x[field]) for x in observations)
                for field in ("ops_per_sec", "wall_ops_per_sec", "request_p50_us", "request_p99_us",
                              "wal_sync_count", "operations_per_sync", "flush_count", "compaction_count")}
        item["comparisons"] = {}
        for denominator in ("off", "baseline"):
            numerator = {int(x["round"]): x for x in variants["on"]}
            control = {int(x["round"]): x for x in variants[denominator]}
            comparison = {}
            for metric in ("ops_per_sec", "wall_ops_per_sec"):
                ratios = [float(numerator[r][metric]) / float(control[r][metric]) for r in sorted(numerator)]
                comparison[metric] = {"median_ratio": statistics.median(ratios),
                                      "bootstrap_95_ci": interval(ratios),
                                      "stable_improvement": interval(ratios)[0] > 1}
            item["comparisons"]["on/" + denominator] = comparison
        result.append(item)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build-group-release")
    parser.add_argument("--baseline-ref", default="dc63d44")
    parser.add_argument("--baseline-dir", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--kv-operations", type=int, default=100000)
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--batch-sizes", type=int, nargs="+", default=[1, 32])
    args = parser.parse_args()
    if min(args.rounds, args.kv_operations, *args.threads, *args.batch_sizes) <= 0:
        parser.error("counts must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    if (args.output / "raw.csv").exists():
        parser.error("output already contains raw.csv; choose a fresh output directory")
    # One child process at a time; no builds/tests run during timed measurements.
    with tempfile.TemporaryDirectory(prefix="kv-group-benchmark-") as temporary:
        scratch = Path(temporary)
        baseline = args.baseline_dir or baseline_binaries(args.baseline_ref, scratch / "baseline")
        metadata = {"platform": platform.platform(), "machine": platform.machine(),
                    "baseline_ref": args.baseline_ref,
                    "head": run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True).stdout.strip(),
                    "compiler": run(["c++", "--version"], capture_output=True).stdout,
                    "benchmark_sha256": sha256(ROOT / "bench/group_commit_bench.cpp"),
                    "source_sha256": {str(p.relative_to(ROOT)): sha256(p)
                        for folder in ("include", "src", "cmake")
                        for p in sorted((ROOT / folder).rglob("*")) if p.is_file()},
                    "binary_sha256": {variant + "/" + name: sha256(directory / name)
                        for variant, directory in (("baseline", baseline), ("current", args.build_dir))
                        for name in ("group_commit_bench",)},
                    "cmake_cache_sha256": sha256(args.build_dir / "CMakeCache.txt"),
                    "rounds": args.rounds, "threads": args.threads, "batch_sizes": args.batch_sizes,
                    "kv_operations": args.kv_operations, "value_size": 100,
                    "memtable_entries_limit": 1024, "level0_sstable_limit": 4,
                    "bootstrap_resamples": 10000, "bootstrap_seed": 20261014,
                    "warmup_round": -1, "timing": "worker release to join; wall includes Close; KV reopen verification excluded"}
        (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
        rows = []
        with (args.output / "raw.csv").open("w", newline="") as output:
            writer = None
            for batch in args.batch_sizes:
                for threads in args.threads:
                    for repeat in range(-1, args.rounds):
                        order = ["baseline", "off", "on"]
                        shift = (repeat + 1) % len(order)
                        order = order[shift:] + order[:shift]
                        for variant in order:
                            name = "group_commit_bench"
                            binary = (baseline if variant == "baseline" else args.build_dir) / name
                            path = scratch / f"db-{len(rows)}"
                            command = [binary, "--path", path, "--operations",
                                args.kv_operations,
                                "--threads", threads, "--batch-size", batch,
                                "--grouping", "on" if variant == "on" else "off"]
                            parsed = list(csv.DictReader(io.StringIO(run(command, capture_output=True).stdout)))
                            if len(parsed) != 1 or parsed[0]["implementation"] != variant:
                                raise RuntimeError("unexpected benchmark output")
                            row = {"round": repeat, **parsed[0]}
                            if int(row["verified"]) != int(row["operations"]):
                                raise RuntimeError("incomplete verification")
                            if int(row["flush_count"]) < 2 or int(row["compaction_count"]) == 0:
                                raise RuntimeError("workload did not exercise flush and compaction")
                            if writer is None:
                                writer = csv.DictWriter(output, fieldnames=list(row)); writer.writeheader()
                            writer.writerow(row); output.flush(); rows.append(row)
                            shutil.rmtree(path)
                        print(f"kv batch={batch} threads={threads} round={repeat} done", flush=True)
                    (args.output / "summary.json").write_text(json.dumps(summarize(rows), indent=2) + "\n")
        summary = summarize(rows)
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        required = [x for x in summary if x["threads"] in (4, 8) and x["batch_size"] == 1]
        failed = [{field: case[field] for field in ("engine", "threads", "batch_size")}
                  | {"control": control, "metric": metric,
                     **case["comparisons"]["on/" + control][metric]}
                  for case in required for control in ("baseline", "off")
                  for metric in ("ops_per_sec", "wall_ops_per_sec")
                  if not case["comparisons"]["on/" + control][metric]["stable_improvement"]]
        passed = bool(required) and not failed
        assessment = {"status": "passed" if passed else "not_met", "total_runs": len(rows),
                      "warmup_runs": sum(int(row["round"]) < 0 for row in rows),
                      "measured_runs": sum(int(row["round"]) >= 0 for row in rows),
                      "required_cases": len(required),
                      "criterion": "batch=1, threads=4/8: both baseline and off controls, frontend and Close-inclusive throughput; bootstrap 95% lower bound > 1",
                      "failed_checks": failed}
        (args.output / "acceptance.json").write_text(json.dumps(assessment, indent=2) + "\n")
        print("performance_acceptance=" + ("passed" if passed else "not_met"), flush=True)
        return 0 if passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
