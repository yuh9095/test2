#define _POSIX_C_SOURCE 200809L
#include "platform/linux/vt.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/vt.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

struct dl_vt {
    int fd;
};

static dl_vt_result classify_probe_error(int error_number)
{
    if (error_number == ENOTTY || error_number == ENODEV ||
        error_number == EOPNOTSUPP) {
        return DL_VT_UNAVAILABLE;
    }
    return DL_VT_SYSTEM_ERROR;
}

dl_vt_result dl_vt_open(const char *device_path, dl_vt **out_vt)
{
    struct stat st;
    struct vt_stat state;
    struct vt_mode mode;
    dl_vt *vt;
    int fd;

    if (out_vt == NULL || device_path == NULL || device_path[0] != '/') {
        return DL_VT_INVALID_ARGUMENT;
    }
    *out_vt = NULL;

    fd = open(device_path, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
    if (fd < 0) {
        return errno == ENOENT || errno == ENODEV || errno == ENOTTY
            ? DL_VT_UNAVAILABLE : DL_VT_SYSTEM_ERROR;
    }

    if (fstat(fd, &st) != 0) {
        (void) close(fd);
        return DL_VT_SYSTEM_ERROR;
    }
    if (!S_ISCHR(st.st_mode)) {
        (void) close(fd);
        return DL_VT_UNAVAILABLE;
    }

    if (ioctl(fd, VT_GETSTATE, &state) < 0) {
        int saved_errno = errno;
        (void) close(fd);
        return classify_probe_error(saved_errno);
    }
    if (ioctl(fd, VT_GETMODE, &mode) < 0) {
        int saved_errno = errno;
        (void) close(fd);
        return classify_probe_error(saved_errno);
    }

    vt = calloc(1U, sizeof(*vt));
    if (vt == NULL) {
        (void) close(fd);
        return DL_VT_SYSTEM_ERROR;
    }
    vt->fd = fd;
    *out_vt = vt;
    return DL_VT_OK;
}

void dl_vt_close(dl_vt *vt)
{
    if (vt == NULL) {
        return;
    }
    if (vt->fd >= 0) {
        (void) close(vt->fd);
    }
    free(vt);
}

bool dl_vt_is_available(const dl_vt *vt)
{
    return vt != NULL && vt->fd >= 0;
}

dl_vt_result dl_vt_get_active(const dl_vt *vt, unsigned int *out_vt_number)
{
    struct vt_stat state;

    if (vt == NULL || vt->fd < 0 || out_vt_number == NULL) {
        return DL_VT_INVALID_ARGUMENT;
    }
    if (ioctl(vt->fd, VT_GETSTATE, &state) < 0) {
        return classify_probe_error(errno);
    }
    *out_vt_number = (unsigned int) state.v_active;
    return DL_VT_OK;
}

dl_vt_result dl_vt_activate(const dl_vt *vt, unsigned int vt_number)
{
    if (vt == NULL || vt->fd < 0 || vt_number == 0U || vt_number > 63U) {
        return DL_VT_INVALID_ARGUMENT;
    }
    if (ioctl(vt->fd, VT_ACTIVATE, (int) vt_number) < 0) {
        return classify_probe_error(errno);
    }
    return DL_VT_OK;
}

dl_vt_result dl_vt_wait_active(const dl_vt *vt, unsigned int vt_number)
{
    int result;

    if (vt == NULL || vt->fd < 0 || vt_number == 0U || vt_number > 63U) {
        return DL_VT_INVALID_ARGUMENT;
    }
    do {
        result = ioctl(vt->fd, VT_WAITACTIVE, (int) vt_number);
    } while (result < 0 && errno == EINTR);

    return result == 0 ? DL_VT_OK : classify_probe_error(errno);
}
