# Session 注册协议（内部接口）

本模块定义供特权 PAM/Turnstile 入口使用的本地注册消息。它是核心 Session API 外围的协议边界，不是 login1 D-Bus API，也不等同于 PAM 模块。

## 安全边界

- 传输层使用 Unix `SOCK_SEQPACKET`，保留消息边界；不能把该结构用于 TCP 或字节流 Socket。
- Socket 适配器通过 `SO_PEERCRED` 获取内核提供的对端凭据，并将真实 peer UID 传给 `dl_session_registration_apply()`。
- 生产守护进程目前将允许的 peer UID 固定为 0。只有 root 对端可以创建或结束 Session。
- Socket 节点权限为 `0600`。父目录必须由系统服务预先创建并保护；不能让不可信用户替换 Socket 节点。
- 消息中的 UID、leader PID、Seat 和会话属性只在 peer 已通过认证后才可信。普通桌面程序不得直接获得注册权限。
- Session ID 仅允许 ASCII 字母、数字、下划线和连字符；OPEN 字段必须在固定数组范围内 NUL 终止，且不能包含控制字符。
- OPEN 和 CLOSE 使用同一固定长度消息结构；未知版本、操作码、标志位、非零保留字段和长度不匹配都会被拒绝。
- CLOSE 不接受额外身份字段，避免关闭请求携带混淆属性。

## 消息

协议 magic 为 DLSG（数值 `0x444c5347`），当前版本为 1。

- OPEN：注册 Session，UID、leader PID 和会话元数据由已授权入口填写。Seat 可为空，适用于 SSH、无头和后台 Session。
- CLOSE：按 Session ID 结束会话。只有通过 peer UID 检查的特权入口可以执行。

响应包含协议 magic、版本和 `dl_error` 状态码。

## 守护进程启用方式

通过 `--session-socket /run/dinit-logind/session.sock` 启用。守护进程使用非阻塞监听 Socket，并通过 `SO_PEERCRED` 检查每个连接的 UID。父目录应由 Dinit 服务配置或安装步骤创建，建议由 root 所有且不可被普通用户写入。

## 生命周期要求与当前限制

PAM/Turnstile 集成应在会话建立成功后发送 OPEN，并在对应 close-session 路径发送 CLOSE。即使客户端异常退出，也必须由后续可信生命周期清理逻辑处理遗留 Session。

leader PID 当前只在核心内保证唯一；协议尚未携带 `/proc/<pid>/stat` starttime，因此不能把 PID 当作跨生命周期的永久身份。核心的按 PID 查询也只匹配 leader PID，不等于完整的进程组/会话成员查询。

当前实现包含消息结构、严格校验、peer UID 授权、Unix Socket 监听器、`SO_PEERCRED` 采集、守护进程事件循环接入和 Socket 集成测试。**PAM 模块与 Turnstile hook 尚未实现**；当前 Socket 只是它们未来使用的特权入口，不应宣称已经接入系统登录流程。
