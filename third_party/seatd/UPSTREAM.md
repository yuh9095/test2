# Vendored seatd / libseat

- Upstream: https://github.com/kennylevinsen/seatd
- Upstream version: 0.9.3
- Pinned upstream commit: 92b1617c50f710699b3a6db6b0cd8197bf3f62ca
- Upstream license: MIT (see LICENSE)

This directory contains the upstream seatd and libseat implementation,
preserved as a separately identifiable third-party component. The Linux
source set is retained; BSD-specific build files and wscons support are not
included in this Linux-only project.

Included functionality:
- seatd UNIX-domain-socket server and client protocol;
- libseat public API and seatd, logind, noop, and optional builtin backends;
- Linux DRM, evdev, and hidraw device handling;
- VT and poller support, seat activation, device revocation, and FD passing;
- upstream unit tests, example, and manual sources.

The upstream logind backend is disabled by default in the vendored Meson
options. It requires elogind or systemd development libraries and is not a
runtime requirement for dinit-logind. The upstream seatd daemon is not
automatically launched by dinit-logind merely because its source is present.

This import does not decide the final authoritative Seat/Session ownership
model. The dinit-logind integration adapter must ensure there is only one
authoritative state source and must not accidentally run a second competing
seat manager.

Keep third-party code identifiable. Put dinit-logind-specific adapters and
changes outside this directory. If upstream files are patched, document the
patch and its reason here, and preserve the upstream MIT notice.
