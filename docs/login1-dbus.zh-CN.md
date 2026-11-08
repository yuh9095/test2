# login1 D-Bus 服务端

dinit-logind 使用 `libsystemd` 的 `sd-bus` API 连接系统 D-Bus；不启动 systemd，
不要求 systemd 作为 PID 1，也不依赖 systemd-logind。

## 已导出接口

Manager：
- `ListSessions()`、`GetSession()`、`GetSessionByPID()`
- `ListSeats()`、`GetSeat()`
- `GetUser()`、`GetUserByPID()`、`ListUsers()`；User 对象提供 `UID`、`Name`、`Sessions`。
- `KillSession(id, who, signal)`：支持 `leader` 的 pidfd 安全信号发送；
  `all` 仅支持 SIGKILL，并要求登记 cgroup 路径可安全打开且 Leader 当前属于该 cgroup。
- `TerminateSession()`：通过 Leader pidfd 发送 SIGTERM，D-Bus 调用立即返回；
  守护进程在 5 秒宽限期后升级为 SIGKILL。若 Session 登记了 cgroup v2 路径，
  会先固定并验证 cgroup 目录及 Leader 成员关系，超时后优先写入该目录的
  `cgroup.kill`；没有可用 cgroup 时仅强制结束已登记的 Leader。

Session：
- 属性：Id、User、Name、Seat、TTY、Display、Remote、Service、Desktop、
  Leader、CGroup、VTNr、Type、Class、Active。
- `Activate()`：更新 Seat 活动 Session；这是逻辑激活，不保证物理 VT 已切换。
- `Lock()`、`Unlock()`：向对应 Session 对象发出 Lock/Unlock 信号。
- `Kill(who, signal)`：`leader` 通过 pidfd 发送指定有效信号；`all` 只允许
  SIGKILL，并使用 cgroup v2 的 `cgroup.kill`。
- `Terminate()`：与 Manager.TerminateSession 使用相同的异步终止状态机。
- `TakeControl(force)`、`ReleaseControl()`：以 D-Bus unique name 绑定控制权；
  校验调用者 UID，强制接管仅允许 root。
- `TakeDevice(major,minor)`：由核心设备管理层打开并验证字符设备，返回 D-Bus Unix FD
  和 inactive 标志。
- `ReleaseDevice()`、`PauseDeviceComplete()`：要求当前 D-Bus 控制者身份匹配。控制者 D-Bus unique name 消失时，NameOwnerChanged 监听器会自动释放该连接持有的所有设备 claim 与管理 FD。

Seat：Id、ActiveSession、CanTTY、CanGraphical、Sessions；`ActivateSession()` 更新逻辑
活动 Session；`SwitchTo(vtnr)`、`SwitchToNext()`、`SwitchToPrevious()` 通过 Linux VT ioctl 执行 seat0 的真实 VT 切换，
容器没有可用 VT 时返回 NotSupported。Session 增加 `VTNr` 属性。
User：导出 UID、Name、Sessions。

对象路径中的非字母数字字符采用十六进制转义。Session 的 User 属性指向已导出的
User 对象路径。

## 安全边界与尚未完成的语义

- **Kill leader**：依赖注册时捕获的 pidfd；不支持 pidfd 或 Leader 已退出时明确失败，
  不回退到裸 PID。D-Bus 调用只允许 root 或 Session 所有者。
- **Kill all**：目前只实现 cgroup v2 的 SIGKILL 路径。cgroup 路径先通过逐组件
  `openat(O_NOFOLLOW)` 安全遍历，再检查 `cgroup.procs` 中存在 Leader PID，
  最后写入 `cgroup.kill`。路径元数据本身不被当作成员身份凭据。
- **Terminate**：已实现异步 SIGTERM、5 秒宽限期和超时 SIGKILL。cgroup v2
  Session 会先固定并验证 cgroup 目录，超时后使用该目录的 `cgroup.kill`；
  无 cgroup 的会话仅能保证强制结束已登记的 Leader，不能声称清理其所有后代进程。
- **设备暂停通知**：核心有 pause state 与超时原语；D-Bus 控制器断连清理已实现，
  会释放该连接拥有的所有 claim 和管理 FD。但 seatd/libseat 的 pause/resume 回调
  尚未映射为 login1 的 PauseDevice/ResumeDevice 信号，客户端 FD 的恢复通知也未完成。
- **VT**：支持 seat0 的 `SwitchTo(vtnr)`、`SwitchToNext()` 和 `SwitchToPrevious()`；
  后两者依据已登记且具有 VTNr 的 Session 选择下一个/上一个 VT，并循环查找。
  容器没有可用 VT 时返回 NotSupported。Seat.Terminate 仍未实现。
- **Session 查询**：`GetSessionByPID` / `GetUserByPID` 当前只按已登记 Leader PID
  匹配，不是整个 Session 的进程索引。
- **Turnstile/PAM**：已有特权注册协议和 PAM 生命周期基础；完整 Turnstile 会话属性、
  XDG_RUNTIME_DIR 生命周期、用户服务管理仍需集成与端到端验证。

## 私有 D-Bus 集成测试

构建系统在检测到 `dbus-run-session` 时注册
`login1-private-dbus-integration`。测试使用临时会话总线，不占用宿主机系统总线，
也不要求 systemd 或 systemd-logind。测试覆盖 Manager/Session 查询、用户查询、
Seat/Session 激活、Lock/Unlock、TakeControl/ReleaseControl，以及真实 pidfd Leader
信号发送与非法 Kill 参数。物理 VT、真实图形设备 FD、pause/resume 和完整桌面客户端
行为仍需在具有相应硬件与 seatd/libseat 环境的系统中验证。

默认生产入口连接系统总线并取得 `org.freedesktop.login1` 名称；
`--no-login1` 可用于不启动 D-Bus 的核心/设备适配器调试。
