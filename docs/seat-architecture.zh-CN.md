# Seat 子系统架构（初始设计）

状态：架构基线，尚未开始实现。本文件描述设计，不代表功能已经可用。

## 1. 目标

- 在 dinit-logind 内建立唯一、权威的 Seat 与设备状态模型。
- 兼容桌面组件使用的 login1 D-Bus Seat 和 Session 设备接口。
- 让使用 libseat logind 后端的客户端无需 systemd PID 1 即可使用 dinit-logind。
- eudev 继续作为独立设备管理器，dinit-logind 消费其设备枚举和热插拔事件。
- 支持单 Seat、多 Seat，以及设备能力受限的容器和 Headless 环境。
- 明确报告硬件能力缺失，不虚构 VT、DRM 主设备或图形 Seat。
- 同时支持 glibc 和 musl。

## 2. 明确不做的事情

- 不在 dinit-logind 内嵌入或复制第二套 seatd 状态管理器。
- 不重新实现一个 libseat 客户端库。
- 不让 Turnstile 负责 Seat 跟踪。Turnstile 上游明确说明它不是 Seat tracker。
- 不依赖 systemd、elogind 或 systemd PID 1。
- 不把 eudev 变成 dinit-logind 的子进程或内部线程。
- 不假设每个 Seat 都有 VT、DRM、键盘或显示器。
- 不在 Seat 子系统内实现 VNC/RDP 远程桌面传输。

## 3. 组件边界

```text
                   应用程序 / 桌面环境
             Xorg、Compositor、会话客户端
                         |
             +-----------+------------+
             |                        |
       login1 D-Bus             libseat 客户端 API
             |                  (logind 后端)
             +-----------+------------+
                         |
                  Seat D-Bus 适配层
                         |
                 +-------v-------+
                 |  SeatManager  |
                 | 唯一权威状态源 |
                 | Seat/Session/ |
                 | Device        |
                 +---+---+---+---+
                     |   |   |
          +----------+   |   +-----------+
          |              |               |
    SessionManager  DeviceManager   VTManager
          |              |               |
     Turnstile       eudev 事件      Linux VT ioctl
     集成层           与设备元数据     可用时才启用
```

这是逻辑边界，不要求每个模块都运行在独立进程。SeatManager、DeviceManager、VTManager 是 dinit-logind 服务内部模块。

### SeatManager

唯一负责维护以下权威状态：
- Seat 身份和能力。
- 每个 Seat 关联的 Session。
- 当前活动 Session 和待处理的激活转换。
- 分配给每个 Seat 的设备记录。
- Session、已授予的设备 FD，以及 pause/resume 状态之间的关系。

只有 SeatManager 可以提交 Seat/Session 激活状态。D-Bus 适配层和兼容层只能调用它，不得维护平行状态。

### DeviceManager

从独立运行的 eudev 获取设备枚举和热插拔事件，解析设备元数据（包括 Seat 分配和 master-of-seat 分类），跟踪设备节点身份，并把 add/change/remove 事件通知 SeatManager。

DeviceManager 不决定哪个 Session 活动，也不独立授予设备访问权限。

### VTManager

仅在内核和运行环境确实提供 VT 能力时，负责活动 VT 监测、VT 切换和 Session/VT 关联。能力不存在时必须明确报告。没有可访问控制台的容器仍然可以使用非 VT Seat 和 Headless Session。

### SessionManager 集成

Turnstile 派生的用户/会话生命周期仍由独立的 SessionManager 负责。SeatManager 通过窄接口将 Session 关联到 Seat，不接管用户记录、PAM、XDG_RUNTIME_DIR、linger 或用户级服务管理器生命周期。

## 4. 一个状态模型，多种协议入口

第一阶段兼容目标是 login1 D-Bus。公开对象模型包括：

- Manager：Seat 枚举和查找。
- Seat：身份、活动 Session、Session 列表、能力属性和 Seat 切换操作。
- Session：Seat 关联，以及设备获取、释放、暂停和恢复操作。

必须区分两类兼容性：

1. **协议兼容性**：D-Bus 名称、签名、对象路径、属性和错误行为是否符合预期。
2. **运行能力**：当前环境是否真的能执行操作，例如切换 VT 或打开 DRM 节点。

不能因为 D-Bus 方法存在就假装操作成功。未支持的操作必须返回稳定、明确且有文档说明的错误。

### 与 libseat 的关系

libseat 是客户端抽象库。现有 libseat 客户端可以通过它的 logind 后端连接 login1 服务。因此：

- 实现 libseat logind 后端实际需要的 login1 行为。
- 不宣称提供所谓的 libseat server API。
- 不启动第二个 seatd daemon，也不维护第二份 Seat/Device/Session 数据库。
- 只有在真实客户端确实需要时，才把 seatd IPC 协议兼容作为单独的后续功能评估。

## 5. Seat 和 Device 数据模型

### Seat 身份

- 有 udev Seat 标识时使用该标识；常见主 Seat 为 `seat0`。
- 不能仅因为任意非主设备带有 `ID_SEAT` 就创建物理 Seat。
- master-of-seat 设备用于确立物理 Seat 的存在及其图形能力。
- 在明确策略或会话跟踪需要时，可以创建没有物理主设备的逻辑/Headless Seat，但不得谎报图形或 VT 能力。
- 只有设备元数据契约明确规定时，设备才默认归入 `seat0`；不能静默地把设备从一个 Seat 移到另一个 Seat。

### Device 身份

设备记录必须基于稳定设备身份，而不能只依赖路径。至少保存：
- 设备节点路径。
- 设备类型（字符/块设备）及 major/minor。
- Seat 归属。
- 是否为 master-of-seat。
- 当前可用状态和移除状态。
- 当前持有该设备打开 FD 的 Session（如果有）。
- 访问状态：active、paused 或等待 pause 完成。

收到设备 change 事件时重新验证身份和元数据。设备移除时，禁止后续打开，并通知仍持有该设备的 Session。

### 主设备与非主设备

主设备决定物理 Seat 是否具有其核心硬件，以及能否报告图形能力。其他设备可以在 Seat 已存在时被跟踪，并在 Session 请求时按需授予。仅有非主设备时，不能让一个已经失去全部主设备的物理 Seat 继续存活。

systemd-logind 的 Seat/Device 模型可作为行为参考：主设备确立 Seat，其他 Session 设备用于访问和热插拔跟踪。这里只参考行为，不直接复制整个上游源码树。

## 6. 设备访问与 FD 生命周期

设备访问必须集中管理，并且绑定到具体 Session：

1. Session 通过 login1 Session 设备 API 请求设备。
2. SeatManager 验证 Session 存在、已关联该 Seat，且当前有权访问设备。
3. DeviceManager 验证设备身份和可用状态。
4. 服务使用 close-on-exec 及必要的安全标志打开设备，并通过 D-Bus FD passing 传递 FD。
5. 服务记录授权关系，以便暂停、恢复、撤销和清理。
6. Release 关闭服务自身持有的 FD 并删除授权记录。客户端已经收到的 FD 不会因为服务关闭自己的 FD 就自动关闭。

切换 Session 时：
- 将旧 Session 的设备授权标记为等待暂停。
- 按 login1 pause-device 信号语义通知客户端。
- 在必要的 pause 握手完成，或明确的超时/失败策略执行后，才激活新 Session。
- Session 再次活动时恢复其符合条件的设备授权。
- 设备移除或客户端断开时，不能留下失效授权记录。

具体 D-Bus 方法和信号签名必须在实现前对照选定的 systemd 兼容基线核验，不能凭记忆猜测。

## 7. Session 激活和 Seat 切换

激活是状态转换，不是简单修改一个属性。

转换流程：
1. 验证目标 Session 属于该 Seat，且没有进入关闭状态。
2. 请求旧活动 Session 暂停设备。
3. 只有真实 VT 可用且目标 Session 有有效 VT 关联时，才执行 VT 切换。
4. 切换成功后才提交活动 Session 状态。
5. 恢复目标 Session 符合条件的设备授权。
6. 状态一致后再发出属性变化和 Session 激活通知。

没有 VT 的 Seat 仍然可以进行逻辑 Session 激活：SeatManager 切换活动逻辑会话，并执行相同的设备 pause/resume 规则。不得伪造 VT 编号。

## 8. 容器与 Headless 行为

容器即使具备 namespace 和 cgroup，也可能没有宿主机 DRM、输入设备或 VT 的访问权。仅凭 namespace 存在不能认定容器拥有图形 Seat。

首阶段容器规则：
- 不能仅因宿主机控制台或电源设备节点在文件系统中可见就打开它们。
- 除非容器明确获得委派的控制 VT，否则不得切换宿主机 VT。
- 不得更改容器委派设备范围之外的宿主机设备 ACL 或权限。
- 没有主设备时报告不具备图形能力。
- 保留 Session 跟踪和非图形 login1 行为。
- 无权访问的设备节点视为不可用，不能导致 daemon 启动失败。

DroidSpaces 首轮验证要分别测试 Headless Seat 和 XFCE4/X11 路径。容器中的 X11 可能使用虚拟 X Server，不一定会触发 DRM/VT 设备交接；测试报告必须准确说明实际覆盖的路径。

## 9. 安全不变量

- 不得直接信任客户端传入的设备路径，必须与当前设备元数据核对。
- 打开设备后验证 major/minor，降低设备节点被替换的竞态风险。
- 所有内部设备 FD 使用 close-on-exec。
- 除非存在经过审计的明确策略，不得向非活动 Session 授予设备。
- 一个 Session 不得释放或暂停另一个 Session 的设备授权。
- Session 关闭、客户端断开、设备移除或 daemon 关闭时，撤销服务持有的授权并清理 ACL/FD 状态。
- 容器内不得更改宿主机 ACL，除非设备已明确委派且操作被限制在容器设备策略范围内。
- D-Bus peer credentials 和 Session 所有权必须参与授权判断；不能只依赖对象路径。

## 10. 实施阶段

### Seat A 阶段：核心模型
- 定义 Seat、SeatDevice、DeviceGrant 数据结构。
- 实现 SeatManager 生命周期和不变量。
- 添加用于确定性单元测试的 fake/in-memory DeviceManager。
- 实现 Seat 能力计算和逻辑 Headless Seat 行为。

### Seat B 阶段：eudev 集成
- 启动时枚举现有设备。
- 订阅 add/change/remove 事件。
- 解析 Seat 和 master-of-seat 元数据。
- 测试设备重新归属、移除和主设备丢失。

### Seat C 阶段：Session 与激活集成
- 实现 Session attach/detach。
- 实现活动 Session 转换和 pause/resume 状态机。
- 在严格能力边界内加入 VTManager 能力探测和 VT 操作。

### Seat D 阶段：login1 兼容
- 导出 Manager 和 Seat 对象、属性及方法。
- 实现客户端需要的 Session 设备方法和信号。
- 增加 D-Bus 集成测试：introspection、签名、错误、FD passing 和激活。

### Seat E 阶段：客户端验证
- 验证 libseat logind 后端。
- 在环境具备所需设备访问能力时验证 Xorg/XFCE4。
- 验证没有 VT/DRM 访问权的 Headless 容器。
- 后续再验证真实多 Seat 主机和 Wayland compositor。

## 11. 必须覆盖的测试

- Seat 创建/删除和身份稳定性。
- seat0 默认行为和非 seat0 物理 Seat。
- 主设备添加/移除及图形能力变化。
- 非主设备添加/移除不能单独创建物理 Seat。
- 设备节点替换（major/minor 不匹配）。
- 活动/非活动 Session 的设备访问授权。
- pause/resume 确认、超时和客户端断开。
- 持有设备时 Session 被移除。
- 持有 FD 时设备被移除。
- VT 不可用和 VT 可用两种路径。
- 没有设备访问权的 Headless 容器启动。
- D-Bus FD passing 和精确 login1 签名。
- glibc 与 musl 两种构建和测试。

## 12. 编码前必须审计的上游参考

- systemd 对应兼容基线中的 `src/login/logind-seat.c`、`logind-device.c`、`logind-session-device.c` 及其 D-Bus 实现。
- libseat 的 logind 后端和公开客户端 API，确认现有客户端实际依赖的行为。
- eudev/libudev 的设备属性和 monitor 行为。
- Turnstile 项目明确的职责范围：Session/Login 跟踪和用户服务管理，不负责 Seat 跟踪。

在审查具体许可证和本项目许可证策略之前，不复制任何上游实现代码。
