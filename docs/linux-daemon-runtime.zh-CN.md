# Linux daemon runtime

The first daemon entry point is a foreground Linux process intended to be
supervised by Dinit. It deliberately does not daemonize, fork, change the
system init process, or require systemd.

## Startup sequence

1. Block SIGINT, SIGTERM, and SIGHUP and receive them through signalfd.
2. Create the single in-process core Manager.
3. Open the libudev monitor.
4. Enumerate the current device inventory after the monitor is active.
5. Poll both the udev monitor FD and the signal FD.
6. Dispatch queued device events in bounded batches.
7. On SIGINT or SIGTERM, close the monitor and destroy the Manager cleanly.

SIGHUP is currently informational only because no reloadable configuration
exists yet. The daemon stays in the foreground; Dinit owns restart and
service lifecycle policy.

## Seat policy in this initial daemon

Only devices carrying an explicit ID_SEAT property are considered. The
daemon does not guess seat0 for unlabelled devices. TTY subsystem devices
contribute TTY/VT capability flags; DRM subsystem devices contribute the
graphical capability flag. This is a conservative first policy and does not
yet provide complete logind device authorization or VT session switching.

## Current limitations

This executable is a real process entry point and eudev event-loop host, but
it is not yet a complete logind service. There is no login1 D-Bus name,
PAM/Turnstile session creation, libseat FD handoff, or power-management API.
Do not configure desktop components to use it as a drop-in elogind replacement
yet.

Build and run in the foreground:

    meson setup build
    meson compile -C build
    ./build/dinit-logind --help
    ./build/dinit-logind

A Dinit service definition can supervise the foreground process with its
normal service command/restart policy. A packaged service file should only be
added once the installation paths and runtime account policy are finalized.
