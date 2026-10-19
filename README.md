# CoFinance

一个支持多人共同记账、实时聊天，并可调用大语言模型提供财务建议的 C++17 后端系统。

> 目标场景：情侣、室友、家庭等需要共同管理财务的小群体（2–20 人）。
> 技术定位：从零实现的 Linux 高并发服务端 —— epoll + 线程池 + 自研 HTTP/WebSocket + MySQL 连接池 + 自研异步日志。

## 当前进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 工程骨架、配置系统、异步日志、信号处理 | ✅ 已完成（57 个检查点，valgrind 0 泄漏） |
| P1 | epoll 主从 Reactor + IO 线程组 + 非阻塞 socket | ✅ 已完成（1000 并发连接，valgrind 0 泄漏） |
| P2 | HTTP 解析 + 路由 + 静态文件 | ⬜ |
| P3 | MySQL 连接池 + DAO + 单元测试框架 | ⬜ |
| P4 | 用户系统与鉴权（注册/登录/会话） | ⬜ |
| P5 | 组与成员（邀请/接受/退出/转让） | ⬜ |
| P6 | 记账（交易、分摊、分页筛选） | ⬜ |
| P7 | 统计与结算（净额、最少笔数转账） | ⬜ |
| P8 | 聊天 + 长轮询 | ⬜ |
| P9 | WebSocket 升级 | ⬜ |
| P10 | AI 助手（DeepSeek） | ⬜ |
| P11 | 前端整合 + 压测 + 文档收尾 | ⬜ |

## 构建与运行

```bash
# 依赖（Ubuntu 24.04）
sudo apt-get install -y cmake ninja-build pkg-config libmysqlclient-dev \
    libssl-dev libcurl4-openssl-dev nlohmann-json3-dev valgrind clang-format wrk

# 配置
cp config/secrets.conf.example config/secrets.conf   # 填入数据库账号密码
chmod 600 config/secrets.conf

# 构建（Debug + ASan/UBSan）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCOFINANCE_SANITIZE=ON
cmake --build build -j16

# 运行
./build/cofinance -c config/server.conf
```

## 测试

```bash
./build/cofinance_tests                    # P0：配置、日志等 57 个检查点
./build/cofinance_net_tests                # P1：socket、eventfd、timerfd、空闲超时 22 个检查点
ctest --test-dir build                     # 通过 CTest 运行两组单测

# P1 集成验收（脚本自动创建临时端口、1000 条连接、回显、CPU 与优雅退出）
ulimit -n 65535
python3 tests/integration/p1_echo_check.py --connections 1000

# 泄漏检查（ASan 与 valgrind 不能同时启用，故单独一个构建目录）
cmake -S . -B build-nosan -DCOFINANCE_SANITIZE=OFF && cmake --build build-nosan -j16
python3 tests/integration/p1_echo_check.py --binary build-nosan/cofinance \
    --connections 100 --valgrind --skip-cpu
```

## 目录结构

```
CoFinance/
├── CMakeLists.txt          # 三个 target：cofinance_core / cofinance / cofinance_tests
├── config/                 # server.conf（可提交）+ secrets.conf（600，已 gitignore）
├── sql/                    # 建表脚本（P3 起）
├── docs/
│   └── design.md           # 需求与设计基线
│                           # （学习/交付类文档不放在仓库里，见下「文档约定」）
├── include/cofinance/      # 对外头文件（按模块分目录）
├── src/
│   ├── main.cpp
│   ├── net/ http/ ws/      # 网络层与协议层（P1/P2/P9）
│   ├── db/ service/ api/   # 数据层、业务层、接口层（P3 起）
│   ├── ai/                 # LLM 调用（P10）
│   └── util/               # 配置、日志、通用工具
├── tests/
│   ├── unit/                # P0 基础单测 + P1 网络单测
│   └── integration/         # 启动真实服务做并发与退出验收
├── www/                    # 前端静态页（P11）
└── third_party/            # 第三方单头库（如需 vendor）
```

## 文档约定

**项目仓库里只放代码、配置和需求基线**，学习/交付类文档一律放在仓库外与需求文档同目录（`/home/ilan/projects/`），避免项目目录里散落一堆 md。

每个阶段结束时，生成**一份**阶段交付文档，固定结构如下：

| 顺序 | 内容 | 说明 |
|---|---|---|
| 1 | 本阶段目录与文件改动 | 目录树 + 标注 [新]/[改]/[删] + 改动统计 |
| 2 | 这个阶段实现了什么 | 大白话，5~6 条，附"怎么自己验证" |
| 3 | 原理笔记（精简） | 这一阶段涉及的底层机制 |
| 4 | 设计取舍 | 决策点 / 选择 / 被否方案 / 一句话理由 |
| 5 | 代码走读 | 只列关键函数与"为什么关键" |
| 6 | 踩坑记录 | 现象 → 根因 → 修复 → 教训 |
| 7 | 面试问答 | 该阶段最可能被追问的问题 |
| 8 | 可复现的验收命令 | 面试前能重跑 |
| 9 | 下一阶段计划 | — |

文件命名：`CoFinance-P<NN>-交付文档.{md,docx}`（在 `/home/ilan/projects/` 下）。
觉得精简版不够深时，另有 `CoFinance-P<NN>-附录-详细版.md` 供选读。

仓库内保留的文档只有 `docs/design.md`（需求与设计基线）。

## 文档索引

- 需求与设计基线：`docs/design.md`（源自《CoFinance 项目需求文档 v3.0》）
- 各阶段交付文档：`/home/ilan/projects/CoFinance-P<NN>-交付文档.{md,docx}`（仓库外）
- 阶段详细版附录：`/home/ilan/projects/CoFinance-P<NN>-附录-详细版.md`（仓库外）

## 已实现的技术要点（P0）

- 配置系统：多来源优先级（环境变量 > secrets.conf > server.conf > 默认值）、启动即校验（fail fast）、密钥自动打码
- 异步日志：有界队列 + 单后台线程、分级背压（丢弃并计数 / 等待后同步兜底）、ERROR 立即刷盘、空闲 1s 刷盘、跨天滚动、UTC 时间戳、内核 tid
- 信号处理：async-signal-safe 的处理器、不设 SA_RESTART、忽略 SIGPIPE、优雅退出（排空队列）
- 构建：C++17、target-based CMake、`-Werror` 全告警、ASan/UBSan 插桩、valgrind 复核

## 已实现的技术要点（P1）

- 主从 Reactor：主线程只做 `accept`，新连接按 round-robin 投递给 4 个 IO worker；每个 worker 独立持有 epoll。
- 连接亲和：一条连接从建立到关闭都在同一个 worker 线程读写，避免多线程争抢同一 socket。
- 非阻塞 + ET：accept 与收发都循环到 `EAGAIN`；写缓冲支持半写续传，只在该连接可写时才注册 `EPOLLOUT`。
- 跨线程投递：worker 使用 eventfd + 任务队列，主线程绝不直接操作其他线程的连接。
- 定时器：timerfd + multimap 实现毫秒级 deadline，空闲超时关闭会自动取消并释放连接资源。
- 背压与保护：单连接输入上限 1 MiB、待发送上限 8 MiB，超限主动断开，避免慢客户端拖垮进程。
- 优雅退出：停止 accept 后，各 worker 关闭全部连接并 join；SIGINT 退出码为 0。

P1 的应用层仍是临时 echo 协议，仅用于验证网络层；P2 会替换为 HTTP 请求解析与路由。

本机实测（WSL2）：1000 条并发空闲连接稳定；逐连接回显一致；空闲 2 秒仅消耗 0.020 秒 CPU；512 KiB 数据覆盖半写路径；Valgrind `0 errors`、`no leaks are possible`。
