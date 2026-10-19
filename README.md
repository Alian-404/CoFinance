# CoFinance

一个支持多人共同记账、实时聊天，并可调用大语言模型提供财务建议的 C++17 后端系统。

> 目标场景：情侣、室友、家庭等需要共同管理财务的小群体（2–20 人）。
> 技术定位：从零实现的 Linux 高并发服务端 —— epoll + 线程池 + 自研 HTTP/WebSocket + MySQL 连接池 + 自研异步日志。

## 当前进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 工程骨架、配置系统、异步日志、信号处理 | ✅ 已完成（57 个检查点，valgrind 0 泄漏） |
| P1 | epoll 主从 Reactor + IO 线程组 + 非阻塞 socket | ✅ 已完成（1000 并发连接，valgrind 0 泄漏） |
| P2 | HTTP 解析 + 路由 + 静态文件 | ✅ 已完成（52 个检查点，keep-alive/pipeline/限制全通过） |
| P3 | MySQL 连接池 + 预处理语句 + 事务 + 用户 DAO | ✅ 已完成（57 个检查点，valgrind 0 errors） |
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

# 建库（只需一次）与建表（可重复执行）
sudo mysql -e "CREATE DATABASE IF NOT EXISTS cofinance DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci;"
mysql -ucofinance -p cofinance < sql/schema.sql

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
./build/cofinance_http_tests               # P2：解析、路由、静态文件 52 个检查点
./build/cofinance_db_tests config/server.conf sql/schema.sql  # P3：连接池/事务/注入 57 个检查点
ctest --test-dir build                     # 通过 CTest 运行四组单测（需 MySQL）

# P1 集成验收（脚本自动创建临时端口、1000 条连接、回显、CPU 与优雅退出）
ulimit -n 65535
python3 tests/integration/p1_echo_check.py --connections 1000

# P2 集成验收（keep-alive、pipeline、半包、body、超限、超时、路径穿越）
python3 tests/integration/p2_http_check.py

# 泄漏检查（ASan 与 valgrind 不能同时启用，故单独一个构建目录）
cmake -S . -B build-nosan -DCOFINANCE_SANITIZE=OFF && cmake --build build-nosan -j16
python3 tests/integration/p1_echo_check.py --binary build-nosan/cofinance_echo_server \
    --connections 20 --valgrind --skip-cpu
python3 tests/integration/p2_http_check.py --binary build-nosan/cofinance --valgrind
valgrind --leak-check=full --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect --error-exitcode=9 \
    ./build-nosan/cofinance_db_tests config/server.conf sql/schema.sql
```

## 目录结构

```
CoFinance/
├── CMakeLists.txt          # 核心库、主程序、三组单测与 P1 echo 验收服务
├── config/                 # server.conf（可提交）+ secrets.conf（600，已 gitignore）
├── sql/schema.sql          # 9 张表的可重复建表脚本
├── docs/
│   └── design.md           # 需求与设计基线
│                           # （学习/交付类文档不放在仓库里，见下「文档约定」）
├── include/cofinance/      # 对外头文件（按模块分目录）
├── src/
│   ├── main.cpp
│   ├── net/ http/ ws/      # 网络层与协议层（P1/P2/P9）
│   ├── db/                 # 连接池、预处理语句、事务、DAO（P3）
│   ├── service/ api/       # 业务层、接口层（P4 起）
│   ├── ai/                 # LLM 调用（P10）
│   └── util/               # 配置、日志、通用工具
├── tests/
│   ├── unit/                # P0/P1/P2/P3 单元测试
│   └── integration/         # 真实服务验收与 P1 echo 测试服务
├── www/                    # 静态文件根目录（P2 先放占位首页，P11 扩展前端）
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

主程序已经切换到 HTTP；P1 的 echo 协议保留在独立 `cofinance_echo_server` 测试 target 中，历史阶段仍可复现验收。

本机实测（WSL2）：1000 条并发空闲连接稳定；逐连接回显一致；空闲 2 秒仅消耗 0.020 秒 CPU；512 KiB 数据覆盖半写路径；Valgrind `0 errors`、`no leaks are possible`。

## 已实现的技术要点（P2）

- 增量 HTTP/1.x 状态机：请求行 → 头部 → 空行 → 按 `Content-Length` 读 body，支持半包和粘包。
- keep-alive 与 pipelining：解析器只消费完整请求，剩余字节留给下一个请求；`Connection: close` 会在响应写完后关闭连接。
- 协议限制：头部 8 KiB、body 1 MiB、请求总时长 10 秒；`Transfer-Encoding`、重复 `Content-Length`、非法 Host/百分号编码都会被拒绝。
- 路由：按 method + 路径分发，支持 `{id}` 路径参数；路径匹配但方法不对时返回 405 与 `Allow`。
- 静态文件：`www/` 根目录、常见 MIME、目录默认 `index.html`，并通过 canonical path 校验阻止 `..` 与符号链接逃逸。
- 每连接独立解析上下文；HTTP 层通过弱引用和定时器接入网络层，不破坏 P1 的线程亲和模型。

本机 P2 实测：keep-alive 连续请求、pipeline 双请求、半包请求、Content-Length body、超大 body/header、Transfer-Encoding、408 超时、路径穿越和 HTTP/1.0 关闭语义全部通过；Valgrind `0 errors`、`no leaks are possible`。

## 已实现的技术要点（P3）

- 连接池：启动时预建固定数量连接；互斥锁 + 条件变量管理借还；等待有超时，关闭会唤醒所有等待者。
- 锁外 IO：持锁只做队列操作，`mysql_ping` 和断线重连都在锁外执行，避免一条慢连接卡住整个池。
- 预处理语句：参数通过 `mysql_stmt_*` 二进制协议发送，注入串只当普通字符串；结果集按需扩容，支持 NULL。
- 事务 RAII：`START TRANSACTION` 后必须显式 `commit()`，作用域提前退出时析构自动 `ROLLBACK`。
- 用户 DAO：`findById`、`findByUsername`、`insert` 全部使用参数化 SQL。
- 健康检查：`/api/health` 借一条连接执行 `SELECT 1`，成功 200，失败 503。
- 会话设置：连接统一 `utf8mb4`，数据库会话时区固定为 UTC，金额仍以整数分存储。

本机 P3 实测：schema 连续加载两次成功，9 张表齐全；100 线程并发借还无死锁；连接池耗尽按 80ms 超时返回；连接被 `KILL` 后下一次借出自动重建；Valgrind `0 errors`、definitely/indirectly lost 均为 0。
