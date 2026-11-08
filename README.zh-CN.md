# dinit-logind

面向使用 [Dinit](https://github.com/davmac314/dinit) 的 Linux 系统的登录、会话、Seat 与设备管理服务。

[English](README.md) | 简体中文

> **项目状态：早期开发阶段。** 仓库已有 C11 核心状态模型、Linux libudev/eudev 设备集成，以及可由 Dinit 监督的前台守护进程。login1 D-Bus、实际 Turnstile/PAM 注册传输、libseat FD 交接和完整 elogind API 兼容仍未完成。

## 构建与测试

依赖：C11 编译器、Meson、Ninja，以及 libudev 开发文件。

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

构建使用严格 ISO C11 模式（`-std=c11`），不使用 GNU11。GitHub Actions 会分别使用 GCC 和 Clang 编译并运行测试。

## 项目方向

通过有选择地吸收成熟项目的设计与实现，构建一套统一的桌面及系统登录基础设施：

- **elogind**：借鉴 login1 D-Bus 模型与实现、用户和会话生命周期、Inhibitor 及电源管理行为。
- **seatd / libseat**：吸收 Seat 激活、设备访问和文件描述符交接机制，同时保留现有 libseat 客户端生态。
- **Turnstile**：吸收适用的 PAM 和用户会话生命周期机制，包括合适的运行时目录管理方式。
- **Dinit**：使用原生服务生命周期管理和现有 cgroup 管理能力。
- **eudev**：提供设备发现与热插拔事件，并继续作为独立设备管理器运行。

本项目的目标是整合并重构架构，而不是让多套独立的会话或 Seat 管理器并行运行。

## 核心设计原则

1. User、Session、Seat 和 Device 使用唯一、权威的内部状态模型。
2. 不要求安装 systemd，也不依赖 systemd PID 1。
3. 保持 Dinit 原生 cgroup 实现不变。本项目不以重建 systemd 沙箱、Slice 或 Scope 机制为目标。
4. 在能够正确实现其行为的前提下，兼容公开的 login1 D-Bus 与 libseat 客户端接口。
5. 支持图形和非图形会话，包括 X11、Wayland、TTY、SSH，以及无头和容器环境。
6. 图形设备访问属于可选能力。无头系统仍须能够管理用户、PAM 会话、运行时目录、Inhibitor 和会话生命周期。
7. 不得对尚未实现的 API 虚报成功。暂不支持的操作必须返回明确且适当的错误。
8. 同时面向 glibc 和 musl 设计与测试。
9. 在导入任何上游代码前，审查代码来源、许可证及依赖边界。

## 当前实现范围

核心位于 `src/core/manager.c`，提供统一的进程内状态模型，管理 Seat、Session、设备、活动 Session 选择、设备身份 generation，以及每个 Session 对设备的逻辑占用。Session 支持可选 Seat，并保存 type/class、PAM service、desktop、TTY、display、leader PID 和 remote 标志，因此 SSH、无头及后台会话不必伪装成图形 Seat 会话。

Linux 平台层通过 libudev 枚举设备并监听 eudev 热插拔事件。Seat 适配层应用调用者提供的策略，缓存 syspath 到 Seat 的映射以保证设备移除时能找到原记录，并将经过设备身份校验的字符设备节点注册到核心。前台 `dinit-logind` 守护进程目前接纳 `tty`、`drm` 和 `input` 子系统设备：优先使用明确的 `ID_SEAT` 属性；管理员也可以通过可选参数 `--default-seat ID`，将没有 `ID_SEAT` 的受支持设备分配给指定 Seat。不指定该参数时，不会擅自把未标记设备分配到 `seat0`。

核心 Session 查询接口提供按 leader PID 查找、Session 数量和按索引读取，供后续 login1 的 `GetSessionByPID` 与 `ListSessions` 等 D-Bus 方法复用。按 PID 查询目前只匹配 leader，不扫描进程树。

新增的内部 Session 注册协议位于 `src/session/registration.c`。它定义固定长度、带版本号的 OPEN/CLOSE 消息，校验 Session ID、字段终止符、标志位和消息长度，并要求调用者先以可信 peer UID 通过授权检查。协议预期运行在 Unix `SOCK_SEQPACKET` 上，由特权 PAM/Turnstile 入口使用；生产传输层必须通过 `SO_PEERCRED` 获取真实对端 UID，并将允许的 UID 固定为 root。当前提交实现的是协议校验和核心状态操作，尚未实现 Socket 监听器、PAM 模块或 Turnstile hook。详细边界见 `docs/session-registration.zh-CN.md`。

## 计划中的内部模块

以下是逻辑边界，不代表必须拆分成多个守护进程：

- 用户与会话生命周期管理
- Seat 与设备管理
- VT 和图形会话集成
- PAM 集成与运行时目录管理
- login1 D-Bus 服务
- 电源管理与 Inhibitor
- libseat 兼容层
- Dinit 与 udev 集成

共享状态必须只有一个真实来源。外部协议和兼容层应调用统一核心，不得各自维护互相冲突的状态。

## 首个验证目标

首个端到端目标是在 DroidSpaces Linux 容器中运行 XFCE4/X11，并在条件允许时验证 X11/VNC 远程访问路径。

同时必须验证非图形登录与会话管理。不能因为首个测试目标使用 X11/VNC，就让 X11 或 VNC 成为核心服务的依赖。

后续验证目标包括物理 Linux 系统、Wayland 合成器、KDE Plasma、GNOME 和无头系统。

## 开发方式

采用渐进式实现，但从一开始就按完整系统进行架构设计和集成测试。在导入上游实现代码前，先完成：

1. 盘点相关上游项目及其实际具备的能力。
2. 记录许可证、代码来源和必要的版权声明。
3. 明确共享对象的所有权及生命周期边界。
4. 定义 login1 和 libseat 的兼容要求。
5. 建立 glibc 与 musl 的构建和测试覆盖。

不假定任何一个上游项目已经具备这里列出的全部能力；每项功能都必须通过源码审查和实际测试确认。

## 许可证

项目许可证尚未确定。在完成许可证兼容性和署名要求审查前，不应将第三方源码复制到本仓库。

## 已引入 seatd / libseat 源码

已将上游 seatd/libseat 0.9.3 的 Linux 源码集放入 `third_party/seatd/`，并保留 MIT 许可证及固定版本来源记录，详见 `third_party/seatd/UPSTREAM.md`。源码包括 seatd 服务端、libseat 公共 API 与后端、Linux 设备和 VT 管理、上游测试、示例及手册。

目前这是源码引入阶段：dinit-logind 根构建尚未编译或启动 vendored seatd，也不宣称已经完成运行时集成。vendored Meson 默认关闭 libseat 的 logind 后端，避免隐式引入 libsystemd/elogind 依赖。后续集成必须确保 Seat/Session 只有一个权威状态源。
