#define _GNU_SOURCE
#include "session/process.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static bool valid_relative_path(const char *path)
{
    const char *component;
    size_t length;

    if (path == NULL) { errno = EINVAL; return false; }
    length = strlen(path);
    if (length == 0U || length > 4095U || path[0] == '/') return false;
    component = path;
    for (const char *cursor = path; ; cursor++) {
        unsigned char ch = (unsigned char) *cursor;
        if (ch == '\0' || ch == '/') {
            size_t n = (size_t) (cursor - component);
            if (n == 0U || (n == 1U && component[0] == '.') ||
                (n == 2U && component[0] == '.' && component[1] == '.')) {
                errno = EINVAL;
                return false;
            }
            if (ch == '\0') break;
            component = cursor + 1;
        } else if (ch < 0x21U || ch == 0x7fU || ch == '\\') {
            errno = EINVAL;
            return false;
        }
    }
    return true;
}

int dl_session_cgroup_openat(int root_fd, const char *relative_path)
{
    struct stat st;
    char *copy;
    char *component;
    int current_fd;

    if (root_fd < 0 || !valid_relative_path(relative_path)) {
        if (root_fd < 0) errno = EBADF;
        return -1;
    }
    if (fstat(root_fd, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    copy = strdup(relative_path);
    if (copy == NULL) return -1;
    current_fd = fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
    if (current_fd < 0) {
        free(copy);
        return -1;
    }

    component = copy;
    for (;;) {
        char *slash = strchr(component, '/');
        int next_fd;
        if (slash != NULL) *slash = '\0';
        next_fd = openat(current_fd, component,
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next_fd < 0) {
            int saved_errno = errno;
            (void) close(current_fd);
            free(copy);
            errno = saved_errno;
            return -1;
        }
        (void) close(current_fd);
        current_fd = next_fd;
        if (slash == NULL) break;
        component = slash + 1;
    }
    free(copy);
    return current_fd;
}

int dl_session_cgroup_contains_pid(
    int cgroup_fd,
    uint32_t pid,
    bool *out_contains)
{
    struct stat st;
    FILE *stream;
    char *line = NULL;
    size_t capacity = 0U;
    ssize_t length;
    int fd;
    int result = 0;

    if (cgroup_fd < 0 || pid == 0U || out_contains == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_contains = false;
    fd = openat(cgroup_fd, "cgroup.procs",
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        (void) close(fd);
        errno = EINVAL;
        return -1;
    }
    stream = fdopen(fd, "r");
    if (stream == NULL) {
        int saved_errno = errno;
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }

    while ((length = getline(&line, &capacity, stream)) >= 0) {
        uint64_t value = 0U;
        ssize_t index;
        if (length == 0) continue;
        for (index = 0; index < length; index++) {
            unsigned char ch = (unsigned char) line[index];
            if (ch == '\n' && index == length - 1) break;
            if (ch < '0' || ch > '9' ||
                value > (UINT32_MAX - (uint64_t)(ch - '0')) / 10U) {
                errno = EIO;
                result = -1;
                goto done;
            }
            value = value * 10U + (uint64_t)(ch - '0');
        }
        if (value == 0U) {
            errno = EIO;
            result = -1;
            goto done;
        }
        if (value == pid) {
            *out_contains = true;
            break;
        }
    }
    if (ferror(stream)) result = -1;

done:
    {
        int saved_errno = errno;
        free(line);
        (void) fclose(stream);
        errno = saved_errno;
    }
    return result;
}

static int pidfd_is_live(int pidfd)
{
#if defined(SYS_pidfd_send_signal)
    if (pidfd < 0) {
        errno = EBADF;
        return -1;
    }
    return (int) syscall(SYS_pidfd_send_signal, pidfd, 0, NULL, 0U);
#else
    (void) pidfd;
    errno = ENOTSUP;
    return -1;
#endif
}

static int pidfd_matches_pid(int pidfd, uint32_t expected_pid)
{
    char path[64];
    char *line = NULL;
    size_t capacity = 0U;
    ssize_t length;
    FILE *stream;
    int fd;
    int result = -1;
    bool found = false;

    if (snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", pidfd) < 0) {
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    stream = fdopen(fd, "r");
    if (stream == NULL) {
        int saved_errno = errno;
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        unsigned long value;
        char *end = NULL;
        char *cursor;
        if (length < 4 || memcmp(line, "Pid:", 4U) != 0) continue;
        found = true;
        cursor = line + 4;
        while (*cursor == ' ' || *cursor == '\t') cursor++;
        errno = 0;
        value = strtoul(cursor, &end, 10);
        if (errno != 0 || end == cursor ||
            (*end != '\n' && *end != '\0') || value != expected_pid) {
            errno = ESRCH;
            result = -1;
        } else {
            result = 0;
        }
        break;
    }
    if (!found) { result = -1; errno = ENOTSUP; }
    free(line);
    (void) fclose(stream);
    return result;
}

int dl_session_cgroup_kill_all(
    int cgroup_fd,
    uint32_t leader_pid,
    int leader_pidfd)
{
    bool contains = false;
    int fd;
    ssize_t written;

    if (cgroup_fd < 0 || leader_pid == 0U || leader_pidfd < 0) {
        errno = EINVAL;
        return -1;
    }
    if (pidfd_is_live(leader_pidfd) != 0) return -1;
    if (pidfd_matches_pid(leader_pidfd, leader_pid) != 0) return -1;
    if (dl_session_cgroup_contains_pid(cgroup_fd, leader_pid, &contains) != 0)
        return -1;
    if (!contains) {
        errno = ESRCH;
        return -1;
    }
    fd = openat(cgroup_fd, "cgroup.kill",
        O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT || errno == EROFS || errno == EACCES)
            errno = ENOTSUP;
        return -1;
    }
    do {
        written = write(fd, "1\n", 2U);
    } while (written < 0 && errno == EINTR);
    if (written != 2) {
        int saved_errno = written < 0 ? errno : EIO;
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }
    return close(fd);
}
