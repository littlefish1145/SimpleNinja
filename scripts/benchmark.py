"""Paired native-process benchmarks; all failures are fatal, every sample is saved."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import time


def run(exe, args, cwd):
    start = time.perf_counter_ns()
    p = subprocess.run([str(exe), *args], cwd=cwd, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, timeout=180)
    elapsed = (time.perf_counter_ns() - start) / 1e6
    if p.returncode:
        raise RuntimeError(f"{exe} {args}: {p.returncode}\n{p.stderr.decode(errors='replace')}")
    return elapsed


def generate(root, n, reference):
    root.mkdir(parents=True, exist_ok=True)
    literal = root / "literal"
    literal.mkdir(exist_ok=True)
    (literal / "build.ninja").write_text(
        "".join(f"build obj/component_{i:06d}/source_file.obj: phony src/component_{i:06d}/source_file.cc\n"
                for i in range(n)) + "build all: phony " +
        " ".join(f"obj/component_{i:06d}/source_file.obj" for i in range(n)) + "\ndefault all\n")
    variables = root / "variables"
    variables.mkdir(exist_ok=True)
    (variables / "build.ninja").write_text(
        "src = src\nobj = obj\nrule cc\n  command = cc -Iinclude -DVERSION=1 -O2 -c $in -o $out\n" +
        "".join(f"build $obj/component_{i:06d}/file.obj: cc $src/component_{i:06d}/file.cc\n"
                f"  FLAGS = -DUNIT={i}\n" for i in range(n)))
    phony = root / "phony"
    phony.mkdir(exist_ok=True)
    (phony / "build.ninja").write_text(
        "".join(f"build p{i}: phony" + (f" p{i-1}" if i % 64 else "") + "\n"
                for i in range(n)) + "build all: phony " +
        " ".join(f"p{i}" for i in range(63, n, 64)) + "\ndefault all\n")
    files = root / "files"
    files.mkdir(exist_ok=True)
    count = min(n, 20000)
    for i in range(count):
        p = files / f"out{i}"
        if not p.exists():
            p.write_bytes(b"x")
    (files / "build.ninja").write_text(
        "rule existing\n  command = cmd /c exit 0\n" if os.name == "nt" else
        "rule existing\n  command = true\n")
    with (files / "build.ninja").open("a") as f:
        f.write("".join(f"build out{i}: existing\n" for i in range(count)))
        f.write("build all: phony " + " ".join(f"out{i}" for i in range(count)) + "\ndefault all\n")
    # Let the reference compute its own command hash. Every edge has exactly
    # that command; timestamps are unnecessary for edges without inputs/restat.
    (files / ".ninja_log").unlink(missing_ok=True)
    run(reference, ["out0"], files)
    line = (files / ".ninja_log").read_text().splitlines()[-1].split("\t")
    (files / ".ninja_log").write_text("# ninja log v7\n" + "".join(
        f"0\t1\t{line[2]}\tout{i}\t{line[-1]}\n" for i in range(count)))
    for exe in (reference,):
        p = subprocess.run([str(exe), "-n"], cwd=files, capture_output=True, check=True)
        if b"no work to do" not in p.stdout:
            raise RuntimeError("file workload is dirty; invalid no-op benchmark")
    return [("startup", ["--version"], root),
            ("parse_literal", ["-t", "rules"], literal),
            ("parse_variables", ["-t", "rules"], variables),
            ("noop_phony", [], phony), ("noop_files", [], files)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sninja", type=Path, default=Path("build-release/sninja.exe"))
    ap.add_argument("--reference", type=Path, default=Path("build-release/ninja_reference.exe"))
    ap.add_argument("--edges", type=int, default=100000)
    ap.add_argument("--repeats", type=int, default=11)
    ap.add_argument("--work", type=Path, default=Path("build-release/bench-data"))
    ap.add_argument("--output", type=Path, default=Path("build-release/benchmark.json"))
    opt = ap.parse_args()
    executables = {"sninja": opt.sninja.resolve(), "ninja": opt.reference.resolve()}
    for exe in executables.values():
        if not exe.is_file():
            raise FileNotFoundError(exe)
    workloads = generate(opt.work.resolve(), opt.edges, executables["ninja"])
    result = {"platform": platform.platform(), "cpu": platform.processor(),
              "edges": opt.edges, "repeats": opt.repeats,
              "binaries": {k: {"path": str(v), "sha256": hashlib.sha256(v.read_bytes()).hexdigest()}
                           for k, v in executables.items()}, "workloads": {}}
    rng = random.Random(42)
    for name, args, cwd in workloads:
        samples = {k: [] for k in executables}
        for exe in executables.values():
            for _ in range(2):
                run(exe, args, cwd)
        for _ in range(opt.repeats):
            order = list(executables)
            rng.shuffle(order)
            for key in order:
                samples[key].append(run(executables[key], args, cwd))
        medians = {k: statistics.median(v) for k, v in samples.items()}
        speedup = medians["ninja"] / medians["sninja"]
        result["workloads"][name] = {"args": args, "samples_ms": samples,
                                    "median_ms": medians, "speedup": speedup}
        print(f"{name:18s} sninja {medians['sninja']:9.3f} ms | "
              f"ninja {medians['ninja']:9.3f} ms | {speedup:.3f}x", flush=True)
        opt.output.parent.mkdir(parents=True, exist_ok=True)
        opt.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
