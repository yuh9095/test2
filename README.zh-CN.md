# dinit-logind

登录、Session、Seat 与设备管理服务，面向使用 Dinit 的 Linux 系统。

> 项目处于早期开发阶段。已有 C11 核心状态模型、libudev/eudev 设备集成、特权 Session 注册 Socket，以及可选构建的 PAM Session 模块。login1 D-Bus、完整 Turnstile hook、libseat FD 交接和完整 elogind API 兼容尚未完成。

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

当前提供 login1 Manager、Session、Seat 的只读查询与属性子集。接口范围见 [login1 D-Bus 文档](docs/login1-dbus.zh-CN.md)。构建需要 `libsystemd` 的 `sd-bus` 开发库，但不依赖 systemd PID 1 或 systemd-logind。

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
