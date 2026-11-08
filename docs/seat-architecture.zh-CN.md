# Seat subsystem architecture (initial design)

Status: design baseline; implementation has not started.

This document defines the Seat subsystem for dinit-logind. It is deliberately scoped to seat discovery, session-to-seat association, active-seat state, device ownership, and the compatibility boundary used by graphical clients. It does not claim that these features are implemented yet.

## 1. Goals

- Provide one authoritative Seat and device state model inside dinit-logind.
- Support the login1 D-Bus Seat and Session device APIs expected by existing desktop components.
- Allow clients using libseat's logind backend to use dinit-logind without a systemd PID 1.
- Keep eudev as the independent device manager and consume its device discovery and hotplug events.
- Support physical multi-seat systems, single-seat systems, and restricted/headless containers.
- Make missing hardware capabilities explicit instead of pretending that a container has a VT, DRM master device, or graphical seat.
- Keep the implementation usable with glibc and musl.

## 2. Non-goals

- Do not embed or fork a second copy of seatd's state manager.
- Do not implement a replacement libseat client library.
- Do not make Turnstile responsible for seat tracking. Turnstile's own README explicitly says it is not a seat tracker.
- Do not require systemd, elogind, or systemd PID 1.
- Do not make eudev a child component or internal thread of dinit-logind.
- Do not assume every seat has a VT, DRM device, keyboard, or display.
- Do not implement remote desktop transport (VNC/RDP) in the Seat subsystem.

## 3. Component boundaries

```text
                   Applications / desktop
             Xorg, compositors, session clients
                         |
             +-----------+------------+
             |                        |
       login1 D-Bus             libseat client API
             |                  (logind backend)
             +-----------+------------+
                         |
                  Seat D-Bus adapter
                         |
                 +-------v-------+
                 |  SeatManager  |
                 | authoritative |
                 | seat/session/ |
                 | device state  |
                 +---+---+---+---+
                     |   |   |
          +----------+   |   +-----------+
          |              |               |
    SessionManager  DeviceManager   VTManager
          |              |               |
     Turnstile       eudev events    Linux VT ioctls
     integration     and metadata    when available
```

The diagram describes logical boundaries, not a requirement that every component run in a separate process. SeatManager, DeviceManager, and VTManager are internal modules in the dinit-logind service.

### SeatManager

Owns the authoritative in-memory state for:
- Seat identity and capabilities.
- Sessions attached to each seat.
- The active session and pending activation transition.
- Device records assigned to each seat.
- The relationship between a session, its granted device FDs, and pause/resume state.

Only SeatManager may commit seat/session activation state. D-Bus and compatibility adapters must call it rather than maintaining parallel state.

### DeviceManager

Consumes device inventory and hotplug events from the independent eudev service. It resolves device metadata (including seat assignment and master-of-seat classification), tracks device-node identity, and notifies SeatManager of add/change/remove events.

DeviceManager does not decide which session is active and does not independently grant device access.

### VTManager

Owns VT-specific operations where the kernel and environment provide them: active VT observation, VT switching, and session/VT association. It reports capability absence cleanly. A container without an accessible controlling console must still be able to use non-VT seats and headless sessions.

### SessionManager integration

The existing Turnstile-derived session/user lifecycle remains a separate responsibility. SeatManager associates a session with a seat through a narrow internal interface; it does not take over user records, PAM, XDG_RUNTIME_DIR, linger, or per-user service-manager lifecycle.

## 4. One state model, multiple protocol adapters

The first compatibility target is the login1 D-Bus API. The public object model should include:

- Manager: seat enumeration and lookup.
- Seat: identity, active session, session list, capability properties, and seat-switch operations.
- Session: seat association and device acquisition/release/pause/resume operations.

The implementation must distinguish these two kinds of compatibility:

1. **Protocol compatibility**: expected D-Bus names, signatures, object paths, properties, and error behavior.
2. **Operational capability**: whether this runtime can actually perform the requested operation (for example, switch a VT or open a DRM node).

A method must not report success merely because its D-Bus name exists. Unsupported operations return a stable, documented error.

### libseat relationship

libseat is a client-side abstraction. Existing libseat clients can use its logind backend to communicate with the login1 service. Therefore:

- Implement the login1 behavior required by libseat's logind backend.
- Do not claim to provide a libseat server API.
- Do not start a second seatd daemon or maintain a second device/session database.
- Consider seatd IPC protocol compatibility only as a separately scoped future feature, if a real client requires it.

## 5. Seat and device model

### Seat identity

- Use the udev seat identifier when present; the conventional primary seat is `seat0`.
- A seat must not be created solely because an arbitrary non-master device has an `ID_SEAT` property.
- Master-of-seat devices establish the existence and graphical capability of a physical seat.
- A logical/headless seat may exist without physical master devices when explicitly created by policy or needed for session tracking; it must not advertise graphical or VT capabilities it does not have.
- Device seat assignment defaults to `seat0` only where the udev/device metadata contract defines that default. Do not silently move a device between seats.

### Device identity

A device record must be keyed by stable device identity, not just by a pathname. At minimum retain:
- Device node path.
- Device type (character/block) and major/minor numbers.
- Seat assignment.
- Master-of-seat classification.
- Current availability and removal state.
- Which session, if any, currently holds an open device FD.
- Whether access is active, paused, or awaiting pause completion.

On a device change event, revalidate identity and metadata. On removal, invalidate future opens and notify any session holding that device.

### Master and non-master devices

Master devices determine whether a physical seat has its primary hardware and whether it can advertise graphical capability. Other devices may be tracked on an existing seat and granted to sessions on demand. A non-master device alone must not keep an otherwise vanished physical seat alive.

This follows the useful distinction in systemd-logind's seat/device model: master devices establish the seat, while additional session devices are tracked for access and hotplug handling. The upstream implementation is a reference for behavior, not a source tree to copy wholesale.

## 6. Device access and FD lifecycle

The device access path must be centralized and session-scoped:

1. A session requests a device through the login1 Session device API.
2. SeatManager verifies that the session exists, is attached to the seat, and is currently permitted to access the device.
3. DeviceManager verifies the current device identity and availability.
4. The service opens the device with close-on-exec and the required safe flags, then passes the FD using D-Bus FD passing.
5. The service records the grant so it can pause, resume, revoke, and clean it up.
6. Release closes the manager-owned FD and removes the grant. Client-owned received FDs are not magically closed by the service.

For a session switch:
- Mark the old session's device grants as pausing.
- Notify the client using the login1 pause-device signal semantics.
- Do not activate the new session until the required pause handshakes have completed or a defined timeout/failure policy has run.
- Resume grants when the session becomes active again.
- Handle device removal and client disconnect without leaving stale grants.

The exact D-Bus method/signal signatures must be checked against the selected systemd compatibility baseline before implementation. Do not invent signatures from memory.

## 7. Session activation and seat switching

Activation is a state transition, not a property assignment.

The transition should:
1. Validate that the target session belongs to the seat and is not closing.
2. Ask the old active session to pause its devices.
3. Perform VT switching only when a real VT is available and the target session has a valid VT association.
4. Commit the active-session change only after the transition succeeds.
5. Resume the target session's eligible device grants.
6. Emit property-change and session activation notifications after the committed state is consistent.

For seats without VTs, activation is still meaningful: the SeatManager switches the active logical session and applies the same device pause/resume rules. Do not return a fake VT number.

## 8. Container and headless behavior

A container may have namespaces and cgroup support while lacking access to host DRM, input, or VT devices. Namespace presence alone does not prove that the container owns a graphical seat.

Initial container rules:
- Do not open host console or power-control devices merely because they are visible in the filesystem.
- Do not perform host VT switching unless the container has an explicitly delegated controlling VT.
- Do not change host device ACLs or permissions outside the container's delegated device set.
- If no master device is available, report no graphical capability.
- Keep session tracking and non-graphical login1 behavior available.
- Treat inaccessible device nodes as unavailable, not as fatal daemon startup errors.

For the first DroidSpaces validation, test a headless Seat and the XFCE4/X11 path separately. X11 in a container may use a virtual X server and may not exercise DRM/VT device handoff; report exactly which path was tested.

## 9. Security invariants

- Never trust a client-supplied device path without resolving it against current device metadata.
- Validate major/minor identity after opening to reduce device-node replacement races.
- Use close-on-exec for all internal device FDs.
- Do not grant a device to an inactive session unless a specific, audited policy permits it.
- Do not let a session release or pause another session's device grant.
- On session close, client disconnect, device removal, or daemon shutdown, revoke manager-owned grants and release ACL/FD state.
- Do not change host ACLs from a container unless the device is explicitly delegated and the operation is confined to the container's device policy.
- Treat D-Bus peer credentials and session ownership as authorization inputs; never rely on object paths alone.

## 10. Implementation sequence

### Seat phase A — core model
- Define Seat, SeatDevice, and DeviceGrant data structures.
- Implement SeatManager lifecycle and invariants.
- Add a fake/in-memory DeviceManager for deterministic unit tests.
- Implement seat capability calculation and logical headless seat behavior.

### Seat phase B — eudev integration
- Enumerate existing devices at startup.
- Subscribe to add/change/remove events.
- Parse seat and master-of-seat metadata.
- Test device reassignment, removal, and master-device loss.

### Seat phase C — session and activation integration
- Attach/detach sessions.
- Implement active-session transitions and pause/resume state machine.
- Add VTManager capability probing and VT operations behind a strict capability boundary.

### Seat phase D — login1 compatibility
- Export Manager and Seat objects/properties/methods.
- Implement the Session device methods/signals needed by clients.
- Add D-Bus integration tests for introspection, signatures, errors, FD passing, and activation.

### Seat phase E — client validation
- Validate libseat's logind backend.
- Validate Xorg/XFCE4 where the environment supports the required device access.
- Validate a headless container with no VT/DRM access.
- Later validate a real multi-seat host and Wayland compositors.

## 11. Required tests

- Seat creation/removal and stable identity.
- seat0 default behavior and non-seat0 physical seats.
- Master-device add/remove and graphical capability changes.
- Non-master device add/remove without creating a physical seat.
- Device-node replacement (major/minor mismatch).
- Active/inactive session access authorization.
- Pause/resume acknowledgement, timeout, and client disconnect.
- Session removal while holding devices.
- Device removal while an FD is held.
- VT unavailable and VT available paths.
- Headless container startup without device access.
- D-Bus FD passing and exact login1 signatures.
- Build and test with both glibc and musl.

## 12. Upstream references to audit before coding

- systemd `src/login/logind-seat.c`, `logind-device.c`, `logind-session-device.c`, and their D-Bus counterparts, pinned to the project's chosen systemd compatibility baseline.
- libseat's logind backend and public client API, to establish the exact behavior existing clients expect.
- eudev/libudev device properties and monitor behavior.
- Turnstile's documented scope: session/login tracking and user service management, not seat tracking.

No upstream implementation should be copied until its exact license and the project's licensing strategy have been reviewed.
