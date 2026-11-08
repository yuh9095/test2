# PAM Session lifecycle integration

When PAM development headers are available, Meson builds and installs `pam_dinit_logind.so` under `libdir/security`. The core daemon remains buildable without PAM development files.

## Start the daemon

The PAM module's default endpoint is `/run/dinit-logind/session.sock`. Start the daemon with the matching explicit option:

```sh
dinit-logind --session-socket /run/dinit-logind/session.sock
```

The parent directory must already exist and be owned by root, with no write access for untrusted users. The daemon creates a 0600 `SOCK_SEQPACKET` socket and accepts only root peer credentials. It removes the socket on clean shutdown only if the path still refers to the inode it created.

## PAM configuration

```
session required pam_dinit_logind.so socket=/run/dinit-logind/session.sock
```

- `pam_sm_open_session` resolves the PAM username to a UID, reads PAM service/TTY/RHOST metadata, generates a random session ID and sends OPEN.
- The session ID is stored in PAM handle data.
- `pam_sm_close_session` retrieves that same ID and sends CLOSE; data is cleared only after success.
- A non-empty PAM_RHOST marks the session remote. Seat is not guessed from a TTY and remains unset.
- Client socket send/receive operations have a two-second timeout.
- This module does not authenticate users or replace PAM auth/account/password modules.

Limitations: open and close must share the same PAM handle. The leader PID is the process executing open_session and must be validated against each login manager's PAM call pattern; it is not a complete process-membership map. Orphan cleanup after crashes, cross-process PAM handle recovery and Turnstile-specific hooks are not implemented. Validate PAM stack order and daemon startup ordering before deployment.

## 生命周期状态机测试

PAM 模块使用独立生命周期状态机：只有 OPEN 传输成功才记录 Session ID；重复 OPEN 被拒绝；CLOSE 传输失败时保留 ID 以便重试；CLOSE 成功后才清空 ID。该状态机通过可注入传输回调进行单元测试。这些测试验证状态转换，不等同于真实 PAM 堆栈或登录管理器的端到端测试。
