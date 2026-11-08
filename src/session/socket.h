#ifndef DINIT_LOGIND_SESSION_SOCKET_H
#define DINIT_LOGIND_SESSION_SOCKET_H

#include "core/manager.h"

#include <sys/types.h>

typedef struct dl_session_socket dl_session_socket;

/* Create a private local SOCK_SEQPACKET endpoint. The caller supplies the
 * UID authorized to register sessions (normally root). */
int dl_session_socket_open(
    dl_session_socket **out_socket,
    dl_manager *manager,
    const char *path,
    uid_t allowed_peer_uid);
int dl_session_socket_get_fd(const dl_session_socket *session_socket);
/* Accept and process up to 32 one-message clients; returns handled count. */
int dl_session_socket_dispatch(dl_session_socket *session_socket);
void dl_session_socket_close(dl_session_socket *session_socket);

#endif
