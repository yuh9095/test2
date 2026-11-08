# Session 查询接口

核心 Session 是 login1 D-Bus 适配器的数据来源。当前查询接口：

- `dl_manager_get_session_properties()`：按 Session ID 读取完整元数据，返回值包含 Session ID 和 UID。
- `dl_manager_find_session_by_leader_pid()`：按已登记的 leader PID 查找 Session。
- `dl_manager_get_session_count()`：取得当前 Session 数量。
- `dl_manager_get_session_at()`：按索引读取 Session，顺序不保证稳定。

## PID 规则

只有非零 leader PID 会参与索引，并且同一时刻不能被两个 Session 重复登记。按 PID 查询只匹配 leader，不扫描进程树，也不把同 UID 的其他进程自动归入该 Session。后续若要实现完整的 login1 `GetSessionByPID` 语义，需要由可信的 Turnstile/PAM 集成层维护进程归属，不能在核心层猜测。

## 生命周期与指针

查询结构中的字符串指针是借用引用，指向核心持有的内存。它们在对应 Session 被移除或 Manager 销毁前有效。D-Bus 适配器应在构造响应时立即复制/序列化，不得跨异步回调长期保存这些指针。

Session 数量和索引读取适合单线程事件循环中的 D-Bus 列表构造。若未来引入多线程访问，需要在 Manager 外围统一加锁，不能让 D-Bus 层直接访问内部链表。
