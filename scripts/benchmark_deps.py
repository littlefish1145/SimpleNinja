"""Large native Ninja deps logs: validate no-op semantics, time and peak memory."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]


def run(exe, args, directory, probe_mode=None, flush_mode=None):
    start = time.perf_counter_ns()
    env = dict(os.environ, NINJA_STATUS="[%f/%t] ")
    if probe_mode is not None:
        env["SNINJA_NOOP_PROBE"] = probe_mode
    if flush_mode is not None:
        env["SNINJA_RECOMPACT_FLUSH"] = flush_mode
    with subprocess.Popen([str(exe), *args], cwd=directory, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE,
                          env=env) as process:
        stdout, stderr = process.communicate(timeout=180)
        sample = {"wall_ms": (time.perf_counter_ns() - start) / 1e6}
        if os.name == "nt":
            creation, exit_time, kernel, user = (ctypes.c_uint64() for _ in range(4))
            get_times = ctypes.windll.kernel32.GetProcessTimes
            get_times.argtypes = [ctypes.c_void_p] + [ctypes.POINTER(ctypes.c_uint64)] * 4
            if not get_times(int(process._handle), ctypes.byref(creation), ctypes.byref(exit_time),
                             ctypes.byref(kernel), ctypes.byref(user)):
                raise ctypes.WinError()
            sample["cpu_ms"] = (kernel.value + user.value) / 10000

            class MemoryCounters(ctypes.Structure):
                _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong)] + [
                    (name, ctypes.c_size_t) for name in (
                        "PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                        "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                        "PagefileUsage", "PeakPagefileUsage")]
            counters = MemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            get_memory = ctypes.windll.psapi.GetProcessMemoryInfo
            get_memory.argtypes = [ctypes.c_void_p, ctypes.POINTER(MemoryCounters), ctypes.c_ulong]
            if not get_memory(int(process._handle), ctypes.byref(counters), counters.cb):
                raise ctypes.WinError()
            sample["peak_working_set_bytes"] = counters.PeakWorkingSetSize

            class IoCounters(ctypes.Structure):
                _fields_ = [(name, ctypes.c_uint64) for name in (
                    "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
                    "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]
            io = IoCounters()
            get_io = ctypes.windll.kernel32.GetProcessIoCounters
            get_io.argtypes = [ctypes.c_void_p, ctypes.POINTER(IoCounters)]
            if not get_io(int(process._handle), ctypes.byref(io)):
                raise ctypes.WinError()
            sample.update(io_read_operations=io.ReadOperationCount,
                          io_write_operations=io.WriteOperationCount,
                          io_read_bytes=io.ReadTransferCount,
                          io_write_bytes=io.WriteTransferCount)
        if process.returncode:
            raise RuntimeError(f"{exe}: {process.returncode}\n{stdout!r}\n{stderr!r}")
    return sample, stdout, stderr


def generate(directory, count, fanout, reference, header_count):
    directory.mkdir(parents=True, exist_ok=True)
    outputs = [f"out{i:06d}" for i in range(count)]
    headers = [f"header{i:06d}.h" for i in range(max(fanout, header_count))]
    # Every command is identical so the reference can supply its command hash
    # and exact platform timestamp. Header timestamps precede all outputs.
    timestamp = time.time_ns() - 10_000_000_000
    for names, stamp in ((headers, timestamp), (outputs, timestamp + 1_000_000_000)):
        for name in names:
            path = directory / name
            if not path.exists():
                path.write_bytes(b"x")
            os.utime(path, ns=(stamp, stamp))
    command = "cmd /c exit 0" if os.name == "nt" else "true"
    manifest = f"rule cc\n  command = {command}\n  deps = gcc\n  depfile = $out.d\n"
    manifest += "".join(f"build {name}: cc\n" for name in outputs)
    manifest += "build all: phony " + " ".join(outputs) + "\ndefault all\n"
    (directory / "build.ninja").write_text(manifest)
    for name in (".ninja_log", ".ninja_deps"):
        (directory / name).unlink(missing_ok=True)
    run(reference, [outputs[0]], directory)
    record = (directory / ".ninja_log").read_text().splitlines()[-1].split("\t")
    mtime = int(record[2])
    (directory / ".ninja_log").write_text("# ninja log v7\n" + "".join(
        f"0\t1\t{mtime}\t{name}\t{record[-1]}\n" for name in outputs))
    with (directory / ".ninja_deps").open("wb") as log:
        log.write(b"# ninjadeps\n" + struct.pack("<i", 4))
        for index, name in enumerate(outputs + headers):
            path = name.encode()
            path += b"\0" * (-len(path) % 4)
            log.write(struct.pack("<I", len(path) + 4) + path + struct.pack("<I", ~index & 0xffffffff))
        for index in range(count):
            ids = [count + (index * 17 + i) % len(headers) for i in range(fanout)]
            log.write(struct.pack("<IIQ", 0x80000000 | 4 * (3 + fanout), index, mtime))
            log.write(struct.pack(f"<{fanout}I", *ids))
    return {"edges": count, "headers": len(headers), "deps_per_edge": fanout,
            "deps_bytes": (directory / ".ninja_deps").stat().st_size}


def check_invalidation(tools, directory, info, probe_modes, flush_modes):
    header = directory / "header000000.h"
    stamp = header.stat()
    expected = sum((-index * 17) % info["headers"] < info["deps_per_edge"]
                   for index in range(info["edges"]))
    try:
        now = time.time_ns()
        os.utime(header, ns=(now, now))
        plans = []
        for key, exe in tools.items():
            _, stdout, stderr = run(exe, ["-n", "-j1"], directory,
                                    probe_modes.get(key), flush_modes.get(key))
            if stderr or len(stdout.splitlines()) != expected:
                raise RuntimeError(f"{key}: expected {expected} invalidated edges, got {stdout!r} {stderr!r}")
            plans.append(stdout)
        if any(plan != plans[0] for plan in plans[1:]):
            raise RuntimeError("The tools disagree on dependency invalidation")
    finally:
        os.utime(header, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
    return expected


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sninja", type=Path, default=ROOT / "build-release/sninja.exe")
    ap.add_argument("--reference", type=Path, default=ROOT / "build-release/ninja_reference.exe")
    ap.add_argument("--extra", action="append", default=[], metavar="NAME=EXE")
    ap.add_argument("--probe-variant", action="append", default=[], metavar="NAME=MODE")
    ap.add_argument("--flush-variant", action="append", default=[], metavar="NAME=MODE",
                    help="same binary: 0=buffered recompact, 1=flush each record")
    ap.add_argument("--edges", type=int, default=20000)
    ap.add_argument("--fanout", type=int, nargs="+", default=[8, 64, 256])
    ap.add_argument("--headers", type=int, default=1024, help="distinct dependency paths")
    ap.add_argument("--repeats", type=int, default=15)
    ap.add_argument("--recompact", action="store_true", help="also measure log rewriting")
    ap.add_argument("--late-dirty", action="store_true", help="dry-run with only the final output missing")
    ap.add_argument("--work", type=Path, default=ROOT / "build-release/deps-bench")
    ap.add_argument("--output", type=Path, default=ROOT / "build-release/deps.json")
    opt = ap.parse_args()
    if min(opt.edges, opt.headers, opt.repeats) < 1 or min(opt.fanout) < 1 or max(opt.fanout) > 131068:
        ap.error("edges, headers and repeats must be positive; fanout must be 1..131068")
    if opt.late_dirty and opt.recompact:
        ap.error("--late-dirty and --recompact measure different cases")
    tools = {"sninja": opt.sninja.resolve(), "ninja": opt.reference.resolve()}
    for extra in opt.extra:
        name, path = extra.split("=", 1)
        tools[name] = Path(path).resolve()
    probe_modes = {}
    for variant in opt.probe_variant:
        name, mode = variant.split("=", 1)
        tools[name] = tools["sninja"]
        probe_modes[name] = mode
    flush_modes = {}
    for variant in opt.flush_variant:
        name, mode = variant.split("=", 1)
        if mode not in ("0", "1"):
            ap.error("--flush-variant MODE must be 0 or 1")
        tools[name] = tools["sninja"]
        flush_modes[name] = mode
    result = {"platform": platform.platform(), "repeats": opt.repeats,
              "probe_modes": probe_modes,
              "flush_modes": flush_modes,
              "binaries": {key: {"path": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
                           for key, exe in tools.items()}, "workloads": {}}
    rng = random.Random(310)
    for fanout in opt.fanout:
        directory = opt.work.resolve() / f"deps{fanout}"
        info = generate(directory, opt.edges, fanout, tools["ninja"], opt.headers)
        last_output = directory / f"out{opt.edges-1:06d}"
        last_stat = last_output.stat()
        if opt.late_dirty:
            last_output.unlink()
        digest = hashlib.sha256((directory / ".ninja_deps").read_bytes()).hexdigest()
        samples = {key: [] for key in tools}
        for repeat in range(opt.repeats + 2):
            order = list(tools)
            rng.shuffle(order)
            for key in order:
                args = ["-n", "-j1"] if opt.late_dirty else []
                sample, stdout, stderr = run(tools[key], args, directory,
                                            probe_modes.get(key), flush_modes.get(key))
                valid = len(stdout.splitlines()) == 1 and b"[1/1]" in stdout if opt.late_dirty else b"no work to do" in stdout
                if not valid or stderr:
                    raise RuntimeError(f"Invalid build plan {key}: {stdout!r} {stderr!r}")
                if repeat >= 2:
                    samples[key].append(sample)
        if hashlib.sha256((directory / ".ninja_deps").read_bytes()).hexdigest() != digest:
            raise RuntimeError("The no-op unexpectedly modified the deps log")
        if opt.late_dirty:
            last_output.write_bytes(b"x")
            os.utime(last_output, ns=(last_stat.st_atime_ns, last_stat.st_mtime_ns))
        info["invalidated_edges_checked"] = check_invalidation(tools, directory, info, probe_modes, flush_modes)
        medians = {key: {metric: statistics.median(s[metric] for s in runs)
                         for metric in runs[0]} for key, runs in samples.items()}
        case = f"late_dirty{fanout}" if opt.late_dirty else f"deps{fanout}"
        result["workloads"][case] = dict(info, samples=samples, medians=medians)
        print(f"fanout={fanout}: " + " | ".join(
            f"{key} {m['wall_ms']:.3f}ms peak={m.get('peak_working_set_bytes', 0)/1048576:.1f}MiB"
            for key, m in medians.items()), flush=True)
        opt.output.parent.mkdir(parents=True, exist_ok=True)
        opt.output.write_text(json.dumps(result, indent=2) + "\n")
        if opt.recompact:
            rewrite_samples = {key: [] for key in tools}
            for repeat in range(opt.repeats + 2):
                order = list(tools)
                rng.shuffle(order)
                for key in order:
                    sample, stdout, stderr = run(tools[key], ["-t", "recompact"], directory,
                                                probe_modes.get(key), flush_modes.get(key))
                    if stdout or stderr:
                        raise RuntimeError(f"Unexpected recompact output: {stdout!r} {stderr!r}")
                    # Every writer must leave a log the other tools can use.
                    for reader_name, reader in tools.items():
                        _, stdout, stderr = run(reader, [], directory,
                                                probe_modes.get(reader_name), flush_modes.get(reader_name))
                        if b"no work to do" not in stdout or stderr:
                            raise RuntimeError("Recompacted logs are not interoperable")
                    if repeat >= 2:
                        rewrite_samples[key].append(sample)
            rewrite_medians = {key: {metric: statistics.median(s[metric] for s in runs)
                                    for metric in runs[0]} for key, runs in rewrite_samples.items()}
            checked = check_invalidation(tools, directory, info, probe_modes, flush_modes)
            result["workloads"][f"recompact{fanout}"] = dict(
                info, invalidated_edges_checked=checked, samples=rewrite_samples, medians=rewrite_medians)
            print(f"recompact fanout={fanout}: " + " | ".join(
                f"{key} {m['wall_ms']:.3f}ms" for key, m in rewrite_medians.items()), flush=True)
            opt.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
