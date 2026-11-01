"""Check sninja against the vendored native Ninja, including incremental state."""
import argparse
import json
import os
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import time
import unittest

TOOLS = [Path("build-release/sninja.exe").resolve(), Path("build-release/ninja_reference.exe").resolve()]

HELPER = r'''
import pathlib, sys
action, out, *args = sys.argv[1:]
p = pathlib.Path(out)
p.parent.mkdir(parents=True, exist_ok=True)
def write(data):
    p.write_text(data)
if action == 'copy':
    write(pathlib.Path(args[0]).read_text())
elif action == 'restat':
    data = pathlib.Path(args[0]).read_text()
    if not p.exists() or p.read_text() != data:
        write(data)
elif action == 'rsp':
    write(pathlib.Path(out + '.rsp').read_text())
elif action == 'gcc':
    write(pathlib.Path(args[0]).read_text() + pathlib.Path('header.h').read_text())
    pathlib.Path(out + '.d').write_text(out + ': ' + args[0] + ' header.h\n', encoding='utf8')
elif action == 'msvc':
    write(pathlib.Path(args[0]).read_text() + pathlib.Path('header.h').read_text())
    print('Note: including file: ' + str(pathlib.Path('header.h').resolve()))
elif action == 'dd':
    write('ninja_dyndep_version = 1\nbuild result | implicit.out: dyndep | generated.h\n')
elif action == 'compile-dd':
    data = pathlib.Path('generated.h').read_text()
    write(data)
    pathlib.Path('implicit.out').write_text(data)
elif action == 'validate':
    write('validated')
elif action == 'fail':
    print('expected failure')
    sys.exit(1)
else:
    raise ValueError(action)
with pathlib.Path('events').open('a') as f:
    f.write(action + ':' + out + '\n')
'''


class Compatibility(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="sninja differential ")
        self.root = Path(self.temp.name)
        self.py = '"' + sys.executable + '"'

    def tearDown(self):
        self.temp.cleanup()

    def directory(self, name, manifest, files=None):
        path = self.root / name
        path.mkdir(parents=True, exist_ok=True)
        (path / "helper.py").write_text(HELPER)
        (path / "build.ninja").write_text(manifest, encoding="utf8")
        for name, data in (files or {}).items():
            p = path / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(data, encoding="utf8")
        return path

    def run_tool(self, exe, directory, *args, expected=0, probe=None, flush=None):
        env = os.environ.copy()
        env.pop("NINJA_STATUS", None)
        env.pop("NINJA_FLAGS", None)
        if probe is not None:
            env["SNINJA_NOOP_PROBE"] = str(probe)
        if flush is not None:
            env["SNINJA_RECOMPACT_FLUSH"] = str(flush)
        p = subprocess.run([str(exe), *args], cwd=directory, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
        if expected is not None:
            self.assertEqual(expected, p.returncode, (args, p.stdout, p.stderr))
        return p

    def same(self, directory, *args):
        a, b = [self.run_tool(exe, directory, *args, expected=None) for exe in TOOLS]
        self.assertEqual((a.returncode, a.stdout, a.stderr), (b.returncode, b.stdout, b.stderr), args)

    def events(self, path):
        p = path / "events"
        return p.read_text().splitlines() if p.exists() else []

    def assert_noop(self, exe, directory):
        before = self.events(directory)
        p = self.run_tool(exe, directory)
        self.assertIn(b"no work to do", p.stdout)
        self.assertEqual(before, self.events(directory))

    def change(self, directory, name, text):
        # NTFS has sub-microsecond timestamps. Avoid future-dating inputs:
        # outputs from a real rebuild must be able to become newer than them.
        p = directory / name
        p.write_text(text)
        latest = max((q.stat().st_mtime_ns for q in directory.iterdir() if q.is_file()), default=0)
        stamp = max(time.time_ns(), latest + 1000)
        os.utime(p, ns=(stamp, stamp))

    def test_cli_and_random_manifests(self):
        self.same(self.root, "--version")
        self.same(self.root, "-t", "list")
        rng = random.Random(637)
        for trial in range(12):
            lines = ["base = global", "rule run", "  command = cc $flags $in -o $out",
                     "  description = building $out"]
            for i in range(80):
                ins = " ".join(f"item_{rng.randrange(i)}" for _ in range(3)) if i else "src$ file.cc"
                lines += [f"build item_{i} | extra_{i}: run {ins} || order_{i % 3}",
                          f"  flags = -DINDEX={i} -DNAME=$base"]
            lines += [f"build order_{i}: phony" for i in range(3)]
            lines += ["default item_79"]
            d = self.directory(f"random{trial}", "\n".join(lines) + "\n")
            for args in [("-t", "rules"), ("-t", "targets", "all"), ("-t", "targets", "rule", "run"),
                         ("-t", "commands"), ("-t", "inputs", "item_79"),
                         ("-t", "query", "item_79"), ("-t", "compdb"),
                         ("-t", "compdb-targets", "item_79"), ("-t", "targets", "depth", "3")]:
                self.same(d, *args)

    def test_cli_early_exits_and_parallel_defaults(self):
        for args in [("--version",), ("-j1", "--version"), ("--version", "--invalid"),
                     ("-d", "stats", "--version"), ("-t", "list"), ("-j2", "-t", "list"),
                     ("-d", "list"), ("-w", "list"), ("-d", "unknown"),
                     ("-w", "unknown"), ("--invalid",), ("-t", "unknown"),
                     ("-h",), ("-j1", "-h"), ("-j0", "--help"), ("-j7", "--help")]:
            with self.subTest(args=args):
                # getopt includes argv[0] in its own diagnostic prefix.
                observed = []
                for exe in TOOLS:
                    run = self.run_tool(exe, self.root, *args, expected=None)
                    observed.append((run.returncode, run.stdout,
                                     run.stderr.replace(os.fsencode(str(exe)), b"ninja")))
                self.assertEqual(observed[0], observed[1])

    def test_diagnostics(self):
        bad = ["rule r\n", "build", "build out: missing\n", "build x: phony\nbuild x: phony\n",
               "x = bad$!escape\n", "build a: phony\n\tbroken\n", "build abc: phony\rbroken\n",
               "default missing\n", "include missing.ninja\n", "pool p\n  depth = -1\n",
               "rule r\n  command = echo hi\n  rspfile = x\n", "build $missing: phony\n",
               "ninja_required_version = 1.13\nx = $^\n"]
        for i, text in enumerate(bad):
            d = self.directory(str(i), text)
            self.same(d, "-t", "targets")

    def test_scopes_escapes_and_include(self):
        d = self.directory("scope", "base = outer\ninclude common.ninja\nsubninja child.ninja\n"
                           "build top: r $base/input\ndefault top\n", {
            "common.ninja": "rule r\n  command = echo $base $in $out\n",
            "child.ninja": "base = child\nbuild escaped$ output: r $base/input\n  base = edge\n"})
        for args in [("-t", "commands", "top", "escaped output"), ("-t", "compdb"),
                     ("-t", "targets", "all"), ("-t", "inputs", "escaped output")]:
            self.same(d, *args)

    def test_response_file(self):
        manifest = f"rule rsp\n  command = {self.py} helper.py rsp $out\n" \
                   "  rspfile = $out.rsp\n  rspfile_content = $in_newline\n" \
                   "build result: rsp first$ input second$ input\n"
        for i, exe in enumerate(TOOLS):
            d = self.directory(str(i), manifest, {"first input": "a", "second input": "b"})
            self.run_tool(exe, d)
            self.assertEqual('"first input"\n"second input"', (d / "result").read_text())
            self.assertFalse((d / "result.rsp").exists())
            self.assert_noop(TOOLS[1-i], d)

    def test_invalid_executable_preserves_fatal_diagnostic(self):
        if os.name != "nt":
            self.skipTest("Windows CreateProcess error semantics")
        import ctypes
        # A deliberately malformed PE must return its error, not wait for a
        # system dialog under CTest's interactive error mode. Both engines
        # inherit the same mode; the production executables are unchanged.
        # https://learn.microsoft.com/windows/win32/api/errhandlingapi/nf-errhandlingapi-seterrormode
        error_mode = ctypes.windll.kernel32.SetErrorMode(0x8003)
        self.addCleanup(ctypes.windll.kernel32.SetErrorMode, error_mode)
        manifest = "rule invalid\n  command = invalid_program.exe\n" + \
                   f"rule copy\n  command = {self.py} helper.py copy $out $in\n" + \
                   "build first: copy input\nbuild bad: invalid\ndefault first bad\n"
        d = self.directory("bad_executable", manifest, {"input": "data", "invalid_program.exe": "not PE"})
        for jobs in (1, 4, 24):
            results = [self.run_tool(exe, d, "-j", str(jobs), expected=1) for exe in TOOLS]
            self.assertEqual(results[0].stderr, results[1].stderr)
            self.assertIn(b"CreateProcess", results[0].stderr)

    def test_large_captured_output(self):
        helper = TOOLS[0].parent / ("sninja_job.exe" if os.name == "nt" else "sninja_job")
        if not helper.is_file():
            self.skipTest("Native output helper was not built")
        manifest = f'rule emit\n  command = "{helper.as_posix()}" --emit 262144\n  description = capture\n'
        manifest += "".join(f"build out{i}: emit\n" for i in range(8))
        d = self.directory("large_output", manifest)
        results = [self.run_tool(exe, d, "--quiet", "-j", "8") for exe in TOOLS]
        self.assertEqual(results[0].stdout, results[1].stdout)
        self.assertEqual(8 * 262144, results[0].stdout.count(b"A"))

    def test_nested_jobserver_limits_both_engines(self):
        if os.name != "nt":
            self.skipTest("Windows semaphore jobserver")
        helper = TOOLS[0].parent / "sninja_job.exe"
        if not helper.is_file():
            self.skipTest("Native timeline helper was not built")
        previous = os.environ.pop("MAKEFLAGS", None)
        if previous is not None:
            self.addCleanup(os.environ.__setitem__, "MAKEFLAGS", previous)

        def build(name, parent, child, shared):
            manifest = f'rule nested\n  command = "{child.as_posix()}" -C $dir\n'
            manifest += 'build first: nested\n  dir = one\nbuild second: nested\n  dir = two\n'
            d = self.directory(name, manifest)
            for subdir in ("one", "two"):
                sub = d / subdir
                sub.mkdir()
                contents = f'rule job\n  command = "{helper.as_posix()}" $out 50000 $index\n'
                contents += "".join(f"build job{i}: job\n  index = {i}\n" for i in range(12))
                (sub / "build.ninja").write_text(contents, encoding="utf8")
            args = ["-j", "4"] + (["--jobserver-pool"] if shared else [])
            self.run_tool(parent, d, *args)
            events = []
            for subdir in ("one", "two"):
                for i in range(12):
                    start, end, index, checksum, hz, pid, magic = struct.unpack(
                        "<QQQQQII", (d / subdir / f"job{i}").read_bytes())
                    self.assertEqual(i, index)
                    self.assertEqual(0x534e4a42, magic)
                    events += [(start, 1), (end, -1)]
            active = peak = 0
            for tick, delta in sorted(events):
                active += delta
                peak = max(peak, active)
            return peak

        for i, (parent, child) in enumerate(((TOOLS[0], TOOLS[1]), (TOOLS[1], TOOLS[0]), (TOOLS[0], TOOLS[0]))):
            self.assertLessEqual(build(f"shared{i}", parent, child, True), 4)
        # A negative control proves the timeline detects oversubscription.
        self.assertGreater(build("unshared", TOOLS[0], TOOLS[0], False), 4)

    def test_unicode_paths(self):
        manifest = f"rule copy\n  command = {self.py} helper.py copy $out $in\n" \
                   "build 结果$ 文件: copy 输入$ 文件\n"
        for i, exe in enumerate(TOOLS):
            d = self.directory(f"中文目录{i}", manifest, {"输入 文件": "data"})
            self.run_tool(exe, d)
            self.assertEqual("data", (d / "结果 文件").read_text())
            self.assert_noop(TOOLS[1-i], d)
            self.same(d, "-t", "commands")

    @unittest.skipUnless(os.name == "nt", "Windows long-path application manifests")
    def test_long_unicode_paths_and_deps_logs(self):
        prefix = "/".join(["中文目录_" + "a" * 45] * 6)
        source, output = prefix + "/source", prefix + "/result"
        manifest = f"builddir = {prefix}/logs\nrule cc\n  command = {self.py} helper.py gcc $out $in\n" \
                   f"  deps = gcc\n  depfile = $out.d\nbuild {output}: cc {source}\n"
        for i, exe in enumerate(TOOLS):
            # CreateProcess's lpCurrentDirectory has its own Windows length
            # limit; exercise long file/log paths from a short working dir.
            d = self.directory(f"long{i}", manifest, {source: "source", "header.h": "header"})
            self.assertGreater(len(str(d / output)), 300)
            self.run_tool(exe, d)
            self.assertEqual("sourceheader", (d / output).read_text())
            self.assert_noop(TOOLS[1-i], d)
            self.same(d, "-t", "deps")
            self.run_tool(TOOLS[1-i], d, "-t", "recompact")
            self.assert_noop(exe, d)

    def test_deps_logs_both_directions(self):
        for deps in ("gcc", "msvc"):
            manifest = f"rule cc\n  command = {self.py} helper.py {deps} $out $in\n  deps = {deps}\n"
            if deps == "gcc":
                manifest += "  depfile = $out.d\n"
            manifest += "build result: cc source\n"
            for i, exe in enumerate(TOOLS):
                d = self.directory(f"{deps}{i}", manifest, {"source": "source", "header.h": "header"})
                self.run_tool(exe, d)
                self.assertTrue((d / ".ninja_deps").exists())
                self.assert_noop(TOOLS[1-i], d)
                self.change(d, "header.h", "changed")
                self.run_tool(TOOLS[1-i], d)
                self.assertEqual("sourcechanged", (d / "result").read_text())
                self.assert_noop(exe, d)
                self.same(d, "-t", "deps")

    def test_recompact_large_records_matches_reference_bytes(self):
        count, headers, fanout = 300, 1280, 1024
        manifest = "rule cc\n  command = cc $out\n  deps = gcc\n"
        manifest += "".join(f"build out{i}: cc\n" for i in range(count))
        d = self.directory("recompact", manifest)
        names = ["dead"] + [f"out{i}" for i in range(count)] + [f"header{i}.h" for i in range(headers)]
        original = bytearray(b"# ninjadeps\n" + struct.pack("<I", 4))
        for index, name in enumerate(names):
            path = name.encode()
            path += b"\0" * (-len(path) % 4)
            original += struct.pack("<I", len(path) + 4) + path + struct.pack("<I", ~index & 0xffffffff)
        def deps_record(out, mtime, inputs):
            return struct.pack("<IIQ", 0x80000000 | (12 + 4 * len(inputs)), out, mtime) + \
                   struct.pack(f"<{len(inputs)}I", *inputs)
        original += deps_record(0, 1, [count + 1])
        for i in range(count):
            inputs = [count + 1 + (i * 17 + j) % headers for j in range(fanout)]
            original += deps_record(i + 1, 0x1122334455667788 + i, inputs)
        # Superseded entries and duplicate inputs must retain the same final
        # record order and IDs as the reference, including across buffer flushes.
        original += deps_record(count, 100, [count + 2, count + 1, count + 2])
        self.assertGreater(len(original), 2 * (512 << 10))
        canonical = []
        for exe, mode in ((TOOLS[0], 0), (TOOLS[0], 1), (TOOLS[1], None)):
            (d / ".ninja_log").write_text("# ninja log v7\n")
            (d / ".ninja_deps").write_bytes(original)
            temporary = d / ".ninja_deps.recompact"
            temporary.write_bytes(b"stale partial rewrite")
            self.run_tool(exe, d, "-t", "recompact", flush=mode)
            self.assertFalse(temporary.exists())
            canonical.append((d / ".ninja_deps").read_bytes())
            self.same(d, "-t", "deps")
        self.assertEqual(canonical[0], canonical[1])
        self.assertEqual(canonical[0], canonical[2])
        self.assertLess(len(canonical[0]), len(original))

    def test_build_log_numeric_spellings_match_rewritten_values(self):
        spellings = [b"", b"0", b"-0", b"+17", b" 23", b"\v31", b"44suffix", b"77\r",
                     b"-19", b"000012", b"2147483647", b"2147483648", b"-2147483648",
                     b"-2147483649", b"9223372036854775807", b"-9223372036854775808",
                     b"9223372036854775808", b"-9223372036854775809",
                     b"999999999999999999999999999999999", b"0x123", b"ff", b"0XfF",
                     b"-ff", b"ffffffffffffffff", b"10000000000000000", b"12\0ignored"]
        names, records = [], []
        for spelling in spellings:
            for column in range(4):
                fields = [b"1", b"2", b"3", b"ff"]
                fields[column] = spelling
                name = "objects/" + "long_component/" * 5 + f"out{len(names)}"
                names.append(name)
                records.append(b"\t".join(fields[:3] + [name.encode(), fields[3]]) + b"\r\n")
        d = self.directory("buildlog numbers", "".join(f"build {name}: phony\n" for name in names))
        original = b"# ninja log v7\r\n" + b"".join(records)
        observed = []
        for exe in TOOLS:
            log = d / ".ninja_log"
            log.write_bytes(original)
            self.run_tool(exe, d, "-t", "recompact")
            lines = log.read_bytes().splitlines()
            self.assertEqual(b"# ninja log v7", lines[0])
            self.assertEqual(len(names) + 1, len(lines))
            observed.append(sorted(lines[1:]))
        self.assertEqual(observed[0], observed[1])

    def test_deps_header_and_block_boundary_recovery(self):
        d = self.directory("reader recovery", "build out: phony\n")
        header = b"# ninjadeps\n" + struct.pack("=I", 4)
        cases = []
        for version in (1, 4, 9):
            for valid in (False, True):
                data = (header[:12] if valid else b"!" + header[1:12]) + struct.pack("=I", version)
                cases.extend(data[:length] for length in range(17))
        prefix = bytearray(header)
        def path_record(name, index):
            name += b"\0" * (-len(name) % 4)
            return struct.pack("=I", len(name) + 4) + name + struct.pack("=I", ~index & 0xffffffff)
        prefix += path_record(b"out", 0) + path_record(b"in", 1)
        for start in ((64 << 10) - 3, (64 << 10) + 1, (128 << 10) - 1):
            # Keep the last path record unaligned, then cut the following size
            # field or payload at each relevant boundary.
            padding = b"p" * (start - len(prefix) - 8)
            before = prefix + struct.pack("=I", len(padding) + 4) + padding + struct.pack("=I", ~2 & 0xffffffff)
            record = struct.pack("=IIQ64I", 0x8000010c, 0, 1234, *([1] * 64))
            cases.extend(before + record[:cut] for cut in (0, 1, 2, 3, 4, 5, 15, len(record)-1, len(record)))
        for index, data in enumerate(cases):
            with self.subTest(case=index):
                observed = []
                for exe in TOOLS:
                    log = d / ".ninja_deps"
                    log.write_bytes(data)
                    run = self.run_tool(exe, d, "-t", "query", "out")
                    observed.append((run.stdout, run.stderr, log.read_bytes() if log.exists() else None))
                self.assertEqual(observed[0], observed[1])

    def test_noop_probe_hit_fallback_and_lock_cleanup(self):
        manifest = f"rule cc\n  command = {self.py} helper.py gcc $out $in\n" \
                   "  deps = gcc\n  depfile = $out.d\n  rspfile = $out.rsp\n" \
                   "  rspfile_content = original\nbuild result: cc source\n"
        d = self.directory("probe", manifest, {"source": "source", "header.h": "header"})
        self.run_tool(TOOLS[1], d)
        hit = self.run_tool(TOOLS[0], d, "-d", "stats", probe=1)
        self.assertIn(b"no-op probe hit", hit.stdout)
        full = self.run_tool(TOOLS[0], d, "-d", "stats", probe=0)
        self.assertNotIn(b"no-op probe hit", full.stdout)
        (d / ".ninja_lock").write_text("stale")
        self.assert_noop(TOOLS[0], d)
        self.assertFalse((d / ".ninja_lock").exists())
        self.change(d, "header.h", "new header")
        self.same(d, "-n", "-j1")
        self.run_tool(TOOLS[0], d)
        self.assertEqual("sourcenew header", (d / "result").read_text())
        self.assert_noop(TOOLS[1], d)
        (d / "header.h").unlink()
        self.same(d, "-n", "-j1")
        (d / "header.h").write_text("restored")
        self.run_tool(TOOLS[1], d)
        self.change(d, "build.ninja", manifest.replace("original", "changed response content"))
        self.same(d, "-n", "-j1")
        self.run_tool(TOOLS[0], d)
        self.assert_noop(TOOLS[1], d)

    def test_prior_elapsed_scheduling_is_preserved(self):
        manifest = f"rule copy\n  command = {self.py} helper.py copy $out $in\n" \
                   "build fast: copy source\nbuild slow: copy source\n" \
                   "build all: phony fast slow\ndefault all\n"
        for i, exe in enumerate(TOOLS):
            d = self.directory(f"history{i}", manifest, {"source": "data"})
            self.run_tool(exe, d, "-j1")
            log = d / ".ninja_log"
            rows = log.read_text().splitlines()
            for j in range(1, len(rows)):
                fields = rows[j].split("\t")
                fields[0] = "0"
                fields[1] = "1000" if fields[3] == "slow" else "1"
                rows[j] = "\t".join(fields)
            log.write_text("\n".join(rows) + "\n")
            (d / "fast").unlink()
            (d / "slow").unlink()
            self.run_tool(exe, d, "-j1")
            self.assertEqual(["copy:slow", "copy:fast"], self.events(d)[-2:])

    def test_noop_probe_mixed_incremental_plans(self):
        count = 64
        manifest = f"rule cc\n  command = {self.py} helper.py gcc $out $in\n" \
                   "  deps = gcc\n  depfile = $out.d\n"
        manifest += "".join(f"build out{i}: cc src{i}\n" for i in range(count))
        manifest += "build all: phony " + " ".join(f"out{i}" for i in range(count)) + "\ndefault all\n"
        files = {f"src{i}": f"source{i}" for i in range(count)}
        files["header.h"] = "header"
        d = self.directory("mixed-probe", manifest, files)
        self.run_tool(TOOLS[1], d, "-j8")
        rng = random.Random(930)
        for trial in range(12):
            index = rng.randrange(count)
            if trial % 4 == 0:
                self.change(d, f"src{index}", f"changed source {trial}")
            elif trial % 4 == 1:
                (d / f"out{index}").unlink()
            elif trial % 4 == 2:
                self.change(d, "header.h", f"changed header {trial}")
            else:
                source = d / f"src{index}"
                saved = source.read_text()
                source.unlink()
                self.same(d, "-n", "-j1")
                source.write_text(saved)
            self.same(d, "-n", "-j1")
            self.run_tool(TOOLS[trial % 2], d, "-j8")
            self.assert_noop(TOOLS[1-trial % 2], d)
            header = (d / "header.h").read_text()
            for i in range(count):
                self.assertEqual((d / f"src{i}").read_text() + header, (d / f"out{i}").read_text())

    def test_dynamic_dependencies(self):
        manifest = f"rule dd\n  command = {self.py} helper.py dd $out\n" \
                   f"rule copy\n  command = {self.py} helper.py copy $out $in\n" \
                   f"rule cc\n  command = {self.py} helper.py compile-dd $out\n" \
                   "build graph.dd: dd\nbuild generated.h: copy source\n" \
                   "build result: cc || graph.dd\n  dyndep = graph.dd\ndefault result\n"
        for i, exe in enumerate(TOOLS):
            d = self.directory(str(i), manifest, {"source": "initial"})
            self.run_tool(exe, d, "-j", "8")
            self.assertEqual("initial", (d / "implicit.out").read_text())
            self.assert_noop(TOOLS[1-i], d)
            self.change(d, "source", "changed")
            self.run_tool(TOOLS[1-i], d, "-j", "8")
            self.assertEqual("changed", (d / "result").read_text())
            self.assertEqual("changed", (d / "implicit.out").read_text())
            self.assert_noop(exe, d)

    def test_restat_prunes_dependents(self):
        manifest = f"rule restat\n  command = {self.py} helper.py restat $out $in\n  restat = 1\n" \
                   f"rule copy\n  command = {self.py} helper.py copy $out $in\n" \
                   "build middle: restat source\nbuild result: copy middle\ndefault result\n"
        for i, exe in enumerate(TOOLS):
            d = self.directory(str(i), manifest, {"source": "same"})
            self.run_tool(exe, d)
            self.change(d, "source", "same")
            self.run_tool(TOOLS[1-i], d)
            self.assertEqual(["restat:middle", "copy:result", "restat:middle"], self.events(d))
            self.assert_noop(exe, d)

    def test_manifest_regeneration(self):
        original = f"rule regen\n  command = {self.py} helper.py copy $out $in\n  generator = 1\n" \
                   f"rule copy\n  command = {self.py} helper.py copy $out $in\n" \
                   "build build.ninja: regen source.ninja\n" \
                   "build result: copy input\ndefault result\n"
        for i, exe in enumerate(TOOLS):
            d = self.directory(str(i), original, {"source.ninja": original, "input": "first", "other": "second"})
            self.run_tool(exe, d)
            self.change(d, "source.ninja", original.replace("result: copy input", "result: copy other"))
            self.run_tool(TOOLS[1-i], d)
            self.assertEqual("second", (d / "result").read_text())
            self.assert_noop(exe, d)

    def test_validation_console_pools_and_keep_going(self):
        manifest = "pool serial\n  depth = 1\n" \
                   f"rule copy\n  command = {self.py} helper.py copy $out $in\n  pool = serial\n" \
                   f"rule validate\n  command = {self.py} helper.py validate $out\n  pool = console\n" \
                   f"rule fail\n  command = {self.py} helper.py fail $out\n" \
                   "build result: copy input |@ check\nbuild check: validate\n" \
                   "build bad: fail\nbuild independent: copy input\ndefault result\n"
        for i, exe in enumerate(TOOLS):
            d = self.directory(str(i), manifest, {"input": "data"})
            self.run_tool(exe, d, "-j", "8")
            self.assertTrue((d / "check").exists())
            self.assert_noop(TOOLS[1-i], d)
            self.run_tool(exe, d, "-j", "1", "-k", "0", "bad", "independent", expected=1)
            self.assertTrue((d / "independent").exists())
            self.run_tool(TOOLS[1-i], d, "-t", "clean")
            self.assertFalse((d / "result").exists())
            self.assertFalse((d / "check").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sninja", type=Path, default=TOOLS[0])
    parser.add_argument("--reference", type=Path, default=TOOLS[1])
    options, remaining = parser.parse_known_args()
    TOOLS = [options.sninja.resolve(), options.reference.resolve()]
    unittest.main(argv=[sys.argv[0], *remaining])
