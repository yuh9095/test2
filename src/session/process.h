#ifndef DINIT_LOGIND_SESSION_PROCESS_H
#define DINIT_LOGIND_SESSION_PROCESS_H

#include <stdbool.h>
#include <stdint.h>

/* Open a cgroup v2 directory below an already-open mount/root directory.
 * Every path component is opened with O_NOFOLLOW; absolute and traversing
 * paths are rejected. The returned descriptor is owned by the caller. */
int dl_session_cgroup_openat(int root_fd, const char *relative_path);

/* Check the cgroup.procs membership list without following symlinks. */
int dl_session_cgroup_contains_pid(
    int cgroup_fd,
    uint32_t pid,
    bool *out_contains);

/* Send a signal to the process identified by a stable pidfd. No PID fallback. */
int dl_session_pidfd_send_signal(int pidfd, int signal_number);

/* Write cgroup.kill only after the stable leader pidfd is live and the
 * registered leader PID is still listed in this exact cgroup directory.
 * Returns -1 with errno=ENOTSUP when cgroup.kill/pidfd support is absent. */
int dl_session_cgroup_kill_all(
    int cgroup_fd,
    uint32_t leader_pid,
    int leader_pidfd);
/* Kill through a directory fd whose path and Leader membership were verified
 * before an asynchronous termination request was accepted. */
int dl_session_cgroup_kill_pinned(int cgroup_fd);

#endif
