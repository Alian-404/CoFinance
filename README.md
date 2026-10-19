# CoFinance

一个支持多人共同记账、实时聊天，并可调用大语言模型提供财务建议的 C++17 后端系统。

> 目标场景：情侣、室友、家庭等需要共同管理财务的小群体（2–20 人）。
> 技术定位：从零实现的 Linux 高并发服务端 —— epoll + 线程池 + 自研 HTTP/WebSocket + MySQL 连接池 + 自研异步日志。

## 当前进度

| 阶段 | 内容 | 状态 |
|---|---|---|
| P0 | 工程骨架、配置系统、异步日志、信号处理 | ✅ 已完成（56 个检查点，valgrind 0 泄漏） |
| P1 | epoll 事件循环 + 线程池 + 非阻塞 socket | ⏳ 进行中 |
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
./build/cofinance_tests                    # 单元测试（沙箱内需 ASAN_OPTIONS=detect_leaks=0）
ctest --test-dir build                     # 通过 CTest 运行

# 泄漏检查（ASan 与 valgrind 不能同时启用，故单独一个构建目录）
cmake -S . -B build-nosan -DCOFINANCE_SANITIZE=OFF && cmake --build build-nosan -j16
timeout -s INT 9 valgrind --leak-check=full --error-exitcode=9 ./build-nosan/cofinance
```

## 目录结构

```
CoFinance/
├── CMakeLists.txt          # 三个 target：cofinance_core / cofinance / cofinance_tests
├── config/                 # server.conf（可提交）+ secrets.conf（600，已 gitignore）
├── sql/                    # 建表脚本（P3 起）
├── docs/
│   ├── learning/           # 原理笔记（PN-xx-*.md），每个阶段的机制与取舍
│   ├── BUGLOG.md           # 真实踩坑记录：现象→复现→定位→根因→修复→教训
│   ├── interview.md        # 面试问答整理
│   └── design.md           # 架构设计（P1 起补全）
├── include/cofinance/      # 对外头文件（按模块分目录）
├── src/
│   ├── main.cpp
│   ├── net/ http/ ws/      # 网络层与协议层（P1/P2/P9）
│   ├── db/ service/ api/   # 数据层、业务层、接口层（P3 起）
│   ├── ai/                 # LLM 调用（P10）
│   └── util/               # 配置、日志、通用工具
├── tests/{unit,integration}/
├── www/                    # 前端静态页（P11）
└── third_party/            # 第三方单头库（如需 vendor）
```

## 文档索引

- 需求与设计基线：`docs/design.md`（源自《CoFinance 项目需求文档 v3.0》）
- 各阶段原理笔记：`docs/learning/`
- 踩坑记录：`docs/BUGLOG.md`
- 面试问答：`docs/interview.md`

## 已实现的技术要点（P0）

- 配置系统：多来源优先级（环境变量 > secrets.conf > server.conf > 默认值）、启动即校验（fail fast）、密钥自动打码
- 异步日志：有界队列 + 单后台线程、分级背压（丢弃并计数 / 等待后同步兜底）、ERROR 立即刷盘、空闲 1s 刷盘、跨天滚动、UTC 时间戳、内核 tid
- 信号处理：async-signal-safe 的处理器、不设 SA_RESTART、忽略 SIGPIPE、优雅退出（排空队列）
- 构建：C++17、target-based CMake、`-Werror` 全告警、ASan/UBSan 插桩、valgrind 复核
