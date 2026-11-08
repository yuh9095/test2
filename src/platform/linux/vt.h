#ifndef DINIT_LOGIND_LINUX_VT_H
#define DINIT_LOGIND_LINUX_VT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dl_vt dl_vt;

typedef enum dl_vt_result {
    DL_VT_OK = 0,
    DL_VT_UNAVAILABLE,
    DL_VT_INVALID_ARGUMENT,
    DL_VT_SYSTEM_ERROR
} dl_vt_result;

/*
 * Open an already-authorized Linux VT device (normally /dev/ttyN or
 * /dev/tty0). This function does not change VT_PROCESS mode or acquire
 * the controlling terminal. The returned object owns its descriptor.
 */
dl_vt_result dl_vt_open(const char *device_path, dl_vt **out_vt);
void dl_vt_close(dl_vt *vt);

bool dl_vt_is_available(const dl_vt *vt);
dl_vt_result dl_vt_get_active(const dl_vt *vt, unsigned int *out_vt_number);

/* Request a kernel VT switch. This does not wait for the switch to finish. */
dl_vt_result dl_vt_activate(const dl_vt *vt, unsigned int vt_number);

/* Wait until the requested VT is active; may block until the kernel reports it. */
dl_vt_result dl_vt_wait_active(const dl_vt *vt, unsigned int vt_number);

#ifdef __cplusplus
}
#endif

#endif
