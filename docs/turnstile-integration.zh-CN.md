# Turnstile 集成设计与当前接口边界

## 结论

dinit-logind 应将 Turnstile 作为用户登录生命周期和用户服务管理的上游，而不是复制一套 Turnstile。Seat/设备管理仍由 dinit-logind 的核心模型与 seatd/libseat 适配层负责；Turnstile 本身明确不负责 Seat 管理。

**当前不能直接实现完整的 libturnstile 同步适配器。** 核对 Chimera Linux Turnstile 上游接口后发现：

- include/turnstile.h 定义了 login/session 的 NEW、REMOVED、CHANGED 事件，以及 turnstile_watch_events() 回调。
- 回调参数只有 Turnstile handle、事件类型、unsigned long id 和用户数据。
- 公开头文件没有按 ID 查询 Session 属性的 API；因此无法可靠获得 UID、leader PID、Seat、type、class、service、TTY、display、remote 等字段。
- 当前 src/lib_backend_turnstile.c 的 dispatch() 与 watch_events() 仍是空实现，不能把 API 声明当作可用的事件流。
- Turnstile 的 PAM 模块通过持久 Unix socket 连接表示登录生命周期；其 daemon 在最后一个连接消失时结束对应登录生命周期。这不是可直接复用的 dinit-logind Session ID/属性接口。

本项目不得通过猜测 ID、读取非公开内部结构、假设事件回调已经实现，来伪造兼容性。

## 职责划分

| 组件 | 权威职责 |
|---|---|
| Turnstile | 用户登录/注销生命周期、用户服务管理、XDG_RUNTIME_DIR |
| dinit-logind | login1 兼容层、Session/Seat/Device 状态、会话查询 |
| seatd/libseat | 图形设备访问与 FD 生命周期 |
| eudevd | 内核设备发现和事件分发 |
| Dinit | 系统服务监督与 cgroup 管理 |

Turnstile 不应成为第二个 SeatManager；dinit-logind 也不应接管 Turnstile 的用户服务管理。

## 可信 Session 数据契约

未来适配器只有在上游提供正式、可查询的 Session API 后才能启用。每个 Session 至少需要：

- 稳定的 session ID，以及可区分 login ID 的关联标识；
- UID；
- leader PID 与进程身份校验信息（Linux 上优先 pidfd，或 PID 加 /proc/PID/stat starttime）；
- 可选 Seat ID；无 Seat 的 SSH、后台、无头会话必须允许；
- type、class、service、desktop、TTY、display；
- remote 标志；
- NEW、CHANGED、REMOVED 生命周期事件。

事件 ID 只用来查询对象，不可直接当成 UID、PID 或 session ID。属性缺失时必须保留 unknown/null，不得擅自补成 seat0 或 local。

## 建议的上游 API 形态

建议向 Turnstile 上游提议稳定的只读属性查询接口，例如：

- turnstile_get_session(ts, id, &properties)
- turnstile_session_properties_clear(&properties)
- 属性结构提供稳定字符串生命周期规则，并包含 UID、leader PID、Seat 和 PAM 会话元数据；
- turnstile_watch_events() 提供可用的 NEW/CHANGED/REMOVED 通知；
- turnstile_dispatch() 真正从 daemon 连接读取和分发事件；
- 连接断开、daemon 重启和对象删除要有明确语义。

名称仅为 API 设计建议，不代表上游已实现或承诺这些符号。

## PAM 部署策略

不要同时让 Turnstile PAM 模块和 dinit-logind PAM 模块各自创建一条独立权威 Session。否则可能出现重复会话、生命周期顺序不一致，以及无法关联同一个登录的问题。

推荐顺序：

1. 在没有 Turnstile 属性 API 的系统上，暂时使用本项目的 PAM 模块独立注册 Session；它不声称自己是 Turnstile Session。
2. 将来启用正式 Turnstile bridge 后，由 bridge 订阅 Turnstile 生命周期并同步到核心 Manager；此时禁用重复的 dinit-logind PAM 注册路径。
3. 如果上游长期不提供属性查询 API，再评估给 Turnstile 提交正式扩展，或通过上游认可的扩展接口传递数据；不要依赖其私有 socket 协议。

## 安全约束

- 不接受普通用户自行声明任意 UID、Seat 或 remote=false。
- 不能仅凭 Unix socket 连接者 UID 就相信其声明的 leader PID；要验证 PID 所属 UID、进程存活和进程身份稳定性。
- 不把 PAM 回调进程 PID 自动当作用户桌面 Session leader。
- 不把 X11/VNC/SSH/TTY 的 local/remote 属性简单混为一谈。
- 断线清理必须基于明确的所有权/生命周期令牌，不能因某个客户端断开就删除其他登录仍在使用的用户级状态。

## 当前实现状态

本文件是接口边界和实现计划，不代表 Turnstile adapter 已实现。当前代码已有独立 PAM 注册路径与核心 Session 查询模型；完整 Turnstile 事件同步、login1 D-Bus、libseat FD 交接仍未完成。
