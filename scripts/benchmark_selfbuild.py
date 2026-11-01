"""Compare optional PCH for sninja's own build using the same compiler and engine."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from configure import developer_environment


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cxx", required=True)
    ap.add_argument("--sninja", type=Path, default=ROOT / "build-release/sninja.exe")
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--work", type=Path, default=ROOT / "build-release/selfbuild-bench")
    ap.add_argument("--output", type=Path, default=ROOT / "build-release/selfbuild.json")
    opt = ap.parse_args()
    env = developer_environment()
    env["NINJA_STATUS"] = "[%f/%t] "
    exe = opt.sninja.resolve()
    def source_hash():
        digest = hashlib.sha256()
        paths = [ROOT / "configure.py", *sorted((ROOT / "src/engine").glob("*")),
                 *sorted((ROOT / "cmake").glob("engine-*.txt"))]
        for path in paths:
            if path.is_file():
                digest.update(path.relative_to(ROOT).as_posix().encode())
                digest.update(path.read_bytes())
        return digest.hexdigest()
    initial_source_hash = source_hash()
    def run(args, cwd):
        start = time.perf_counter_ns()
        p = subprocess.run(list(map(str, args)), cwd=cwd, env=env, capture_output=True, timeout=300)
        if p.returncode:
            raise RuntimeError(f"{args}:\n{p.stdout.decode(errors='replace')}\n{p.stderr.decode(errors='replace')}")
        return (time.perf_counter_ns() - start) / 1e6, p.stdout
    directories = {k: (opt.work / k).resolve() for k in ("plain", "pch")}
    commands = {}
    for mode, directory in directories.items():
        run([sys.executable, ROOT / "configure.py", "--builddir", directory,
             "--cxx", opt.cxx] + (["--pch"] if mode == "pch" else []), ROOT)
        commands[mode] = hashlib.sha256(run([exe, "-t", "commands"], directory)[1]).hexdigest()
    samples = {mode: [] for mode in directories}
    rng = random.Random(181)
    for repeat in range(opt.repeats + 1):
        order = list(directories)
        rng.shuffle(order)
        for mode in order:
            directory = directories[mode]
            run([exe, "-t", "clean"], directory)
            for name in (".ninja_log", ".ninja_deps"):
                (directory / name).unlink(missing_ok=True)
            elapsed, output = run([exe, "-j", opt.jobs], directory)
            edges = len(re.findall(rb"(?m)^\[\d+/\d+\]", output))
            if edges != (41 if mode == "pch" else 40):
                raise RuntimeError(f"Unexpected build edges: {mode}: {edges}\n{output!r}")
            binary = directory / ("sninja.exe" if sys.platform == "win32" else "sninja")
            if run([binary, "--version"], directory)[1] != run([exe, "--version"], directory)[1]:
                raise RuntimeError("Self-built engine returned the wrong version")
            if b"no work to do" not in run([binary], directory)[1]:
                raise RuntimeError("Self-built engine did not reach a clean state")
            if repeat:
                samples[mode].append(elapsed)
            print(f"{mode} repeat {repeat}: {elapsed:.3f} ms" + (" (warmup)" if not repeat else ""), flush=True)
    medians = {k: statistics.median(v) for k, v in samples.items()}
    if source_hash() != initial_source_hash:
        raise RuntimeError("Sources changed during the benchmark; discard the timing samples")
    result = {"compiler": opt.cxx, "jobs": opt.jobs, "repeats": opt.repeats,
              "source_sha256": initial_source_hash,
              "engine_sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
              "command_sha256": commands, "samples_ms": samples, "median_ms": medians,
              "speedup": medians["plain"] / medians["pch"]}
    opt.output.parent.mkdir(parents=True, exist_ok=True)
    opt.output.write_text(json.dumps(result, indent=2) + "\n")
    print(f"PCH speedup {result['speedup']:.3f}x", flush=True)


if __name__ == "__main__":
    main()
