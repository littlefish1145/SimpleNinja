"""Native short jobs: paired timings, child timelines, parent CPU, verified outputs."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def run(exe, args, cwd, env):
    start = time.perf_counter_ns()
    with subprocess.Popen([str(exe), *map(str, args)], cwd=cwd, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
        out, _ = process.communicate(timeout=120)
        elapsed = (time.perf_counter_ns() - start) / 1e6
        cpu_ms = None
        if os.name == "nt":
            creation, exit_time, kernel, user = (ctypes.c_uint64() for _ in range(4))
            get_times = ctypes.windll.kernel32.GetProcessTimes
            get_times.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_uint64)] * 4
            if not get_times(int(process._handle), ctypes.byref(creation), ctypes.byref(exit_time),
                             ctypes.byref(kernel), ctypes.byref(user)):
                raise ctypes.WinError()
            cpu_ms = (kernel.value + user.value) / 10000
        if process.returncode:
            raise RuntimeError(f"{exe} {args}: {process.returncode}\n{out.decode(errors='replace')}")
    return {"wall_ms": elapsed, "parent_cpu_ms": cpu_ms}, out


def timeline(directory, count, chain):
    events, records = [], []
    for index in range(count):
        data = (directory / f"job{index}").read_bytes()
        if len(data) != 48:
            raise RuntimeError(f"Incomplete job {index}: {len(data)} bytes")
        start, end, recorded_index, checksum, frequency, pid, magic = struct.unpack("<QQQQQII", data)
        if magic != 0x534e4a42 or recorded_index != index or end < start or not pid:
            raise RuntimeError(f"Bad job record {index}")
        if chain and index >= chain and start < records[index-chain][1]:
            raise RuntimeError(f"Dependency ran before its input finished: job{index}")
        records.append((start, end))
        events += [(start, 1), (end, -1)]
    active = peak = 0
    for tick, delta in sorted(events):
        active += delta
        peak = max(peak, active)
    starts = sorted(start for start, _ in records)
    gaps = [(b-a) * 1e6 / frequency for a, b in zip(starts, starts[1:])]
    return {"child_span_ms": (max(end for _, end in records) - starts[0]) * 1000 / frequency,
            "median_start_gap_us": statistics.median(gaps) if gaps else 0,
            "peak_busy_children": peak,
            "sum_child_busy_ms": sum(end-start for start, end in records) * 1000 / frequency}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sninja", type=Path, default=ROOT / "build-release/sninja.exe")
    ap.add_argument("--reference", type=Path, default=ROOT / "build-release/ninja_reference.exe")
    ap.add_argument("--helper", type=Path, default=ROOT / "build-release/sninja_job.exe")
    ap.add_argument("--extra", action="append", default=[], metavar="NAME=EXE")
    ap.add_argument("--variant", action="append", default=[], metavar="NAME=ENV_VALUE",
                    help="sninja variant with SNINJA_ASYNC_LAUNCH set to ENV_VALUE")
    ap.add_argument("--count", type=int, default=256)
    ap.add_argument("--jobs", type=int, nargs="+", default=[1, 2, 4, 8, 24])
    ap.add_argument("--work-us", type=int, nargs="+", default=[0, 5000])
    ap.add_argument("--chain", type=int, default=0, help="dependency lanes; 0 means independent jobs")
    ap.add_argument("--repeats", type=int, default=7)
    ap.add_argument("--work", type=Path, default=ROOT / "build-release/jobs-bench")
    ap.add_argument("--output", type=Path, default=ROOT / "build-release/jobs.json")
    opt = ap.parse_args()
    tools = {"sninja": opt.sninja.resolve(), "ninja": opt.reference.resolve()}
    for extra in opt.extra:
        name, exe = extra.split("=", 1)
        tools[name] = Path(exe).resolve()
    base_env = os.environ.copy()
    base_env["NINJA_STATUS"] = "[%f/%t] "
    variants = {key: base_env.copy() for key in tools}
    for extra in opt.variant:
        name, value = extra.split("=", 1)
        tools[name] = tools["sninja"]
        variants[name] = dict(base_env, SNINJA_ASYNC_LAUNCH=value)
    result = {"count": opt.count, "chain": opt.chain, "repeats": opt.repeats,
        "binaries": {key: {"path": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
                     for key, exe in tools.items()},
        "helper_sha256": hashlib.sha256(opt.helper.read_bytes()).hexdigest(), "workloads": {}}
    result["launch_modes"] = {key: env.get("SNINJA_ASYNC_LAUNCH", "default") for key, env in variants.items()}
    opt.work.mkdir(parents=True, exist_ok=True)
    rng = random.Random(418)
    command = str(opt.helper.resolve()).replace("\\", "/").replace("$", "$$")
    for work_us in opt.work_us:
        directory = opt.work.resolve() / f"work{work_us}_chain{opt.chain}"
        directory.mkdir(exist_ok=True)
        lines = ["rule job", f'  command = "{command}" $out {work_us} $index']
        for index in range(opt.count):
            dependency = f" job{index-opt.chain}" if opt.chain and index >= opt.chain else ""
            lines += [f"build job{index}: job{dependency}", f"  index = {index}"]
        lines += ["build all: phony " + " ".join(f"job{i}" for i in range(opt.count)), "default all"]
        (directory / "build.ninja").write_text("\n".join(lines) + "\n", encoding="utf8")
        for jobs in opt.jobs:
            samples = {key: [] for key in tools}
            for repeat in range(opt.repeats + 1):
                order = list(tools)
                rng.shuffle(order)
                for key in order:
                    exe, env = tools[key], variants[key]
                    run(exe, ["-t", "clean"], directory, env)
                    (directory / ".ninja_log").unlink(missing_ok=True)
                    sample, out = run(exe, ["-j", jobs], directory, env)
                    sample.update(timeline(directory, opt.count, opt.chain))
                    entries = [line.split("\t") for line in (directory / ".ninja_log").read_text().splitlines()
                               if line and not line.startswith("#")]
                    if len(entries) != opt.count:
                        raise RuntimeError(f"Expected {opt.count} log records, got {len(entries)}")
                    sample["build_log_ms"] = max(int(e[1]) for e in entries) - min(int(e[0]) for e in entries)
                    if repeat:
                        samples[key].append(sample)
            medians = {key: {metric: statistics.median(s[metric] for s in runs if s[metric] is not None)
                             for metric in runs[0] if runs[0][metric] is not None}
                       for key, runs in samples.items()}
            result["workloads"][f"work{work_us}_j{jobs}"] = {"samples": samples, "medians": medians}
            print(f"work={work_us}us j={jobs}: " + " | ".join(
                f"{key} {values['wall_ms']:.2f}ms cpu={values.get('parent_cpu_ms', 0):.1f}ms "
                f"gap={values['median_start_gap_us']:.0f}us peak={values['peak_busy_children']}"
                for key, values in medians.items()), flush=True)
            opt.output.parent.mkdir(parents=True, exist_ok=True)
            opt.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf8")


if __name__ == "__main__":
    main()
