# CoFinance 项目需求文档 v3.0（评审修订版 · 开发基线）

> 基于 v1.0 评审修订 · v2.0 架构重构 · v3.0 吸收外部评审意见并逐条拍板
> 决策人：技术负责人（AI 结对）· 语言：C++17 · 用途：秋招简历项目
> 本版为开发基线（Frozen Baseline）：P0 启动后任何需求/表结构变更都必须追加到第 0 章的变更记录，不允许口头改需求。

## 0. 修订说明

### 0.1 v1.0 → v2.0（架构重构）

| 项 | v1.0 做法 | v2.0 做法 | 原因 |
|---|---|---|---|
| 关系模型 | bindings 一对一绑定 | finance_groups + group_members 多人组 | 室友/家庭通常 ≥3 人，一对一只是 N=2 的特例 |
| 表数量 | 5 张 | 9 张 | 新增组、成员、会话、分类、结算；已读状态并入成员表 |
| 金额类型 | DECIMAL(10,2) | amount_cents BIGINT（分） | 全链路整数运算，彻底消除浮点与舍入误差 |
| 已读状态 | messages.is_read | group_members.last_read_message_id | 一条消息在多人组里没有唯一的「已读位」 |
| 鉴权 | 「返回 token 或 session」，无对应表 | sessions 表 + token 只存哈希 | 原设计无法落地 |
| 密码 | SHA256 加盐 | PBKDF2-HMAC-SHA256，10 万次迭代 | 单次 SHA256 抗不住 GPU 爆破 |
| 实时通道 | 长轮询 + WebSocket 双通道 | P8 长轮询，P9 WebSocket；HTTP 负责持久化与补拉 | 低延迟推送与可靠补拉分离 |
| 代码复用 | 复用 TinyWebServer | 全部从零实现 | 已确认；秋招需要能讲清每一行 |
| API 风格 | /api/register 动词式 | RESTful 资源式 | 更规范，也更容易讲出设计理由 |
| 交付物 | 只有代码 | 代码 + 原理笔记 + 踩坑记录 + 面试问答 | 秋招的胜负手是「能讲清楚」 |

### 0.2 v2.0 → v3.0（外部评审意见处置）

本轮外部评审共 9 条意见：**采纳 4 条、部分采纳 1 条、驳回 2 条、1 条不成立、1 条按原设计保留**（逐条理由见附录 B）。同时技术负责人补充了 8 个评审未发现的真实缺陷（E-1 ~ E-8），其中 4 条升格为 ADR。

| 变动 | v2.0 | v3.0 |
|---|---|---|
| 定长标识列字符集 | CHAR 列沿用 utf8mb4 | ascii + ascii_bin，索引项体积降到 1/4（ADR-18） |
| 幂等键语义 | 只说「客户端生成」 | 明确服务端**不代生成**，缺失即不保证幂等（ADR-16） |
| 消息拉取游标 | 直接 id > after_id | 重叠窗口 + 客户端去重，防自增 id 提交乱序漏消息（ADR-15） |
| 退出 / 解散 | 未定义余额处理 | 余额未结清则拒绝退出，owner 可 force 强制解散（ADR-17） |
| 账号生命周期 | 只有登出 | 增加修改密码、注销账户（软删） |
| 组角色 | 只有 owner / member | 增加转让 owner 接口 |
| 分类管理 | 有表无接口 | 补齐分类增删查，并定义「分类下有交易不可删」 |
| 长轮询参数 | after_id 语义含糊 | 明确缺省语义，响应回带 server_max_id |
| 限流与后台清理 | 只在错误码里提了一句 429 | 新增 5.8 节：限流、挂起数上限、过期数据清理 |
| 消息撤回 / 账本导出 | 未提及 | 驳回，列入二期候选（附录 B） |

## 1. 项目概述

- 项目名称：CoFinance
- 一句话描述：一个支持多人共同记账、实时聊天，并可调用大语言模型提供财务建议的 C++ 后端系统。
- 目标用户：情侣、室友、家庭等需要共同管理财务的小群体（2–20 人）。
- 核心价值：让共同财务透明、可查、可结算，并能用自然语言问账。
- 技术定位：从零实现的 Linux C++ 高并发服务端，覆盖 epoll、线程池、HTTP/WebSocket 协议、MySQL 连接池与事务、异步日志、外部 API 调用。

明确的 Non-goals（本期不做，避免范围失控）：

- 不做真实银联/支付通道对接，结算是「记账式」的，不产生真实资金流动。
- 不做多币种汇率换算，currency 字段只做预留，全局按 CNY 记账。
- 不做移动端 App、不做 APNs/FCM 离线推送。
- 不做复杂的 RBAC 权限体系，只有 owner / member 两级。
- 不做端到端加密聊天（可作为面试时的「如果要做」延伸谈资）。
- 不做消息撤回、不做账本导出 CSV（列入二期候选清单，理由见附录 B）：它们只扩张 CRUD 面，对并发、协议、事务这些核心难点没有增量。

## 2. 技术决策记录（ADR）

每条 ADR 记录「决策 / 理由 / 代价」，这也是面试时最能体现工程判断力的部分。

### ADR-01 关系模型用多人组而非一对一绑定

- 决策：用 finance_groups（组）+ group_members（成员）建模，「绑定」退化为「邀请并加入一个 2 人组」。
- 理由：室友、家庭天然是多人场景；一对一模型一旦要加人就需要改表、改 API、改前端。组模型让所有下游表统一挂 group_id。
- 代价：多一层成员状态机（待接受/已加入/已拒绝/已退出/被移除），鉴权必须每次校验「请求者是否是该组成员」。

### ADR-02 金额统一用「分」的整数存储

- 决策：数据库用 amount_cents BIGINT，C++ 用 int64_t，接口出入参也用整数分。
- 理由：double 表示 0.1+0.2 != 0.3；DECIMAL 虽精确，但 MySQL 返回的是字符串，C++ 侧仍要自己实现定点数解析与进位，容易写错。
- 代价：展示层需要除以 100 格式化；将来若要支持两位以上小数（如汇率），需要引入 Decimal 类型。

### ADR-03 已读状态用「水位线」而不是逐条已读

- 决策：group_members.last_read_message_id 记录每个成员已读到哪条消息；某人是否已读某条消息 = 他的水位线 >= 该消息 id。
- 理由：逐条 message_reads 表在 20 人群里是 N×M 行，写入放大严重；水位线是 O(1) 更新，且天然支持「未读数」计算。
- 代价：无法表达「跳读」（读了 100 条没读 98 条），也无法做单条消息的精确已读回执列表。若将来需要，可加一张 message_reads 表做补充。

### ADR-04 鉴权用「不透明随机 token + 服务端会话表」

- 决策：登录签发 32 字节随机 token（hex 64 字符），数据库只存 SHA256(token)，有效期 7 天，活跃即滑动续期。
- 理由：DB 泄漏时 token 无法直接使用；相比 JWT 不需要引入额外库，且天然支持「登出即失效」「踢下线」——JWT 反而做不到。
- 代价：每个请求多一次 sessions 查询（可加本地 LRU 缓存优化）；多实例部署需要共享存储。

### ADR-05 实时通道：长轮询先行，WebSocket 后补，最终双通道并存

- 决策：P8 实现长轮询（GET /api/groups/{id}/messages/poll?after_id=&timeout=30），P9 再实现 WebSocket 升级。
- 理由：长轮询能复用已有 HTTP 层快速跑通全链路；WebSocket 需要握手、帧解析、掩码、分片、心跳，属于本项目最重的一块，放在全链路稳定之后做。
- 代价：长轮询有连接空转开销；WebSocket 增加握手、帧状态机、掩码、分片与心跳复杂度。P9 完成后两条通道共用同一消息提交通知，但客户端仍以 HTTP 增量拉取作为断线恢复的可靠兜底。

### ADR-06 HTTP 层自研，只做必需子集

- 决策：HTTP/1.1，支持 GET/POST/PUT/DELETE、Content-Length 请求体、keep-alive、JSON；不支持 chunked 请求体、不支持 multipart、不做 HTTPS 服务端。
- 理由：范围可控且足够支撑全部业务；协议的难点（半包/粘包/状态机）该有的都有，讲解价值不会被边缘特性淹没。
- 代价：文件上传类需求需要另想办法（见 ADR-07）。

### ADR-07 头像走预置 URL，不做文件上传

- 决策：avatar_url 存相对路径，前端提供一组预置头像供选择。
- 理由：multipart/form-data 解析成本高，对简历叙事增益极低。
- 代价：用户无法上传自定义图片；二期若要做，可单独加 POST /api/files + multipart 解析模块。

### ADR-08 AI 模块独立线程池 + 外部依赖降级

- 决策：AIService 走独立有界线程池调用 DeepSeek（libcurl + TLS），连接/总超时分离，失败返回可读降级；API Key 只放 config/secrets.conf（gitignore）。
- 理由：外部 API 的延迟不可控，绝不能占用核心业务线程池，否则一次网络抖动会拖垮整个服务。
- 代价：多一层线程池与结果回传机制；需要处理「用户已断开但 LLM 刚返回」的悬垂回调。

### ADR-09 事务边界与金额一致性校验

- 决策：新增交易 = 「写 transactions + 批量写 transaction_participants + 更新统计」放进同一个 MySQL 事务；并在服务层强校验 Σshare_cents == amount_cents。
- 理由：分摊之和不等于总额会直接污染结算结果，必须做业务约束而不是只靠前端。
- 代价：事务持有连接时间变长，需要关注连接池容量与锁等待。

### ADR-10 幂等键防止重复提交

- 决策：messages 与 transactions 都支持客户端生成的 client_token / client_msg_id，服务端建唯一索引；重复请求返回首次结果而非报错。
- 理由：长轮询场景下客户端重试很常见，没有幂等键就会出现重复扣账这种致命 bug。
- 代价：需要额外的冲突处理分支；唯一索引冲突要能识别出来并回查。

### ADR-11 列表接口用游标分页而非 OFFSET

- 决策：统一 limit + cursor（上一页最后一条的 id），返回 next_cursor，无更多时为 null。
- 理由：OFFSET 在深翻页时性能线性劣化；游标分页 O(1) 定位，且不会因新数据插入而重复/漏项。
- 代价：无法跳页；客户端只能无限滚动或「加载更多」。

### ADR-12 全链路越权校验（防 IDOR）

- 决策：所有 /api/groups/{gid}/... 接口必须先在中间件校验「当前用户是该组 status=1 的成员」，不满足直接 403。
- 理由：这类「改个 id 就能看别人账本」的漏洞是同类学生项目最常见的硬伤，也是面试官最爱抓的点。
- 代价：每请求多一次成员校验查询（可由本地缓存或单条 JOIN 优化）。

### ADR-13 密码用 PBKDF2-HMAC-SHA256

- 决策：16 字节随机盐 + 10 万次迭代，存 "pbkdf2_sha256$iterations$salt$hash" 格式串。
- 理由：OpenSSL 自带实现，零新依赖；相比单次 SHA256 加盐，暴力破解成本高 5 个数量级。
- 代价：登录耗时约 10–50ms（可接受，且本身就是防爆破的天然限流）。

### ADR-14 时间统一 UTC 存储

- 决策：所有 DATETIME 存 UTC，接口返回 ISO8601 带 Z；由前端做本地化。
- 理由：避免时区/夏令时引起「这个月的账算错了」这类看似玄学的问题。
- 代价：调试时需要在心里做时差换算；日志也建议统一 UTC。

### ADR-15 消息拉取使用「重叠窗口 + 客户端去重」，而非严格 id > after_id

- 决策：poll / 历史接口返回 {items, server_max_id}；客户端下次以 server_max_id − K（K=20）作为 after_id 回拉，并按消息 id 去重。
- 理由：InnoDB 自增 id 在「分配」时确定，但事务「提交」顺序可能乱序。若 id=100 的事务尚未提交、id=101 已提交，此刻的快照读只能看到 101；客户端把游标推进到 101 之后，100 才提交，就永远拉不到了。长轮询按 id 增量拉取会**静默漏消息**，且概率随并发升高 —— 这是「看起来对、压力下才错」的典型问题。
- 代价：客户端必须实现去重与窗口回退，接口多返回一个字段。备选方案是给 poll 查询加锁读（LOCK IN SHARE MODE）强制串行化，但会把高频只读路径拖进锁竞争，坚决不采用。

### ADR-16 幂等键必须由客户端生成，服务端不代生成

- 决策：client_token / client_msg_id 由客户端生成并在重试时复用；服务端只做「有则校验、无则照常写入」，绝不代生成。
- 理由：幂等的前提是「同一个逻辑请求携带同一个 key」。若服务端在缺失时自行生成 UUID，重试请求就会拿到一个全新的 key，去重彻底失效 —— 这比不做幂等更有害，因为它给出的是**虚假的安全感**。
- 代价：不传 client_token 的请求不具备幂等保证（文档中显式声明，不假装安全）。MySQL 唯一索引对 NULL 不生效，正好对应「无 key 不参与去重」的语义。

### ADR-17 退出 / 解散必须处理未结清余额

- 决策：成员退出或组解散前先计算该成员在本组的净额；净额非 0 且未选择强制时返回 1015 拒绝；owner 解散可传 force=true 强制通过，并把最终余额快照写入一条系统消息存档。
- 理由：成员退出后仍背着未结清的账，会让「谁欠谁」的名单里长期挂着一个已退出用户，账目永远无法收敛。这是 v2 的真实漏洞，评审未发现。
- 代价：退出流程多一次聚合查询；前端需要给出「先结算」的引导与强制退出二次确认。

### ADR-18 定长标识列使用 ascii 字符集

- 决策：token_hash(64)、client_token(36)、client_msg_id(36)、currency(3) 等纯 ASCII 定长列，显式声明 CHARACTER SET ascii COLLATE ascii_bin。
- 理由：utf8mb4 下 CHAR(64) 的索引项最多占 256 字节，ascii 下只有 64 字节，索引更小、缓存命中更高、比较更快（ascii_bin 是二进制比较，语义也更精确）。
- 代价：内容必须确实是 ASCII（token/hex 天然满足）；需要逐列声明，容易遗漏，用建表 checklist 兜住。

## 3. 功能需求

### 3.1 用户系统

- 注册：用户名 + 密码；校验用户名唯一、格式（4–20 位字母数字下划线）、密码强度（≥8 位）。
- 登录：返回 token、expires_at 与用户信息；同一账号允许多端登录（多会话）。
- 登出：删除当前会话；支持「登出所有设备」。
- 个人信息：查看/修改昵称、头像 URL。
- 会话过期：7 天；过期请求返回 401 + 明确错误码，前端跳登录页。
- 修改密码：校验旧密码，成功后吊销该用户的其它所有会话（当前会话保留）。
- 注销账户：软删（deleted_at = 注销时间），吊销全部会话，退出所有组；历史交易保留，列表展示为「已注销用户」。
- 账号注销与禁用都不物理删除交易记录，因为账目必须保持可追溯。

### 3.2 组与成员

- 创建组：创建者为 owner，自动成为 status=1 的成员。
- 邀请：owner 或成员按用户名邀请他人；被邀请人在「我的邀请」里看到待处理列表。
- 接受/拒绝：被邀请人操作；接受后 status=1 并写入 joined_at。
- 退出：普通成员可退出（status=3）；owner 退出前必须转让或解散。
- 移除：owner 可移除成员（status=4）。
- 解散：owner 解散后组内成员不再可见该组（软删，保留数据）。
- 查询：我的组列表（带未读数、成员数、最近一条消息/最近的账）。
- 转让 owner：仅 owner 可发起；事务内完成「更新 finance_groups.owner_id + 原 owner 降级 + 目标成员升级」，目标必须是组内 status=1 的成员。
- 退出前置条件：该成员结算后的净额必须为 0，否则拒绝退出（错误码 1015，见 ADR-17）。
- 解散前置：owner 可传 force=true 强制解散；解散前把最终余额快照写入一条系统消息，保证账目可追溯。
- 约束：单组最多 20 人；不能邀请自己；同一用户在同一组只有一条记录。待接受邀请超过 7 天自动过期（见 5.8）。

### 3.3 共同账本

- 新增交易：类型（支出/收入）、金额（分）、分类、备注、付款人、参与人及分摊（均分或自定义）、幂等键。
- 编辑/删除：仅创建者或 owner 可改；删除为软删（deleted_at），统计口径自动排除。
- 列表：按时间倒序游标分页；支持按分类、付款人和单个交易日期筛选。界面只保留一个日期，API 仍用 UTC 半开区间表达该本地自然日。
- 统计：总支出、总收入、分类占比、按成员支出、按成员应收应付。
- 净额：「谁欠谁」——balance(u) = Σ付款 − Σ分摊（收入项方向相反），再叠加已确认结算。
- 一键结算：先拆分最多零和子集，再组内配对最大债权/最大债务，给出全局最少笔数建议；确认后落库为 settlement。
- 结算记录：可查看历史结算。
- 分类管理：系统预置分类不可改删；组内自定义分类可增、可查、可删；分类下仍有未删除交易时禁止删除（错误码 1014）。
- 与退出的联动：成员退出前必须结清净额，避免「已退出但仍欠钱」的悬空账（ADR-17）。

### 3.4 实时聊天

- 发送消息：文本消息 + 系统消息（如「A 添加了一笔 120.00 元的餐饮支出」）。
- 历史消息：游标分页拉取，支持 after_id 增量拉取。
- 已读：上报 last_read_message_id；未读数 = 该组内 id 大于自己水位线的消息数。
- 长轮询：单次最长挂起 30s，有新消息立即返回；无消息则超时返回空。
- 游标语义：after_id 缺省 = 只拉「此刻之后的新消息」（以服务端当前最大 id 为下界）；after_id 非空 = 拉取 id > after_id 的消息。响应统一回带 server_max_id。
- 不丢消息约定：客户端下次以 server_max_id − 20 作为 after_id 回拉并按消息 id 去重（ADR-15），这是应对自增 id 提交乱序的必要代价。
- WebSocket（P9）：握手后全双工推送，30s 心跳，断线由客户端重连并补拉增量。

### 3.5 AI 助手（二期）

- 提问：「这个月我们花了多少？」「谁花得最多？」
- 实现：把该组聚合后的统计（不是原始明细）放进 prompt，调 DeepSeek 返回自然语言回答。
- 省钱建议：基于分类占比与环比生成建议。
- 约束：超时降级、限流（每用户每分钟 5 次）、不传个人隐私明细、Key 不入库不入仓。

## 4. 数据库设计

### 4.1 通用约定

- 引擎 InnoDB，字符集 utf8mb4（兼容 emoji），排序规则 utf8mb4_0900_ai_ci。
- 主键统一 BIGINT UNSIGNED AUTO_INCREMENT；金额统一 *_cents BIGINT。
- 时间字段统一 DATETIME，存 UTC，默认 CURRENT_TIMESTAMP，updated_at 用 ON UPDATE CURRENT_TIMESTAMP。
- 逻辑删除用 deleted_at（NULL = 未删）；状态用 TINYINT 枚举并在 COMMENT 中写明含义。
- 表名 group 是 MySQL 8.0 保留字（窗口函数 GROUPS），故用 finance_groups 规避，并在 SQL 中避免裸写 groups。
- 纯 ASCII 定长标识列显式声明 CHARACTER SET ascii COLLATE ascii_bin（ADR-18）：token_hash、client_token、client_msg_id、currency。
- 所有主键 / 外键统一 BIGINT UNSIGNED，避免有符号溢出与跨表 JOIN 时的隐式类型转换导致索引失效。

### 4.2 表结构

**表 1：users（用户表）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| id | BIGINT UNSIGNED | PK, AI | 用户 ID |
| username | VARCHAR(50) | UNIQUE, NOT NULL | 登录名 |
| password_hash | VARCHAR(255) | NOT NULL | PBKDF2 格式串 |
| nickname | VARCHAR(50) | NOT NULL DEFAULT '' | 昵称 |
| avatar_url | VARCHAR(255) | NOT NULL DEFAULT '' | 头像相对路径 |
| status | TINYINT | NOT NULL DEFAULT 0 | 0=正常, 1=禁用, 2=已注销 |
| deleted_at | DATETIME | NULL | 注销时间（软删） |
| created_at | DATETIME | NOT NULL | 创建时间 |
| updated_at | DATETIME | NOT NULL | 更新时间 |

**表 2：finance_groups（共同财务组）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| id | BIGINT UNSIGNED | PK, AI | 组 ID |
| name | VARCHAR(50) | NOT NULL | 组名（如「402 宿舍」） |
| owner_id | BIGINT UNSIGNED | NOT NULL, IDX | 组长 |
| status | TINYINT | NOT NULL DEFAULT 0 | 0=正常, 1=已解散 |
| created_at | DATETIME | NOT NULL | 创建时间 |
| updated_at | DATETIME | NOT NULL | 更新时间 |

**表 3：group_members（组成员与邀请）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| id | BIGINT UNSIGNED | PK, AI | 主键 |
| group_id | BIGINT UNSIGNED | NOT NULL, UK(group_id,user_id) | 组 ID |
| user_id | BIGINT UNSIGNED | NOT NULL | 用户 ID |
| role | TINYINT | NOT NULL DEFAULT 1 | 0=owner, 1=member |
| status | TINYINT | NOT NULL DEFAULT 0 | 0=待接受, 1=已加入, 2=已拒绝, 3=已退出, 4=被移除, 5=邀请已过期 |
| invited_by | BIGINT UNSIGNED | NOT NULL DEFAULT 0 | 邀请人，0=创建者自助加入 |
| last_read_message_id | BIGINT UNSIGNED | NOT NULL DEFAULT 0 | 已读水位线（ADR-03） |
| created_at | DATETIME | NOT NULL | 邀请时间 |
| joined_at | DATETIME | NULL | 加入时间 |

**表 4：categories（消费分类）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| id | BIGINT UNSIGNED | PK, AI | 分类 ID |
| group_id | BIGINT UNSIGNED | NOT NULL DEFAULT 0, UK(group_id,name) | 0=系统预置分类 |
| name | VARCHAR(50) | NOT NULL | 分类名 |
| icon | VARCHAR(50) | NOT NULL DEFAULT '' | 图标标识 |
| sort | INT | NOT NULL DEFAULT 0 | 排序 |

**表 5：transactions（交易记录）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| id | BIGINT UNSIGNED | PK, AI | 交易 ID |
| group_id | BIGINT UNSIGNED | NOT NULL, IDX(group_id,type,created_at) | 所属组 |
| payer_id | BIGINT UNSIGNED | NOT NULL | 付款人 |
| type | TINYINT | NOT NULL DEFAULT 0 | 0=支出, 1=收入 |
| amount_cents | BIGINT | NOT NULL | 金额（分），恒为正 |
| currency | CHAR(3) ascii | NOT NULL DEFAULT 'CNY' | 币种（预留） |
| category_id | BIGINT UNSIGNED | NOT NULL DEFAULT 0 | 分类 |
| note | VARCHAR(255) | NOT NULL DEFAULT '' | 备注 |
| split_type | TINYINT | NOT NULL DEFAULT 0 | 0=均分, 1=自定义 |
| client_token | CHAR(36) ascii | NULL, UK(created_by,client_token) | 幂等键，客户端生成（ADR-10 / 16） |
| created_by | BIGINT UNSIGNED | NOT NULL | 创建人 |
| created_at | DATETIME | NOT NULL | 创建时间 |
| updated_at | DATETIME | NOT NULL | 更新时间 |
| deleted_at | DATETIME | NULL | 软删标记 |

**表 6：transaction_participants（交易参与人与分摊）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| id | BIGINT UNSIGNED | PK, AI | 主键 |
| transaction_id | BIGINT UNSIGNED | NOT NULL, UK(transaction_id,user_id) | 交易 ID |
| user_id | BIGINT UNSIGNED | NOT NULL, IDX | 参与人 |
| share_cents | BIGINT | NOT NULL DEFAULT 0 | 该用户分摊（分） |

**表 7：messages（消息）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| id | BIGINT UNSIGNED | PK, AI | 消息 ID |
| group_id | BIGINT UNSIGNED | NOT NULL, IDX(group_id,id) | 所属组 |
| sender_id | BIGINT UNSIGNED | NOT NULL | 发送者（系统消息为 0） |
| type | TINYINT | NOT NULL DEFAULT 0 | 0=文本, 1=系统消息 |
| content | TEXT | NOT NULL | 内容 |
| client_msg_id | CHAR(36) ascii | NULL, UK(sender_id,client_msg_id) | 幂等键，客户端生成（ADR-16） |
| created_at | DATETIME | NOT NULL | 发送时间 |

**表 8：sessions（登录会话）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| token_hash | CHAR(64) ascii | PK, ascii_bin | SHA256(token) 十六进制 |
| user_id | BIGINT UNSIGNED | NOT NULL, IDX | 用户 |
| created_at | DATETIME | NOT NULL | 签发时间 |
| expires_at | DATETIME | NOT NULL, IDX | 过期时间 |
| last_seen_at | DATETIME | NOT NULL | 最近活跃（滑动续期用） |

**表 9：settlements（结算记录）**

| 字段 | 类型 | 约束 | 说明 |
|---|---|---|---|
| id | BIGINT UNSIGNED | PK, AI | 结算 ID |
| group_id | BIGINT UNSIGNED | NOT NULL, IDX(group_id,status) | 所属组 |
| from_user_id | BIGINT UNSIGNED | NOT NULL | 付款方（应付） |
| to_user_id | BIGINT UNSIGNED | NOT NULL | 收款方（应收） |
| amount_cents | BIGINT | NOT NULL | 金额（分） |
| status | TINYINT | NOT NULL DEFAULT 0 | 0=待确认, 1=已确认, 2=已取消 |
| created_by | BIGINT UNSIGNED | NOT NULL DEFAULT 0 | 发起人 |
| created_at | DATETIME | NOT NULL | 创建时间 |
| confirmed_at | DATETIME | NULL | 确认时间 |

### 4.3 关键建表 SQL

```sql
CREATE TABLE users (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  username      VARCHAR(50)  NOT NULL,
  password_hash VARCHAR(255) NOT NULL,
  nickname      VARCHAR(50)  NOT NULL DEFAULT '',
  avatar_url    VARCHAR(255) NOT NULL DEFAULT '',
  status        TINYINT      NOT NULL DEFAULT 0 COMMENT '0=正常,1=禁用,2=已注销',
  deleted_at    DATETIME     NULL DEFAULT NULL COMMENT '注销时间',
  created_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_username (username)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='用户表';

CREATE TABLE finance_groups (
  id         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  name       VARCHAR(50) NOT NULL,
  owner_id   BIGINT UNSIGNED NOT NULL,
  status     TINYINT NOT NULL DEFAULT 0 COMMENT '0=正常,1=已解散',
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_owner (owner_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='共同财务组';

CREATE TABLE group_members (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id      BIGINT UNSIGNED NOT NULL,
  user_id       BIGINT UNSIGNED NOT NULL,
  role          TINYINT NOT NULL DEFAULT 1 COMMENT '0=owner,1=member',
  status        TINYINT NOT NULL DEFAULT 0 COMMENT '0=待接受,1=已加入,2=已拒绝,3=已退出,4=被移除,5=邀请已过期',
  invited_by    BIGINT UNSIGNED NOT NULL DEFAULT 0,
  last_read_message_id BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  joined_at     DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_group_user (group_id, user_id),
  KEY idx_user_status (user_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='组成员与邀请';

CREATE TABLE categories (
  id       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=系统预置',
  name     VARCHAR(50) NOT NULL,
  icon     VARCHAR(50) NOT NULL DEFAULT '',
  sort     INT NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uk_group_name (group_id, name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='消费分类';

CREATE TABLE transactions (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id     BIGINT UNSIGNED NOT NULL,
  payer_id     BIGINT UNSIGNED NOT NULL,
  type         TINYINT NOT NULL DEFAULT 0 COMMENT '0=支出,1=收入',
  amount_cents BIGINT NOT NULL COMMENT '金额(分)，恒为正',
  currency     CHAR(3) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT 'CNY',
  category_id  BIGINT UNSIGNED NOT NULL DEFAULT 0,
  note         VARCHAR(255) NOT NULL DEFAULT '',
  split_type   TINYINT NOT NULL DEFAULT 0 COMMENT '0=均分,1=自定义',
  client_token CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL DEFAULT NULL,
  created_by   BIGINT UNSIGNED NOT NULL,
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  deleted_at   DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_creator_token (created_by, client_token),
  KEY idx_group_time (group_id, type, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='交易记录';

CREATE TABLE transaction_participants (
  id             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  transaction_id BIGINT UNSIGNED NOT NULL,
  user_id        BIGINT UNSIGNED NOT NULL,
  share_cents    BIGINT NOT NULL DEFAULT 0,
  PRIMARY KEY (id),
  UNIQUE KEY uk_tx_user (transaction_id, user_id),
  KEY idx_user (user_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='交易参与人与分摊';

CREATE TABLE messages (
  id            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id      BIGINT UNSIGNED NOT NULL,
  sender_id     BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=系统消息',
  type          TINYINT NOT NULL DEFAULT 0 COMMENT '0=文本,1=系统',
  content       TEXT NOT NULL,
  client_msg_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL DEFAULT NULL,
  created_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_sender_client (sender_id, client_msg_id),
  KEY idx_group_id (group_id, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='消息';

CREATE TABLE sessions (
  token_hash   CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  user_id      BIGINT UNSIGNED NOT NULL,
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  expires_at   DATETIME NOT NULL,
  last_seen_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (token_hash),
  KEY idx_user (user_id),
  KEY idx_expires (expires_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='登录会话';

CREATE TABLE settlements (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  group_id     BIGINT UNSIGNED NOT NULL,
  from_user_id BIGINT UNSIGNED NOT NULL COMMENT '付款方(应付)',
  to_user_id   BIGINT UNSIGNED NOT NULL COMMENT '收款方(应收)',
  amount_cents BIGINT NOT NULL,
  status       TINYINT NOT NULL DEFAULT 0 COMMENT '0=待确认,1=已确认,2=已取消',
  created_by   BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at   DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  confirmed_at DATETIME NULL DEFAULT NULL,
  PRIMARY KEY (id),
  KEY idx_group (group_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='结算记录';
```

> MySQL 空值陷阱提醒（两个方向都要记住）：
> 唯一索引把 NULL 视为互不相同 —— 所以 client_token 允许为空（无幂等键的交易不会互相冲突）；
> 反过来，系统分类用 group_id=0 而不是 NULL，否则 uk_group_name 拦不住重复的系统分类。

### 4.4 资金计算公式

- 交易净额：balance_tx(u) = +Σ(该用户作为 payer 的支出) − Σ(该用户在所有支出中的 share) − Σ(收入项中 payer 的金额) + Σ(收入项中的 share)。
- 结算净额：balance_settle(u) = +Σ(已确认结算中 u 作为 from_user 的金额) − Σ(u 作为 to_user 的金额)。付款方完成转账后债务减少，因此 from 的余额上升、to 的余额下降。
- 最终净额：balance(u) = balance_tx(u) + balance_settle(u)。> 0 表示「别人欠他」，< 0 表示「他欠别人」。
- 结算建议：先把非零余额划分成尽可能多的零和子集（n≤20，子集 DP，O(n·2^n)），再在每个子集内按最大债权/最大债务贪心配对。这样总笔数严格最少，最多 n−1 笔；相同金额按 user_id 排序保证结果稳定。
- 均分精度：amount_cents 除人数除不尽时，余数依次分给前 k 个人（k = 余数），保证 Σshare 精确等于 amount。

## 5. 模块设计

### 5.1 分层与依赖方向

依赖必须单向：api → service → db，net/http 只被 api 使用，util 被所有层使用但 util 不反向依赖任何业务。

| 层 | 职责 | 关键组件 |
|---|---|---|
| net | 事件循环、连接生命周期、线程池、定时器 | EventLoop(epoll)、Connection、ThreadPool、TimerQueue、Notifier(eventfd) |
| http | 协议解析与生成、路由、静态文件、鉴权中间件 | HttpParser、HttpRequest/Response、Router、StaticFileHandler、AuthMiddleware |
| db | 连接池、SQL 执行、结果集、事务 RAII | ConnectionPool、ConnectionGuard、TransactionGuard、各 DAO |
| service | 业务规则、事务边界、越权校验 | UserService、GroupService、TransactionService、SettleService、MessageService、AIService |
| api | 参数解析与校验、调用 service、序列化 | 各 Controller、统一响应封装、错误码映射 |
| util | 配置、日志、JSON、字符串、时间、加密 | Config、Logger、Json、Crypto、TimeUtil |
| ai | 外部大模型调用与降级 | LlmClient(libcurl)、PromptBuilder、专用线程池 |
| web | 浏览器单页与交互 | 原生 ES Modules、hash 路由、视图 cleanup、WebSocket/长轮询客户端 |

设计要点：Controller 里不允许出现 SQL；Service 里不允许出现 HTTP 相关类型；DAO 只返回结构体，不返回 JSON。

### 5.2 并发模型（本项目最核心的难点）

- 主从 Reactor：main 线程只负责 accept，把新连接按 round-robin 分给 N 个 worker 线程，每个 worker 持有自己的 epoll 实例与定时器。
- 一个连接固定绑定一个 worker（连接级的线程亲和性），避免多线程同时读写同一 fd 造成竞态与响应交叉。
- worker 内是「单线程事件循环 + 非阻塞 IO」；耗时的数据库操作在 worker 线程同步执行，由连接池限流（池满则短暂等待或直接返回 503），不引入异步 DB 的复杂度。
- 跨线程通知：每个 worker 一个 eventfd + 任务队列（mpsc）。这是长轮询能落地的关键——外部线程只能往队列里塞任务并写 eventfd 唤醒目标 loop，绝不直接碰别人的连接。
- 定时器：用 timerfd 挂进 epoll（比在 epoll_wait 上用最小超时更精确、更好讲），统一管理「会话超时、长轮询超时、WebSocket 心跳」。

**长轮询为什么不占线程池线程（必考）**

直觉方案是「收到 poll 请求就让线程 sleep 30 秒再返回」，这会把线程池瞬间打满，1000 个挂起请求就能让服务停滞。正确做法：

1. 解析请求后先检查 DB 是否有 after_id 之后的新消息；有则直接返回，结束。
2. 没有则把该连接标记为 POLLING，从 epoll 摘掉读事件（或保留只处理对端关闭），并把请求登记到 PollRegistry[group_id]。
3. 用 TimerQueue 注册一个 30s 超时任务；超时触发则返回空结果并清理登记。
4. 任何线程调用 notify(group_id) 时，把「唤醒这批 poll 连接」的任务投递到连接所属 worker 的队列；worker 被 eventfd 唤醒后生成响应并重新注册写事件。
5. 连接被对端关闭 / 超时 / 送达，三条路径都必须能正确注销定时器与登记，否则就是 use-after-free 或内存泄漏。

先查库再登记会丢消息（查完与登记之间到达的消息没人通知），所以实现里用「先登记，再查一次库，有则立即触发」，这是典型的「注册-检查」竞态处理。

按 id 增量拉取还有一个必须处理的前提：自增 id 的「分配顺序」不等于「提交顺序」（ADR-15）。因此服务端返回的 server_max_id 不能直接当成不可回退的水位线，客户端必须做「重叠窗口 + 按 id 去重」，否则高并发下会静默漏消息。这类「看起来对、压力下才错」的问题，正是 P8 要用并发压测 + 消息完整性校验（发 N 条收 N 条）反复验证的部分。

**WebSocket 升级（P9）**

- 握手：校验 `Upgrade: websocket`、`Connection: Upgrade`、版本 13 与 16 字节 Base64 key；服务端返回原始 `101` 与 `Sec-WebSocket-Accept = Base64(SHA1(key + GUID))`。
- 帧解析：HTTP 头之后可能紧跟首帧，因此 Upgrade 成功时把当前读缓冲剩余字节直接交给 WebSocket 会话；之后该连接的 message callback 不再进入 HTTP 解析器。
- 掩码与分片：客户端帧必须掩码；控制帧不能分片且载荷不超过 125 字节；文本分片按 `Text(FIN=0) + Continuation... + Continuation(FIN=1)` 聚合后再统一校验 UTF-8。
- 心跳：连接从全局 HTTP 空闲定时器切换到 WebSocket 会话定时器；30s 发 Ping，90s 无入站活动用 1001 关闭，避免僵尸连接长期占用订阅名额。
- 推送：`RealtimeRegistry` 在锁内复制订阅回调，锁外执行；回调只把任务 `runInLoop` 回连接所属 worker，绝不跨线程写 socket。
- 生命周期：成员退出、被移除、组解散在数据库事务提交后注销订阅；订阅后再次校验成员身份，封堵“握手校验完成后恰好被移除”的竞态窗口。
- 可靠性：WebSocket 只做实时通知；断线期间的消息仍持久化在 MySQL，客户端重连后按 `server_max_id`/`after_id` 通过 HTTP 补拉并去重。

**AI 异步调用（P10）**

- 路由：两个 AI 接口注册为异步路由，HTTP 处理线程只完成鉴权、路径/JSON 校验和任务入队；返回 `Pending` 后连接等待完成回调，不阻塞 Reactor。
- 线程池：默认 2 个工作线程、64 个队列槽位；任务在池内取统计、构造 prompt、执行 curl 请求。队列满直接返回 `503/2002`，避免外部依赖抖动向进程内存传导。
- 降级：`llm.connect_timeout_ms` 与 `llm.timeout_ms` 分离；未配置 Key、上游 5xx/坏 JSON/超大响应返回 `503/2002`，超时返回 `504/2002`，核心记账与聊天接口不受影响。
- 取消：客户端断开时取消令牌置位；curl progress callback 中止传输，工作线程在调用完成回调前再次检查令牌，防止向已释放连接回写。
- 数据边界：prompt 只含当前月/上月聚合统计、分类占比与成员昵称级收支汇总；备注、用户名、逐笔明细、聊天内容和 API Key 均不进入 prompt。
- 限流与缓存：每用户 5 次/分钟滑动窗口；省钱建议按组和聚合数据指纹缓存到 UTC 当日结束，账本变化后指纹变化，不会命中旧结果。

### 5.3 HTTP 层

- 状态机解析：请求行 → 头字段 → 空行 → 按 Content-Length 读 body；关键在于半包（一次 read 拿到半个请求）与粘包（一次读到 1.5 个请求）都要正确处理，读缓冲剩余字节要留给下一次解析。
- keep-alive：解析完一个请求后重置状态机，保留读缓冲；Connection: close 或解析出错才关闭。
- 限制：请求行与头部总长 8KB、body 1MB、超时 10s，防止慢速攻击（Slowloris）。
- 路由：静态前缀表/正则表匹配，支持路径参数 {id}；按 method + 路径分发到 Controller。
- 鉴权中间件：从 Authorization: Bearer 取 token → SHA256 → 查 sessions（带 LRU 缓存）→ 校验过期 → 注入 userId 到请求上下文；组内接口再校验成员身份（ADR-12）。

### 5.4 数据库层

- 连接池：启动时预建 N 条连接（如 CPU 核数 ×2 + 磁盘数），用互斥锁 + 条件变量做借还；借出时校验连接有效性（ping 或 mysql_ping），避免 MySQL 8 小时空闲断连问题；RAII 归还。
- DAO：每张表一个 DAO，只提供参数化查询（防 SQL 注入，这一点面试常问）。
- 事务：TransactionGuard 在构造时 BEGIN、析构时若未 commit 则 ROLLBACK，杜绝漏提交/漏回滚。
- 要点：结算、记账等写操作使用事务；统计类查询用聚合 SQL 而不是把数据拉到内存里算（除非确实需要复杂逻辑）。

### 5.5 业务层规则速览

- UserService：注册查重、密码哈希、登录签发会话、会话滑动续期。
- GroupService：创建/邀请/接受/拒绝/退出/移除/解散，成员状态机校验（例如只有 status=0 的邀请才能被接受）。
- TransactionService：金额与分摊校验（Σshare == amount）、参与人必须是组内成员、写事务、发系统消息。
- SettleService：净额计算、子集 DP + 组内贪心结算建议、确认落库。
- MessageService：发消息（幂等）、拉取、已读水位线更新、触发长轮询通知。

### 5.6 日志与可观测性

- 异步日志：业务线程只做「格式化 + 入队」，后台线程负责落盘；队列满时按级别降级（drop DEBUG/WARN，保 INFO 以上），绝不阻塞业务线程。
- 访问日志：method 路径 状态码 耗时 用户 id 连接 id；错误日志带 request_id 便于串联。
- 轻量指标：当前连接数、QPS、连接池使用率、定时器数量、长轮询挂起数，通过 /api/metrics（仅内网/带 token）暴露，压测时用来定位瓶颈。

### 5.7 测试策略

- 单元测试：Service 层用一次性测试库（每个用例独立 schema 或用事务回滚）；解析器/连接池/路由这类纯逻辑重点覆盖边界。
- 集成测试：bash + curl 脚本跑通「注册→建组→邀请→记账→统计→结算→聊天」全流程，作为每阶段的验收凭证。
- 压测：wrk 压普通接口；长轮询单独写多线程客户端（模拟 M 条并发挂起连接）；WebSocket 用 wscat 或自写脚本。
- 内存与并发：Debug 构建全量跑一遍 -fsanitize=address,undefined；并发相关模块用 -fsanitize=thread 跑一遍。

### 5.8 限流、防滥用与后台清理

- 限流：按用户维度的令牌桶 —— 写接口 60 次/分钟，LLM 接口 5 次/分钟，超限返回 429（3001）。
- 长轮询挂起上限：每用户最多 5 条、每组最多 200 条挂起连接，超限返回 3002，防止单用户把服务端资源占满。
- 请求约束：请求体 ≤1MB、头部 ≤8KB、解析与读超时 10s、单 IP 连接数上限，抵御 Slowloris 类慢速攻击。
- 后台清理（挂在主 loop 的定时任务，不占业务线程）：过期 sessions 清理、超过 7 天的待接受邀请置为过期（status=5）、孤儿长轮询登记回收。
- 登录失败计数：同一用户名 / IP 连续失败 10 次锁定 5 分钟，配合 PBKDF2 的天然耗时抵御在线爆破。

### 5.9 前端架构（P11）

- 交付形态：一个 `www/index.html` 作为入口，CSS、SVG 和 ES Modules 都是静态资源，由现有 C++ HTTP 服务直接托管；不引入 React/Vue、npm 运行时或打包器，面试演示只需启动一个进程。
- 分层：`core/` 只放跨页面能力（API、DOM 转义、路由、格式化、会话、状态、弹窗），`views/` 按登录、组列表、概览、账本、聊天、设置拆分。视图通过参数拿依赖，不直接保存全局连接。
- 路由：使用 hash 路由，服务端无需为每个页面配置 history fallback。路径固定为 `#/login`、`#/register`、`#/groups` 和 `#/groups/{gid}/{overview|ledger|chat|settings}`；未知标签回落到概览，未知路径显示 404。
- 页面生命周期：路由切换先调用旧视图的 `cleanup()`，再挂载新视图。聊天页在 cleanup 中停止长轮询、关闭 WebSocket、abort 历史和 AI 请求；设置页移除滚动监听和未执行的动画帧，防止旧页面继续修改 DOM。
- 请求层：统一封装 JSON、Bearer token、超时和 `AbortSignal`，把非 2xx 统一变成带错误码的 `ApiError`；401 会清理登录态并跳回登录页。
- 安全渲染：模板函数对插值默认做 HTML 转义，只有 `SafeHtml` 片段可绕过；入口 CSP 仅允许同源脚本/样式、同源 HTTP/WS 连接和本地 data 图片，禁止内联脚本、对象与外部脚本源。
- 凭证：token 存在 `localStorage`，刷新后仍可恢复会话。该选择实现简单、便于本地演示，但会放大 XSS 后果；当前用默认转义与 CSP 收敛风险，生产环境可改为 HttpOnly Cookie + CSRF 防护。
- 实时降级：聊天优先建立同源 WebSocket；握手失败、断线或心跳超时后切到 `GET /messages/poll`。长轮询用重叠窗口（当前最大 id 向前退 20）补偿自增 id 提交乱序，再按消息 id 去重。
- 数值与时间：金额输入只接受最多两位小数并转整数分；均摊允许 0 分，但普通交易金额必须大于 0。服务端不带时区的时间统一按 UTC 解释，展示时再本地化。
- 日期筛选：使用原生日期控件，限制在 `2000-01-01` 到浏览器当天；提交时同时校验格式、真实自然日和范围，避免 `20666` 或 `02-30` 进入查询。
- 写入后展示：创建/更新交易先使用服务端返回结果更新本地列表，再静默刷新；若新流水不匹配当前筛选则清空筛选，刷新失败时仍保留已写入记录。
- 设置页信息架构：分区导航包含组资料、成员与邀请、分类管理、账号与安全、危险操作。成员区同时展示成员列表和“我收到的待处理邀请”；危险操作把退出/解散、转让/移除、注销账号放在明确确认流程后。
- 响应式：桌面为固定侧栏 + 内容区，移动端侧栏改为抽屉；设置导航横向滚动，成员、邀请和危险操作在窄屏改为可换行布局，避免按钮挤压正文。

## 6. API 设计

### 6.1 统一约定

- 基址 /api，请求与响应均为 JSON（UTF-8）；静态资源走 /。
- 响应体固定结构：{"code": 0, "msg": "ok", "data": {...}}，code=0 表示成功；同时使用语义正确的 HTTP 状态码（400/401/403/404/409/429/500）。
- 鉴权：Authorization: Bearer <token>。
- 时间：ISO8601 UTC，如 2026-09-20T08:12:33Z。
- 金额：整数分，字段名以 _cents 结尾。
- 分页：?limit=20&cursor=<上一页最后一条 id>，响应 {"items":[...], "next_cursor": 123 或 null}。
- 幂等：写接口接受**客户端生成**的 client_token（UUID），同 key 重复提交返回首次结果。服务端不代生成 key，未携带即不保证幂等（ADR-16）。token 按创建者全局唯一：同一用户把已用于其他组的 token 再用到新组时返回 1018，而不是跨组复用旧交易。

### 6.2 错误码表

| code | HTTP | 含义 |
|---|---|---|
| 0 | 200 | 成功 |
| 1001 | 400 | 参数缺失或格式错误 |
| 1002 | 401 | 未登录 / token 无效 |
| 1003 | 401 | token 已过期 |
| 1004 | 403 | 无权限（非该组成员 / 非 owner） |
| 1005 | 404 | 资源不存在 |
| 1006 | 409 | 用户名已存在 |
| 1007 | 401 | 用户名或密码错误 |
| 1008 | 409 | 该用户已在组内 / 已有待处理邀请 |
| 1009 | 409 | 邀请状态非法（已处理过） |
| 1010 | 400 | 分摊金额之和必须等于总金额 |
| 1011 | 400 | 不能邀请自己 |
| 1012 | 409 | 组成员数量已达上限或组内仅剩 owner |
| 1013 | 400 | 参与人必须都是组成员 |
| 2001 | 500 | 数据库错误 |
| 2002 | 504 | 上游 LLM 超时或不可用 |
| 1014 | 409 | 分类下仍有交易，无法删除 |
| 1015 | 409 | 存在未结清余额，需先结算或强制退出 |
| 1016 | 409 | 分类名已存在 |
| 1017 | 400 | 目标用户不是组内成员 / 组内至少需保留一名 owner |
| 1018 | 409 | client_token 已在其他组使用 |
| 3001 | 429 | 请求过于频繁 |
| 3002 | 429 | 长轮询挂起连接数超限 |

### 6.3 用户与鉴权

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | /api/auth/register | 注册（username, password, nickname） |
| POST | /api/auth/login | 登录，返回 token 与 expires_at |
| POST | /api/auth/logout | 登出当前会话 |
| POST | /api/auth/logout_all | 登出所有设备 |
| GET | /api/users/me | 获取当前用户信息 |
| PUT | /api/users/me | 修改昵称、头像 |
| GET | /api/users/search?q= | 按用户名搜索用户（用于邀请） |
| PUT | /api/users/me/password | 修改密码（校验旧密码，成功后吊销其它会话） |
| DELETE | /api/users/me | 注销账户（软删 + 吊销全部会话） |

### 6.4 组与成员

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | /api/groups | 创建组（name），创建者为 owner |
| GET | /api/groups | 我的组列表（含成员数、未读数、最近消息） |
| GET | /api/groups/{gid} | 组详情 |
| PUT | /api/groups/{gid} | 改组名（owner） |
| DELETE | /api/groups/{gid} | 解散组（owner） |
| GET | /api/groups/{gid}/members | 成员列表 |
| POST | /api/groups/{gid}/members | 邀请成员（username） |
| DELETE | /api/groups/{gid}/members/{uid} | 移除成员（owner） |
| POST | /api/groups/{gid}/members/leave | 主动退出 |
| GET | /api/invites | 我的待处理邀请 |
| POST | /api/invites/{invite_id}/accept | 接受邀请 |
| POST | /api/invites/{invite_id}/reject | 拒绝邀请 |
| PUT | /api/groups/{gid}/members/{uid}/role | 转让 owner（body: {"role":"owner"}，事务内完成升降级） |

### 6.5 账本

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | /api/groups/{gid}/transactions | 新增交易（type, amount_cents, category_id, note, payer_id, participants[], split_type, client_token） |
| GET | /api/groups/{gid}/transactions | 列表（cursor, limit, category_id, from, to, payer_id） |
| GET | /api/groups/{gid}/transactions/{tid} | 交易详情（含分摊明细） |
| PUT | /api/groups/{gid}/transactions/{tid} | 修改（创建者或 owner） |
| DELETE | /api/groups/{gid}/transactions/{tid} | 软删 |
| GET | /api/groups/{gid}/stats?from=&to= | 总支出、分类占比、按人统计 |
| GET | /api/groups/{gid}/balances | 谁欠谁（净额列表） |
| POST | /api/groups/{gid}/settlements/suggest | 生成最少笔数转账建议（不落库） |
| POST | /api/groups/{gid}/settlements | 确认结算（落库） |
| GET | /api/groups/{gid}/settlements | 结算历史 |
| GET | /api/categories | 分类列表（系统预置 + 该组自定义） |
| POST | /api/groups/{gid}/categories | 新增组内自定义分类 |
| DELETE | /api/groups/{gid}/categories/{cid} | 删除自定义分类（分类下有交易则返回 1014） |

### 6.6 聊天

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | /api/groups/{gid}/messages | 发送消息（content, client_msg_id） |
| GET | /api/groups/{gid}/messages | 历史消息（cursor, limit, after_id） |
| GET | /api/groups/{gid}/messages/poll?after_id=&timeout=30 | 长轮询拉新（P8） |
| POST | /api/groups/{gid}/messages/read | 上报已读水位线 |
| GET | /api/groups/{gid}/messages/unread | 未读数 |
| WS | /api/ws?token=&group_id= | WebSocket 长连接（P9，握手后升级） |

> 长轮询参数语义（ADR-15）：after_id 缺省表示「只拉此刻之后的新消息」（以服务端当前最大 id 为下界，不是从 0 开始全量拉）；非空表示拉取 id > after_id。响应形如 `{"items":[...], "server_max_id": 1234}`。客户端下次以 server_max_id − 20 作为 after_id 回拉并按 id 去重，用于规避自增 id 提交乱序导致的漏消息。

> WebSocket 入口使用 query token（浏览器原生 WebSocket 不能自定义 Authorization 头），但服务端日志只记录 path、不记录 query。连接后事件：客户端发 `{"type":"send","content":"...","client_msg_id":"<uuid>"}` 或 `{"type":"read","last_read_message_id":N}`；服务端发 `ready`、`message`、`ack`、`read`、`error`。token 只在握手时鉴权，客户端应设置重连与 HTTP 补拉策略。

### 6.7 AI（P10）

| 方法 | 路径 | 说明 |
|---|---|---|
| POST | /api/groups/{gid}/ai/ask | 提问，body: {question}，返回 answer |
| GET | /api/groups/{gid}/ai/suggestions | 生成省钱建议（可缓存当日结果） |

响应统一为 `{"code":0,"msg":"ok","data":{...}}`。`ask` 返回 `answer`、`cached`、`elapsed_ms`、`model` 与当前统计区间；`suggestions` 同样返回这些字段，缓存命中时 `cached=true`。两个接口都要求正式组成员身份；未配置 Key 不影响其他业务接口。

## 7. 技术栈

| 层级 | 技术选型 | 备注 |
|---|---|---|
| 语言 | C++17 | 主要用 std::mutex/condition_variable/optional/string_view/filesystem，不引入协程 |
| 构建 | CMake 3.16+ | 三个 target：cofinance（可执行）、cofinance_core（静态库，便于单测）、tests |
| 网络 | epoll（ET 为主，LT 可选）+ 非阻塞 socket | 自研 Reactor |
| 并发 | std::thread 线程池 + eventfd 唤醒 + timerfd 定时器 | 不引第三方网络库 |
| 协议 | HTTP/1.1 自研解析器，后期加 WebSocket | 不用 libmicrohttpd 之类 |
| 数据库 | MySQL 8.0 + MySQL C API | 自研连接池与 DAO |
| JSON | nlohmann/json（单头文件，vendored 到 third_party） | 明确版本与来源 |
| 加密 | OpenSSL（PBKDF2、SHA256、随机数、WebSocket 握手需要的 SHA1/Base64） | |
| HTTP 客户端 | libcurl | 仅 AIService 使用，带 TLS |
| 日志 | 自研异步日志 | 双缓冲或阻塞队列 |
| 测试 | doctest（单头）+ bash/curl 集成脚本 + ASan/UBSan/TSan | |
| 压测 | wrk + 自写长轮询客户端 | 记录实测数据进 README |
| 前端 | 原生 HTML/CSS/JS 单页 | 无框架、无打包步骤；登录/组列表/概览/账本/聊天/设置 |
| 环境 | WSL2 Ubuntu + MySQL 8.0 + g++ 13 | |

## 8. 工程结构

```
CoFinance/
├── CMakeLists.txt
├── README.md                  # 项目介绍、架构图、压测数据、快速开始
├── config/
│   ├── server.conf            # 端口、线程数、连接池大小、超时
│   └── secrets.conf.example   # DB 密码、LLM API Key（真实文件 gitignore）
├── sql/
│   ├── schema.sql             # 建表脚本
│   └── seed_categories.sql    # 预置分类
├── docs/
│   └── design.md              # 架构、需求基线与接口设计
├── include/cofinance/         # 对外头文件
├── src/
│   ├── main.cpp
│   ├── net/                   # EventLoop, Connection, ThreadPool, TimerQueue, Notifier
│   ├── http/                  # HttpParser, Router, Response, StaticFile, AuthMiddleware
│   ├── ws/                    # WebSocket 握手与帧编解码（P9）
│   ├── db/                    # ConnectionPool, TransactionGuard, 各 DAO
│   ├── service/               # UserService, GroupService, TransactionService, ...
│   ├── api/                   # Controller 与路由注册
│   ├── ai/                    # LlmClient, PromptBuilder
│   └── util/                  # Config, Logger, Json, Crypto, TimeUtil
├── tests/
│   ├── frontend/              # DOM 无关的纯函数测试
│   ├── unit/                  # P0-P10 单元测试
│   └── integration/           # 真实服务 Python/HTTP 验收
├── www/
│   ├── index.html             # 唯一入口与 CSP
│   └── assets/
│       ├── css/app.css        # 视觉系统与响应式
│       ├── avatars/           # 预置 SVG
│       └── js/
│           ├── app.js         # 路由与页面生命周期
│           ├── core/          # API、路由、DOM、格式化、状态
│           └── views/         # 各业务页面
└── third_party/               # nlohmann/json、doctest（vendored，注明版本与许可）
```

## 9. 开发顺序与验收标准

| 阶段 | 目标 | 验收标准 | 讲解重点 |
|---|---|---|---|
| P0 | 工程骨架、配置、日志 | CMake 编译通过；二进制能启动并读配置；日志异步双写文件与终端；Ctrl+C 优雅退出无泄漏 | 异步日志的队列设计与丢日志策略；CMake 现代写法 |
| P1 | epoll 事件循环 + 线程池 | 1000 条并发空闲连接稳定不掉线；echo 回显压测无丢失；无 busy-loop 占满 CPU | ET vs LT、惊群、EAGAIN 处理、Reactor vs Proactor |
| P2 | HTTP 解析 + 路由 + 静态文件 | curl 打通 /api/ping 与静态页；keep-alive 复用连接；构造半包/粘包/超大 body 用例全部正确 | 状态机解析、粘包半包、Content-Length、慢速攻击防护 |
| P3 | MySQL 连接池 + DAO | 单元测试通过；100 线程并发借还连接无死锁无泄漏；断线自动重连 | 池化、RAII、条件变量、MySQL 空闲断连 |
| P4 | 用户系统与鉴权 | 注册/登录/登出/me 全通；无 token 401、过期 401、伪造 token 401 | PBKDF2 vs bcrypt、token 会话 vs JWT、时序攻击 |
| P5 | 组与成员 | 邀请→接受→成员列表全通；非成员访问组接口 403（含恶意 id 越权测试） | 状态机、IDOR 越权、中间件设计 |
| P6 | 记账 | 新增交易含分摊校验；均分除不尽时余数分配正确；分页与筛选正确 | 事务边界、分摊精度、游标分页 vs OFFSET |
| P7 | 统计与结算 | 净额计算正确（含收入项）；结算建议笔数最少；结算后净额归零 | 聚合 SQL、子集 DP、一致性与对账 |
| P8 | 聊天 + 长轮询 | 2000 条并发挂起连接不占线程池线程；消息 100ms 内送达；超时、对端断开、送达三条路径都无泄漏（ASan 干净） | 本文 5.2 全节，本项目最大难点 |
| P9 | WebSocket | 浏览器原生 WebSocket 收发成功；分片帧与 ping/pong 处理正确；断线重连补拉不丢消息 | 握手算法、帧格式、掩码、心跳、粘包 |
| P10 | AI 助手 | 提问返回自然语言回答；LLM 超时返回 504 且不影响其他请求；Key 不进仓库 | 外部依赖隔离、prompt 设计、超时与降级 |
| P11 | 前端 + 压测 + 文档 | 完整演示流程可跑；wrk 与长轮询压测数据写进 README；文档齐全 | 性能瓶颈定位（perf/strace）、项目叙事 |

约定：每个阶段结束都必须「能编译 + 能用 curl 演示 + 通过该阶段验收」才进入下一阶段；不允许并行开多个未完成阶段。

评审补充的接口已并入既有阶段，不单独开阶段：修改密码 / 注销账户 → P4；转让 owner、退出前余额校验 → P5；分类管理 → P6；净额与结算联动 → P7。

## 10. 学习方法与交付物约定（面向秋招）

代码由我实现，但每个模块必须同时产出以下五件东西，缺一不算完成：

1. 原理笔记 docs/learning/PN-xx-*.md：这个模块底层是怎么工作的（例如 epoll 的 ET 为什么必须读到 EAGAIN、MySQL 事务隔离级别与间隙锁、TCP 粘包的成因）。
2. 设计取舍：至少两个替代方案 + 为什么没选（例如为什么不用 Redis 做会话、为什么不用异步 DB 驱动）。
3. 代码走读：关键函数逐行讲解，重点是状态机、并发与错误处理路径。
4. 踩坑记录 docs/BUGLOG.md：统一格式「现象 → 复现 → 定位手段（gdb/strace/valgrind/ASan/perf/tcpdump）→ 根因 → 修复 → 教训」。我会如实记录开发过程中真实出现的问题，不编造。
5. 面试问答 docs/interview.md：该模块最可能被追问的 3–5 个问题及回答思路。

另外：每阶段的验收用一个可复现的脚本或命令记录在 README 中，方便你面试前重跑一遍演示。

## 11. 质量与性能目标

- 编译：-Wall -Wextra -Werror；Debug 带 ASan/UBSan；Release 带 -O2。
- 代码：clang-format 统一风格；关键并发与协议代码必须有注释说明「为什么」，而不是「做了什么」。
- 测试：Service 层单测覆盖核心分支；集成脚本跑通全流程；每次提交前跑一遍 ASan。
- 目标（以本机 WSL 实测为准，只记录真实数据）：
  - 普通读接口 p99 < 50ms；写接口 p99 < 100ms。
  - 长轮询：≥2000 条并发挂起连接，整机 CPU 占用不随连接数线性上涨（证明没有线程被阻塞）。
  - 内存：长轮询与 WebSocket 场景下，连接建立-释放 1 万个循环后 RSS 稳定不增长。
  - 压测：wrk 出 QPS/p99 曲线，长轮询出「连接数-延迟」曲线，都要写进 README。
- 安全：参数化 SQL、密码哈希、token 存哈希、越权校验、请求体大小限制、限流。

## 12. 风险清单

| 风险 | 影响 | 对策 |
|---|---|---|
| 长轮询与连接生命周期管理出错 | 悬垂指针、崩溃、内存泄漏 | 连接一律 shared_ptr + weak_ptr 回调；定时器注销；ASan/TSan 全量跑 |
| 线程池被长耗时操作占满 | 服务整体卡死（长轮询、LLM 调用） | 长轮询走事件驱动不占线程；LLM 用独立线程池；DB 由连接池限流 |
| 连接池耗尽或长事务 | 请求排队、雪崩 | 连接池上限 + 借出超时 + 事务内不做网络 IO |
| 跨组越权（IDOR） | 数据泄漏，面试硬伤 | 统一成员校验中间件 + 越权测试用例 |
| 金额精度与分摊误差 | 结算结果错误 | 分存储 + Σshare 强校验 + 余数分配策略 |
| 重复提交 / 客户端重试 | 重复记账 | 幂等键 + 唯一索引 + 冲突回查 |
| 时区导致统计错月 | 结果不可信 | 全 UTC 存储 + 前端本地化 |
| 外部 LLM 不稳定 | 接口超时、线程耗尽 | 独立线程池 + 超时 + 降级 + 限流 |
| 单机 MySQL 并发写瓶颈 | 压测数据难看 | 分清读/写瓶颈，必要时加缓存；讲清楚瓶颈在哪比刷高 QPS 更重要 |
| 范围膨胀 | 做不完 | 严格按 Non-goals 砍需求；每阶段只做一件事 |
| 自增 id 提交乱序导致漏消息 | 长轮询静默丢消息，平时极难复现 | 重叠窗口 + 客户端去重（ADR-15）；压测时做「发 N 收 N」完整性校验 |
| 未读数每次 COUNT(*) | 组列表接口随消息量变慢 | 组列表用一条聚合 SQL 带出各未读数，必要时加缓存 |
| 成员退出后余额悬空 | 账目永远无法收敛 | 退出 / 解散前置校验（ADR-17） |
| 定长标识列字符集写错 | 索引膨胀，性能悄悄下降 | 建表 checklist + 表结构 review（ADR-18） |

## 附录 A：与 TinyWebServer 的关系（不复用代码，但可作为叙事对比）

本项目不复用 TinyWebServer 的任何代码，从零实现；但在面试中可以把两者做对比，这恰好能体现你对每个机制的理解深度：

| 维度 | TinyWebServer | CoFinance |
|---|---|---|
| 定位 | 静态文件 + 简单 CGI 注册登录 | 多用户业务系统，REST API + 实时推送 |
| 并发模型 | 线程池 + epoll（Reactor/Proactor） | 主从 Reactor，连接亲和到固定 loop |
| HTTP | 状态机解析 GET/POST，无 REST | 状态机解析，支持 PUT/DELETE、JSON、路由参数、中间件 |
| 数据库 | 连接池 + CGI 里直接拼 SQL | 连接池 + DAO 分层 + 事务 RAII |
| 实时 | 无 | 长轮询（事件驱动挂起）+ WebSocket |
| 对外依赖 | 纯本地 | 调用外部 LLM，需要超时与降级设计 |
| 难点 | 高并发连接处理 | 长轮询的跨线程唤醒与连接生命周期管理 |

面试时可主动说明的差异点：为什么 CoFinance 的长轮询不能用「线程 sleep」实现；为什么业务分层比 CGI 直连数据库更好维护；为什么金额不能用 double。

## 附录 B：外部评审意见处置表（9 条）

| # | 评审意见 | 处置 | 理由 |
|---|---|---|---|
| P0-1 | group_members 缺 last_read_message_id 的索引 | **不成立** | 未读数查询是 messages WHERE group_id=? AND id>?，由 idx_group_id(group_id,id) 支撑；group_members 侧只按 (group_id,user_id) 定位记录，给 last_read_message_id 单独建索引不会有任何查询用到（评审自己也得出「不用加」的结论）。 |
| P0-2 | token_hash 应使用 ascii 字符集 | **采纳并推广** | 采纳；并推广到 client_token / client_msg_id / currency（ADR-18）。 |
| P0-3 | client_token 为空时由服务端生成 UUID | **驳回** | 幂等键必须由客户端生成并在重试时复用。服务端代生成会让重试拿到一个全新 key，去重彻底失效，反而制造虚假的安全感（ADR-16）。 |
| P1-4 | 增加注销账户接口 | **采纳** | 成本低，且能顺带定义「注销用户的历史账目如何展示」，是真实的业务问题。 |
| P1-5 | 增加转让 owner 接口 | **采纳** | v2 写了「owner 退出前必须转让或解散」却没定义转让接口，属于自相矛盾的缺口。 |
| P1-6 | 增加分类管理接口 | **采纳** | categories 表有 group_id 却没有配套 API，设计不闭环；同时必须定义「分类下有交易不可删」。 |
| P1-7 | 明确 after_id 的语义 | **采纳一半并修正** | 语义按评审建议确定（缺省 = 以当前最大 id 为下界）。但仅此不够：自增 id 提交乱序仍会漏消息，故追加 ADR-15 的重叠窗口方案。 |
| P2-8 | 增加消息撤回 | **驳回（列入二期）** | 属于 CRUD 面扩张，对并发 / 协议 / 事务三大难点没有增量；性价比低于把 P8、P9 做扎实。 |
| P2-9 | 增加账本导出 CSV | **驳回（列入二期）** | 同上；且会引入文件生成与下载响应分支，收益主要是「演示好看」。 |

### 技术负责人在本轮额外发现（评审未提及）

| # | 问题 | 处置 |
|---|---|---|
| E-1 | 自增 id 提交乱序 → 长轮询静默漏消息 | ADR-15：重叠窗口 + 客户端去重 |
| E-2 | 幂等键存在被误用的风险（服务端代生成） | ADR-16：明确拒绝 |
| E-3 | 成员退出后余额悬空，账目无法收敛 | ADR-17：退出 / 解散前置校验 |
| E-4 | 定长标识列的字符集导致索引膨胀 | ADR-18 |
| E-5 | 缺「修改密码」接口 | 并入 6.3 与 P4 |
| E-6 | 限流只在错误码里出现，没有模块设计 | 新增 5.8 节 |
| E-7 | 过期会话、过期邀请无清理机制 | 新增 5.8 节后台清理 |
| E-8 | 未读数用 COUNT(*) 会让组列表变慢 | 组列表改用聚合 SQL 一次带出，并登记进风险清单 |
