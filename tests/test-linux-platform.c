#include "platform/linux/udev.h"
#include "platform/linux/vt.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>

int main(void)
{
    dl_vt *vt = NULL;
    unsigned int active = 0U;

    assert(dl_vt_open(NULL, &vt) == DL_VT_INVALID_ARGUMENT);
    assert(vt == NULL);
    assert(dl_vt_open("/dev/null", &vt) == DL_VT_UNAVAILABLE);
    assert(vt == NULL);
    assert(dl_vt_get_active(NULL, &active) == DL_VT_INVALID_ARGUMENT);
    assert(dl_vt_activate(NULL, 1U) == DL_VT_INVALID_ARGUMENT);
    assert(dl_vt_wait_active(NULL, 1U) == DL_VT_INVALID_ARGUMENT);

    assert(dl_udev_get_fd(NULL) == -1);
    assert(errno == EINVAL);
    assert(dl_udev_enumerate(NULL, NULL, NULL) == -1);
    assert(errno == EINVAL);
    assert(dl_udev_dispatch(NULL, NULL, NULL) == -1);
    assert(errno == EINVAL);

    puts("Linux platform API tests passed");
    return 0;
}
