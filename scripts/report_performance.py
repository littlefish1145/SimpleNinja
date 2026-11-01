"""Write the current measured performance report, rejecting mixed engine hashes."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build-release"


def main():
    current = hashlib.sha256((BUILD / "sninja.exe").read_bytes()).hexdigest()
    reference = hashlib.sha256((BUILD / "ninja_reference.exe").read_bytes()).hexdigest()
    def read(name, engine=True):
        data = json.loads((BUILD / name).read_text())
        if engine:
            if (data["binaries"]["sninja"]["sha256"] != current or
                    data["binaries"]["ninja"]["sha256"] != reference):
                raise RuntimeError(f"Engine hashes do not match the current build: {name}")
        return data
    broad = read("benchmark-path-final.json")
    deps = read("deps-path-final.json")
    log = read("buildlog-path-final.json")
    plain = read("compilation-path-final.json")
    pch = read("compilation-path-pch-final.json")
    startup = read("startup-path-final.json")
    parser = read("clparser-path-final.json", False)
    selfbuilds = {compiler: read(f"selfbuild-path-{compiler}.json", False) for compiler in ("clang", "msvc")}
    for data in selfbuilds.values():
        if data["engine_sha256"] != current:
            raise RuntimeError("Self-build benchmark used a different engine")
    lines = ["# 性能记录（2026-11-01，编译与输出解析优化）", "",
        "Windows 11 26200，Intel Core Ultra 9 275HX，24 核 / 24 线程。",
        "主引擎和原版均由 Clang 18.1.8、Release/O3/ThinLTO、动态 CRT 构建。",
        "生产引擎为 C++23，原版为 C++17；原版源码提交 `4e4df1e567eb3c1475a51af261cba2bfff60b4be`。", "",
        f"- 当前 sninja SHA256：`{current}`",
        f"- 原版 SHA256：`{reference}`", "",
        "除专用解析程序与自编译产物外，以下正式计时均核对为上述二进制。基准串行运行，",
        "没有并发编译、测试或其他基准。双方随机交错、取中位数，保存全部样本；主要为暖缓存。",
        "Windows 主程序及启动测试副本统一为 Medium 完整性。单位为 ms，速度比为原版耗时 / sninja 耗时。", "",
        "## 实际 C++ 编译", "",
        "120 个 C++ 编译单元、MSVC 19.44 /O2、-j24、每项 7 轮交错。每轮执行生成程序验证结果。",
        "共享头文件与本地头文件均修改实际内容；本地修改必须恰好重编译一个单元、归档并链接。",
        "每种模式下两种引擎的编译命令逐字节一致。PCH 模式额外创建稳定标准头文件的预编译头。", "",
        "| 模式 / 用例 | sninja | 原版 | 原版/sninja |",
        "| --- | ---: | ---: | ---: |"]
    names = {"clean_j24": "全量", "incremental_header": "公共头文件修改",
             "incremental_local": "单个本地头文件修改", "noop_compiled": "编译后空构建"}
    for mode, data in (("普通", plain), ("PCH", pch)):
        for name, row in data["workloads"].items():
            m = row["median_ms"]
            lines.append(f"| {mode} / {names[name]} | {m['sninja']:.3f} | {m['ninja']:.3f} | {m['ninja']/m['sninja']:.3f}× |")
    lines += ["", "相同 sninja 启用 PCH 前后的耗时变化：", ""]
    for name in ("clean_j24", "incremental_header", "incremental_local"):
        a = plain["workloads"][name]["median_ms"]["sninja"]
        b = pch["workloads"][name]["median_ms"]["sninja"]
        lines.append(f"- {names[name]}：{a:.3f} → {b:.3f}，耗时降低 {(1-b/a)*100:.1f}%。")
    lines += ["", "这部分是改变项目编译方式的收益。PCH 默认为 OFF，执行既有 build.ninja 时不改写命令。",
              "两种模式在不同目录分别测量，不能把跨模式的比值解释为纯引擎优化；空构建也未稳定领先。", "",
              "数据：`compilation-path-final.json`、`compilation-path-pch-final.json`。", "",
              "## 引擎自身的编译", "",
              "同一正式引擎调度，普通/PCH 随机交错，预热 1 轮、正式 3 轮，-j12。",
              "使用独立 configure.py 生成清单，清理所有编译产物后计时；运行自编译程序并验证空构建。",
              "每轮源码保持相同，结束时重新核对源码摘要。", "",
              "| 编译器 | 普通 | PCH | 普通/PCH |", "| --- | ---: | ---: | ---: |"]
    for name, data in selfbuilds.items():
        m = data["median_ms"]
        lines.append(f"| {name} | {m['plain']:.3f} | {m['pch']:.3f} | {m['plain']/m['pch']:.3f}× |")
    lines += ["", "数据：`selfbuild-path-clang.json`、`selfbuild-path-msvc.json`。", "",
              "## MSVC 输出解析", "",
              "专用原生程序链接各自的完整核心库。每项 10 万条记录，预热 2 轮、交错测量 21 轮，",
              "仅计解析函数时间；依赖集合和保留诊断的摘要必须一致。这是局部基准，不能当作完整编译提速。", "",
              "| 输入 | sninja | 原版 | 原版/sninja |", "| --- | ---: | ---: | ---: |"]
    for name, row in parser["workloads"].items():
        m = row["median_ms"]
        lines.append(f"| {name} | {m['sninja']:.3f} | {m['ninja']:.3f} | {m['ninja']/m['sninja']:.3f}× |")
    lines += ["", "覆盖自定义前缀、混合系统路径、CR/LF、无换行尾部、空行、诊断内嵌 NUL、",
              "根目录、尾斜线、大小写、相对路径、UNC 和跨盘符。数据：`clparser-path-final.json`。", "",
              "## 解析与不含依赖日志的空构建", "",
              "每项预热 2 轮、交错测量 21 轮。数据：`benchmark-path-final.json`。", "",
              "| 用例 | sninja | 原版 | 原版/sninja |", "| --- | ---: | ---: | ---: |"]
    for name, row in broad["workloads"].items():
        m = row["median_ms"]
        lines.append(f"| {name} | {m['sninja']:.3f} | {m['ninja']:.3f} | {m['ninja']/m['sninja']:.3f}× |")
    m = startup["median_ms"]
    lines += ["", f"另做相同路径轮换的启动测量，4 轮各 101 次：{m['sninja']:.3f}/{m['ninja']:.3f} ms。",
              "启动时仅执行 --version；数据：`startup-path-final.json`。", "",
              "## 大型依赖日志", "",
              "2 万输出、1024 个头文件，分别每输出 8/64/256 个依赖，各交错测量 15 轮。",
              "空构建不得改动日志；重写结果交叉读取并核对精确的头文件失效计划。", "",
              "| 用例 | sninja | 原版 | 原版/sninja | 峰值 MiB：sninja/原版 |",
              "| --- | ---: | ---: | ---: | ---: |"]
    for name, row in deps["workloads"].items():
        a, b = row["medians"]["sninja"], row["medians"]["ninja"]
        lines.append(f"| {name} | {a['wall_ms']:.3f} | {b['wall_ms']:.3f} | {b['wall_ms']/a['wall_ms']:.3f}× | "
                     f"{a['peak_working_set_bytes']/2**20:.1f}/{b['peak_working_set_bytes']/2**20:.1f} |")
    lines += ["", "数据：`deps-path-final.json`。65536 个不同头文件的旧版大范围测量归档在历史报告中，",
              "其 7.56–8.15× 结果属于 ee9216d5… 版本，不作为当前二进制的测量。", "",
              "## 构建日志历史加载", "",
              "2 万长输出路径，1/3/7 轮历史，各交错 21 次；查询不得改动日志，重写后的值与原版一致。", "",
              "| 历史 | sninja | 原版 | 原版/sninja |", "| --- | ---: | ---: | ---: |"]
    for name, row in log["workloads"].items():
        a, b = row["medians"]["sninja"]["wall_ms"], row["medians"]["ninja"]["wall_ms"]
        lines.append(f"| {name} | {a:.3f} | {b:.3f} | {b/a:.3f}× |")
    lines += ["", "数据：`buildlog-path-final.json`。", "",
              "## 瓶颈与验证范围", "",
              "独立插桩构建从直接子进程句柄读取 CPU/存活时间，保持命令不变。此前单轮分析中，",
              "普通公共头文件重建平均约 20.5 个编译器存活，合计 CPU 约 18.2 核；PCH 后约 16.9 个存活、",
              "CPU 约 17.7 核，中位排队时间从约 6ms 增至 43ms。该数据来自输出解析修改前的插桩版本，",
              "用于判断编译 CPU 与进程启动的瓶颈，不用于当前正式排名；不包含后代进程 CPU。",
              "原始分析：`compiler-traces-plain.json`、`compiler-traces-pch.json`。", "",
              "Release 和 PCH Release 各通过 482 个单测、53 个套件，以及三种模式各 21 组差分测试。",
              "MSVC ASan 同样通过 482 个测试。Clang/MSVC 的普通与 PCH 自举均验证阶段 1/2 SHA256 一致并空构建。",
              "日志前缀为 `compiler-path-final-*` 和 `compiler-path-bootstrap-*`，原始文件均在 build-release/。", "",
              "尚不能证明所有工作负载都优于原版；低并发、固定启动成本、长链、冷缓存与尾延迟仍需优化验证。",
              "实际验证平台仍为 Windows x64；完整兼容性与理论极限目标保持进行中。", "",
              "此前完整报告归档为 [整块读取阶段](PERFORMANCE-before-compiler-output.md)。",
              "本报告由 `python scripts/report_performance.py` 从原始 JSON 生成，拒绝混用正式引擎摘要。", ""]
    report = ROOT / "docs/PERFORMANCE.md"
    archive = ROOT / "docs/PERFORMANCE-before-compiler-output.md"
    if not archive.exists():
        archive.write_bytes(report.read_bytes())
    report.write_text("\n".join(lines), encoding="utf8")
    print(f"Wrote {report}; engine SHA256 {current}")


if __name__ == "__main__":
    main()
