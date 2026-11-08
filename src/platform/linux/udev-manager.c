#define _POSIX_C_SOURCE 200809L
#include "platform/linux/udev-manager.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct dl_udev_managed_device {
    char *syspath;
    char *seat_id;
    uint32_t major_number;
    uint32_t minor_number;
    struct dl_udev_managed_device *next;
} dl_udev_managed_device;

struct dl_udev_seat_monitor {
    dl_udev *udev;
    dl_manager *manager;
    dl_udev_seat_policy policy;
    void *userdata;
    int callback_error;
    dl_udev_managed_device *devices;
};

static char *copy_string(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL || value[0] == '\0') {
        return NULL;
    }
    length = strlen(value);
    copy = malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, value, length + 1U);
    }
    return copy;
}

static int manager_error_to_errno(dl_error error)
{
    switch (error) {
    case DL_OK:
        return 0;
    case DL_ERROR_NO_MEMORY:
        return ENOMEM;
    case DL_ERROR_NOT_FOUND:
        return ENOENT;
    case DL_ERROR_ALREADY_EXISTS:
        return EEXIST;
    case DL_ERROR_BUSY:
        return EBUSY;
    case DL_ERROR_LIMIT:
        return EOVERFLOW;
    case DL_ERROR_INVALID_ARGUMENT:
    case DL_ERROR_INVALID_STATE:
    default:
        return EINVAL;
    }
}

static dl_udev_managed_device **find_device_link(
    dl_udev_seat_monitor *monitor,
    const char *syspath)
{
    dl_udev_managed_device **link = &monitor->devices;

    while (*link != NULL) {
        if (strcmp((*link)->syspath, syspath) == 0) {
            break;
        }
        link = &(*link)->next;
    }
    return link;
}

static int remove_tracked_device(
    dl_udev_seat_monitor *monitor,
    const char *syspath)
{
    dl_udev_managed_device **link;
    dl_udev_managed_device *device;
    dl_error result;

    if (syspath == NULL || syspath[0] == '\0') {
        return 0;
    }
    link = find_device_link(monitor, syspath);
    device = *link;
    if (device == NULL) {
        return 0;
    }

    result = dl_manager_remove_device(
        monitor->manager,
        device->seat_id,
        device->major_number,
        device->minor_number);
    if (result != DL_OK && result != DL_ERROR_NOT_FOUND) {
        errno = manager_error_to_errno(result);
        return -1;
    }

    *link = device->next;
    free(device->syspath);
    free(device->seat_id);
    free(device);
    return 1;
}

static int ensure_seat(dl_udev_seat_monitor *monitor, const char *seat_id)
{
    dl_error result = dl_manager_add_seat(monitor->manager, seat_id);

    if (result == DL_OK || result == DL_ERROR_ALREADY_EXISTS) {
        return 0;
    }
    errno = manager_error_to_errno(result);
    return -1;
}

int dl_udev_seat_monitor_apply(
    dl_udev_seat_monitor *monitor,
    const dl_udev_device_info *device)
{
    const char *action;
    const char *seat_id = NULL;
    unsigned int capabilities = DL_DEVICE_CAP_NONE;
    bool selected;
    char *seat_copy;
    char *syspath_copy;
    dl_udev_managed_device *record;
    dl_error result;
    int removed;

    if (monitor == NULL || device == NULL) {
        errno = EINVAL;
        return -1;
    }
    action = device->action;

    if (action != NULL && strcmp(action, "remove") == 0) {
        removed = remove_tracked_device(monitor, device->syspath);
        if (removed != 0) {
            return removed;
        }

        /* A remove can arrive before initial enumeration populated our cache. */
        selected = monitor->policy(
            device, &seat_id, &capabilities, monitor->userdata);
        if (!selected || seat_id == NULL || seat_id[0] == '\0') {
            return 0;
        }
        result = dl_manager_remove_device(
            monitor->manager, seat_id,
            device->major_number, device->minor_number);
        if (result == DL_OK) {
            return 1;
        }
        if (result == DL_ERROR_NOT_FOUND) {
            return 0;
        }
        errno = manager_error_to_errno(result);
        return -1;
    }

    if (action != NULL && strcmp(action, "add") != 0 &&
        strcmp(action, "change") != 0) {
        return 0;
    }

    /* Reconcile change/add events against the last version we registered. */
    removed = remove_tracked_device(monitor, device->syspath);
    if (removed < 0) {
        return -1;
    }

    if (!device->is_character_device || device->devnode == NULL ||
        device->syspath == NULL || device->syspath[0] == '\0') {
        return 0;
    }

    selected = monitor->policy(
        device, &seat_id, &capabilities, monitor->userdata);
    if (!selected || seat_id == NULL || seat_id[0] == '\0') {
        return 0;
    }

    seat_copy = copy_string(seat_id);
    syspath_copy = copy_string(device->syspath);
    if (seat_copy == NULL || syspath_copy == NULL) {
        free(seat_copy);
        free(syspath_copy);
        errno = ENOMEM;
        return -1;
    }

    if (ensure_seat(monitor, seat_id) != 0) {
        free(seat_copy);
        free(syspath_copy);
        return -1;
    }

    result = dl_manager_register_device_node(
        monitor->manager,
        seat_id,
        device->major_number,
        device->minor_number,
        device->devnode,
        device->master_of_seat,
        capabilities,
        &(uint64_t){0});
    if (result != DL_OK) {
        free(seat_copy);
        free(syspath_copy);
        errno = manager_error_to_errno(result);
        return -1;
    }

    record = calloc(1U, sizeof(*record));
    if (record == NULL) {
        (void) dl_manager_remove_device(
            monitor->manager, seat_id,
            device->major_number, device->minor_number);
        free(seat_copy);
        free(syspath_copy);
        errno = ENOMEM;
        return -1;
    }
    record->syspath = syspath_copy;
    record->seat_id = seat_copy;
    record->major_number = device->major_number;
    record->minor_number = device->minor_number;
    record->next = monitor->devices;
    monitor->devices = record;
    return 1;
}

static bool monitor_device_callback(
    const dl_udev_device_info *device,
    void *userdata)
{
    dl_udev_seat_monitor *monitor = userdata;

    if (dl_udev_seat_monitor_apply(monitor, device) < 0) {
        monitor->callback_error = errno != 0 ? errno : EIO;
        return false;
    }
    return true;
}

int dl_udev_seat_monitor_open(
    dl_udev_seat_monitor **out_monitor,
    dl_manager *manager,
    dl_udev_seat_policy policy,
    void *userdata)
{
    dl_udev_seat_monitor *monitor;

    if (out_monitor == NULL || manager == NULL || policy == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_monitor = NULL;
    monitor = calloc(1U, sizeof(*monitor));
    if (monitor == NULL) {
        return -1;
    }
    monitor->manager = manager;
    monitor->policy = policy;
    monitor->userdata = userdata;
    if (dl_udev_open(&monitor->udev) != 0) {
        free(monitor);
        return -1;
    }
    *out_monitor = monitor;
    return 0;
}

void dl_udev_seat_monitor_close(dl_udev_seat_monitor *monitor)
{
    dl_udev_managed_device *device;

    if (monitor == NULL) {
        return;
    }
    device = monitor->devices;
    while (device != NULL) {
        dl_udev_managed_device *next = device->next;
        (void) dl_manager_remove_device(
            monitor->manager, device->seat_id,
            device->major_number, device->minor_number);
        free(device->syspath);
        free(device->seat_id);
        free(device);
        device = next;
    }
    dl_udev_close(monitor->udev);
    free(monitor);
}

int dl_udev_seat_monitor_get_fd(const dl_udev_seat_monitor *monitor)
{
    if (monitor == NULL) {
        errno = EINVAL;
        return -1;
    }
    return dl_udev_get_fd(monitor->udev);
}

int dl_udev_seat_monitor_enumerate(dl_udev_seat_monitor *monitor)
{
    int result;

    if (monitor == NULL) {
        errno = EINVAL;
        return -1;
    }
    monitor->callback_error = 0;
    result = dl_udev_enumerate(
        monitor->udev, monitor_device_callback, monitor);
    if (result < 0) {
        return -1;
    }
    if (monitor->callback_error != 0) {
        errno = monitor->callback_error;
        return -1;
    }
    return 0;
}

int dl_udev_seat_monitor_dispatch(dl_udev_seat_monitor *monitor)
{
    int result;

    if (monitor == NULL) {
        errno = EINVAL;
        return -1;
    }
    monitor->callback_error = 0;
    result = dl_udev_dispatch(
        monitor->udev, monitor_device_callback, monitor);
    if (monitor->callback_error != 0) {
        errno = monitor->callback_error;
        return -1;
    }
    return result;
}
