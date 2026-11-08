#define _GNU_SOURCE
#include "session/socket.h"
#include "session/registration.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int connect_socket(const char *path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void) strcpy(address.sun_path, path);
    assert(connect(fd, (struct sockaddr *) &address, sizeof(address)) == 0);
    return fd;
}

static void send_request(
    dl_session_socket *server,
    int client_fd,
    const dl_session_registration_request *request,
    dl_session_registration_response *response)
{
    assert(send(client_fd, request, sizeof(*request), 0) ==
        (ssize_t) sizeof(*request));
    assert(dl_session_socket_dispatch(server) == 1);
    assert(recv(client_fd, response, sizeof(*response), 0) ==
        (ssize_t) sizeof(*response));
    assert(response->magic == DL_SESSION_REGISTRATION_MAGIC);
    assert(response->version == DL_SESSION_REGISTRATION_VERSION);
}

int main(void)
{
    dl_manager *manager = NULL;
    dl_session_socket *server = NULL;
    dl_session_socket *denied_server = NULL;
    dl_session_registration_request request;
    dl_session_registration_response response;
    char directory[] = "/tmp/dinit-logind-test-XXXXXX";
    char path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
    char denied_path[sizeof(((struct sockaddr_un *) 0)->sun_path)];
    struct stat socket_stat;
    int client_fd;

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/session.sock", directory) > 0);
    assert(snprintf(denied_path, sizeof(denied_path),
        "%s/denied.sock", directory) > 0);
    assert(dl_manager_create(&manager) == DL_OK);
    assert(dl_manager_add_seat(manager, "seat0") == DL_OK);
    assert(dl_session_socket_open(&server, manager, path, getuid()) == 0);
    assert(dl_session_socket_get_fd(server) >= 0);
    assert(stat(path, &socket_stat) == 0);
    assert((socket_stat.st_mode & 0777) == 0600);

    client_fd = connect_socket(path);
    memset(&request, 0, sizeof(request));
    request.magic = DL_SESSION_REGISTRATION_MAGIC;
    request.version = DL_SESSION_REGISTRATION_VERSION;
    request.operation = DL_SESSION_REGISTRATION_OPEN;
    request.uid = (uint32_t) getuid();
    request.leader_pid = (uint32_t) getpid();
    (void) strcpy(request.session_id, "socket_test");
    (void) strcpy(request.seat_id, "seat0");
    (void) strcpy(request.type, "tty");
    request.flags = DL_SESSION_REGISTRATION_FLAG_REMOTE;
    send_request(server, client_fd, &request, &response);
    assert(response.status == DL_OK);
    assert(dl_manager_get_session_count(manager) == 1U);
    (void) close(client_fd);

    /* The server intentionally handles one request per connection. */
    client_fd = connect_socket(path);
    memset(&request, 0, sizeof(request));
    request.magic = DL_SESSION_REGISTRATION_MAGIC;
    request.version = DL_SESSION_REGISTRATION_VERSION;
    request.operation = DL_SESSION_REGISTRATION_CLOSE;
    (void) strcpy(request.session_id, "socket_test");
    send_request(server, client_fd, &request, &response);
    assert(response.status == DL_OK);
    assert(dl_manager_get_session_count(manager) == 0U);
    (void) close(client_fd);

    assert(dl_session_socket_open(
        &denied_server, manager, denied_path, (uid_t) (getuid() + 1U)) == 0);
    client_fd = connect_socket(denied_path);
    memset(&request, 0, sizeof(request));
    request.magic = DL_SESSION_REGISTRATION_MAGIC;
    request.version = DL_SESSION_REGISTRATION_VERSION;
    request.operation = DL_SESSION_REGISTRATION_OPEN;
    request.uid = (uint32_t) getuid();
    (void) strcpy(request.session_id, "must_be_denied");
    send_request(denied_server, client_fd, &request, &response);
    assert(response.status == DL_ERROR_INVALID_STATE);
    assert(dl_manager_get_session_count(manager) == 0U);
    (void) close(client_fd);

    dl_session_socket_close(denied_server);
    dl_session_socket_close(server);
    assert(access(path, F_OK) != 0 && errno == ENOENT);
    assert(access(denied_path, F_OK) != 0 && errno == ENOENT);
    assert(rmdir(directory) == 0);
    dl_manager_destroy(manager);
    puts("session socket tests passed");
    return 0;
}
