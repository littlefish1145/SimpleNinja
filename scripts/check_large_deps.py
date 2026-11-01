"""Exercise real deps-log recovery beyond 2 GiB, using sparse zero-ID payloads."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def make_log(path, records):
    stride = 1 << 19
    prefix = b"# ninjadeps\n" + struct.pack("<I", 4)
    prefix += struct.pack("<I", 8) + b"out\0" + struct.pack("<I", 0xffffffff)
    complete_size = len(prefix) + records * stride
    with path.open("wb") as log:
        if os.name == "nt":
            import msvcrt
            ioctl = ctypes.windll.kernel32.DeviceIoControl
            ioctl.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p, ctypes.c_ulong,
                              ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p]
            returned = ctypes.c_ulong()
            if not ioctl(msvcrt.get_osfhandle(log.fileno()), 0x900c4, None, 0, None, 0,
                         ctypes.byref(returned), None):
                raise ctypes.WinError()
        log.truncate(complete_size + 5)
        log.seek(0)
        log.write(prefix)
        record_header = struct.pack("<IIQ", 0x80000000 | (stride - 4), 0, 1)
        for i in range(records):
            log.seek(len(prefix) + i * stride)
            log.write(record_header)
        log.seek(complete_size)
        log.write(struct.pack("<I", 0x8000000c) + b"\0")
    return complete_size


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--sninja", type=Path, default=ROOT / "build-release/sninja.exe")
    ap.add_argument("--reference", type=Path)
    ap.add_argument("--records", type=int, default=4097)
    ap.add_argument("--work", type=Path, default=ROOT / "build-release/large-log-check")
    ap.add_argument("--output", type=Path, default=ROOT / "build-release/large-log-recovery.json")
    opt = ap.parse_args()
    if opt.records < 4096:
        ap.error("at least 4096 records are required to cross the 2 GiB boundary")
    opt.work.mkdir(parents=True, exist_ok=True)
    tools = {"sninja": opt.sninja.resolve()}
    if opt.reference:
        tools["ninja"] = opt.reference.resolve()
    result = {"records": opt.records, "runs": {}}
    with tempfile.TemporaryDirectory(dir=opt.work.resolve(), prefix="large-log-") as temp:
        directory = Path(temp)
        (directory / "build.ninja").write_text("build out: phony\n")
        path = directory / ".ninja_deps"
        for name, exe in tools.items():
            expected_size = make_log(path, opt.records)
            start = time.perf_counter()
            previous_error_mode = ctypes.windll.kernel32.SetErrorMode(0x8003) if os.name == "nt" else None
            try:
                run = subprocess.run([str(exe), "-t", "query", "out"], cwd=directory,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=180)
            finally:
                if previous_error_mode is not None:
                    ctypes.windll.kernel32.SetErrorMode(previous_error_mode)
            elapsed = time.perf_counter() - start
            item = {"path": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest(),
                    "seconds": elapsed, "returncode": run.returncode,
                    "expected_size": expected_size, "actual_size": path.stat().st_size,
                    "stdout": run.stdout.decode(errors="replace"), "stderr": run.stderr.decode(errors="replace")}
            result["runs"][name] = item
            if name == "sninja":
                if run.returncode or item["actual_size"] != expected_size:
                    raise RuntimeError(f"Large log recovery failed: {item}")
                if b"premature end of file; recovering" not in run.stderr:
                    raise RuntimeError(f"Expected recovery warning: {item}")
            print(f"{name}: return={run.returncode}, size={item['actual_size']}, expected={expected_size}, {elapsed:.3f}s", flush=True)
    opt.output.parent.mkdir(parents=True, exist_ok=True)
    opt.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
