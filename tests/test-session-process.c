#define _GNU_SOURCE
#include "session/process.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static void write_file(const char *path, const char *value)
{
    FILE *stream = fopen(path, "w");
    assert(stream != NULL);
    assert(fputs(value, stream) >= 0);
    assert(fclose(stream) == 0);
}

static void test_safe_cgroup_open_and_membership(void)
{
    char root[] = "/tmp/dinit-logind-cgroup-XXXXXX";
    char path[512];
    char file[512];
    char linkpath[512];
    bool contains = false;
    int root_fd;
    int cgroup_fd;
    int pidfd = -1;
    assert(mkdtemp(root) != NULL);
    assert(snprintf(path, sizeof(path), "%s/user.slice", root) > 0);
    assert(mkdir(path, 0700) == 0);
    assert(snprintf(path, sizeof(path), "%s/user.slice/session.scope", root) > 0);
    assert(mkdir(path, 0700) == 0);
    assert(snprintf(file, sizeof(file), "%s/cgroup.procs", path) > 0);
    {
        char contents[64];
        assert(snprintf(contents, sizeof(contents), "%u\n", (unsigned int)getpid()) > 0);
        write_file(file, contents);
    }
    assert(snprintf(file, sizeof(file), "%s/cgroup.kill", path) > 0);
    write_file(file, "");
    assert(snprintf(linkpath, sizeof(linkpath), "%s/user.slice/escape", root) > 0);
    assert(symlink("session.scope", linkpath) == 0);

    root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(root_fd >= 0);
    cgroup_fd = dl_session_cgroup_openat(root_fd, "user.slice/session.scope");
    assert(cgroup_fd >= 0);
    assert(dl_session_cgroup_contains_pid(
        cgroup_fd, (uint32_t)getpid(), &contains) == 0);
    assert(contains);
    assert(dl_session_cgroup_contains_pid(
        cgroup_fd, (uint32_t)getpid() + 1U, &contains) == 0);
    assert(!contains);

    errno = 0;
    assert(dl_session_cgroup_openat(root_fd, "../outside") < 0);
    assert(errno == EINVAL);
    errno = 0;
    assert(dl_session_cgroup_openat(root_fd, "/absolute") < 0);
    assert(errno == EINVAL);
    errno = 0;
    assert(dl_session_cgroup_openat(root_fd, "user.slice//session.scope") < 0);
    assert(errno == EINVAL);
    errno = 0;
    assert(dl_session_cgroup_openat(root_fd, "user.slice/escape") < 0);
    assert(errno == ELOOP || errno == ENOTDIR);

    errno = ENOSYS;
#if defined(SYS_pidfd_open)
    pidfd = (int) syscall(SYS_pidfd_open, getpid(), 0U);
#endif
    if (pidfd >= 0) {
        int kill_result;
        errno = 0;
        kill_result = dl_session_cgroup_kill_all(
            cgroup_fd, (uint32_t)getpid() + 1U, pidfd);
        if (kill_result < 0 && (errno == ENOTSUP || errno == ENOSYS)) {
            /* pidfd_send_signal is not available in this kernel. */
        } else {
            assert(kill_result < 0 && errno == ESRCH);
            kill_result = dl_session_cgroup_kill_all(
                cgroup_fd, (uint32_t)getpid(), pidfd);
            assert(kill_result == 0);
            assert(snprintf(file, sizeof(file), "%s/cgroup.kill", path) > 0);
            {
                char contents[8] = {0};
                FILE *stream = fopen(file, "r");
                assert(stream != NULL);
                assert(fgets(contents, sizeof(contents), stream) != NULL);
                assert(strcmp(contents, "1\n") == 0);
                assert(fclose(stream) == 0);
            }
        }
        assert(close(pidfd) == 0);
    } else {
        assert(errno == ENOSYS || errno == EINVAL || errno == EPERM);
    }

    assert(close(cgroup_fd) == 0);
    assert(close(root_fd) == 0);
    assert(unlink(linkpath) == 0);
    assert(snprintf(file, sizeof(file), "%s/cgroup.procs", path) > 0);
    assert(unlink(file) == 0);
    assert(snprintf(file, sizeof(file), "%s/cgroup.kill", path) > 0);
    assert(unlink(file) == 0);
    assert(rmdir(path) == 0);
    assert(snprintf(path, sizeof(path), "%s/user.slice", root) > 0);
    assert(rmdir(path) == 0);
    assert(rmdir(root) == 0);
}

int main(void)
{
    test_safe_cgroup_open_and_membership();
    puts("session process safety tests passed");
    return 0;
}
