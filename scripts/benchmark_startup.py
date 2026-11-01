"""Paired Ninja startup measurements, with optional path crossover controls."""
import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", action="append", default=[], metavar="NAME=PATH")
    ap.add_argument("--repeats", type=int, default=101)
    ap.add_argument("--warmup", type=int, default=10)
    ap.add_argument("--waves", type=int, default=4)
    ap.add_argument("--crossover", action="store_true", help="rotate binaries through the same temporary paths")
    ap.add_argument("--work", type=Path, default=ROOT / "build-release/startup-bench")
    ap.add_argument("--output", type=Path, default=ROOT / "build-release/startup.json")
    opt = ap.parse_args()
    if opt.repeats < 1 or opt.waves < 1 or opt.warmup < 0:
        ap.error("repeats/waves must be positive and warmup must be nonnegative")
    tools = {}
    for item in opt.exe or ["sninja=build-release/sninja.exe", "ninja=build-release/ninja_reference.exe"]:
        name, path = item.split("=", 1)
        if name in tools:
            ap.error(f"duplicate executable name: {name}")
        tools[name] = Path(path).resolve()
    result = {"platform": platform.platform(), "repeats": opt.repeats, "warmup": opt.warmup,
              "crossover": opt.crossover,
              "copy_integrity_level": "medium" if opt.crossover and os.name == "nt" else None,
              "binaries": {
                  name: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                  for name, path in tools.items()}, "waves": []}
    opt.work.mkdir(parents=True, exist_ok=True)
    opt.output.parent.mkdir(parents=True, exist_ok=True)
    rng = random.Random(94)
    expected = None
    all_samples = {name: [] for name in tools}
    with tempfile.TemporaryDirectory(dir=opt.work.resolve(), prefix="crossover-") as temp:
        directory = Path(temp)
        names = list(tools)
        for wave in range(opt.waves):
            paths = dict(tools)
            if opt.crossover:
                for index, name in enumerate(names):
                    path = directory / f"program{(index + wave) % len(names)}{tools[name].suffix}"
                    shutil.copyfile(tools[name], path)
                    if os.name == "nt":
                        # Workspace copies can inherit a Low label even when
                        # both production executables are Medium integrity.
                        subprocess.run(["icacls", str(path), "/setintegritylevel", "M"],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                    paths[name] = path
            samples = {name: [] for name in tools}
            for repeat in range(opt.warmup + opt.repeats):
                order = list(tools)
                rng.shuffle(order)
                for name in order:
                    start = time.perf_counter_ns()
                    run = subprocess.run([str(paths[name]), "--version"], stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, timeout=30, check=True)
                    elapsed = (time.perf_counter_ns() - start) / 1e6
                    output = (run.stdout, run.stderr)
                    if expected is None:
                        expected = output
                    if output != expected:
                        raise RuntimeError(f"{name} has different --version behavior: {output!r}")
                    if repeat >= opt.warmup:
                        samples[name].append(elapsed)
                        all_samples[name].append(elapsed)
            medians = {name: statistics.median(values) for name, values in samples.items()}
            result["waves"].append({"paths": {name: str(path) for name, path in paths.items()},
                                    "samples_ms": samples, "median_ms": medians})
            result["median_ms"] = {name: statistics.median(values) for name, values in all_samples.items()}
            opt.output.write_text(json.dumps(result, indent=2) + "\n")
            print(f"wave {wave}: " + " | ".join(f"{name} {ms:.4f}ms" for name, ms in medians.items()), flush=True)


if __name__ == "__main__":
    main()
