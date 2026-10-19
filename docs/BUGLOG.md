# CoFinance 踩坑与 Bug 记录

> 记录规则：只记录**真实发生**的问题，不编造。每条包含六个部分：
> **现象 → 复现 → 定位手段 → 根因 → 修复 → 教训**。
> 「定位手段」一栏尤其重要——面试官更关心你**怎么找到**问题，而不是你最后改了什么。

---

## B-001 日期格式化缓冲区可能被截断（编译器抓到的隐患）

- **阶段**：P0　**严重级别**：中（潜在的内存安全/日志错乱）
- **现象**：`cmake --build` 失败：
  ```
  src/util/Common.cpp:85:43: error: '%02d' directive output may be truncated writing between
  2 and 11 bytes into a region of size between 4 and 11 [-Werror=format-truncation=]
  ```
- **复现**：编译 `Common.cpp` 中的 `utcDateString()`，`-Wall -Wextra -Werror` 下必现。
- **定位手段**：编译器直接给出了错误位置与原因——`-Wformat-truncation` 会按**类型全值域**推导最坏输出长度。
- **根因**：我用 `char buf[16]` 装 `"%04d-%02d-%02d"`，凭直觉认为「年月日最多 10 个字符」。但 `tm_year + 1900` 是 `int`，编译器必须按 `int` 的最大值（10 位数字）推导：`11 + 1 + 11 + 1 + 11 = 35` 字节，`buf[16]` 确实可能被截断。截断本身还会导致日期字符串错乱（比如日志文件名变成半个日期），属于「平时不炸、边界才炸」的隐患。
- **修复**：放弃 `snprintf` 拼日期，改用手工定宽拼接（`appendPadded()`），并把年月日、时分秒都 `clamp` 到合法区间。附带好处：输出宽度恒定，日志时间戳能对齐、日志文件名能按字典序排序。
- **教训**：
  1. 「缓冲区够用」是直觉，不是证明；涉及 `%d`/`%s` 的固定缓冲区宽度必须按类型全值域推导。
  2. 这类问题**编译器免费帮我发现了**——这就是坚持 `-Wall -Wextra -Werror` 的价值。相反，如果当初有 `-Wno-format-truncation` 之类的「先让它编过」，隐患就埋进日志系统里了。
  3. 时间格式化这类「输出必须定宽」的场景，手工拼接比 `snprintf` 更可控。

---

## B-002 只有部分 target 被 sanitizer 插桩，测试链接失败

- **阶段**：P0　**严重级别**：高（构建系统错误，且现象极具误导性）
- **现象**：`cofinance` 主程序链接成功，但 `cofinance_tests` 链接失败，输出几十行：
  ```
  undefined reference to `__asan_report_load8'
  undefined reference to `__ubsan_handle_type_mismatch_v1'
  undefined reference to `__ubsan_handle_pointer_overflow'
  ```
- **复现**：`cmake -DCOFINANCE_SANITIZE=ON` 后构建测试 target。
- **定位手段**：先看「谁引用了这些符号」——`libcofinance_core.a(Logger.cpp.o)`；再看这些符号属于谁——ASan/UBSan 运行时。于是问题从「链接器报错」变成「**插桩代码与运行时不对称**」：库被插桩了，消费它的可执行文件没链接运行时。
- **根因**：`CMakeLists.txt` 里用 `foreach(t cofinance_core cofinance)` 逐个 target 添加 sanitizer 选项，**漏掉了 `cofinance_tests`**。静态库中的插桩目标文件会引用 `__asan_*`/`__ubsan_*`，凡是链接它的 target 都必须同时链接 sanitizer 运行时。
- **修复**：改为目录级 `add_compile_options(-fsanitize=address,undefined ...)` + `add_link_options(...)`，并把它放到**所有 target 定义之前**（这两个命令只对之后创建的 target 生效）。
- **教训**：
  1. 插桩/取消插桩是**全局开关**，不该用「枚举 target」的方式维护——漏一个就报一堆看不懂的符号错误。
  2. 链接错误的正确读法：**先确认缺的符号属于哪个库/运行时**，再反推是谁没链上；不要盯着 `undefined reference` 本身发呆。
  3. CMake 里「命令写在哪个位置」是有语义的（目录级设置只影响其后的 target），这类顺序敏感的写法要写注释。

---

## B-003 单元测试断言了并发流水线的瞬时状态（flaky test）

- **阶段**：P0　**严重级别**：中（测试自身不可靠，会浪费大量时间）
- **现象**：测试首次运行时 57 个检查点里失败 1 个：
  ```
  FAIL tests/unit/test_main.cpp:233  cofinance::Logger::instance().writtenCount() == 0
  ```
  重跑时有时通过、有时失败。
- **复现**：在入队 200 条日志后立刻检查 `writtenCount()==0`，多跑几次必现间歇性失败。
- **定位手段**：思考「这个断言想表达什么」——我原本想证明「日志是异步的（此刻还没落盘）」。但后台线程是独立调度的，主线程检查的那一瞬间它可能已经消费了若干条。**任何瞬时值都不确定**，所以断言本身不成立。
- **根因**：把「异步」错误地理解成了「同步落后的一个瞬间」。异步流水线的中间状态在并发下没有确定值。
- **修复**：删除该断言，改为只断言**最终状态**（`shutdown()` 之后所有日志都必须落在文件里，包括最后一条 `第 199 条`）。这同时也验证了更重要的事实：**shutdown 会排空队列，不丢日志**。
- **教训**：
  1. 并发/异步组件的测试只能断言**最终状态**或**偏序关系**（如「A 一定在 B 之前」），不能断言瞬时计数。
  2. flaky test 比没有测试更糟——它会训练你忽略测试失败。
  3. 写测试前先问自己：「这个断言表达的性质，在任何合法调度下都必须成立吗？」不是，就不该写。

---

## B-004 整文件重写时丢掉了 `namespace util {`，编译器报错在文件末尾

- **阶段**：P0　**严重级别**：低（自身操作失误）
- **现象**：`Common.cpp:153: error: expected declaration before '}' token`，报错位置在文件**最后一行**。
- **复现**：用整文件覆盖的方式重写 `Common.cpp` 后编译。
- **定位手段**：`expected declaration before '}'` 说明「有括号没配对，而且多出来的右括号在函数体外」。真正的问题通常**在最前面**——检查发现重写时漏掉了 `namespace util {` 这一行，于是后面的函数没有被包进 `util` 命名空间，而结尾多出一个 `}`。
- **根因**：我在一次修改中同时做了「修 bug」和「整文件重写」，重写过程中丢失了一行结构代码。
- **修复**：补回 `namespace util {`。
- **教训**：
  1. C++ 的「括号/命名空间不匹配」错误常常报在文件末尾，但根因在文件开头——**看报错要往上游找第一个不合常理的地方**。
  2. 修改应当**尽量小且聚焦**；需要整文件重写时，事后要用 `git diff` 检查结构性内容（命名空间、`#include`、大括号）是否被动过。

---

## 环境类问题（不是代码 bug，但会反复遇到）

| 问题 | 现象 | 应对 |
|---|---|---|
| LeakSanitizer 在受限沙箱下不可用 | `LeakSanitizer has encountered a fatal error ... does not work under ptrace` | ① `ASAN_OPTIONS=detect_leaks=0` 只关泄漏检测（其余 ASan/UBSan 检查仍在）；② 用 valgrind 做泄漏检查 |
| ASan 与 valgrind 不能同时使用 | 两者都做内存插桩，互相冲突 | 维护两个构建目录：`build/`（ASan+UBSan）、`build-nosan/`（valgrind） |
| 沙箱内无法创建 socket | `bind`/`connect` 返回 `EPERM` | 运行服务、连接 MySQL、压测时需要提权执行 |
