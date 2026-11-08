#ifndef DINIT_LOGIND_LINUX_UDEV_MANAGER_H
#define DINIT_LOGIND_LINUX_UDEV_MANAGER_H

#include "core/manager.h"
#include "platform/linux/udev.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dl_udev_seat_monitor dl_udev_seat_monitor;

/*
 * Select the Seat and capabilities for a device.
 * Return true only when the device belongs to a Seat managed by this
 * instance. The returned seat_id is borrowed and copied before return.
 * Returning false leaves the device unmanaged; an absent ID_SEAT is never
 * silently mapped to seat0.
 */
typedef bool (*dl_udev_seat_policy)(
    const dl_udev_device_info *device,
    const char **seat_id,
    unsigned int *capabilities,
    void *userdata);

/* Manager remains owned by caller and must outlive this monitor. */
int dl_udev_seat_monitor_open(
    dl_udev_seat_monitor **out_monitor,
    dl_manager *manager,
    dl_udev_seat_policy policy,
    void *userdata);

void dl_udev_seat_monitor_close(dl_udev_seat_monitor *monitor);

/* Borrowed non-blocking descriptor for custom poll/epoll event loops. */
int dl_udev_seat_monitor_get_fd(const dl_udev_seat_monitor *monitor);

/* Register the current inventory. Open monitor before enumerating. */
int dl_udev_seat_monitor_enumerate(dl_udev_seat_monitor *monitor);

/* Dispatch one queued event; returns 1, 0 when empty, or -1 on error. */
int dl_udev_seat_monitor_dispatch(dl_udev_seat_monitor *monitor);

/*
 * Wait for device events and dispatch a bounded batch (at most 64 events).
 * timeout_milliseconds: -1 waits indefinitely, 0 polls, positive values
 * bound the wait. Returns dispatched event count, 0 on timeout, or -1.
 * EINTR is returned to the caller so its main loop can process signals.
 */
int dl_udev_seat_monitor_poll(
    dl_udev_seat_monitor *monitor,
    int timeout_milliseconds);

/*
 * Apply one device record. Returns 1 if a managed device was changed,
 * 0 if ignored, or -1 on error with errno set.
 */
int dl_udev_seat_monitor_apply(
    dl_udev_seat_monitor *monitor,
    const dl_udev_device_info *device);

#ifdef __cplusplus
}
#endif

#endif
