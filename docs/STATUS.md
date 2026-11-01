# 当前状态：性能优化已暂停（2026-11-01）

目标保持不变：C++23 优化分支，读取 build.ninja、兼容 Ninja 行为、支持自举，
全面提升性能并尽可能接近可验证的实际极限。当前仍未证明完整兼容、所有负载领先或理论极限。

性能优化已按用户要求停止，目标保持暂停；本次仅整理 ASCII 文字 Logo 和项目文档。

## 当前实现

- 生产引擎是 src/engine，包含完整引擎；没有执行外部 Ninja 的回退。
  原框架 src/core、src/simd、src/cli 默认关闭。
- 对照版本为 Ninja 提交 4e4df1e567eb3c1475a51af261cba2bfff60b4be，需自行获取。
  当前兼容版本字符串为 1.14.0.git。工作目录没有顶层 Git 仓库。
- build-release 使用 Clang 18.1.8、C++23、Release/O3/ThinLTO、动态 CRT。
  sninja.exe SHA256：5892cbf4dc82e959b9554be08dbe78ccbc1e97e7e6cf64c0cfd5831c354de4cd。
  原版 C++17 使用相同优化/CRT，SHA256：5194b549570fd4cca42b5402fb508b9521ea5f83feba405e2b465aaabbf4ac80。
- 当前新增：MSVC 输出使用借用行视图，前缀/后缀检查不分配，不复制整行做小写转换。
  Windows 相对化直接比较组件、一次复制剩余路径，保留尾部空组件和 UNC/特殊路径回退。
- 新增可选 PCH：CMake 的 SNINJA_PCH=ON、build.ps1 -Pch、configure.py --pch。
  独立自举支持 Clang/MSVC，种子及阶段 1/2 均使用 PCH；MSVC 强制按 C++ 编译 getopt.c。
  默认关闭 PCH；执行其他工程 build.ninja 时保持编译命令原样。
- 新增 SNINJA_PROFILE_JOBS 编译开关，默认关闭。开启后通过 SNINJA_JOB_TRACE 指定记录前缀，
  从直接子进程句柄记录排队、CreateProcess、CPU 和存活时间，不包装编译命令。
  每个父 PID 使用独立 JSONL；直接子进程 CPU 不包含其后代进程。
- 既有优化保留：SSE2 有界扫描、借用路径、拥有所有权的 arena、单次驻留探测、
  单指针内联邻接、连续小绑定表/大绑定哈希、目录缓存、C++23 原位文件读取。
- Windows 单后台线程自适应创建进程，主线程同时调度和回收。保留父进程全部可继承句柄；
  内部文件打开时设为不可继承。IOCP 使用 release/acquire 发布并精确排空取消事件。
- 空构建先验证静态图，再直接检查日志叶依赖。复杂状态交给完整扫描；无 dyndep 时可复用
  原始 stat 结果，回滚 phony 合成时间戳。解释模式保持完整扫描。
- 依赖日志使用紧凑 32 位 ID、整块读取、最多 576KiB 缓冲、完整记录追加并逐条刷新。
  重写时私有临时文件批量缓冲，确认关闭成功后替换；提交后原地重映射已有记录。
  写入/关闭/替换失败时恢复原 ID 和记录。日志偏移、历史计数与截断支持超过 2GiB。
- 构建日志借用历史路径查找，仅新条目分配路径；普通整数字段采用有界 from_chars，
  特殊拼写和溢出继续使用原 CRT 语义。CLI 早退跳过无用的默认并行度查询。

## 验证

- 最终 Release 和 PCH Release 各通过 482 个单测、53 个套件。
  各自 CTest 的同步、异步、完整扫描三种模式均通过全部 21 组差分测试。
  日志：compiler-path-final-ctest.log、compiler-path-final-pch-ctest.log。
- MSVC 19.44 AddressSanitizer 通过全部 482 个单测：compiler-path-final-asan-tests.log。
  本机 Clang 18 ASan 运行库初始化有故障，未计为通过。
- 最终插桩版本通过 check_job_trace.py：管道输出、console、可执行文件缺失、CPU 时间有效性
  和时间戳顺序均正确；不将插桩程序的耗时用于正式排名。
- 新测试覆盖大小写后缀、空前缀尾部、空格/制表符、NUL 诊断、CR/LF、无换行尾部，
  以及相对化的根目录、尾部空组件、大小写和组件边界。
  原生解析基准还与原版核对完整依赖集合及诊断摘要，涵盖 UNC 和跨盘符。
- 普通 Clang 自举阶段 1/2 SHA256 相同：
  9a291c9f00ca409b2cea46f6013c6bd9befe376e44855355a7d253af68d28ded。
- 普通 MSVC 自举阶段 1/2 SHA256 相同：
  1058a183c316c15d84d3a5884d4e7ae93ef7afbd328d9ea0785624b28521e3d1。
- PCH Clang 自举阶段 1/2 SHA256 相同：
  6b05d431eb3cc43894113a78838092d91d986b350808fc6a77f8d969abf20e2f。
- PCH MSVC 自举阶段 1/2 SHA256 相同：
  bd4354d465a00f3cf4493ca250dfef7fd14fe864be86d3a3b0a22c1b777566ba。
  四种构建均验证阶段 2 空构建；Clang 使用含空格路径。
  日志：compiler-path-bootstrap-clang.log、compiler-path-bootstrap-msvc.log，及对应 -pch.log。
  上一验证轮还确认删除 PCH 会调度重建 PCH、消费者及链接。
- 既有差分覆盖日志交叉读写、动态依赖、响应文件、restat、验证、Windows jobserver、
  超长中文路径、启动失败、129 种损坏日志输入以及混合增量。
- 超过 2GiB 的真实稀疏日志恢复由历史 ee9216d5… Release/ASan 验证：
  2148007969 精确截断为 2148007964 字节，原版 fastfail。相关恢复代码之后未变。
  数据：large-log-fixed-final.json、large-log-fixed-final-asan.json；不把失败计时当成提速。

## 性能证据与后续

当前表格和原始数据名称见 PERFORMANCE.md；新生成器会核对所有正式基准的引擎摘要。
普通/PCH 编译策略分开统计，局部解析基准与整体编译收益分开说明；保留所有不利结果。
历史 ee9216d5… 综合矩阵归档于 PERFORMANCE-before-compiler-output.md。

待验证范围包括固定启动成本、低并发/单文件增量、PCH 后的进程启动瓶颈，以及复杂图的
扫描与邻接存储。需要扩展冷缓存、长链、尾延迟、更多实际工程与其他平台的验证。
文件替换目前仍有 unlink 后 rename 的窗口，临时文件清理失败的恢复也需进一步核实。
本机未找到 sccache/ccache；编译缓存尚未实现，不能计入已有收益。

## 环境注意事项

- 基准期间不得并发编译、测试或运行其他基准；真实编译基准自身除外。
- Windows 复制的 EXE 会继承工作目录 Low 标签，必须与主程序/原版统一为 Medium：
  icacls 路径 /setintegritylevel M。启动轮换脚本自动处理。
- 环境键名需归一化，避免 PATH/Path 重复。开发环境函数已处理。
- 本机 MSVC 只有中文资源，VSLANG=1033 不保证英文；DetectIncludes.cmake 保留实际原始前缀。
- 并发单测必须隔离 TEMP/TMP，CTest 已使用各自构建目录的 test-tmp。
- 全部验证日志及 JSON 位于 build-release/。旧框架的 82 项历史测试不属于生产引擎验证。
