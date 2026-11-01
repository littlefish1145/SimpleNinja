#!/usr/bin/env python3
"""Generate a C++23 self-hosting build.ninja; --bootstrap needs no existing Ninja."""
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
WINDOWS = os.name == "nt"


def developer_environment():
    env = {k.upper(): v for k, v in os.environ.items()} if WINDOWS else os.environ.copy()
    if not WINDOWS:
        return env
    env["VSLANG"] = "1033"
    if env.get("VCTOOLSINSTALLDIR"):
        return env
    installer = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
    vswhere = installer / "Microsoft Visual Studio/Installer/vswhere.exe"
    candidates = []
    if vswhere.exists():
        found = subprocess.check_output([str(vswhere), "-latest", "-products", "*",
            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath"], text=True).strip()
        if found:
            candidates.append(Path(found) / "VC/Auxiliary/Build/vcvars64.bat")
    candidates.append(Path("D:/vs2022/VC/Auxiliary/Build/vcvars64.bat"))
    for script in candidates:
        if script.exists():
            # cmd.exe has different quoting rules from the CRT argv parser.
            dump = subprocess.check_output(
                f'cmd /d /s /c ""{script}" >nul 2>&1 && set"',
                text=True, errors="replace")
            for line in dump.splitlines():
                key, sep, value = line.partition("=")
                if sep and key:
                    env[key.upper()] = value
            env["VSLANG"] = "1033"
            return env
    raise RuntimeError("Install the Visual Studio C++ x64 tools or run in a Developer Command Prompt")


def quote(args):
    return subprocess.list2cmdline([str(x) for x in args]) if WINDOWS else shlex.join(map(str, args))


def ninja_path(path):
    return str(path).replace("\\", "/").replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


def value(text):
    return text.replace("$", "$$")


def command(args, cwd, env):
    p = subprocess.run(list(map(str, args)), cwd=cwd, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if p.returncode:
        raise RuntimeError(f"{quote(args)}\n{p.stdout.decode(errors='replace')}")
    return p.stdout


def msvc_include_prefix(compiler, env):
    with tempfile.TemporaryDirectory(prefix="sninja-includes-") as directory:
        root = Path(directory)
        header = root / "probe.h"
        source = root / "probe.cpp"
        header.write_text("// probe\n", encoding="utf8")
        source.write_text(f'#include "{header.as_posix()}"\n', encoding="utf8")
        output = command([compiler, "/nologo", "/showIncludes", "/c", source,
                          "/Fo" + str(root / "probe.obj")], root, env)
        expected = header.as_posix().encode("utf8")
        for line in output.splitlines():
            index = line.replace(b"\\", b"/").find(expected)
            if index > 0:
                return line[:index].decode("utf8")
        raise RuntimeError(f"Cannot determine compiler include prefix: {output!r}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--builddir", type=Path, default=ROOT / "build-bootstrap")
    ap.add_argument("--cxx", default=os.environ.get("CXX"))
    ap.add_argument("--bootstrap", action="store_true")
    ap.add_argument("--verify", action="store_true", help="rebuild with stage 1 and compare stage 2")
    ap.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 12))
    ap.add_argument("--no-lto", action="store_true")
    ap.add_argument("--pch", action="store_true", help="precompile common standard headers (Clang/MSVC)")
    ap.add_argument("--runtime", choices=("static", "dynamic"), default="dynamic",
                    help="Windows CRT linkage (default: dynamic)")
    opt = ap.parse_args()
    env = developer_environment()
    compiler = opt.cxx or next((shutil.which(c, path=env.get("PATH")) for c in
        (["clang++", "cl"] if WINDOWS else ["clang++", "g++", "c++"])
        if shutil.which(c, path=env.get("PATH"))), None)
    compiler = shutil.which(compiler, path=env.get("PATH")) if compiler else None
    if not compiler:
        raise RuntimeError("A C++23 compiler is required; pass --cxx")
    is_cl = Path(compiler).stem.lower() in ("cl", "clang-cl")
    is_clang = "clang" in Path(compiler).stem.lower()
    if opt.pch and not (is_cl or is_clang):
        raise RuntimeError("--pch currently supports Clang and MSVC; omit it for this compiler")
    out = opt.builddir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    suffix = ".obj" if WINDOWS else ".o"
    binary = "sninja.exe" if WINDOWS else "sninja"
    lists = [ROOT / "cmake/engine-common.txt", ROOT / f"cmake/engine-{'win32' if WINDOWS else 'posix'}.txt"]
    sources = [line for p in lists for line in p.read_text().splitlines() if line]
    sources.append("ninja.cc")
    defines = ["NDEBUG"]
    if WINDOWS:
        defines += ["NOMINMAX", "_CRT_SECURE_NO_WARNINGS", "_CRT_NONSTDC_NO_DEPRECATE",
                    "_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"]
    if is_cl:
        flags = ["/nologo", "/TP", "/std:c++latest", "/O2", "/GR-", "/EHsc",
                 "/MD" if opt.runtime == "dynamic" else "/MT", "/Zc:__cplusplus"]
        flags += ["/D" + x for x in defines]
        flags += ["/I" + str(ROOT / "src/engine")]
        ldflags = ["/link", "/Brepro", "/OPT:REF", "/OPT:ICF"]
        if not opt.no_lto:
            flags.append("/GL")
            ldflags.append("/LTCG")
    else:
        flags = ["-std=c++23", "-O3", "-fno-rtti", "-fno-exceptions"]
        if WINDOWS and is_clang:
            flags += ["-fms-runtime-lib=" + ("dll" if opt.runtime == "dynamic" else "static")]
        flags += ["-D" + x for x in defines]
        flags += ["-I" + str(ROOT / "src/engine")]
        ldflags = []
        if not WINDOWS:
            flags += ["-pthread", "-DUSE_PPOLL=1"] if sys.platform.startswith("linux") else ["-pthread"]
            ldflags += ["-pthread"]
        if not opt.no_lto:
            flags += ["-flto=thin" if is_clang else "-flto"]
            ldflags += ["-flto=thin" if is_clang else "-flto"]
        if is_clang:
            ldflags += ["-fuse-ld=lld"]
        if WINDOWS:
            ldflags += ["-Wl,/Brepro"]
    if WINDOWS:
        manifest = ROOT / "cmake/ninja.manifest"
        ldflags += (["/MANIFEST:EMBED", f"/MANIFESTINPUT:{manifest}"] if is_cl else
                    ["-Wl,/MANIFEST:EMBED", f"-Wl,/MANIFESTINPUT:{manifest}"])

    pch_header = ROOT / "src/engine/precompiled.h"
    pch_source = out / "obj/precompiled.cc"
    (out / "obj").mkdir(exist_ok=True)
    if opt.pch and is_cl:
        contents = "// Standard headers are supplied by /FI.\n"
        if not pch_source.exists() or pch_source.read_text() != contents:
            pch_source.write_text(contents)

    def pch_file(directory):
        return directory / "precompiled.pch"

    def pch_command(directory, dependencies=False):
        base = [compiler, *flags]
        if is_cl:
            return base + (["/showIncludes"] if dependencies else []) + [
                "/Yc" + str(pch_header), "/FI" + str(pch_header),
                "/Fp" + str(pch_file(directory)), "/c", str(pch_source),
                "/Fo" + str(directory / ("precompiled" + suffix))]
        return base + ["-x", "c++-header"] + (["-MMD", "-MF", str(pch_file(directory)) + ".d"]
            if dependencies else []) + [str(pch_header), "-o", str(pch_file(directory))]

    def compile_command(source, obj, dependencies=False, pch_directory=None):
        base = [compiler, *flags]
        if opt.pch:
            directory = pch_directory or Path("obj")
            base += (["/Yu" + str(pch_header), "/FI" + str(pch_header),
                      "/Fp" + str(pch_file(directory))] if is_cl else
                     ["-include-pch", str(pch_file(directory))])
        if is_cl:
            return base + (["/showIncludes"] if dependencies else []) + ["/c", str(source), "/Fo" + str(obj)]
        # getopt.c is intentionally C++ despite its filename.
        return base + ["-x", "c++"] + (["-MMD", "-MF", str(obj) + ".d"] if dependencies else []) + ["-c", str(source), "-o", str(obj)]

    def link_command(objects, output):
        return ([compiler, "/nologo", *map(str, objects), "/Fe" + str(output), *ldflags] if is_cl else
                [compiler, *map(str, objects), *ldflags, "-o", str(output)])

    regen = [sys.executable, ROOT / "configure.py", "--builddir", out, "--cxx", compiler,
             "--runtime", opt.runtime]
    if opt.no_lto:
        regen.append("--no-lto")
    if opt.pch:
        regen.append("--pch")
    lines = ["# Generated by configure.py. C++23; no CMake or Ninja installation required.",
             "ninja_required_version = 1.10", "rule cxx",
             "  command = " + value(quote(compile_command("$in", "$out", True))).replace("$$in", "$in").replace("$$out", "$out"),
             "  description = CXX $out"]
    if is_cl:
        lines += ["  deps = msvc", "  msvc_deps_prefix = " + value(msvc_include_prefix(compiler, env))]
    else:
        lines += ["  depfile = $out.d", "  deps = gcc"]
    pch_outputs = []
    if opt.pch:
        pch_outputs = ([Path("obj") / ("precompiled" + suffix)] if is_cl else []) + [pch_file(Path("obj"))]
        lines += ["rule pch", "  command = " + value(quote(pch_command(Path("obj"), True))),
                  "  description = PCH " + ninja_path(pch_file(Path("obj")))]
        if is_cl:
            lines += ["  deps = msvc", "  msvc_deps_prefix = " + value(msvc_include_prefix(compiler, env))]
        else:
            lines += ["  deps = gcc", "  depfile = " + ninja_path(pch_file(Path("obj"))) + ".d"]
        outputs = ninja_path(pch_outputs[0])
        if len(pch_outputs) > 1:
            outputs += " | " + " ".join(ninja_path(p) for p in pch_outputs[1:])
        lines += ["build " + outputs + ": pch " + ninja_path(pch_header) +
                  (" " + ninja_path(pch_source) if is_cl else "")]
    link = value(quote(link_command(["$in"], "$out"))).replace("$$in", "$in").replace("$$out", "$out")
    if WINDOWS:
        link = 'cmd /d /s /c "' + link + ' && icacls $out /setintegritylevel M >nul"'
    lines += ["rule link", "  command = " + link, "  description = LINK $out",
              "rule configure", "  command = " + value(quote(regen)), "  generator = 1",
              "build build.ninja: configure " + " ".join(ninja_path(p) for p in [ROOT / "configure.py", *lists])]
    objects = []
    for source in sources:
        obj = Path("obj") / (Path(source).stem + suffix)
        objects.append(obj)
        lines += [f"build {ninja_path(obj)}: cxx {ninja_path(ROOT / 'src/engine' / source)}" +
                  (" | " + ninja_path(pch_file(Path("obj"))) if opt.pch else "")]
    if opt.pch and is_cl:
        objects.append(pch_outputs[0])
    lines += [f"build {binary}: link " + " ".join(ninja_path(o) for o in objects) +
              (" | " + ninja_path(manifest) if WINDOWS else ""), f"default {binary}"]
    (out / "build.ninja").write_text("\n".join(lines) + "\n", encoding="utf8")
    (out / "obj").mkdir(exist_ok=True)
    print(f"Configured {out / 'build.ninja'} ({compiler})", flush=True)

    if opt.bootstrap:
        seed_dir = out / ".bootstrap"
        seed_dir.mkdir(exist_ok=True)
        seed_objects = [seed_dir / p.name for p in objects]
        if opt.pch:
            print("Stage 0: precompile standard headers", flush=True)
            command(pch_command(seed_dir), out, env)
        print(f"Stage 0: compile {len(sources)} files directly with {opt.jobs} workers", flush=True)
        with ThreadPoolExecutor(max_workers=opt.jobs) as pool:
            tasks = [pool.submit(command, compile_command(ROOT / "src/engine" / s, o,
                                                       pch_directory=seed_dir), out, env)
                     for s, o in zip(sources, seed_objects)]
            for task in as_completed(tasks):
                task.result()
        seed = seed_dir / binary
        command(link_command(seed_objects, seed), out, env)
        if WINDOWS:
            command(["icacls", seed, "/setintegritylevel", "M"], out, env)
        print("Stage 1: seed executable builds sninja through build.ninja", flush=True)
        print(command([seed, "-j", opt.jobs], out, env).decode(errors="replace"), end="")

    if opt.verify:
        stage1 = out / binary
        if not stage1.exists():
            raise RuntimeError("--verify needs --bootstrap or an existing stage 1 binary")
        saved = out / ".bootstrap" / ("stage1.exe" if WINDOWS else "stage1")
        saved.parent.mkdir(exist_ok=True)
        shutil.copy2(stage1, saved)
        if WINDOWS:
            command(["icacls", saved, "/setintegritylevel", "M"], out, env)
        command([saved, "-t", "clean"], out, env)
        print("Stage 2: stage 1 builds a fresh executable", flush=True)
        print(command([saved, "-j", opt.jobs], out, env).decode(errors="replace"), end="")
        first_hash = hashlib.sha256(saved.read_bytes()).hexdigest()
        second_hash = hashlib.sha256(stage1.read_bytes()).hexdigest()
        if first_hash != second_hash:
            raise RuntimeError(f"Bootstrap binary mismatch: {first_hash} != {second_hash}")
        noop = command([stage1], out, env)
        if b"no work to do" not in noop:
            raise RuntimeError(f"Stage 2 is not clean: {noop!r}")
        print(f"Bootstrap verified: identical stage 1/2 SHA256 {second_hash}; stage 2 no-op", flush=True)


if __name__ == "__main__":
    main()
