# Session 注册协议（内部接口）

本模块定义供特权 PAM/Turnstile 入口使用的本地注册消息。它是核心 Session API 外围的协议边界，不是 login1 D-Bus API，也不是 PAM 模块本身。

## 安全边界

- 传输层必须使用 Unix SOCK_SEQPACKET，以保留消息边界；不能把该结构直接用于 TCP 或字节流 Socket。
- Socket 适配器必须通过 SO_PEERCRED 获取对端凭据，并将真实 peer UID 传给 dl_session_registration_apply()。
- 生产配置的 allowed_peer_uid 必须为 0。只有 root/PAM/Turnstile 特权入口可以创建或结束 Session。
- 消息中的 UID、leader PID、Seat 和会话属性只在 peer 已通过认证后才可信。普通桌面程序不得直接获得注册权限。
- Session ID 仅允许 ASCII 字母、数字、下划线和连字符；所有字段必须在固定数组范围内 NUL 终止，且不能包含控制字符。
- OPEN 和 CLOSE 使用同一固定长度消息结构；未知版本、操作码、标志位、非零保留字段和长度不匹配都会被拒绝。
- CLOSE 消息不能携带额外身份字段，避免客户端用关闭操作覆盖或混淆会话属性。

## 消息

协议 magic 为 DLSG（数值 0x444c5347），当前版本为 1。

- OPEN：注册 Session，UID、leader PID 和会话元数据由可信入口填写。Seat 可为空，适用于 SSH、无头和后台 Session。
- CLOSE：按 Session ID 结束会话。只有通过 peer UID 检查的特权入口可以执行。

响应包含协议 magic、版本和 dl_error 状态码。

## 生命周期要求

PAM/Turnstile 集成应在会话建立成功后发送 OPEN，并在对应 close-session 路径发送 CLOSE。即使客户端异常退出，也必须由后续的可信生命周期清理逻辑处理遗留 Session。leader PID 当前只在核心内保证唯一；此协议版本尚未携带 /proc starttime，因此不能把 PID 当作跨生命周期的永久身份。

## 当前实现边界

当前提交实现了消息结构、严格校验、peer UID 授权检查和核心 Manager 的 OPEN/CLOSE 调用，并有单元测试。实际 Unix Socket 监听器、SO_PEERCRED 采集、PAM 模块和 Turnstile hook 尚未接入；在这些组件完成前，不应把该协议描述为已可供系统登录流程直接使用。
