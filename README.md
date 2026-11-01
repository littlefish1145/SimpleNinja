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

运行 `sninja --help` 查看命令行选项。兼容性范围与已知限制见 [项目状态](docs/STATUS.md)，性能测量方法及结果见 [性能文档](docs/PERFORMANCE.md)。

## 项目结构

- `src/engine/`：生产构建引擎。
- `configure.py`、`cmake/`：自举和 CMake 构建配置。
- `tests/`、`scripts/`：测试、构建与性能工具。
- `ninja/`：用于对照的原版 Ninja 源码快照。
- `docs/`：开发、状态和性能记录。

## 许可与来源

生产引擎基于 Ninja，保留原作者版权声明及 [Apache License 2.0](src/engine/COPYING)。修改范围见 [README.sninja](src/engine/README.sninja)，第三方组件说明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
