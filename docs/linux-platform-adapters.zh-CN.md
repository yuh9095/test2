# Linux device and VT adapters

This directory contains Linux-only platform adapters. The core state model
remains independent of D-Bus, PAM, udev, seatd, and systemd.

## eudev/libudev adapter

`udev.c` wraps libudev enumeration and the udev netlink monitor. It reports
device node identity (major/minor), syspath, subsystem, devtype, `ID_SEAT`,
and `ID_SEAT_MASTER` metadata to a caller-supplied callback. String members
are borrowed for the duration of the callback.

## eudev-to-Seat Manager integration

`udev-manager.c` connects the libudev adapter to the core Manager. It
enumerates existing devices, applies add/change/remove events to the core
device registry, and caches the syspath-to-Seat mapping so removal still
works when udev no longer supplies a devnode or `ID_SEAT`.

Seat assignment is policy-driven. The policy callback selects a Seat and
capability flags. Devices without an explicit policy decision are ignored;
the adapter never silently assigns an unlabelled device to `seat0`. Only
character devices are registered for session device access. The adapter
creates a core Seat when the policy first selects it.

## Event-loop integration

Open the monitor before enumerating the initial inventory so events arriving
during enumeration remain queued:

1. Create the core Manager.
2. Open `dl_udev_seat_monitor`.
3. Call `dl_udev_seat_monitor_enumerate()`.
4. Add `dl_udev_seat_monitor_get_fd()` to the daemon's epoll loop, or call
   `dl_udev_seat_monitor_poll()` from a poll-based loop.
5. When readable, call `dl_udev_seat_monitor_dispatch()`, or let
   `dl_udev_seat_monitor_poll()` wait and dispatch automatically.

The poll helper dispatches at most 64 queued events per call to avoid
starving other daemon work. A timeout of -1 waits indefinitely, 0 performs
a non-blocking poll, and a positive timeout allows the caller to service
other timers and IPC. EINTR is returned to the caller so it can process
signals. The raw FD remains available for a daemon that already uses epoll.

The adapter does not start eudevd, change udev rules, or independently grant
device access. Device authorization and FD lifecycle remain core Manager
responsibilities, with the login1 D-Bus transport still to be connected.

## VT adapter

`vt.c` probes a caller-authorized VT device using `VT_GETSTATE` and
`VT_GETMODE`. A missing VT interface is reported as
`DL_VT_UNAVAILABLE`; it is not treated as a daemon-fatal error.

The adapter can query the active VT, request `VT_ACTIVATE`, and wait for
`VT_WAITACTIVE`. It does not set `VT_PROCESS`, install signal handlers,
change keyboard mode, or take over a controlling terminal. Those operations
require explicit Session/VT lifecycle policy and are intentionally not
performed as a side effect of probing.

## Build dependency

The Linux platform target uses the `libudev` pkg-config dependency. This is
provided by eudev-compatible installations and is independent of systemd.
