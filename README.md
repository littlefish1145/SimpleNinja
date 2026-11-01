# sninja

sninja 是基于 Ninja 源码重构的 C++23 构建引擎，读取 `build.ninja` 并执行其中的构建命令。项目主要针对清单解析、依赖日志和 Windows 任务调度进行优化，当前主要验证平台为 Windows x64。

## 功能

- 读取 Ninja 清单、变量、`include` 和 `subninja`。
- 支持增量构建、gcc/MSVC 依赖、动态依赖、响应文件、任务池和 `restat`。
- 支持独立自举和两阶段构建验证。

## 构建

需要 Python 3 和 C++23 编译器。Windows 上需要 Visual Studio C++ x64 工具链。

```powershell
python configure.py --bootstrap --verify
.\build-bootstrap\sninja.exe --version
```

也可以使用 CMake 3.24 或更高版本构建并运行测试：

```powershell
.\scripts\build.ps1
```

首次构建测试时，如未找到 GoogleTest，CMake 会下载固定版本并验证摘要。更多选项及平台要求见 [开发文档](docs/DEVELOPMENT.md)。

## 使用

```powershell
# 在指定工程目录中构建
.\build-bootstrap\sninja.exe -C path\to\project
# 查看待执行命令
.\build-bootstrap\sninja.exe -C path\to\project -n
# 列出目标
.\build-bootstrap\sninja.exe -C path\to\project -t targets
```

运行 `sninja --help` 查看命令行选项。对照测试脚本需要自行提供原版 Ninja 可执行文件。兼容性范围与已知限制见 [项目状态](docs/STATUS.md)，性能测量方法及结果见 [性能文档](docs/PERFORMANCE.md)。

## 性能与内存占用

以下为 2026-11-01 已记录版本的测量结果。平台为 Windows 11、Intel Core Ultra 9 275HX（24 核 / 24 线程）。sninja 和原版 Ninja 均使用 Clang 18.1.8、Release / O3 / ThinLTO、动态 CRT 构建；sninja 使用 C++23，原版使用 C++17。

双方随机交错运行，取耗时中位数，主要为暖缓存。清单解析与普通空构建测量 21 轮，依赖日志测量 15 轮，实际编译测量 7 轮。加速比为原版耗时除以 sninja 耗时，大于 1 表示 sninja 更快。

### 耗时

| 场景 | sninja | 原版 Ninja | 加速比 |
| --- | ---: | ---: | ---: |
| 普通路径解析，10 万边 | 95.478 ms | 164.663 ms | 1.72× |
| 变量路径解析，10 万边 | 125.880 ms | 166.542 ms | 1.32× |
| 图空构建，10 万节点 | 65.385 ms | 108.379 ms | 1.66× |
| 真实文件空构建，2 万文件 | 32.600 ms | 43.457 ms | 1.33× |
| 高依赖空构建，2 万输出、每输出 256 个依赖 | 59.774 ms | 170.924 ms | 2.86× |
| 同规模依赖日志重写 | 69.838 ms | 429.621 ms | 6.15× |
| 实际 C++ 全量编译，120 单元、`-j24` | 2.553 s | 2.748 s | 1.08× |
| 公共头文件修改后重编译 | 2.671 s | 2.955 s | 1.11× |
| 实际 C++ 全量编译，双方启用 PCH | 1.771 s | 2.149 s | 1.21× |
| 公共头文件重编译，双方启用 PCH | 1.649 s | 1.976 s | 1.20× |
| 普通编译后的空构建 | 16.126 ms | 14.249 ms | 0.88× |

实际编译使用 MSVC 19.44 /O2，每种模式下双方执行相同的编译命令，并运行生成程序检查结果。PCH 是可选编译策略，默认关闭，其收益单独统计；sninja 不会自动改写已有 `build.ninja` 中的编译命令。

### 峰值内存

依赖日志用例均包含 2 万输出和 1024 个不同的头文件，每个输出分别记录 8、64 或 256 个依赖。

| 场景 | sninja 峰值 | 原版 Ninja 峰值 |
| --- | ---: | ---: |
| 空构建，每输出 8 个依赖 | 19.6 MiB | 25.2 MiB |
| 空构建，每输出 64 个依赖 | 24.9 MiB | 55.0 MiB |
| 空构建，每输出 256 个依赖 | 40.3 MiB | 144.2 MiB |
| 日志重写，每输出 8 个依赖 | 17.9 MiB | 22.7 MiB |
| 日志重写，每输出 64 个依赖 | 22.8 MiB | 40.2 MiB |
| 日志重写，每输出 256 个依赖 | 38.4 MiB | 99.3 MiB |

内存指标为构建工具进程的峰值工作集（Windows `PeakWorkingSetSize`），表中取各轮峰值的中位数，1 MiB = 1024² 字节，不包含编译器、链接器等子进程。高依赖空构建中，sninja 的峰值工作集约降低 72%；同规模日志重写约降低 61%。

启动在相同路径轮换测量中为 9.410 / 9.407 ms，基本持平；普通编译后的空构建仍有回退，当前数据不能证明所有负载都优于原版。完整测量条件、版本 SHA256 和数据来源见 [性能文档](docs/PERFORMANCE.md)。

## 项目结构

- `src/engine/`：生产构建引擎。
- `configure.py`、`cmake/`：自举和 CMake 构建配置。
- `tests/`、`scripts/`：测试、构建与性能工具。
- `docs/`：开发、状态和性能记录。

## 许可与来源

生产引擎基于 Ninja，保留原作者版权声明及 [Apache License 2.0](src/engine/COPYING)。修改范围见 [README.sninja](src/engine/README.sninja)，第三方组件说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
