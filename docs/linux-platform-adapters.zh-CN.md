# Linux device and VT adapters

This directory contains Linux-only platform adapters. The core state model
remains independent of D-Bus, PAM, udev, seatd, and systemd.

## eudev/libudev adapter

`udev.c` wraps libudev enumeration and the udev netlink monitor. It reports
device node identity (major/minor), syspath, subsystem, devtype, `ID_SEAT`,
and `ID_SEAT_MASTER` metadata to a caller-supplied callback. String members
are borrowed for the duration of the callback.

## eudev-to-Seat Manager integration

`udev-manager.c` connects that adapter to the existing core Manager. It
enumerates devices, exposes the monitor FD for poll/epoll, and applies add,
change, and remove events to the core device registry. It caches the last
syspath-to-Seat/device mapping so a remove event can clean up the correct
core device even when udev no longer supplies a devnode or `ID_SEAT`.

Seat assignment is deliberately policy-driven. The supplied policy callback
chooses the Seat and capability flags. The adapter does not silently assign
devices without an explicit Seat decision to `seat0`; a policy can use
`ID_SEAT` where available and apply any additional Linux-specific rules it
needs. Only character devices are registered for session device access.

The integration creates a core Seat when the policy first selects it. It
does not start eudevd, change udev rules, grant access by itself, or replace
the caller's responsibility to run the monitor FD in its event loop. Device
FD authorization remains in the core Manager and the future login1 D-Bus
transport.

Open the monitor before calling enumerate so events occurring during the
initial inventory scan are queued for subsequent dispatch. Call
`dl_udev_seat_monitor_dispatch()` whenever its FD becomes readable.

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
