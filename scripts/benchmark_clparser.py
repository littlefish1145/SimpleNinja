"""Compare MSVC output parsing time and exact result digests with upstream Ninja."""
import argparse
import hashlib
import json
from pathlib import Path
import random
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sninja", type=Path, default=ROOT / "build-release/sninja_clparser_bench.exe")
    ap.add_argument("--reference", type=Path, default=ROOT / "build-release/ninja_clparser_bench.exe")
    ap.add_argument("--repeats", type=int, default=21)
    ap.add_argument("--lines", type=int, default=100000)
    ap.add_argument("--work", type=Path, default=ROOT / "build-release/clparser-fixtures")
    ap.add_argument("--output", type=Path, default=ROOT / "build-release/clparser.json")
    opt = ap.parse_args()
    opt.work.mkdir(parents=True, exist_ok=True)
    tools = {"sninja": opt.sninja.resolve(), "ninja": opt.reference.resolve()}
    result = {"repeats": opt.repeats, "lines": opt.lines, "binaries": {
        k: {"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
        for k, p in tools.items()}, "workloads": {}}
    rng = random.Random(918)
    for case, prefix in [("project", b"Note: including file: "),
                         ("mixed_system", b"Note: including file: "),
                         ("custom_prefix", b"Dependency: "),
                         ("normalization", b"Note: including file: ")]:
        data = bytearray(b"UNIT.CpP\r\n")
        for i in range(opt.lines):
            if case == "normalization":
                path = ["C:/", "C:/src/project/", "C:/SRC/project/../include/h.h",
                        "../include/h.h", ".", "C:/src//project/./h.h",
                        "//server/share/h.h", "D:/sdk/h.h", "C:/src/project/h.h"][i % 9]
            elif case == "mixed_system" and i % 3:
                path = f"C:/Program Files/Microsoft Visual Studio/VC/include/sdk{i % 2048}.h"
            else:
                path = f"C:/src/project/include/component{i % 64}/header{i % 2048}.h"
            data += prefix + b" " * (i % 4) + path.encode() + (b"\r\n" if i % 2 else b"\n")
        # Preserve diagnostics, embedded NULs, empty lines and the final partial line.
        data += b"\r\nwarning\x00with payload\rsomething.CPP\n" + prefix + b"   \nlast"
        fixture = opt.work / (case + ".txt")
        fixture.write_bytes(data)
        samples = {key: [] for key in tools}
        expected = None
        for repeat in range(opt.repeats + 2):
            order = list(tools)
            rng.shuffle(order)
            for key in order:
                p = subprocess.run([str(tools[key]), str(fixture.resolve()), prefix.decode()],
                                   cwd=ROOT, capture_output=True, check=True, timeout=60)
                row = json.loads(p.stdout)
                signature = {k: v for k, v in row.items() if k != "parse_ms"}
                if expected is None:
                    expected = signature
                if signature != expected:
                    raise RuntimeError(f"Parser result mismatch: {case}, {key}: {signature} != {expected}")
                if repeat >= 2:
                    samples[key].append(row["parse_ms"])
        medians = {k: statistics.median(v) for k, v in samples.items()}
        result["workloads"][case] = {"samples_ms": samples, "median_ms": medians,
            "speedup": medians["ninja"] / medians["sninja"], "result": expected,
            "input_sha256": hashlib.sha256(data).hexdigest(), "input_bytes": len(data)}
        print(f"{case}: sninja {medians['sninja']:.3f} ms, ninja {medians['ninja']:.3f} ms; "
              f"{medians['ninja'] / medians['sninja']:.3f}x", flush=True)
    opt.output.parent.mkdir(parents=True, exist_ok=True)
    opt.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
