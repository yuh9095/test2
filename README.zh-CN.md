# dinit-logind

登录、Session、Seat 与设备管理服务，面向使用 Dinit 的 Linux 系统。

> 项目处于早期开发/功能原型阶段。已有 C11 Session/Seat 核心、login1 D-Bus 服务、特权 PAM Session 注册、pidfd/cgroup 进程控制、Linux VT 操作、D-Bus 设备 FD 传递，以及 libseat logind 后端所需的设备暂停/恢复通知。完整 elogind API、Turnstile 生命周期同步、inhibitor、电源管理和真实桌面环境验证仍未完成。

## 构建

依赖 C11 编译器、Meson、Ninja 和 libudev 开发文件。构建 PAM 模块还需要 PAM 开发文件。

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

## PAM

安装 PAM 开发文件后会构建 `pam_dinit_logind.so`。在 PAM service 的 session 阶段配置：

```
session required pam_dinit_logind.so socket=/run/dinit-logind/session.sock
```

模块在 open_session 注册 Session，在 close_session 注销相同 Session ID；不执行认证，不替代其他 PAM 模块。Seat 不从 TTY 猜测。限制与调用流程见 `docs/pam-session-integration.zh-CN.md`。

## login1 D-Bus 服务端

当前提供 login1 Manager、Session、Seat、User 的查询和部分操作。Session 支持 SetType、TakeControl、TakeDevice、ReleaseDevice、PauseDeviceComplete；设备 FD 通过 D-Bus Unix FD 传递。Seat 活动 Session 切换时发出 PauseDevice("pause")，客户端确认后释放管理 FD；超时发送 "force"，设备被 eudev 移除时发送 "gone"。重新激活 Session 后，客户端可再次调用 TakeDevice 获取新 FD。这里的 force 是协议通知和服务端 FD 清理，不是内核对客户端已复制 FD 的通用撤销。支持 pidfd 安全的 `Kill(leader, signal)`、经过 cgroup v2 成员核验的 `Kill(all, SIGKILL)`，以及异步 `Terminate()` / `TerminateSession()`（SIGTERM、5 秒宽限期、超时 SIGKILL；无 cgroup 时仅能保证终止登记的 Leader）。root-only `Seat.Terminate()` 也已实现：只对该 Seat 的 Session 安排相同的异步终止流程；多个 Session 的安排不是事务，若中途失败，已安排的终止不会回滚。接口范围见 [login1 D-Bus 文档](docs/login1-dbus.zh-CN.md)。构建需要 `libsystemd` 的 `sd-bus` 开发库，但不依赖 systemd PID 1 或 systemd-logind。

## Session 注册与 cgroup 元数据

特权 Unix `SOCK_SEQPACKET` 注册协议已升级到版本 2，可选携带相对于统一 cgroup 挂载点的 Session cgroup 路径。服务端拒绝绝对路径、空路径组件、`.`、`..` 和控制字符，并在核心模型中保存该元数据；login1 Session 的 `CGroup` 属性会以标准绝对路径形式公开它。

协议仍接受旧版 v1 消息（包括旧长度的 socket 数据包），未提供 cgroup 信息的旧客户端可以继续注册 Session。核心还会在注册时尝试捕获 Leader 的 pidfd，保存稳定的进程身份引用；内核不支持 pidfd 或进程已退出时，安全复制接口会明确失败，不回退到裸 PID。cgroup 路径本身仍只是登记元数据，不自动证明成员身份。执行 `Kill(all, SIGKILL)` 时，服务端会通过 cgroup 文件描述符逐组件安全遍历，核对 `cgroup.procs` 中存在已由 pidfd 固定身份的 Leader，再写入 `cgroup.kill`；`Kill(leader, signal)` 则直接使用 pidfd，绝不回退到裸 PID。异步 `Terminate()` / `TerminateSession()` 会先通过 pidfd 向 Leader 发送 SIGTERM，5 秒后若仍未退出则升级为 SIGKILL；若登记了 cgroup v2 路径，会在请求时安全固定并验证该 cgroup，超时后使用 `cgroup.kill` 清理整个 cgroup。没有 cgroup 时只保证终止 Leader。

## 设备与 libseat 集成

login1 服务端已实现 vendored libseat logind 后端使用的 D-Bus 设备控制子集，包括 SetType、TakeControl、TakeDevice、ReleaseDevice、PauseDevice、PauseDeviceComplete，以及 Active 属性变化通知。完整 libseat logind backend 仍依赖 sd-login 提供的 Session/Seat 发现元数据；本项目尚未实现 systemd 兼容的 /run/systemd/sessions 数据文件或 libsystemd 兼容层，因此不能宣称所有 libseat 自动发现路径已经打通。

## Turnstile 集成

Turnstile 的接口边界、可信 Session 数据契约与集成路线见 [Turnstile 集成设计](docs/turnstile-integration.zh-CN.md)。在上游属性查询和事件分发 API 可用前，不启用不完整的伪适配器。

## 架构原则

- Session、Seat、Device 只有一个权威核心状态模型。
- 不依赖 systemd PID 1；Dinit 管理服务生命周期和自身 cgroup。
- eudevd 独立运行，dinit-logind 消费设备事件。
- SSH、TTY、图形和无头 Session 均可存在；无 Seat 会话不伪装成 seat0。
- 未实现的 login1/libseat API 不得虚报成功。
- 面向 glibc 和 musl；引入第三方源码前审查许可证。

首个验证目标是 DroidSpaces 容器中的 XFCE4/X11、X11/VNC，以及纯 TTY、SSH 和无头 Session。图形栈不是核心服务依赖。

项目许可证尚未确定；第三方源码许可证和署名必须在引入前审查。
