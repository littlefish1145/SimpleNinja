# 开发与测量

[返回 README](../README.md) · [性能记录](PERFORMANCE.md) · [项目状态](STATUS.md)

性能优化已按要求暂停。本文保留构建选项、实现说明和复现命令，供查阅。

## CMake 构建和测试

```powershell
.\scripts\build.ps1
# 指定编译器：
.\scripts\build.ps1 -Compiler D:/LLVM/bin/clang++.exe
# 用预编译头加快本项目自身的编译：
.\scripts\build.ps1 -Pch
```

脚本初始化开发环境、配置 Release、构建并运行 CTest，任一步失败都会返回失败。
默认输出 `build-release/sninja.exe`。首次构建测试需要下载固定版本、校验 SHA256 的 GoogleTest。

也可以在开发者命令行中直接运行：

```powershell
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 12
ctest --test-dir build-release --output-on-failure
```

CMake 开关：

Windows 的 CMake 构建同样默认使用动态 CRT；可通过
`-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` 显式选择静态 CRT。
性能对照程序使用与 sninja 相同的运行库设置。

| 选项 | 默认值 | 用途 |
| --- | --- | --- |
| `SNINJA_LTO` | ON | Release 链接时优化 |
| `BUILD_TESTING` | ON | 引擎测试及新增测试 |
| `SNINJA_STD_ADJACENCY` | OFF | 使用 std::vector，供邻接存储优化的消融比较 |
| `SNINJA_BUILD_BENCHMARKS` | ON | 构建原生短任务及并发时间线辅助程序 |
| `SNINJA_PCH` | OFF | 预编译引擎和测试共用的标准头文件 |
| `SNINJA_PROFILE_JOBS` | OFF | Windows 子进程排队、启动及 CPU 时间插桩 |
| `SNINJA_BUILD_LEGACY` | OFF | 构建原框架，需 NASM |

本机的中文 MSVC 输出会被 CMake 默认转码错误，进而漏记头文件依赖。
`cmake/DetectIncludes.cmake` 按实际原始输出探测前缀，避免头文件修改后仍链接旧对象。

## 实现

- 严格按输入长度限制的 SSE2 扫描；非 x86 平台使用标量实现。
- 普通清单路径临时借用原文，变量路径仍在边作用域建立后求值。
- 已规范化的路径直接驻留到节点表；复杂路径使用原版规范化逻辑。
- 稳定地址的对象分块存储，节点驻留只探测一次哈希表。
- 常见单元素邻接表内嵌一个指针，保持三个机器字布局，减少小额堆分配。
  大邻接表采用较小的增长比例，限制高扇出依赖图的闲置容量。
- 小作用域使用连续有序绑定表，超过 16 项转为哈希表，避免大作用域的二次复杂度。
- Windows 目录时间戳使用连续哈希缓存；文件通过 C++23 `resize_and_overwrite` 直接读取。
- 单输出脏检查缓存内嵌；命令路径在无需恢复反斜线时直接借用。
- 子进程直接创建可继承的输出管道端点，共享每个进程集合的 NUL 输入句柄。
- Windows 连续启动任务时，由单个后台线程创建进程，主线程同时回收输出和调度；
  空队列、串行依赖链和稀疏任务直接启动。保留父进程全部可继承句柄。
  内部日志、响应文件等在打开时设为不可继承，消除与后台启动的竞争窗口。
- 依赖日志从头部开始按整块读取，避免 stdio 再拆成小读操作；缓冲区最多 576KiB。
  一遍校验并转换依赖 ID，普通追加的完整记录合并写入并逐条刷新。
- 日志重写的临时文件采用批量缓冲，检查最终关闭错误后再替换正式日志；
  提交后原地重映射已有依赖 ID，避免复制整套记录。普通追加仍逐记录刷新。
- 依赖记录保留 32 位 ID，在加载图时解析为节点；记录头与 ID 数组一次分配，
  避免日志中再保存一份完整指针数组，并在日志销毁或记录替换时释放存储。
- 常见空构建先检查静态图与文件存在性，再直接验证日志依赖，复用原版时间戳、
  命令哈希、restat 与 generator 检查，避免复制大量输入和反向邻接。
  不满足条件时使用完整扫描；无 dyndep 的图可复用探测得到的原始 stat 结果。
- Windows 日志恢复使用 64 位截断接口，完整记录偏移和历史记录计数不再受 32 位 long 限制。
- 构建日志借用历史行的路径做查找，仅新记录保存路径字符串；常规整数字段用有界解析，
  特殊拼写与溢出仍交给原 CRT 解析器。版本号、工具列表等退出路径跳过无用的 CPU 查询。
- MSVC 输出按字符串视图逐行处理，文件名判断只检查后缀，系统头文件匹配无需复制并转换整行。
  Windows 路径相对化直接比较组件并复制剩余路径，避免为每条头文件记录创建组件数组。

`SNINJA_ASYNC_LAUNCH=0` 可禁用后台启动，供诊断和性能消融使用；默认值和 `auto`
按并行度启用，`1` 允许后台启动。两种启用方式均保留串行、稀疏任务的直接启动策略。
`SNINJA_NOOP_PROBE=0` 可关闭空构建快路径。日志中含生成输入、验证、dyndep、独立 depfile 等
复杂状态由完整扫描处理；`-d explain` 保持完整扫描以保留诊断。
`SNINJA_RECOMPACT_FLUSH=1` 恢复重写时逐记录刷新，供批量写入的性能消融使用。

原型保留在 `src/core`、`src/simd`、`src/cli`，旧说明保存在 `docs/legacy-readme.md`。
旧原型的 timeline/spawner 专用开关不属于当前生产引擎。当前调试选项沿用 Ninja 的 `-d` / `-w`。
源码来源、修改范围和许可见 `src/engine/README.sninja`、`THIRD_PARTY_NOTICES.md`。

## 可复现测量

```powershell
python scripts/benchmark.py --repeats 21 --output build-release/benchmark-final.json
python scripts/benchmark_builds.py --repeats 5 --jobs 1 4 12 24
python scripts/benchmark_builds.py --pch --repeats 7 --jobs 24 --work build-release/compile-pch
python scripts/benchmark_selfbuild.py --cxx D:/LLVM/bin/clang++.exe
python scripts/benchmark_clparser.py
python scripts/benchmark_jobs.py --count 128 --jobs 1 4 24 --variant sync=0
python scripts/benchmark_deps.py --repeats 15 --recompact
python scripts/benchmark_deps.py --repeats 15 --recompact --flush-variant flushed=1
python scripts/benchmark_deps.py --headers 65536 --fanout 64 256 --repeats 15
python scripts/benchmark_deps.py --probe-variant full=0 --repeats 15
python scripts/benchmark_deps.py --late-dirty --fanout 256 --probe-variant full=0
python scripts/benchmark_build_log.py --history 1 3 7 --repeats 31
python scripts/benchmark_startup.py --crossover --waves 4
python scripts/check_large_deps.py
python scripts/differential.py -v
```

基准使用同一编译器、相同 Release/LTO 设置编译本地原版；交错执行两种程序、预热后取中位数，
保存所有样本和二进制 SHA256，所有命令必须成功。不会使用 Chocolatey 的 .NET 启动器作为对照。
`benchmark.py` 覆盖启动、10 万边解析与图遍历、2 万真实文件的空构建。
`benchmark_builds.py` 构建真正的 CMake/C++ 工程，并运行生成的程序检查结果；覆盖不同并行度、
共享头文件、单个本地头文件增量重建与编译后空构建，可用 `--extra NAME=EXE` 比较旧版。
修改头文件时会改变实际内容，并检查生成程序的计算结果；本地头文件修改必须恰好构建三个目标。
`--pch` 在测试工程中启用稳定标准头文件的 PCH，双方执行相同命令。测量时应关闭其他编译任务。
Windows 上复制旧版程序后，应使用 `icacls 路径 /setintegritylevel M`，与主构建的中完整性标签
保持一致；继承目录的低完整性标签会影响子进程权限，甚至使链接器无法写系统临时目录。
启动路径轮换脚本会统一临时副本的完整性标签。
`benchmark_jobs.py` 使用原生子进程，记录父进程 CPU、任务时间线、启动间隔和并发数，
并支持 `--chain` 依赖链。`benchmark_deps.py` 检查大型依赖日志的空构建、峰值内存、
头文件失效传播，以及可选的日志重写与交叉读取，并记录 Windows 进程级读写次数和字节数。
`--late-dirty` 检查末尾输出缺失时的回退成本。`check_large_deps.py` 使用超过 2GiB
的稀疏日志，验证保留所有完整记录并恢复损坏尾部，结果保存为 JSON。

编译瓶颈分析可单独配置 `-DSNINJA_PROFILE_JOBS=ON`，再用
`benchmark_builds.py --sninja build-profile/sninja.exe --trace build-release/compiler-traces`。
`analyze_job_trace.py` 汇总 JSONL 中的排队、CreateProcess、存活和 CPU 时间。
插桩直接读取子进程句柄，不包装编译命令；只统计直接子进程，不包含其后代进程的 CPU。
插桩构建的耗时不用于正式速度排名。默认构建不包含这些记录代码。
`python scripts/check_job_trace.py` 可检查插桩版本的管道、console 和启动失败记录。

Windows x64 是目前实际验证的平台；其他平台仍需单独验证。
