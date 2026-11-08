# PAM Session lifecycle integration

When PAM development headers are available, Meson builds and installs `pam_dinit_logind.so` under `libdir/security`. The core daemon remains buildable without PAM development files.

Example PAM session entry:

```
session required pam_dinit_logind.so socket=/run/dinit-logind/session.sock
```

The default socket is `/run/dinit-logind/session.sock`. The root-owned daemon must create it before the PAM stack invokes this module; its parent directory must be protected.

- `pam_sm_open_session` resolves the PAM username to a UID, reads PAM service/TTY/RHOST metadata, generates a random session ID and sends OPEN.
- The session ID is stored in PAM handle data.
- `pam_sm_close_session` retrieves that same ID and sends CLOSE; data is cleared only after success.
- A non-empty PAM_RHOST marks the session remote. Seat is not guessed from a TTY and remains unset.
- This module does not authenticate users or replace PAM auth/account/password modules.

Limitations: open and close must share the same PAM handle. The leader PID is the process executing open_session and must be validated against each login manager's PAM call pattern; it is not a complete process-membership map. Orphan cleanup after crashes, cross-process PAM handle recovery and Turnstile-specific hooks are not implemented. Validate PAM stack order and daemon startup ordering before deployment.
