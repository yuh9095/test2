# Linux device and VT adapters

This directory contains Linux-only platform adapters. It is deliberately
separate from the core state model and does not own Seat or Session policy.

## eudev/libudev adapter

`udev.c` wraps libudev enumeration and the udev netlink monitor. It reports
device node identity (major/minor), syspath, subsystem, devtype, `ID_SEAT`,
and `ID_SEAT_MASTER` metadata to a caller-supplied callback. String members
are borrowed for the duration of the callback.

The adapter does not create Seats, grant device FDs, or decide whether a
device is authorized. The caller must apply the project Seat policy and must
not assume that an absent `ID_SEAT` property means `seat0`.

The monitor descriptor is non-owning and may be integrated with poll/epoll.
Call `dl_udev_dispatch()` after it becomes readable.

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
