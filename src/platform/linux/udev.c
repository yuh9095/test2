#define _POSIX_C_SOURCE 200809L
#include "platform/linux/udev.h"

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

struct dl_udev {
    struct udev *context;
    struct udev_monitor *monitor;
    int monitor_fd;
};

static bool property_is_true(const char *value)
{
    return value != NULL &&
        (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
         strcmp(value, "yes") == 0);
}

static bool dispatch_device(
    struct udev_device *udev_device,
    dl_udev_device_callback callback,
    void *userdata)
{
    struct dl_udev_device_info info;
    const char *action;
    const char *devnode;
    dev_t device_number;
    struct stat st;

    if (udev_device == NULL || callback == NULL) {
        return false;
    }

    memset(&info, 0, sizeof(info));
    action = udev_device_get_action(udev_device);
    devnode = udev_device_get_devnode(udev_device);
    device_number = udev_device_get_devnum(udev_device);

    info.action = action;
    info.syspath = udev_device_get_syspath(udev_device);
    info.devnode = devnode;
    info.subsystem = udev_device_get_subsystem(udev_device);
    info.devtype = udev_device_get_devtype(udev_device);
    info.seat_id = udev_device_get_property_value(udev_device, "ID_SEAT");
    info.master_of_seat = property_is_true(
        udev_device_get_property_value(udev_device, "ID_SEAT_MASTER"));

    if (device_number == 0) {
        return true;
    }
    if (devnode == NULL) {
        if (action != NULL && strcmp(action, "remove") == 0) {
            info.major_number = major(device_number);
            info.minor_number = minor(device_number);
            return callback(&info, userdata);
        }
        return true;
    }
    if (stat(devnode, &st) != 0) {
        /* Device removal races are normal; let the caller ignore stale nodes. */
        if (action != NULL && strcmp(action, "remove") == 0) {
            info.major_number = major(device_number);
            info.minor_number = minor(device_number);
            return callback(&info, userdata);
        }
        return true;
    }
    if (S_ISCHR(st.st_mode)) {
        info.is_character_device = true;
    } else if (S_ISBLK(st.st_mode)) {
        info.is_block_device = true;
    } else {
        return true;
    }

    if (major(st.st_rdev) != major(device_number) ||
        minor(st.st_rdev) != minor(device_number)) {
        return true;
    }
    info.major_number = major(device_number);
    info.minor_number = minor(device_number);
    return callback(&info, userdata);
}

int dl_udev_open(dl_udev **out_udev)
{
    dl_udev *udev;

    if (out_udev == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_udev = NULL;

    udev = calloc(1U, sizeof(*udev));
    if (udev == NULL) {
        return -1;
    }
    udev->monitor_fd = -1;
    udev->context = udev_new();
    if (udev->context == NULL) {
        free(udev);
        errno = ENOMEM;
        return -1;
    }
    udev->monitor = udev_monitor_new_from_netlink(udev->context, "udev");
    if (udev->monitor == NULL ||
        udev_monitor_enable_receiving(udev->monitor) < 0) {
        dl_udev_close(udev);
        errno = EIO;
        return -1;
    }
    udev->monitor_fd = udev_monitor_get_fd(udev->monitor);
    if (udev->monitor_fd < 0) {
        dl_udev_close(udev);
        errno = EIO;
        return -1;
    }
    {
        int flags = fcntl(udev->monitor_fd, F_GETFL);
        if (flags < 0 ||
            fcntl(udev->monitor_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            dl_udev_close(udev);
            return -1;
        }
    }
    *out_udev = udev;
    return 0;
}

void dl_udev_close(dl_udev *udev)
{
    if (udev == NULL) {
        return;
    }
    if (udev->monitor != NULL) {
        udev_monitor_unref(udev->monitor);
    }
    if (udev->context != NULL) {
        udev_unref(udev->context);
    }
    free(udev);
}

int dl_udev_enumerate(
    dl_udev *udev,
    dl_udev_device_callback callback,
    void *userdata)
{
    struct udev_enumerate *enumerator;
    struct udev_list_entry *entry;

    if (udev == NULL || callback == NULL) {
        errno = EINVAL;
        return -1;
    }
    enumerator = udev_enumerate_new(udev->context);
    if (enumerator == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (udev_enumerate_scan_devices(enumerator) < 0) {
        udev_enumerate_unref(enumerator);
        errno = EIO;
        return -1;
    }
    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerator)) {
        struct udev_device *device = udev_device_new_from_syspath(
            udev->context, udev_list_entry_get_name(entry));
        if (device != NULL) {
            bool keep_going = dispatch_device(device, callback, userdata);
            udev_device_unref(device);
            if (!keep_going) {
                udev_enumerate_unref(enumerator);
                return 0;
            }
        }
    }
    udev_enumerate_unref(enumerator);
    return 0;
}

int dl_udev_get_fd(const dl_udev *udev)
{
    if (udev == NULL) {
        errno = EINVAL;
        return -1;
    }
    return udev->monitor_fd;
}

int dl_udev_dispatch(
    dl_udev *udev,
    dl_udev_device_callback callback,
    void *userdata)
{
    struct udev_device *device;

    if (udev == NULL || callback == NULL) {
        errno = EINVAL;
        return -1;
    }
    errno = 0;
    device = udev_monitor_receive_device(udev->monitor);
    if (device == NULL) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == 0) {
            return 0;
        }
        return -1;
    }
    (void) dispatch_device(device, callback, userdata);
    udev_device_unref(device);
    return 1;
}
