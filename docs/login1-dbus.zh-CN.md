# login1 D-Bus 服务端

dinit-logind 使用 `libsystemd` 的 `sd-bus` API 连接系统 D-Bus；不启动 systemd，
不要求 systemd 作为 PID 1，也不依赖 systemd-logind。

## 已导出接口

Manager：
- `ListSessions()`、`GetSession()`、`GetSessionByPID()`
- `ListSeats()`、`GetSeat()`
- `TerminateSession()`、`KillSession()`（当前明确返回 NotSupported）

Session：
- 基础属性：Id、User、Name、Seat、TTY、Display、Remote、Service、Desktop、
  Leader、Type、Class、Active
- `Activate()`：更新核心模型中的 Seat 活动 Session，并发送 Active 属性变化通知。
  当前尚未连接 VT ioctl，因此不等同于完整物理 VT 切换。
- `Lock()`、`Unlock()`：向对应 Session 对象发出 Lock/Unlock 信号。
- `TakeControl(force)`、`ReleaseControl())：以 D-Bus unique name 绑定控制权；
  校验调用者 UID，强制接管仅允许 root。
- `TakeDevice(major,minor)`：由核心设备管理层打开并验证字符设备，返回 D-Bus Unix FD
  和 inactive 标志。
- `ReleaseDevice()`、`PauseDeviceComplete())：要求当前 D-Bus 控制者身份匹配。

Seat：Id、ActiveSession、CanTTY、CanGraphical。
User：新增最小对象，导出 UID、Name。

对象路径中的非字母数字字符采用十六进制转义。Session 的 User 属性现在指向已导出的
User 对象路径。

## 尚未完成的语义

- **Terminate / Kill**：目前注册方法但返回 `org.freedesktop.DBus.Error.NotSupported`。
  核心 Session 只记录 Leader PID，没有完整进程成员表或可信 cgroup 归属；
  在没有可靠成员关系和 PID 生命周期保护前，不能安全地向不相关进程发送信号。
- **Activate**：目前切换逻辑活动 Session，不执行 VT_ACTIVATE / VT_WAITACTIVE。
- **设备暂停通知**：核心具备 pause state，但尚未将 seatd/libseat 的暂停/恢复事件完整映射为
  login1 的 PauseDevice / ResumeDevice 信号。
- **D-Bus 运行时测试**：本提交尚未增加私有 D-Bus 总线端到端测试；当前 CI 的构建和单元测试
  不证明真实桌面客户端兼容。
- `GetSessionByPID` 仍只匹配已登记 Leader PID；PID reuse / pidfd 保护及全 Session
  进程映射仍待实现。

默认启动需要系统总线运行，且不能有其他服务占用 `org.freedesktop.login1`。
`--no-login1` 可用于不启动 D-Bus 的核心/设备适配器调试。
