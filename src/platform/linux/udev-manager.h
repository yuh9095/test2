#ifndef DINIT_LOGIND_LINUX_UDEV_MANAGER_H
#define DINIT_LOGIND_LINUX_UDEV_MANAGER_H

#include "core/manager.h"
#include "platform/linux/udev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dl_udev_seat_monitor dl_udev_seat_monitor;

/*
 * Select the Seat and capabilities for a device.
 *
 * Return true only when the device belongs to a Seat managed by this
 * instance. The returned seat_id is borrowed and is copied before this
 * callback returns. Returning false leaves the device outside logind
 * management; in particular, an absent ID_SEAT is never silently mapped
 * to seat0.
 */
typedef bool (*dl_udev_seat_policy)(
    const dl_udev_device_info *device,
    const char **seat_id,
    unsigned int *capabilities,
    void *userdata);

/*
 * Create a udev monitor connected to the supplied core Manager.
 * The Manager remains owned by the caller and must outlive the monitor.
 */
int dl_udev_seat_monitor_open(
    dl_udev_seat_monitor **out_monitor,
    dl_manager *manager,
    dl_udev_seat_policy policy,
    void *userdata);

void dl_udev_seat_monitor_close(dl_udev_seat_monitor *monitor);

/* Borrowed non-blocking descriptor for poll/epoll integration. */
int dl_udev_seat_monitor_get_fd(const dl_udev_seat_monitor *monitor);

/* Register the current device inventory. Call after opening the monitor. */
int dl_udev_seat_monitor_enumerate(dl_udev_seat_monitor *monitor);

/* Dispatch one queued event; returns 1, 0 when empty, or -1 on error. */
int dl_udev_seat_monitor_dispatch(dl_udev_seat_monitor *monitor);

/*
 * Apply one device record. This is also exposed for deterministic adapter
 * tests and callers that already receive udev_device objects elsewhere.
 * Returns 1 if a managed device was added/updated/removed, 0 if ignored,
 * and -1 on error with errno set.
 */
int dl_udev_seat_monitor_apply(
    dl_udev_seat_monitor *monitor,
    const dl_udev_device_info *device);

#ifdef __cplusplus
}
#endif

#endif
