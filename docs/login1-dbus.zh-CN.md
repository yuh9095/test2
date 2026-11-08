# login1 D-Bus 服务端（初始接口）

dinit-logind 使用 `libsystemd` 提供的 `sd-bus` API 连接系统 D-Bus 并取得
`org.freedesktop.login1` 名称。这只是 D-Bus 通信库依赖，不启动 systemd，
不要求 systemd 作为 PID 1，也不依赖 systemd-logind。

## 当前导出接口

Manager 对象 `/org/freedesktop/login1`（`org.freedesktop.login1.Manager`）：
- `ListSessions() -> a(susso)`
- `GetSession(s) -> o`
- `GetSessionByPID(u) -> o`
- `ListSeats() -> a(so)`
- `GetSeat(s) -> o`

Session 对象位于 `/org/freedesktop/login1/session/<id>`，导出 Id、User、
Name、Seat、TTY、Display、Remote、Service、Desktop、Leader、Type、Class、
Active 属性。Seat 对象位于 `/org/freedesktop/login1/seat/<id>`，导出 Id、
ActiveSession、CanTTY、CanGraphical 属性。

对象路径中的非字母数字字符采用十六进制转义，避免将外部 ID 直接拼入
D-Bus object path。

## 语义限制

核心模型目前只记录 Session leader PID。因此 `GetSessionByPID` 只匹配
已登记的 leader PID，不代表覆盖 Session 内所有进程。进程归属表、PID
生命周期和 PID 重用防护完成前，不应宣称其具备完整 login1 语义。

未实现的属性不会伪造值；Activate、Terminate、Kill、Lock、Unlock、
TakeControl、ReleaseControl 和设备 FD 交接等操作尚未注册。User 属性
返回 UID 与 user object path，本阶段尚未导出 User 对象。

默认启动需要系统总线运行，且不能有其他服务占用 login1 名称。
`--no-login1` 可用于不启动 D-Bus 的核心/设备适配器调试。
