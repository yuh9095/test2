#ifndef DINIT_LOGIND_SESSION_REGISTRATION_H
#define DINIT_LOGIND_SESSION_REGISTRATION_H

#include "core/manager.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define DL_SESSION_REGISTRATION_MAGIC UINT32_C(0x444c5347)
#define DL_SESSION_REGISTRATION_VERSION 1U

typedef enum dl_session_registration_operation {
    DL_SESSION_REGISTRATION_OPEN = 1U,
    DL_SESSION_REGISTRATION_CLOSE = 2U
} dl_session_registration_operation;

/* Local Unix-socket wire message. Clients must zero-initialize this structure.
 * The transport must preserve message boundaries (SOCK_SEQPACKET). */
typedef struct dl_session_registration_request {
    uint32_t magic;
    uint16_t version;
    uint16_t operation;
    uint32_t uid;
    uint32_t leader_pid;
    uint32_t flags;
    uint32_t reserved;
    char session_id[65];
    char seat_id[65];
    char type[32];
    char class_name[32];
    char service[65];
    char desktop[65];
    char tty[65];
    char display[65];
} dl_session_registration_request;

typedef struct dl_session_registration_response {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int32_t status;
} dl_session_registration_response;

#define DL_SESSION_REGISTRATION_FLAG_REMOTE (1U << 0)

/* Apply one complete request after authenticating the peer credentials.
 * Production callers should set allowed_peer_uid to 0 (root). */
dl_error dl_session_registration_apply(
    dl_manager *manager,
    uid_t peer_uid,
    uid_t allowed_peer_uid,
    const dl_session_registration_request *request,
    size_t request_size,
    dl_session_registration_response *response);

#endif
