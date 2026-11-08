# dinit-logind

A login, session, seat, and device-management service for systems using
[Dinit](https://github.com/davmac314/dinit).

[简体中文](README.zh-CN.md)

> **Status: early development.** The repository contains an initial C11 core
> model, Linux libudev/eudev device integration, and a foreground daemon
> supervised by Dinit. The login1 D-Bus service, Turnstile session creation,
> libseat FD handoff, and full elogind API compatibility are not yet complete.

## Build and test

Requirements: a C11 compiler, Meson, Ninja, and libudev development files.

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

The build uses strict ISO C11 mode (`-std=c11`), not GNU11. GitHub Actions
builds and tests with both GCC and Clang.

## Project direction

Build one integrated desktop and system login infrastructure by selectively
reusing mature ideas and implementations from:

- **elogind** — the login1 D-Bus model and implementation, user/session
  lifecycle, inhibitors, and power-management behavior.
- **seatd / libseat** — seat activation, device access, and file-descriptor
  handoff, while preserving the existing libseat client ecosystem.
- **Turnstile** — useful PAM and user-session lifecycle mechanisms, including
  runtime-directory handling where appropriate.
- **Dinit** — native service lifecycle and existing cgroup management.
- **eudev** — device discovery and hotplug events, remaining an independent
  device manager.

This is an integration and architectural refactoring effort, not a plan to run
three independent session/seat managers side by side.

## Core design principles

1. Maintain one authoritative internal model for users, sessions, seats, and
   devices.
2. Do not require systemd or a systemd PID 1.
3. Keep Dinit's native cgroup implementation unchanged. Do not recreate
   systemd's sandboxing, slice, or scope machinery as a project goal.
4. Preserve the public login1 D-Bus and libseat client interfaces where their
   behavior can be implemented correctly.
5. Support graphical and non-graphical sessions: X11, Wayland, TTY, SSH, and
   headless/container environments.
6. Treat graphical device access as an optional capability. A headless system
   must still be able to manage users, PAM sessions, runtime directories,
   inhibitors, and session lifecycles.
7. Do not report success for unimplemented APIs. Unsupported operations must
   return explicit, appropriate errors.
8. Design for both glibc and musl.
9. Audit source provenance, licenses, and dependency boundaries before
   importing code from upstream projects.

## Current implementation scope

The core in `src/core/manager.c` provides one in-process model for Seats,
Sessions, devices, active-session selection, device identity generations,
and logical per-session device claims.

The Linux platform layer uses libudev to enumerate devices and monitor eudev
hotplug events. Its Seat adapter applies a caller-supplied policy, caches
syspath-to-Seat identity for reliable removal, and registers verified
character-device nodes with the core. The foreground `dinit-logind` daemon
currently accepts `tty`, `drm`, and `input` devices. It prefers explicit
`ID_SEAT` metadata; an optional `--default-seat ID` enables an administrator
to assign otherwise-unlabelled supported devices to a chosen Seat. Without
that option, unlabelled devices are not silently assigned to `seat0`.

This is device discovery and core registration, not yet a complete login1
service or graphical session manager. Logical device claims and the current
device-FD APIs are not yet connected to Unix FD transfer over D-Bus.

## Planned internal areas

These are conceptual boundaries, not a commitment to separate daemons:

- User and session lifecycle
- Seat and device management
- VT and graphical-session integration
- PAM integration and runtime directories
- login1 D-Bus service
- Power management and inhibitors
- libseat compatibility
- Dinit and udev integration

The implementation should have a single source of truth for shared state.
External protocols and compatibility layers must call into that core rather
than maintain parallel state.

## Initial validation target

The first end-to-end target is a DroidSpaces Linux container running an
XFCE4/X11 desktop, with a remote X11/VNC path where available. This target
must also validate non-graphical login/session behavior; it is not a reason to
make X11 or VNC a dependency of the core service.

Later validation targets include physical Linux systems, Wayland compositors,
KDE Plasma, GNOME, and headless systems.

## Development approach

Work incrementally, but design and test the complete integration as one
system. Before importing implementation code:

1. Inventory the relevant upstream components and their actual capabilities.
2. Record licenses, provenance, and required notices.
3. Define shared object ownership and lifecycle boundaries.
4. Define the login1 and libseat compatibility requirements.
5. Establish build and test coverage for glibc and musl.

No upstream component is assumed to provide every capability listed above;
each feature must be verified against its implementation and tested.

## License

The project license has not yet been selected. No third-party source code
should be copied into this repository until license compatibility and
attribution requirements have been reviewed.

## Vendored seatd / libseat

The upstream seatd/libseat 0.9.3 Linux source set is vendored under
`third_party/seatd/`, with its MIT license and pinned provenance recorded in
`third_party/seatd/UPSTREAM.md`. It includes the seatd server, libseat public
API and backends, Linux device/VT handling, tests, example, and manual sources.

This is a source import only: the root dinit-logind build does not yet build or
launch the vendored seatd daemon, and no runtime integration is claimed yet.
The upstream libseat logind backend is disabled by default in its vendored
Meson options to avoid an implicit libsystemd/elogind dependency. Integration
must preserve one authoritative Seat/Session state source.
