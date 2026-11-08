# Linux 设备与 VT 适配层

本目录包含仅面向 Linux 的平台适配层。核心状态模型不依赖 D-Bus、PAM、udev、seatd 或 systemd。

## eudev/libudev 适配层

`udev.c` 使用 libudev 完成设备枚举和 udev netlink 事件监听，并向回调提供设备节点身份（major/minor）、syspath、subsystem、devtype、`ID_SEAT` 和 `ID_SEAT_MASTER` 属性。字符串只在回调执行期间有效。

## eudev 到 Seat Manager 的集成

`udev-manager.c` 将 libudev 适配层连接到核心 Manager。它枚举现有设备，将 add/change/remove 事件应用到核心设备注册表，并缓存 syspath 到 Seat 的映射，以便设备移除时即使 udev 不再提供 devnode 或 `ID_SEAT`，也能找到原来的设备记录。

Seat 分配由策略回调决定。回调选择 Seat 和设备能力标志；没有明确策略结果的设备会被忽略。适配层不会自行把未标记设备分配给 `seat0`。只有字符设备会注册为可供会话访问的设备。策略首次选择某个 Seat 时，适配层会在核心中创建它。

守护进程当前只接纳 `tty`、`drm` 和 `input` 子系统的字符设备。设备若带有 `ID_SEAT`，优先使用该值；否则只有在管理员显式指定 `--default-seat ID` 时才会分配到默认 Seat。默认 Seat 参数仅是明确的部署策略，不会改变 eudev 属性，也不会启动或接管 eudevd。

例如，在确认容器中的设备确实属于同一个逻辑 Seat 后，可以由 Dinit 服务配置传入：

```sh
dinit-logind --default-seat seat0
```

如果不指定该参数，只有带有明确 `ID_SEAT` 的受支持设备才会进入 Seat Manager。不要在多 Seat 主机上盲目启用默认 Seat。

## 事件循环集成

先打开事件监视器，再枚举初始设备清单，以便枚举期间到达的事件仍保留在队列中：

1. 创建核心 Manager。
2. 打开 `dl_udev_seat_monitor`。
3. 调用 `dl_udev_seat_monitor_enumerate()`。
4. 将 `dl_udev_seat_monitor_get_fd()` 加入守护进程的 epoll 循环，或在 poll 循环中调用 `dl_udev_seat_monitor_poll()`。
5. FD 可读时调用 `dl_udev_seat_monitor_dispatch()`，也可以让 `dl_udev_seat_monitor_poll()` 自动等待并分发。

poll 辅助函数每次最多分发 64 个排队事件，避免设备事件长期占用守护进程。超时 -1 表示无限等待，0 表示立即轮询，正数表示等待毫秒数。EINTR 会交给调用者处理，以便主循环及时处理信号。已经使用 epoll 的守护进程仍可直接使用原始 FD。

适配层不会启动 eudevd、修改 udev 规则或自行授予设备访问权限。设备授权和 FD 生命周期仍由核心 Manager 负责；login1 D-Bus 传输层仍待接入。

## VT 适配层

`vt.c` 使用 `VT_GETSTATE` 和 `VT_GETMODE` 探测调用者已授权打开的 VT 设备。缺少 VT 接口时返回 `DL_VT_UNAVAILABLE`，不会导致守护进程退出。

适配层可以查询当前活动 VT、请求 `VT_ACTIVATE` 并等待 `VT_WAITACTIVE`。它不会设置 `VT_PROCESS`、安装信号处理器、修改键盘模式或接管控制终端。这些操作需要明确的 Session/VT 生命周期策略，不能作为探测的副作用。

## 构建依赖

Linux 平台目标依赖 pkg-config 的 `libudev`。eudev 兼容系统提供该接口，它与 systemd 无关。
