"""Compare real CMake C++ compilation, checking every exit code and output."""
import argparse
import hashlib
import json
import os
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


def run(exe, args, cwd, env):
    start = time.perf_counter_ns()
    p = subprocess.run([str(exe), *map(str, args)], cwd=cwd, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    elapsed = (time.perf_counter_ns() - start) / 1e6
    if p.returncode:
        raise RuntimeError(f"{exe} {args} failed in {cwd}:\n" + p.stdout.decode(errors="replace"))
    return elapsed, p.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sninja", type=Path, default=ROOT / "build-release/sninja.exe")
    ap.add_argument("--reference", type=Path, default=ROOT / "build-release/ninja_reference.exe")
    ap.add_argument("--units", type=int, default=120)
    ap.add_argument("--repeats", type=int, default=5)
    ap.add_argument("--jobs", type=int, nargs="+", default=[1, 4, 12, 24])
    ap.add_argument("--variant", action="append", default=[], metavar="NAME=LAUNCH_MODE")
    ap.add_argument("--extra", action="append", default=[], metavar="NAME=EXE")
    ap.add_argument("--probe-variant", action="append", default=[], metavar="NAME=MODE")
    ap.add_argument("--pch", action="store_true", help="precompile stable standard headers in the workload")
    ap.add_argument("--trace", type=Path, help="trace output root; requires a SNINJA_PROFILE_JOBS build")
    ap.add_argument("--work", type=Path, default=ROOT / "build-release/compile-bench")
    ap.add_argument("--output", type=Path, default=ROOT / "build-release/compilation.json")
    opt = ap.parse_args()
    env = developer_environment()
    env["NINJA_STATUS"] = "[%f/%t] "
    tools = {"sninja": opt.sninja.resolve(), "ninja": opt.reference.resolve()}
    environments = {key: env.copy() for key in tools}
    for extra in opt.extra:
        name, path = extra.split("=", 1)
        tools[name] = Path(path).resolve()
        environments[name] = env.copy()
    for variant in opt.variant:
        name, mode = variant.split("=", 1)
        tools[name] = tools["sninja"]
        environments[name] = dict(env, SNINJA_ASYNC_LAUNCH=mode)
    for variant in opt.probe_variant:
        name, mode = variant.split("=", 1)
        tools[name] = tools["sninja"]
        environments[name] = dict(env, SNINJA_NOOP_PROBE=mode)
    project = opt.work.resolve()
    project.mkdir(parents=True, exist_ok=True)
    sources = []
    for i in range(opt.units):
        name = f"unit{i}.cpp"
        sources.append(name)
        (project / name).write_text('#include "shared.h"\n' +
            ('#include "local.h"\n' if i == 0 else '') +
            f'int unit{i}() {{ return calculate({i})' + (' + LOCAL_OFFSET' if i == 0 else '') + '; }\n')
    header = project / "shared.h"
    header_body = ("#pragma once\n#define SHARED_OFFSET @OFFSET@\n#include <array>\n#include <algorithm>\n#include <numeric>\n"
                      "inline int calculate(int x) { std::array<int, 32> data{}; "
                      "std::iota(data.begin(), data.end(), x); std::sort(data.begin(), data.end()); "
                      "return std::accumulate(data.begin(), data.end(), 0) + SHARED_OFFSET; }\n")
    shared_offset = local_offset = 0
    header.write_text(header_body.replace("@OFFSET@", "0"))
    local = project / "local.h"
    local.write_text("#pragma once\n#define LOCAL_OFFSET 0\n")
    (project / "main.cpp").write_text('#include <cstdio>\n' +
        "\n".join(f"int unit{i}();" for i in range(opt.units)) +
        '\nint main() { long long sum = 0;\n' +
        "\n".join(f"sum += unit{i}();" for i in range(opt.units)) +
        '\nstd::printf("%lld\\n", sum); return 0; }\n')
    (project / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.24)\n"
        "project(compile_bench LANGUAGES CXX)\n" +
        f'include("{(ROOT / "cmake/DetectIncludes.cmake").as_posix()}")\n' +
        "add_library(units STATIC " + " ".join(sources) +
        ")\n" + ("target_precompile_headers(units PRIVATE <array> <algorithm> <numeric>)\n" if opt.pch else "") +
        "add_executable(sample main.cpp)\ntarget_link_libraries(sample PRIVATE units)\n")
    build = project / "build"
    run("cmake", ["-S", project, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                  f"-DCMAKE_MAKE_PROGRAM={tools['ninja']}"], project, env)
    plans = [run(t, ["-t", "commands"], build, env)[1] for t in tools.values()]
    if any(plan != plans[0] for plan in plans[1:]):
        raise RuntimeError("The tools disagree on compiler commands")
    base_expected = sum(32 * i + 496 for i in range(opt.units))
    sample = build / ("sample.exe" if os.name == "nt" else "sample")
    trace_directory = opt.trace.resolve() / ("run-" + str(time.time_ns())) if opt.trace else None
    if trace_directory:
        trace_directory.mkdir(parents=True)
    result = {"units": opt.units, "repeats": opt.repeats, "jobs": opt.jobs, "pch": opt.pch,
              "trace_directory": str(trace_directory) if trace_directory else None,
              "command_sha256": hashlib.sha256(plans[0]).hexdigest(),
              "launch_modes": {k: v.get("SNINJA_ASYNC_LAUNCH", "default") for k, v in environments.items()},
              "probe_modes": {k: v.get("SNINJA_NOOP_PROBE", "default") for k, v in environments.items()},
              "binaries": {k: {"path": str(v), "sha256": hashlib.sha256(v.read_bytes()).hexdigest()}
                           for k, v in tools.items()}, "workloads": {}}
    rng = random.Random(96)
    def measure_case(name, args, prepare, must_build, exact_edges=None):
        samples = {key: [] for key in tools}
        edge_counts = {key: [] for key in tools}
        for repeat in range(opt.repeats):
            order = list(tools)
            rng.shuffle(order)
            for key in order:
                prepare(tools[key])
                case_env = environments[key]
                if trace_directory:
                    case_env = dict(case_env, SNINJA_JOB_TRACE=str(trace_directory / f"{name}-{key}-{repeat}"))
                elapsed, stdout = run(tools[key], args, build, case_env)
                edges = len(re.findall(rb"(?m)^\[\d+/\d+\]", stdout))
                if must_build and edges < opt.units:
                    raise RuntimeError(f"Expected at least {opt.units} build edges: {stdout!r}")
                if not must_build and exact_edges is None and b"no work to do" not in stdout:
                    raise RuntimeError(f"No-op performed work: {stdout!r}")
                if exact_edges is not None and edges != exact_edges:
                    raise RuntimeError(f"Expected exactly {exact_edges} edges, got {edges}: {stdout!r}")
                expected = str(base_expected + shared_offset * opt.units + local_offset).encode()
                if run(sample, [], build, env)[1].strip() != expected:
                    raise RuntimeError("Compiled program produced incorrect results")
                samples[key].append(elapsed)
                edge_counts[key].append(edges)
        medians = {k: statistics.median(v) for k, v in samples.items()}
        speedup = medians["ninja"] / medians["sninja"]
        result["workloads"][name] = {"samples_ms": samples, "edge_counts": edge_counts,
                                     "median_ms": medians, "speedup": speedup}
        print(f"{name:18} " + " | ".join(f"{key} {value:9.3f} ms" for key, value in medians.items()) +
              f" | reference/sninja {speedup:.3f}x", flush=True)
        opt.output.parent.mkdir(parents=True, exist_ok=True)
        opt.output.write_text(json.dumps(result, indent=2) + "\n")

    def clean(exe):
        run(exe, ["-t", "clean"], build, env)
        for filename in (".ninja_log", ".ninja_deps"):
            (build / filename).unlink(missing_ok=True)

    def touch(exe):
        nonlocal shared_offset
        shared_offset += 1
        header.write_text(header_body.replace("@OFFSET@", str(shared_offset)))

    def change_local(exe):
        nonlocal local_offset
        local_offset += 1
        local.write_text(f"#pragma once\n#define LOCAL_OFFSET {local_offset}\n")

    for jobs in opt.jobs:
        measure_case(f"clean_j{jobs}", ["-j", jobs], clean, True)
    measure_case("incremental_header", ["-j", max(opt.jobs)], touch, True)
    measure_case("incremental_local", ["-j", max(opt.jobs)], change_local, False, exact_edges=3)
    measure_case("noop_compiled", [], lambda exe: None, False)


if __name__ == "__main__":
    main()
