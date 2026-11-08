#include "session/registration.h"

#include <stdbool.h>
#include <string.h>

static bool bounded_string(const char *value, size_t capacity, bool required)
{
    size_t index;

    for (index = 0U; index < capacity; index++) {
        unsigned char ch = (unsigned char) value[index];
        if (ch == '\0') {
            return !required || index != 0U;
        }
        if (ch < 0x20U || ch == 0x7fU) {
            return false;
        }
    }
    return false;
}

static bool valid_session_id(const char *value)
{
    size_t index;

    if (!bounded_string(value,
            sizeof(((dl_session_registration_request *) 0)->session_id), true)) {
        return false;
    }
    for (index = 0U; value[index] != '\0'; index++) {
        unsigned char ch = (unsigned char) value[index];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static bool bytes_are_zero(const char *value, size_t size)
{
    size_t index;
    for (index = 0U; index < size; index++) {
        if (value[index] != '\\0') return false;
    }
    return true;
}

static const char *optional_value(const char *value)
{
    return value[0] == '\0' ? NULL : value;
}

static bool validate_open_request(const dl_session_registration_request *request)
{
    return request->leader_pid != 0U &&
        valid_session_id(request->session_id) &&
        bounded_string(request->seat_id, sizeof(request->seat_id), false) &&
        bounded_string(request->type, sizeof(request->type), false) &&
        bounded_string(request->class_name, sizeof(request->class_name), false) &&
        bounded_string(request->service, sizeof(request->service), false) &&
        bounded_string(request->desktop, sizeof(request->desktop), false) &&
        bounded_string(request->tty, sizeof(request->tty), false) &&
        bounded_string(request->display, sizeof(request->display), false) &&
        (request->flags & ~DL_SESSION_REGISTRATION_FLAG_REMOTE) == 0U &&
        request->reserved == 0U;
}

dl_error dl_session_registration_apply(
    dl_manager *manager,
    uid_t peer_uid,
    uid_t allowed_peer_uid,
    const dl_session_registration_request *request,
    size_t request_size,
    dl_session_registration_response *response)
{
    dl_error result = DL_ERROR_INVALID_ARGUMENT;

    if (response == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    response->magic = DL_SESSION_REGISTRATION_MAGIC;
    response->version = DL_SESSION_REGISTRATION_VERSION;
    response->reserved = 0U;
    response->status = (int32_t) DL_ERROR_INVALID_ARGUMENT;

    if (manager == NULL || request == NULL ||
        request_size != sizeof(*request)) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    if (peer_uid != allowed_peer_uid) {
        result = DL_ERROR_INVALID_STATE;
        goto done;
    }
    if (request->magic != DL_SESSION_REGISTRATION_MAGIC ||
        request->version != DL_SESSION_REGISTRATION_VERSION) {
        result = DL_ERROR_INVALID_ARGUMENT;
        goto done;
    }

    if (request->operation == DL_SESSION_REGISTRATION_OPEN) {
        dl_session_properties properties = {0};

        if (!validate_open_request(request)) {
            result = DL_ERROR_INVALID_ARGUMENT;
            goto done;
        }
        properties.seat_id = optional_value(request->seat_id);
        properties.type = optional_value(request->type);
        properties.class_name = optional_value(request->class_name);
        properties.service = optional_value(request->service);
        properties.desktop = optional_value(request->desktop);
        properties.tty = optional_value(request->tty);
        properties.display = optional_value(request->display);
        properties.leader_pid = request->leader_pid;
        properties.remote =
            (request->flags & DL_SESSION_REGISTRATION_FLAG_REMOTE) != 0U;
        result = dl_manager_add_session_with_properties(
            manager, request->session_id, request->uid, &properties);
    } else if (request->operation == DL_SESSION_REGISTRATION_CLOSE) {
        if (!valid_session_id(request->session_id) ||
            request->reserved != 0U || request->flags != 0U ||
            request->uid != 0U || request->leader_pid != 0U ||
            request->seat_id[0] != '\0' || request->type[0] != '\0' ||
            request->class_name[0] != '\0' || request->service[0] != '\0' ||
            request->desktop[0] != '\0' || request->tty[0] != '\0' ||
            request->display[0] != '\0') {
            result = DL_ERROR_INVALID_ARGUMENT;
            goto done;
        }
        result = dl_manager_remove_session(manager, request->session_id);
    } else {
        result = DL_ERROR_INVALID_ARGUMENT;
    }

done:
    response->status = (int32_t) result;
    return result;
}
