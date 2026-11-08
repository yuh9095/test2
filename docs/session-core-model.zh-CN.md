# Session 核心模型

## 目的

Session 是用户登录生命周期的对象，不等同于 Seat。一个本地图形 Session 通常关联 Seat；SSH、后台任务、无头容器会话可以没有 Seat。

核心 API `dl_manager_add_session_with_properties()` 接收调用方已经验证的会话元数据，并将字符串复制到核心对象中。当前字段包括：

- `seat_id`：可选；非空时必须引用已经创建的 Seat。
- `type`：例如 `tty`、`x11`、`wayland`。
- `class_name`：例如 `user`、`greeter`、`background`。
- `service`：建立会话的 PAM service。
- `desktop`：桌面环境标识。
- `tty`、`display`：会话终端和显示标识。
- `leader_pid`：会话 leader PID。
- `remote`：是否为远程会话。

`dl_manager_add_session()` 保留为兼容旧调用的简化入口，默认 class 为 `user`、type 为 `unspecified`。

## 安全边界

这些字段只是核心状态模型，不会自行证明调用方提供的元数据真实可信。后续 Turnstile/PAM 接入必须通过受信任的注册通道提交数据，并验证调用者身份、UID、leader PID 和会话生命周期。不能仅凭任意进程传入的环境变量创建可信 Session，也不能把所有会话标记为本地会话。

无 Seat Session 不参与 Seat 活动状态，也不能取得 Seat 设备或 claim 图形设备。它仍可被查询和正常移除。

## 当前未实现

- PAM session open/close hook。
- Turnstile 到 dinit-logind 的可信注册协议。
- leader PID 存活监测和 PID 重用防护。
- login1 D-Bus Session 对象及属性导出。
- Polkit 本地/远程会话判定适配。

这些功能必须建立在同一核心 Session 对象上，避免 Turnstile、login1 和设备管理各自维护重复状态。
