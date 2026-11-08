#include "core/manager.h"
#include "platform/linux/udev-manager.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysmacros.h>

static bool select_explicit_seat(
    const dl_udev_device_info *device,
    const char **seat_id,
    unsigned int *capabilities,
    void *userdata)
{
    (void) userdata;
    if (device->seat_id == NULL || device->seat_id[0] == '\0') {
        return false;
    }
    *seat_id = device->seat_id;
    *capabilities = DL_DEVICE_CAP_TTY;
    return true;
}

int main(void)
{
    dl_manager *manager = NULL;
    dl_udev_seat_monitor *monitor = NULL;
    dl_udev_device_info device;
    dl_seat_capabilities capabilities;
    uint64_t generation = 0U;

    assert(dl_manager_create(&manager) == DL_OK);
    assert(dl_udev_seat_monitor_open(
        &monitor, manager, select_explicit_seat, NULL) == 0);
    assert(dl_udev_seat_monitor_get_fd(monitor) >= 0);

    memset(&device, 0, sizeof(device));
    device.action = "add";
    device.syspath = "/sys/devices/virtual/tty/tty-test";
    device.devnode = "/dev/null";
    device.seat_id = "seat-test";
    device.major_number = (uint32_t) major( makedev(1, 3) );
    device.minor_number = (uint32_t) minor( makedev(1, 3) );
    device.is_character_device = true;
    device.master_of_seat = true;

    assert(dl_udev_seat_monitor_apply(monitor, &device) == 1);
    generation = dl_manager_device_generation(
        manager, "seat-test", device.major_number, device.minor_number);
    assert(generation != 0U);
    assert(dl_manager_get_seat_capabilities(
        manager, "seat-test", &capabilities) == DL_OK);
    assert(capabilities.has_tty);

    /* Devices without an explicit Seat assignment are deliberately ignored. */
    device.syspath = "/sys/devices/virtual/tty/unassigned";
    device.seat_id = NULL;
    assert(dl_udev_seat_monitor_apply(monitor, &device) == 0);

    /* Removal uses the cached Seat even when udev no longer has a devnode. */
    device.action = "remove";
    device.syspath = "/sys/devices/virtual/tty/tty-test";
    device.major_number = (uint32_t) major( makedev(1, 3) );
    device.minor_number = (uint32_t) minor( makedev(1, 3) );
    device.devnode = NULL;
    device.seat_id = NULL;
    assert(dl_udev_seat_monitor_apply(monitor, &device) == 1);
    assert(dl_manager_device_generation(
        manager, "seat-test", device.major_number, device.minor_number) == 0U);

    assert(dl_udev_seat_monitor_get_fd(NULL) == -1);
    assert(errno == EINVAL);

    dl_udev_seat_monitor_close(monitor);
    dl_manager_destroy(manager);
    puts("udev-to-manager integration tests passed");
    return 0;
}
