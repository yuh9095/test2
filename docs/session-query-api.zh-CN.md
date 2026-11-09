# Session 查询接口

核心 Session 是 login1 D-Bus 适配器的数据来源。当前查询接口：

- `dl_manager_get_session_properties()`：按 Session ID 读取完整元数据，返回值包含 Session ID 和 UID。
- `dl_manager_find_session_by_leader_pid()`：按已登记的 leader PID 查找 Session。login1 适配器在此基础上还会用 pidfd 固定查询进程身份，并扫描已登记 cgroup 的直接 `cgroup.procs` 成员关系。
- `dl_manager_get_session_count()`：取得当前 Session 数量。
- `dl_manager_get_session_at()`：按索引读取 Session，顺序不保证稳定。

## PID 规则

只有非零 leader PID 会参与索引，并且同一时刻不能被两个 Session 重复登记。login1 的 `GetSessionByPID()` / `GetUserByPID()` 首先匹配 Leader；否则，仅在目标 PID 可成功打开 pidfd、仍保持存活，且其 PID 出现在已登记 Session cgroup 的直接 `cgroup.procs` 中时才返回该 Session。当前不递归扫描子 cgroup，也不按 UID 猜测进程归属。cgroup 元数据缺失或 pidfd 不可用时，非 Leader 查询会明确返回未找到。

## 生命周期与指针

查询结构中的字符串指针是借用引用，指向核心持有的内存。它们在对应 Session 被移除或 Manager 销毁前有效。D-Bus 适配器应在构造响应时立即复制/序列化，不得跨异步回调长期保存这些指针。

Session 数量和索引读取适合单线程事件循环中的 D-Bus 列表构造。若未来引入多线程访问，需要在 Manager 外围统一加锁，不能让 D-Bus 层直接访问内部链表。
