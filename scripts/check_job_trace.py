"""Check opt-in Windows tracing on pipe, console and failed process launches."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sninja", type=Path, default=ROOT / "build-profile/sninja.exe")
    ap.add_argument("--job", type=Path, default=ROOT / "build-release/sninja_job.exe")
    opt = ap.parse_args()
    import os
    with tempfile.TemporaryDirectory(prefix="trace-check-", dir=ROOT / "build-release") as temporary:
        directory = Path(temporary)
        job = subprocess.list2cmdline([str(opt.job.resolve()), "--emit", "7"]).replace("$", "$$")
        (directory / "build.ninja").write_text(
            f"rule emit\n  command = {job}\n"
            "rule missing\n  command = sninja-intentionally-nonexistent-98174.exe\n"
            "build pipe: emit\nbuild console: emit\n  pool = console\nbuild bad: missing\n"
            "default pipe console bad\n")
        prefix = directory / "trace"
        env = dict(os.environ, SNINJA_JOB_TRACE=str(prefix), SNINJA_ASYNC_LAUNCH="1")
        with subprocess.Popen([str(opt.sninja.resolve()), "-j", "3", "-k", "0"],
                              cwd=directory, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as p:
            stdout, stderr = p.communicate(timeout=30)
            if p.returncode != 1:
                raise RuntimeError((p.returncode, stdout, stderr))
            trace = directory / f"trace.{p.pid}.jsonl"
            rows = [json.loads(line) for line in trace.read_text().splitlines()]
            if len(rows) != 3 or any(r["parent_pid"] != p.pid for r in rows):
                raise RuntimeError(rows)
        failures = [r for r in rows if r["exit_code"]]
        successful = [r for r in rows if not r["exit_code"]]
        if len(failures) != 1 or failures[0]["pid"] or failures[0]["times_valid"]:
            raise RuntimeError(rows)
        if len(successful) != 2 or not all(r["times_valid"] and r["pid"] for r in successful):
            raise RuntimeError(rows)
        if not any(r["first_output"] for r in successful):
            raise RuntimeError("Pipe output was not traced")
        for row in rows:
            if not (row["frequency"] > 0 and row["queued"] <= row["started"] <= row["create_begin"] <=
                    row["create_end"] <= row["finished"]):
                raise RuntimeError(row)
        print("Job trace: pipe output, console, missing executable, child times and timestamp ordering passed")


if __name__ == "__main__":
    main()
