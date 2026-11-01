"""Measure real build-log loading with long paths and repeated history rows."""
import argparse
import hashlib
import json
from pathlib import Path
import platform
import random
import statistics

from benchmark_deps import run

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sninja", type=Path, default=ROOT / "build-release/sninja.exe")
    ap.add_argument("--reference", type=Path, default=ROOT / "build-release/ninja_reference.exe")
    ap.add_argument("--extra", action="append", default=[], metavar="NAME=EXE")
    ap.add_argument("--outputs", type=int, default=20000)
    ap.add_argument("--history", type=int, nargs="+", default=[1, 3, 7])
    ap.add_argument("--repeats", type=int, default=15)
    ap.add_argument("--work", type=Path, default=ROOT / "build-release/buildlog-bench")
    ap.add_argument("--output", type=Path, default=ROOT / "build-release/buildlog.json")
    opt = ap.parse_args()
    if min(opt.outputs, opt.repeats, *opt.history) < 1:
        ap.error("outputs, history and repeats must be positive")
    tools = {"sninja": opt.sninja.resolve(), "ninja": opt.reference.resolve()}
    for extra in opt.extra:
        name, path = extra.split("=", 1)
        tools[name] = Path(path).resolve()
    result = {"platform": platform.platform(), "repeats": opt.repeats,
              "binaries": {name: {"path": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
                           for name, exe in tools.items()}, "workloads": {}}
    rng = random.Random(12837)
    paths = ["objects/" + "long_component/" * 5 + f"source_{i:06d}.obj" for i in range(opt.outputs)]
    for history in opt.history:
        directory = opt.work.resolve() / f"history{history}"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "build.ninja").write_text("".join(f"build {path}: phony\n" for path in paths))
        data = bytearray(b"# ninja log v7\n")
        for pass_number in range(history):
            for i, path in enumerate(paths):
                start = pass_number * opt.outputs + i
                data += (f"{start}\t{start+7}\t{1000000000000+start}\t{path}\t"
                         f"{0x8123456780000000+start:x}\n").encode()
        log = directory / ".ninja_log"
        log.write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        # Dry-run suppresses automatic recompaction even above the history
        # threshold, so every timed process loads exactly the same bytes.
        arguments = ["-n", "-t", "query", paths[0]]
        _, expected, errors = run(tools["ninja"], arguments, directory)
        if errors or b"input: phony" not in expected:
            raise RuntimeError((expected, errors))
        samples = {name: [] for name in tools}
        for repeat in range(opt.repeats + 2):
            order = list(tools)
            rng.shuffle(order)
            for name in order:
                sample, stdout, stderr = run(tools[name], arguments, directory)
                if stdout != expected or stderr:
                    raise RuntimeError(f"Different query result from {name}: {stdout!r} {stderr!r}")
                if repeat >= 2:
                    samples[name].append(sample)
        if hashlib.sha256(log.read_bytes()).hexdigest() != digest:
            raise RuntimeError("Read-only queries modified the build log")
        # Cross-check all parsed values and ownership by having each engine
        # rewrite the original history, then compare every canonical row.
        rewritten = []
        for name, exe in tools.items():
            log.write_bytes(data)
            _, stdout, stderr = run(exe, ["-t", "recompact"], directory)
            if stdout or stderr:
                raise RuntimeError((name, stdout, stderr))
            rows = log.read_bytes().splitlines()
            if rows[0] != b"# ninja log v7" or len(rows) != opt.outputs + 1:
                raise RuntimeError(f"Wrong recompaction from {name}")
            rewritten.append(sorted(rows[1:]))
        if any(rows != rewritten[0] for rows in rewritten[1:]):
            raise RuntimeError("The engines disagree on parsed build-log values")
        medians = {name: {metric: statistics.median(s[metric] for s in runs)
                          for metric in runs[0]} for name, runs in samples.items()}
        result["workloads"][f"history{history}"] = dict(
            outputs=opt.outputs, history=history, bytes=len(data), samples=samples,
            medians=medians, log_unchanged=True, recompact_values_match=True)
        print(f"history={history}: " + " | ".join(
            f"{name} {m['wall_ms']:.3f}ms" +
            (f" peak={m['peak_working_set_bytes']/1048576:.1f}MiB" if "peak_working_set_bytes" in m else "")
            for name, m in medians.items()), flush=True)
        opt.output.parent.mkdir(parents=True, exist_ok=True)
        opt.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
