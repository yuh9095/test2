#ifndef DINIT_LOGIND_LINUX_UDEV_H
#define DINIT_LOGIND_LINUX_UDEV_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dl_udev dl_udev;

typedef enum dl_udev_action {
    DL_UDEV_ACTION_ADD = 0,
    DL_UDEV_ACTION_CHANGE,
    DL_UDEV_ACTION_REMOVE,
    DL_UDEV_ACTION_OTHER
} dl_udev_action;

/*
 * All string members are borrowed and valid only for the duration of the
 * callback. The caller must copy any value it needs to retain.
 */
typedef struct dl_udev_device_info {
    const char *action;
    const char *syspath;
    const char *devnode;
    const char *subsystem;
    const char *devtype;
    const char *seat_id;
    uint32_t major_number;
    uint32_t minor_number;
    bool is_character_device;
    bool is_block_device;
    bool master_of_seat;
} dl_udev_device_info;

typedef bool (*dl_udev_device_callback)(
    const dl_udev_device_info *device,
    void *userdata);

/* Create a libudev context and a non-blocking kernel event monitor. */
int dl_udev_open(dl_udev **out_udev);
void dl_udev_close(dl_udev *udev);

/* Enumerate currently present device nodes. Return 0 or -1 with errno set. */
int dl_udev_enumerate(
    dl_udev *udev,
    dl_udev_device_callback callback,
    void *userdata);

/* File descriptor suitable for poll/epoll; owned by dl_udev. */
int dl_udev_get_fd(const dl_udev *udev);

/*
 * Receive and dispatch one queued event.
 * Returns 1 when an event was delivered, 0 when no event is queued,
 * and -1 on error with errno set.
 */
int dl_udev_dispatch(
    dl_udev *udev,
    dl_udev_device_callback callback,
    void *userdata);

#ifdef __cplusplus
}
#endif

#endif
