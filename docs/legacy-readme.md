# sninja

A Ninja-compatible build tool for Windows, written from scratch in C++20.

It reads the same `build.ninja` manifests, writes the same `.ninja_log` /
`.ninja_deps` files, and is interchangeable with the reference
[ninja](https://github.com/ninja-build/ninja) in either direction: you can
build with one and rebuild incrementally with the other.

On a 120-translation-unit MSVC project it builds slightly faster than ninja at
`-j 8` and above, by up to 15% at high parallelism. At low `-j` and on the cheap
operations (no-op builds, manifest parsing) the two are indistinguishable.

---

## Quick start

```powershell
# Build sninja itself (needs MSVC and NASM on PATH)
.\scripts\build.ps1

# Use it on any CMake project
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
.\build\sninja.exe -C build
```

> `scripts\build.ps1` is a thin wrapper around `cmake --build build`. Its real
> job is loading the MSVC environment (`vcvars64.bat`) into the current shell
> first, so you do not have to run it from a Developer Command Prompt.

---

## Compatibility

Verified in both directions on every change:

| | |
|---|---|
| Manifest | `.ninja` syntax, `$`/`:` escapes, `include`/`subninja`, `pool`, `defaults`, phony, order-only deps, `restat` |
| Build log | `.ninja_log` **v7**, field order `start end mtime output hash` |
| Deps log | `.ninja_deps` **v4**; `deps = gcc`, `deps = zero`, `deps = msvc` |
| Compiler errors | MSVC `/showIncludes` parsing, localized output |
| CLI | flags and `-t` tools listed below |

The build-log field order was determined empirically against the reference
binary, not from documentation. Writing `output` and `hash` in the other order
produces a file the reference ninja silently misreads.

### Command line

```
sninja [options] [targets]

  -f FILE               read FILE as the build manifest
  -t TOOL               run a tool
  -j N                  build with N parallel jobs (default: P-core count)
  -k                    keep going after a failure
  -n                    dry run
  -v                    show commands
  --build-logfile=FILE  write the build log to FILE
  --version             print the ninja compatibility version
```

`-t` tools: `clean`, `missing-deps`, `targets`, `rules`, `commands`, `inputs`,
`dump`, `edges`, plus `restat`.

> `restat` is supported but is not yet listed in `--help`. The help text is
> generated separately from the tool dispatch, so the two can drift.


---

## Performance

120 translation units / 136 edges, MSVC Release, best of 3.

The comparison build is **native x86-64 ninja**, not a launcher:

```
C:\Users\fish_\AppData\Local\Programs\Python\Python312\Scripts\ninja.exe
  1.13.0.git.kitware.jobserver-pipe-1
```

> **Check your `ninja` before trusting a comparison.** `C:\choco\bin\ninja.exe`
> reports version 1.13.2 but is a Chocolatey shim: a 32-bit .NET launcher that
> starts the CLR and then execs the real binary. It costs ~46 ms before touching
> a manifest, so it makes any short-running command look catastrophically slow.
> Earlier revisions of this README were measured against it and reported
> implausible wins (up to 3.8x on manifest parsing); against real ninja the
> honest figure is "no measurable difference".

### Clean build

| `-j` | sninja | ninja | |
|---|---|---|---|
| 1 | 6.04s | 5.98s | −1% |
| 2 | 3.07s | 3.05s | −1% |
| 4 | **1.68s** | 1.72s | +2% |
| 8 | **1.12s** | 1.17s | +4% |
| 16 | **0.91s** | 1.00s | +9% |
| 24 | **0.82s** | 0.97s | +15% |

The advantage grows with parallelism. At `-j 1` and `-j 2` the build is
subprocess-bound and the two tools are the same speed.

### Everything else

| | sninja | ninja | |
|---|---|---|---|
| Incremental (touch shared header, 123 edges) | **0.90s** | 1.04s | +13% |
| No-op build | 14.3ms | 10.8ms | −32% |
| Manifest parse (`-t targets`) | 8.2ms | 7.6ms | −7% |
| Process startup (`--version`, parses nothing) | 6.6ms | 6.6ms | — |

At this manifest size both tools' startup floor (~6.6 ms) dominates, so the parse
and no-op numbers are within noise and **ninja is marginally ahead of sninja on
both**. Those two rows are the honest result; earlier claims of 2.3x and 3.8x
there came from the shim.

The incremental figure is real: it is a full 123-edge rebuild, where the same
`-j`-dependent advantage applies as in the clean build.


---

## Design notes

### Scheduling

A work-stealing deque per worker (`src/simd/steal.asm`, hand-written Win64 NASM),
a global in-flight budget, and a non-blocking start-poll-reap loop. A worker never
sits in `WaitForSingleObject` while a compiler runs — it starts the next one.

Reaping waits in the kernel on the child process handles via
`WaitForMultipleObjects`, rather than polling on a timer. Replacing a fixed 1 ms
poll cut the number of wakeups from 500 to 124 and took 7% off `-j 4`.

The work-stealing deque uses fixed chunks and never reuses indices while a thief
might be reading them. An earlier version recycled slots and raced.

### Spawning children

At low `-j`, `CreateProcess` (~4.3 ms per edge) is a measurable fraction of each
worker's wall clock, so it is moved to a dedicated spawner thread. This is a win
only while there are few enough workers to share the cost, because the spawner is
a single thread and becomes a queue of its own once it cannot keep up:

| `-j` | spawner | in-worker |
|---|---|---|
| 2 | **3.06s** | 3.28s |
| 4 | **1.72s** | 2.12s |
| 8 | **1.18s** | 1.40s |
| 16 | 0.96s | **0.93s** |
| 24 | 1.02s | **0.77s** |

So the spawner is enabled automatically at `-j <= 8` and off above it. Override
with `SNINJA_SPAWNER=1` / `SNINJA_SPAWNER=0`. Both paths are covered by
`scripts/test_launch_paths.ps1` on every change.

### Ctrl-C and child cleanup

Background children are created with `CREATE_NEW_PROCESS_GROUP`, mirroring the
reference ninja. A console Ctrl-C therefore does *not* reach them — sninja
decides what to stop, which means sninja is also responsible for stopping them.
Two independent mechanisms cover this:

1. A `SetConsoleCtrlHandler` handler signals an event; workers notice it at their
   next wait. The handler only sets an event, because console handlers run on a
   deadline and must not take a lock a build thread might hold.
2. Every child is assigned to a job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`,
   so the OS reaps the tree however sninja exits — including being killed — and
   including grandchildren such as the helpers `cl.exe` spawns.

`CREATE_NEW_PROCESS_GROUP` also made the compilers measurably *faster*: under
`CREATE_NO_WINDOW` they took ~18% longer in total, which was most of an old gap
against the reference at low `-j`. That is why the flag matches ninja's rather
than the more obvious choice.

### SIMD

`src/simd` holds hand-written assembly plus runtime dispatch, used for manifest
scanning and the work-stealing deque.

There is deliberately **no** SIMD hashing. Measured on 5000 real commands, a
scalar hash runs in 0.065 ms (24 GB/s); hashing is not on the critical path, so
there is nothing to win.

---

## Instrumentation

All of it is off unless the environment variable is set, and the off path costs
one predictable branch per event.

| Variable | Effect |
|---|---|
| `SNINJA_TIME_PHASES=1` | Aggregate phase timings to stderr: spawn, child run, reap, finish, handle counts, worker idle breakdown |
| `SNINJA_TIMELINE=<file>` | Per-edge execution timeline as CSV: start, launch, exit, finish, waits, empty scans |
| `SNINJA_TIMELINE_FLUSH_MS=<n>` | Publish a timeline snapshot every *n* ms, so a hung or killed build still leaves data |
| `SNINJA_SPAWNER=0\|1` | Force the launch path, overriding the automatic `-j` threshold |
| `SNINJA_TRACE_SUBPROCESS=1` | Log cwd and command line per child |

Analysing a timeline:

```powershell
$env:SNINJA_TIMELINE = "$env:TEMP\build.csv"
$env:SNINJA_TIMELINE_FLUSH_MS = "200"   # survive a hang or a kill
.\build\sninja.exe -C build -j 4
python .\scripts\timeline.py "$env:TEMP\build.csv" -j 4 --ascii
```

`timeline.py` reports ramp / middle / tail, per-worker time budgets, edge
duration percentiles, and which edges were still running when the snapshot was
taken. Snapshots are published by writing a temporary and renaming it into place,
so the file on disk is always a complete, readable snapshot.

---

## Testing

```powershell
.\scripts\build.ps1
.\build\sninja_test.exe      # 82 unit tests
ctest --test-dir build       # 4 suites
```

Integration suites (each loads the MSVC environment itself):

| Script | Covers |
|---|---|
| `test_sched.ps1` | Scheduler sweeps and ninja interoperability |
| `test_restat.ps1` | Build-log recovery from a real CMake project |
| `test_restat_rule.ps1` | Real `restat = 1` semantics |
| `test_clean.ps1` | `-t clean`, including `clean dead` |
| `test_interrupt.ps1` | Ctrl-C cleanup and child-process leaks |
| `test_timeline_kill.ps1` | Timeline durability when the process is killed |
| `test_launch_paths.ps1` | Both launch paths, plus the automatic `-j` threshold |

Several of these carry a **negative control** — a scenario that is expected to
fail. They exist because "the file was there" or "nothing leaked" is only
evidence if the opposite is reachable:

- `test_interrupt.ps1` disables both cleanup mechanisms and asserts children
  *do* leak.
- `test_timeline_kill.ps1` disables periodic flushing and asserts *no* timeline
  is published.

Helpers: `scripts/make_sample_project.ps1`, `scripts/make_bench_project.ps1`
(generate projects), `scripts/bench.ps1` (compare against reference ninja),
`scripts/sninja_proc.ps1` (dot-source; start/kill/console-signal sninja).

---

## Layout

```
src/core     portable core: lexer, manifest parser, graph, build log, deps log,
             build engine, scheduler, subprocess, timeline
src/simd     hand-written x86-64 assembly and runtime dispatch
src/cli      argument parsing and tool dispatch
tests        unit tests
scripts      build, test and benchmark scripts
```

`src/core` is mostly portable C++; Win32 specifics live in `subprocess.cc`,
`job.cc`, `interrupt.cc`, `util.cc` and the SIMD assembly.

---

## Known gaps

- `deps = gcc` and `deps = zero` are implemented and unit-tested, but only
  `deps = msvc` has been exercised against a real toolchain here.
- There is no `dyndep` support.
- `Cleanup()` has end-to-end coverage but no unit test.
- Test coverage of the core is partial: `graph_test`, `state_test` and
  `build_test` from the upstream suite have not been ported.
- P-core / E-core detection exists but the affinity handling is untested on
  hybrid-partition machines other than the development one.
