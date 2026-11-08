#include "session/registration.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void initialize_request(dl_session_registration_request *request)
{
    memset(request, 0, sizeof(*request));
    request->magic = DL_SESSION_REGISTRATION_MAGIC;
    request->version = DL_SESSION_REGISTRATION_VERSION;
}

static void test_open_and_close(void)
{
    dl_manager *manager = NULL;
    dl_session_registration_request request;
    dl_session_registration_response response;
    dl_session_properties properties;
    uid_t peer = getuid();

    assert(dl_manager_create(&manager) == DL_OK);
    assert(dl_manager_add_seat(manager, "seat0") == DL_OK);

    initialize_request(&request);
    request.operation = DL_SESSION_REGISTRATION_OPEN;
    request.uid = 1000U;
    request.leader_pid = 4242U;
    (void) strcpy(request.session_id, "c1");
    (void) strcpy(request.seat_id, "seat0");
    (void) strcpy(request.type, "x11");
    (void) strcpy(request.class_name, "user");
    (void) strcpy(request.service, "turnstile");
    (void) strcpy(request.desktop, "xfce");
    (void) strcpy(request.display, ":0");

    assert(dl_session_registration_apply(
        manager, peer, peer, &request, sizeof(request), &response) == DL_OK);
    assert(response.magic == DL_SESSION_REGISTRATION_MAGIC);
    assert(response.version == DL_SESSION_REGISTRATION_VERSION);
    assert(response.status == DL_OK);
    assert(dl_manager_get_session_properties(manager, "c1", &properties) == DL_OK);
    assert(properties.uid == 1000U);
    assert(properties.leader_pid == 4242U);
    assert(properties.seat_id != NULL && strcmp(properties.seat_id, "seat0") == 0);
    assert(properties.service != NULL &&
        strcmp(properties.service, "turnstile") == 0);

    initialize_request(&request);
    request.operation = DL_SESSION_REGISTRATION_CLOSE;
    (void) strcpy(request.session_id, "c1");
    assert(dl_session_registration_apply(
        manager, peer, peer, &request, sizeof(request), &response) == DL_OK);
    assert(response.status == DL_OK);
    assert(dl_manager_get_session_properties(manager, "c1", &properties) ==
        DL_ERROR_NOT_FOUND);

    dl_manager_destroy(manager);
}

static void test_remote_unseated_session_and_authentication(void)
{
    dl_manager *manager = NULL;
    dl_session_registration_request request;
    dl_session_registration_response response;
    dl_session_properties properties;
    uid_t peer = getuid();

    assert(dl_manager_create(&manager) == DL_OK);
    initialize_request(&request);
    request.operation = DL_SESSION_REGISTRATION_OPEN;
    request.uid = 1001U;
    request.leader_pid = 4343U;
    request.flags = DL_SESSION_REGISTRATION_FLAG_REMOTE;
    (void) strcpy(request.session_id, "ssh_1");
    (void) strcpy(request.type, "tty");
    (void) strcpy(request.class_name, "user");
    (void) strcpy(request.service, "sshd");
    (void) strcpy(request.tty, "pts/2");

    assert(dl_session_registration_apply(
        manager, peer, peer, &request, sizeof(request), &response) == DL_OK);
    assert(dl_manager_get_session_properties(manager, "ssh_1", &properties) == DL_OK);
    assert(properties.seat_id == NULL);
    assert(properties.remote);

    assert(dl_session_registration_apply(
        manager, (uid_t) (peer + 1U), peer, &request, sizeof(request), &response) ==
        DL_ERROR_INVALID_STATE);
    assert(response.status == DL_ERROR_INVALID_STATE);

    dl_manager_destroy(manager);
}

static void test_reject_malformed_messages(void)
{
    dl_manager *manager = NULL;
    dl_session_registration_request request;
    dl_session_registration_response response;
    uid_t peer = getuid();

    assert(dl_manager_create(&manager) == DL_OK);
    initialize_request(&request);
    request.operation = DL_SESSION_REGISTRATION_OPEN;
    (void) strcpy(request.session_id, "../bad");
    assert(dl_session_registration_apply(
        manager, peer, peer, &request, sizeof(request), &response) ==
        DL_ERROR_INVALID_ARGUMENT);
    assert(dl_session_registration_apply(
        manager, peer, peer, &request, sizeof(request) - 1U, &response) ==
        DL_ERROR_INVALID_ARGUMENT);

    initialize_request(&request);
    request.operation = DL_SESSION_REGISTRATION_OPEN;
    (void) strcpy(request.session_id, "zero_pid");
    assert(dl_session_registration_apply(
        manager, peer, peer, &request, sizeof(request), &response) ==
        DL_ERROR_INVALID_ARGUMENT);

    initialize_request(&request);
    request.operation = DL_SESSION_REGISTRATION_CLOSE;
    (void) strcpy(request.session_id, "valid");
    request.service[sizeof(request.service) - 1U] = 'x';
    assert(dl_session_registration_apply(
        manager, peer, peer, &request, sizeof(request), &response) ==
        DL_ERROR_INVALID_ARGUMENT);

    initialize_request(&request);
    request.operation = DL_SESSION_REGISTRATION_CLOSE;
    (void) strcpy(request.session_id, "valid");
    request.flags = DL_SESSION_REGISTRATION_FLAG_REMOTE;
    assert(dl_session_registration_apply(
        manager, peer, peer, &request, sizeof(request), &response) ==
        DL_ERROR_INVALID_ARGUMENT);

    dl_manager_destroy(manager);
}

int main(void)
{
    test_open_and_close();
    test_remote_unseated_session_and_authentication();
    test_reject_malformed_messages();
    puts("session registration tests passed");
    return 0;
}
