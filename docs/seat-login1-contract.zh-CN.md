# login1 Seat / Session 设备接口契约（systemd 262 基线）

状态：接口审计记录，供后续实现和测试使用；不是已实现功能声明。

参考基线：systemd 262 的 `org.freedesktop.login1` D-Bus 文档，以及 libseat logind backend 当前源码。实现时必须将方法签名、信号顺序和 FD 语义作为兼容要求，而不能只实现同名空方法。

## 1. Seat 对象接口

对象路径：

```text
/org/freedesktop/login1/seat/<seat-id>
```

接口：`org.freedesktop.login1.Seat`

方法：

| 方法 | 输入 | 语义 |
|---|---|---|
| `Terminate` | 无 | 终止该 Seat 上的会话，必须经过会话策略检查 |
| `ActivateSession` | `s session_id` | 激活属于此 Seat 的 Session |
| `SwitchTo` | `u vtnr` | 切换到指定 VT；无 VT 能力时返回明确错误 |
| `SwitchToNext` | 无 | 切换到下一个 VT Session；仅适用于有 VT 的 Seat |
| `SwitchToPrevious` | 无 | 切换到上一个 VT Session；仅适用于有 VT 的 Seat |

属性：

| 属性 | D-Bus 类型 | 说明 |
|---|---|---|
| `Id` | `s` | Seat ID |
| `ActiveSession` | `(so)` | 活动 Session 的 ID 和对象路径；无活动 Session 时按兼容约定表示 |
| `CanTTY` | `b` | 是否具备文本 TTY 登录能力 |
| `CanGraphical` | `b` | 是否具备图形 Seat 能力 |
| `Sessions` | `a(so)` | 该 Seat 当前 Session 列表 |
| `IdleHint` | `b` | Seat 空闲提示 |
| `IdleSinceHint` | `t` | 实时时钟微秒时间戳 |
| `IdleSinceHintMonotonic` | `t` | 单调时钟微秒时间戳 |

当 ActiveSession、Sessions、CanGraphical、CanTTY 或 idle 状态变化时，必须发送兼容的属性变化通知。

## 2. Session 控制器接口

对象路径：

```text
/org/freedesktop/login1/session/<session-id>
```

接口：`org.freedesktop.login1.Session`

设备管理方法：

| 方法 | 精确签名 | 规则 |
|---|---|---|
| `TakeControl` | `b -> ()` | 每个 Session 同时只能有一个 D-Bus 控制连接；force 替换现有控制器仅允许 root |
| `ReleaseControl` | `() -> ()` | 释放控制权，并隐式释放该控制器取得的全部设备 |
| `TakeDevice` | `u,u -> h,b` | 输入 major/minor，返回 FD 和 inactive 标志 |
| `ReleaseDevice` | `u,u -> ()` | 释放指定设备 |
| `PauseDeviceComplete` | `u,u -> ()` | 确认已完成 pause 请求 |

相关信号：

| 信号 | 精确签名 | 语义 |
|---|---|---|
| `PauseDevice` | `u,u,s` | 设备暂停/移除通知 |
| `ResumeDevice` | `u,u,h` | 设备恢复，并传递新的 FD |

`TakeDevice` 只允许 Session 当前控制器调用。D-Bus 调用者的有效 UID 必须是该 Session 所属用户 UID 或 root。设备必须属于 Session 所属 Seat。设备不能在同一 Session 中重复取得，直到原授权被释放。

## 3. PauseDevice 的三种类型

`PauseDevice(major, minor, type)` 的 type 不是随意的状态文本，必须识别以下值：

- `pause`：请求客户端协作暂停。客户端关闭/停用其设备使用后，必须调用 `PauseDeviceComplete(major, minor)`。服务端需要有超时策略，不能无限等待。
- `force`：访问已经被服务端强制暂停，是异步通知；客户端不需要调用 PauseDeviceComplete。
- `gone`：设备已经移除。客户端不应再等待该设备的 ResumeDevice，也无需再调用 ReleaseDevice；若同 major/minor 的新设备加入，可重新 TakeDevice。

服务端发送信号时，必须只发送给该 Session 当前控制器。

## 4. FD 生命周期

### TakeDevice

- 输入是字符设备 major/minor，不接受客户端任意路径作为设备身份。
- 服务端必须通过当前设备清单解析设备，并核对它属于该 Session 的 Seat。
- 只支持经过明确审计的设备类别；未知类别返回不支持/设备不可用错误，不能无条件 open 任意字符设备。
- 打开设备时使用 close-on-exec，并执行设备类型所需的安全检查。
- 通过 D-Bus Unix FD passing 返回文件描述符。
- 返回的 inactive 布尔值必须反映该 Session 当时是否非活动，不能简单固定为 false。
- 对 inactive Session 的设备访问必须保持受限；不能因为返回 FD 就允许其继续有效使用。

### Pause / Resume

- `PauseDevice` 发送时机必须先于 Session `Active` 属性变化通知。
- `ResumeDevice` 发送时机必须在 Session `Active` 属性变化之后。
- ResumeDevice 必须传递新打开的 FD。客户端应切换到新 FD 并关闭旧 FD，不能假定底层内核 open-file-description 一定相同。
- Session 即使 Active=true，设备仍可能因其他原因保持 paused；Active 不能直接等同于每个 DeviceGrant 都 active。
- 设备被移除时发出 `gone`，清除设备授权和等待状态。

## 5. 控制连接断开

当控制器调用 ReleaseControl，或 D-Bus 控制连接断开：
- 自动释放该控制器通过 TakeDevice 获得的全部设备。
- 清理待处理 pause 确认。
- 清除控制器所有权。
- 不得遗留指向已断开连接的授权对象。
- 新控制器必须重新 TakeControl，之后才能申请设备。

不能只依赖客户端主动调用 ReleaseDevice；连接断开清理是协议语义的一部分。

## 6. 内部数据模型约束

建议的逻辑对象（具体语言和 ABI 后续确定）：

### Seat

- `id`
- `sessions`
- `active_session_id`（可空）
- `devices`
- `capabilities`：TTY、Graphical、VT switching 等明确能力
- `pending_activation`（可空）

### SeatDevice

- `major`, `minor`
- `devnode`（由设备管理器解析，不由客户端提供）
- `syspath`
- `seat_id`
- `device_kind`（仅列举已经实现并审计的类型）
- `master_of_seat`
- `present`
- `generation`（设备移除/重建后递增，避免旧授权误认新设备）

### DeviceGrant

- `session_id`
- `controller_connection_id`
- `major`, `minor`
- `device_generation`
- `manager_fd`
- `state`：active / pause-pending / paused / gone / released
- `pause_deadline`（仅 pause-pending 时有效）

FD 所有权必须明确：服务端持有的 manager FD 与通过 D-Bus 发送后由客户端持有的 FD 是不同的文件描述符引用。关闭其中一个不等于关闭另一个。

## 7. 激活状态机

一次 Session 激活转换至少包含：

```text
ACTIVE(old)
  |
  v
PAUSE_REQUESTED(old devices)
  |
  +-- all acknowledgements received --> COMMIT_ACTIVE(new)
  |
  +-- timeout / forced pause --------> FORCE_PAUSE(old)
                                          |
                                          v
                                    COMMIT_ACTIVE(new)
                                          |
                                          v
                                  RESUME(new devices)
```

VT Seat 的物理 VT 切换和非 VT Seat 的逻辑切换必须分开实现。非 VT Seat 不能因为没有 VT 而无法切换逻辑活动 Session；VT Seat 则不能伪造成功的 VT 切换。

必须保证：
- 同一 Seat 同时最多一个活动 Session。
- 同一 Seat 同时最多一个待处理激活转换；并发请求必须串行化或按明确规则取消旧请求。
- 新 Session 未提交活动前，不能提前发出 Active=true。
- pause 超时后有界地完成转换，不能永久卡住。
- 旧 Session 的迟到 PauseDeviceComplete 不能影响后续设备 generation 或新的激活转换。

## 8. libseat logind backend 的最低兼容行为

libseat logind backend 依赖 login1 Session 的控制器和设备生命周期，而不是连接 seatd 私有 IPC：

1. 通过 login1 Manager 查询当前进程 Session。
2. 对该 Session 调用 TakeControl。
3. 读取 Active 状态并监听 PropertiesChanged。
4. 监听 PauseDevice。
5. 对 `pause` 请求停用 Seat，并调用 PauseDeviceComplete。
6. 对 `force` 请求停用 Seat，不等待确认。
7. 对 `gone` 通知客户端设备已经消失。
8. Active 状态恢复后重新启用 Seat；由 login1 的 ResumeDevice FD 更新设备句柄。

因此，首阶段应优先实现 login1 兼容，不需要同时实现 seatd 私有协议。

## 9. 错误和能力策略

- Session 不存在：返回标准 D-Bus UnknownObject/兼容错误。
- 调用者不是 Session 用户或 root：拒绝控制器/设备访问。
- 已有控制器且未授权 force：返回 busy 类错误。
- 设备不属于 Session Seat：拒绝访问。
- 不支持的设备类别：明确返回不支持/设备不可用错误。
- 设备已移除：拒绝新授权，并按 gone 语义清理旧授权。
- Seat 无 VT：SwitchTo 类方法返回明确的不支持错误；逻辑 Session Activate 仍可支持。
- 容器没有委派设备：不打开宿主机节点，不创建虚假的图形能力。

错误名称和精确映射应在 D-Bus adapter 实现时与 systemd 262 的实际行为逐项对齐，并通过兼容测试固定下来。

## 10. 测试清单

- D-Bus introspection：对象路径、方法签名、信号签名和属性类型。
- TakeControl：正常取得、重复取得、root force、非 root force、断开连接。
- TakeDevice：合法设备、未知设备、错误 Seat、重复申请、inactive Session。
- ReleaseDevice：正常释放、重复释放、设备已 gone。
- PauseDevice：pause/force/gone 三种类型。
- PauseDeviceComplete：正确确认、无 pending 时确认、迟到确认、超时。
- ResumeDevice：新 FD 传递、旧 FD 替换。
- Session 切换：信号和 Active 属性变化顺序。
- 无 VT Seat：逻辑激活可用，SwitchTo 明确失败。
- 容器：无 DRM/VT 访问权时 daemon 仍可运行。

## 11. 上游参考

- systemd 262 login1 D-Bus API：<https://www.freedesktop.org/software/systemd/man/262/org.freedesktop.login1.html>
- libseat logind backend：<https://github.com/kennylevinsen/seatd/blob/master/libseat/backend/logind.c>
- systemd 262 Session device 实现：`src/login/logind-session-device.c`
- systemd 262 Seat 实现：`src/login/logind-seat.c`

本文件记录协议和行为要求。复制或改写上游实现之前，仍须完成许可证审计。


## Seat.Terminate 实现说明

仅 root 可以调用。服务端会为该 Seat 上每个已登记 Session 安排与 Session.Terminate 相同的 pidfd SIGTERM、5 秒宽限期和 SIGKILL 升级。若某个 Session 无法安全安排，调用返回错误；该操作不是事务，先前已安排的 Session 不会自动回滚。集成测试使用独立 Seat 和子进程验证目标范围与权限边界。
