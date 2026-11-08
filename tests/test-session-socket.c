#define _GNU_SOURCE
#include "session/socket.h"
#include "session/registration.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(void)
{
    dl_manager *manager = NULL;
    dl_session_socket *server = NULL;
    dl_session_registration_request request;
    dl_session_registration_response response;
    struct sockaddr_un address;
    char directory[] = "/tmp/dinit-logind-test-XXXXXX";
    char path[sizeof(address.sun_path)];
    int client_fd;

    assert(mkdtemp(directory) != NULL);
    assert(snprintf(path, sizeof(path), "%s/session.sock", directory) > 0);
    assert(dl_manager_create(&manager) == DL_OK);
    assert(dl_session_socket_open(&server, manager, path, getuid()) == 0);
    assert(dl_session_socket_get_fd(server) >= 0);

    client_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    assert(client_fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    (void) strcpy(address.sun_path, path);
    assert(connect(client_fd, (struct sockaddr *) &address, sizeof(address)) == 0);

    memset(&request, 0, sizeof(request));
    request.magic = DL_SESSION_REGISTRATION_MAGIC;
    request.version = DL_SESSION_REGISTRATION_VERSION;
    request.operation = DL_SESSION_REGISTRATION_OPEN;
    request.uid = 1000U;
    (void) strcpy(request.session_id, "socket_test");
    (void) strcpy(request.type, "tty");
    request.flags = DL_SESSION_REGISTRATION_FLAG_REMOTE;
    assert(send(client_fd, &request, sizeof(request), 0) ==
        (ssize_t) sizeof(request));
    assert(dl_session_socket_dispatch(server) == 1);
    assert(recv(client_fd, &response, sizeof(response), 0) ==
        (ssize_t) sizeof(response));
    assert(response.magic == DL_SESSION_REGISTRATION_MAGIC);
    assert(response.version == DL_SESSION_REGISTRATION_VERSION);
    assert(response.status == DL_OK);

    (void) close(client_fd);
    dl_session_socket_close(server);
    assert(access(path, F_OK) != 0);
    assert(rmdir(directory) == 0);
    dl_manager_destroy(manager);
    puts("session socket tests passed");
    return 0;
}
