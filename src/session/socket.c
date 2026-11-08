#define _GNU_SOURCE
#include "session/socket.h"
#include "session/registration.h"

#include <errno.h>
#include <stdbool.h>
#include <poll.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct dl_session_socket {
    int fd;
    char *path;
    dev_t device;
    ino_t inode;
    dl_manager *manager;
    uid_t allowed_peer_uid;
};

static int handle_client(dl_session_socket *server, int client_fd)
{
    struct ucred credentials;
    struct pollfd readable;
    socklen_t credentials_size = sizeof(credentials);
    struct iovec vector;
    struct msghdr message;
    dl_session_registration_request request;
    dl_session_registration_response response;
    ssize_t received;
    uid_t peer_uid = (uid_t) -1;
    int ready;

    memset(&request, 0, sizeof(request));
    memset(&response, 0, sizeof(response));
    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED,
            &credentials, &credentials_size) == 0 &&
        credentials_size == sizeof(credentials)) {
        peer_uid = credentials.uid;
    }

    readable.fd = client_fd;
    readable.events = POLLIN;
    readable.revents = 0;
    do {
        ready = poll(&readable, 1U, 100);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || (readable.revents & POLLIN) == 0) {
        return 0;
    }

    memset(&message, 0, sizeof(message));
    vector.iov_base = &request;
    vector.iov_len = sizeof(request);
    message.msg_iov = &vector;
    message.msg_iovlen = 1U;
    received = recvmsg(client_fd, &message, MSG_TRUNC);
    if (received < 0) {
        return 0;
    }

    if ((size_t) received != sizeof(request) &&
        (size_t) received != DL_SESSION_REGISTRATION_V1_SIZE) {
        response.magic = DL_SESSION_REGISTRATION_MAGIC;
        response.version = DL_SESSION_REGISTRATION_VERSION;
        response.status = (int32_t) DL_ERROR_INVALID_ARGUMENT;
    } else if ((message.msg_flags & MSG_TRUNC) != 0) {
        response.magic = DL_SESSION_REGISTRATION_MAGIC;
        response.version = DL_SESSION_REGISTRATION_VERSION;
        response.status = (int32_t) DL_ERROR_INVALID_ARGUMENT;
    } else {
        (void) dl_session_registration_apply(
            server->manager, peer_uid, server->allowed_peer_uid,
            &request, (size_t) received, &response);
    }

    if (send(client_fd, &response, sizeof(response), MSG_NOSIGNAL) !=
        (ssize_t) sizeof(response)) {
        return 0;
    }
    return 1;
}

int dl_session_socket_open(
    dl_session_socket **out_socket,
    dl_manager *manager,
    const char *path,
    uid_t allowed_peer_uid)
{
    dl_session_socket *server;
    struct sockaddr_un address;
    struct stat existing;
    socklen_t address_length;
    bool bound = false;
    int fd;

    if (out_socket == NULL || manager == NULL || path == NULL || path[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    *out_socket = NULL;
    if (strlen(path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (lstat(path, &existing) == 0) {
        errno = EADDRINUSE;
        return -1;
    }
    if (errno != ENOENT) {
        return -1;
    }

    server = calloc(1U, sizeof(*server));
    if (server == NULL) {
        return -1;
    }
    server->fd = -1;
    server->path = strdup(path);
    if (server->path == NULL) {
        free(server);
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        free(server->path);
        free(server);
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void) memcpy(address.sun_path, path, strlen(path) + 1U);
    address_length = (socklen_t) (offsetof(struct sockaddr_un, sun_path) +
        strlen(path) + 1U);
    if (bind(fd, (struct sockaddr *) &address, address_length) != 0) {
        int saved_errno = errno;
        (void) close(fd);
        free(server->path);
        free(server);
        errno = saved_errno;
        return -1;
    }
    bound = true;
    if (chmod(path, 0600) != 0 || listen(fd, 16) != 0 ||
        lstat(path, &existing) != 0) {
        int saved_errno = errno;
        (void) close(fd);
        if (bound) {
            (void) unlink(path);
        }
        free(server->path);
        free(server);
        errno = saved_errno;
        return -1;
    }

    server->fd = fd;
    server->device = existing.st_dev;
    server->inode = existing.st_ino;
    server->manager = manager;
    server->allowed_peer_uid = allowed_peer_uid;
    *out_socket = server;
    return 0;
}

int dl_session_socket_get_fd(const dl_session_socket *session_socket)
{
    return session_socket == NULL ? -1 : session_socket->fd;
}

int dl_session_socket_dispatch(dl_session_socket *session_socket)
{
    int handled = 0;

    if (session_socket == NULL || session_socket->fd < 0) {
        errno = EINVAL;
        return -1;
    }
    while (handled < 8) {
        int client_fd = accept4(
            session_socket->fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return handled;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        {
            int result = handle_client(session_socket, client_fd);
            int saved_errno = errno;
            (void) close(client_fd);
            errno = saved_errno;
            if (result > 0) {
                handled++;
            }
        }
    }
    return handled;
}

void dl_session_socket_close(dl_session_socket *session_socket)
{
    if (session_socket == NULL) {
        return;
    }
    if (session_socket->fd >= 0) {
        (void) close(session_socket->fd);
    }
    if (session_socket->path != NULL) {
        struct stat current;
        if (lstat(session_socket->path, &current) == 0 &&
            current.st_dev == session_socket->device &&
            current.st_ino == session_socket->inode) {
            (void) unlink(session_socket->path);
        }
    }
    free(session_socket->path);
    free(session_socket);
}
