"""Summarize native job traces; child CPU excludes descendant processes."""
import argparse
import json
from pathlib import Path
import re
import statistics


def summary(records):
    if not records:
        return {"jobs": 0}
    valid = [r for r in records if r["times_valid"]]
    def qpc_delta(row, a, b):
        return (row[b] - row[a]) * 1000 / row["frequency"]
    fields = {"queue_ms": [qpc_delta(r, "queued", "started") for r in records],
              "setup_ms": [qpc_delta(r, "started", "create_begin") for r in records],
              "create_ms": [qpc_delta(r, "create_begin", "create_end") for r in records],
              "command_ms": [qpc_delta(r, "queued", "finished") for r in records],
              "child_lifetime_ms": [(r["exit_100ns"] - r["creation_100ns"]) / 10000 for r in valid],
              "child_cpu_ms": [(r["kernel_100ns"] + r["user_100ns"]) / 10000 for r in valid]}
    result = {"jobs": len(records), "async_jobs": sum(r["async"] for r in records),
              "valid_process_times": len(valid),
              "metrics": {k: {"median": statistics.median(v), "sum": sum(v), "max": max(v)}
                          for k, v in fields.items() if v}}
    if valid:
        begin = min(r["creation_100ns"] for r in valid)
        end = max(r["exit_100ns"] for r in valid)
        span = (end - begin) / 10000
        events = sorted([(r["creation_100ns"], 1) for r in valid] +
                        [(r["exit_100ns"], -1) for r in valid])
        active = peak = 0
        for _, change in events:
            active += change
            peak = max(peak, active)
        result.update(child_span_ms=span, peak_live_children=peak,
                      average_live_children=sum(fields["child_lifetime_ms"]) / span if span else 0,
                      average_child_cpu_cores=sum(fields["child_cpu_ms"]) / span if span else 0)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("directory", type=Path)
    ap.add_argument("--output", type=Path)
    opt = ap.parse_args()
    result = {"note": "Direct-child CPU only; compiler wrappers and linkers may have descendants.", "traces": {}}
    for path in sorted(opt.directory.rglob("*.jsonl")):
        records = [json.loads(line) for line in path.read_text(encoding="utf8").splitlines() if line]
        for row in records:
            if not (row["queued"] <= row["started"] <= row["create_begin"] <= row["create_end"] <= row["finished"]):
                raise RuntimeError(f"Invalid timestamp ordering: {path}: {row}")
        compiler = [r for r in records if re.search(r"(?:^|[\\/\s\"])(?:cl|clang-cl|clang\+\+|g\+\+)(?:\.exe)?(?:\"|\s)",
                                                    r["command"], re.I)]
        result["traces"][str(path)] = {"all": summary(records), "compiler_commands": summary(compiler)}
        if compiler and result["traces"][str(path)]["compiler_commands"]["valid_process_times"]:
            s = result["traces"][str(path)]["compiler_commands"]
            m = s["metrics"]
            print(f"{path.name}: {len(compiler)} compiler commands; queue median {m['queue_ms']['median']:.2f}ms, "
                  f"create median {m['create_ms']['median']:.2f}ms, child CPU median {m['child_cpu_ms']['median']:.2f}ms, "
                  f"lifetime median {m['child_lifetime_ms']['median']:.2f}ms, "
                  f"average live {s['average_live_children']:.1f}, CPU cores {s['average_child_cpu_cores']:.1f}")
    if opt.output:
        opt.output.parent.mkdir(parents=True, exist_ok=True)
        opt.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
