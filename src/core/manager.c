#include "core/manager.h"

#include <stdlib.h>
#include <string.h>

typedef struct dl_device {
    uint32_t major_number;
    uint32_t minor_number;
    uint64_t generation;
    struct dl_device *next;
} dl_device;

typedef struct dl_seat {
    char *id;
    char *active_session_id;
    dl_device *devices;
    struct dl_seat *next;
} dl_seat;

typedef struct dl_claim {
    uint32_t major_number;
    uint32_t minor_number;
    dl_device_pause_state pause_state;
    struct dl_claim *next;
} dl_claim;

typedef struct dl_session {
    char *id;
    char *seat_id;
    uint32_t uid;
    dl_claim *claims;
    struct dl_session *next;
} dl_session;

struct dl_manager {
    dl_seat *seats;
    dl_session *sessions;
    uint64_t next_generation;
};

static char *copy_string(const char *value)
{
    size_t length;
    char *copy;

    if (value == NULL || value[0] == '\0') {
        return NULL;
    }

    length = strlen(value);
    if (length == SIZE_MAX) {
        return NULL;
    }

    copy = malloc(length + 1U);
    if (copy != NULL) {
        memcpy(copy, value, length + 1U);
    }
    return copy;
}

static dl_seat *find_seat(const dl_manager *manager, const char *seat_id)
{
    dl_seat *seat;

    for (seat = manager->seats; seat != NULL; seat = seat->next) {
        if (strcmp(seat->id, seat_id) == 0) {
            return seat;
        }
    }
    return NULL;
}

static dl_session *find_session(
    const dl_manager *manager,
    const char *session_id)
{
    dl_session *session;

    for (session = manager->sessions; session != NULL; session = session->next) {
        if (strcmp(session->id, session_id) == 0) {
            return session;
        }
    }
    return NULL;
}

static dl_device *find_device(
    const dl_seat *seat,
    uint32_t major_number,
    uint32_t minor_number)
{
    dl_device *device;

    for (device = seat->devices; device != NULL; device = device->next) {
        if (device->major_number == major_number &&
            device->minor_number == minor_number) {
            return device;
        }
    }
    return NULL;
}

static bool device_exists_anywhere(
    const dl_manager *manager,
    uint32_t major_number,
    uint32_t minor_number)
{
    const dl_seat *seat;

    for (seat = manager->seats; seat != NULL; seat = seat->next) {
        if (find_device(seat, major_number, minor_number) != NULL) {
            return true;
        }
    }
    return false;
}

static bool session_has_claim(
    const dl_session *session,
    uint32_t major_number,
    uint32_t minor_number)
{
    const dl_claim *claim;

    for (claim = session->claims; claim != NULL; claim = claim->next) {
        if (claim->major_number == major_number &&
            claim->minor_number == minor_number) {
            return true;
        }
    }
    return false;
}

static void free_claims(dl_claim *claim)
{
    while (claim != NULL) {
        dl_claim *next = claim->next;
        free(claim);
        claim = next;
    }
}

static void remove_claims_for_device(
    dl_manager *manager,
    uint32_t major_number,
    uint32_t minor_number)
{
    dl_session *session;

    for (session = manager->sessions; session != NULL; session = session->next) {
        dl_claim **cursor = &session->claims;

        while (*cursor != NULL) {
            dl_claim *claim = *cursor;
            if (claim->major_number == major_number &&
                claim->minor_number == minor_number) {
                *cursor = claim->next;
                free(claim);
            } else {
                cursor = &claim->next;
            }
        }
    }
}

dl_error dl_manager_create(dl_manager **out_manager)
{
    dl_manager *manager;

    if (out_manager == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    *out_manager = NULL;

    manager = calloc(1U, sizeof(*manager));
    if (manager == NULL) {
        return DL_ERROR_NO_MEMORY;
    }
    manager->next_generation = 1U;
    *out_manager = manager;
    return DL_OK;
}

void dl_manager_destroy(dl_manager *manager)
{
    dl_seat *seat;
    dl_session *session;

    if (manager == NULL) {
        return;
    }

    seat = manager->seats;
    while (seat != NULL) {
        dl_seat *next = seat->next;
        dl_device *device = seat->devices;

        while (device != NULL) {
            dl_device *device_next = device->next;
            free(device);
            device = device_next;
        }
        free(seat->id);
        free(seat->active_session_id);
        free(seat);
        seat = next;
    }

    session = manager->sessions;
    while (session != NULL) {
        dl_session *next = session->next;
        free_claims(session->claims);
        free(session->id);
        free(session->seat_id);
        free(session);
        session = next;
    }

    free(manager);
}

dl_error dl_manager_add_seat(dl_manager *manager, const char *seat_id)
{
    dl_seat *seat;

    if (manager == NULL || seat_id == NULL || seat_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    if (find_seat(manager, seat_id) != NULL) {
        return DL_ERROR_ALREADY_EXISTS;
    }

    seat = calloc(1U, sizeof(*seat));
    if (seat == NULL) {
        return DL_ERROR_NO_MEMORY;
    }
    seat->id = copy_string(seat_id);
    if (seat->id == NULL) {
        free(seat);
        return DL_ERROR_NO_MEMORY;
    }

    seat->next = manager->seats;
    manager->seats = seat;
    return DL_OK;
}

dl_error dl_manager_remove_seat(dl_manager *manager, const char *seat_id)
{
    dl_seat **cursor;

    if (manager == NULL || seat_id == NULL || seat_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }

    cursor = &manager->seats;
    while (*cursor != NULL) {
        dl_seat *seat = *cursor;
        if (strcmp(seat->id, seat_id) == 0) {
            dl_session *session;
            if (seat->devices != NULL) {
                return DL_ERROR_BUSY;
            }
            for (session = manager->sessions; session != NULL; session = session->next) {
                if (strcmp(session->seat_id, seat_id) == 0) {
                    return DL_ERROR_BUSY;
                }
            }
            *cursor = seat->next;
            free(seat->active_session_id);
            free(seat->id);
            free(seat);
            return DL_OK;
        }
        cursor = &(*cursor)->next;
    }
    return DL_ERROR_NOT_FOUND;
}

dl_error dl_manager_add_device(
    dl_manager *manager,
    const char *seat_id,
    uint32_t major_number,
    uint32_t minor_number,
    uint64_t *out_generation)
{
    dl_seat *seat;
    dl_device *device;

    if (manager == NULL || seat_id == NULL || seat_id[0] == '\0' ||
        out_generation == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    *out_generation = 0U;

    seat = find_seat(manager, seat_id);
    if (seat == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (device_exists_anywhere(manager, major_number, minor_number)) {
        return DL_ERROR_ALREADY_EXISTS;
    }
    if (manager->next_generation == 0U) {
        return DL_ERROR_LIMIT;
    }

    device = calloc(1U, sizeof(*device));
    if (device == NULL) {
        return DL_ERROR_NO_MEMORY;
    }
    device->major_number = major_number;
    device->minor_number = minor_number;
    device->generation = manager->next_generation;
    manager->next_generation++;
    device->next = seat->devices;
    seat->devices = device;
    *out_generation = device->generation;
    return DL_OK;
}

dl_error dl_manager_remove_device(
    dl_manager *manager,
    const char *seat_id,
    uint32_t major_number,
    uint32_t minor_number)
{
    dl_seat *seat;
    dl_device **cursor;

    if (manager == NULL || seat_id == NULL || seat_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    seat = find_seat(manager, seat_id);
    if (seat == NULL) {
        return DL_ERROR_NOT_FOUND;
    }

    cursor = &seat->devices;
    while (*cursor != NULL) {
        dl_device *device = *cursor;
        if (device->major_number == major_number &&
            device->minor_number == minor_number) {
            *cursor = device->next;
            remove_claims_for_device(manager, major_number, minor_number);
            free(device);
            return DL_OK;
        }
        cursor = &device->next;
    }
    return DL_ERROR_NOT_FOUND;
}

dl_error dl_manager_add_session(
    dl_manager *manager,
    const char *session_id,
    uint32_t uid,
    const char *seat_id)
{
    dl_session *session;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0' ||
        seat_id == NULL || seat_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    if (find_session(manager, session_id) != NULL) {
        return DL_ERROR_ALREADY_EXISTS;
    }
    if (find_seat(manager, seat_id) == NULL) {
        return DL_ERROR_NOT_FOUND;
    }

    session = calloc(1U, sizeof(*session));
    if (session == NULL) {
        return DL_ERROR_NO_MEMORY;
    }
    session->id = copy_string(session_id);
    session->seat_id = copy_string(seat_id);
    if (session->id == NULL || session->seat_id == NULL) {
        free(session->id);
        free(session->seat_id);
        free(session);
        return DL_ERROR_NO_MEMORY;
    }
    session->uid = uid;
    session->next = manager->sessions;
    manager->sessions = session;
    return DL_OK;
}

dl_error dl_manager_remove_session(dl_manager *manager, const char *session_id)
{
    dl_session **cursor;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }

    cursor = &manager->sessions;
    while (*cursor != NULL) {
        dl_session *session = *cursor;
        if (strcmp(session->id, session_id) == 0) {
            dl_seat *seat = find_seat(manager, session->seat_id);
            if (seat != NULL && seat->active_session_id != NULL &&
                strcmp(seat->active_session_id, session_id) == 0) {
                free(seat->active_session_id);
                seat->active_session_id = NULL;
            }
            *cursor = session->next;
            free_claims(session->claims);
            free(session->id);
            free(session->seat_id);
            free(session);
            return DL_OK;
        }
        cursor = &(*cursor)->next;
    }
    return DL_ERROR_NOT_FOUND;
}

dl_error dl_manager_set_active_session(
    dl_manager *manager,
    const char *seat_id,
    const char *session_id)
{
    dl_seat *seat;
    dl_session *session;
    char *replacement = NULL;

    if (manager == NULL || seat_id == NULL || seat_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    seat = find_seat(manager, seat_id);
    if (seat == NULL) {
        return DL_ERROR_NOT_FOUND;
    }

    if (session_id != NULL) {
        if (session_id[0] == '\0') {
            return DL_ERROR_INVALID_ARGUMENT;
        }
        session = find_session(manager, session_id);
        if (session == NULL || strcmp(session->seat_id, seat_id) != 0) {
            return DL_ERROR_INVALID_STATE;
        }
        replacement = copy_string(session_id);
        if (replacement == NULL) {
            return DL_ERROR_NO_MEMORY;
        }
    }

    free(seat->active_session_id);
    seat->active_session_id = replacement;
    return DL_OK;
}

bool dl_manager_session_is_active(
    const dl_manager *manager,
    const char *session_id)
{
    const dl_session *session;
    const dl_seat *seat;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0') {
        return false;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return false;
    }
    seat = find_seat(manager, session->seat_id);
    return seat != NULL && seat->active_session_id != NULL &&
        strcmp(seat->active_session_id, session_id) == 0;
}

dl_error dl_manager_claim_device(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number,
    bool *out_session_inactive)
{
    dl_session *session;
    dl_seat *seat;
    dl_device *device;
    dl_session *other;
    dl_claim *claim;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0' ||
        out_session_inactive == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    *out_session_inactive = false;

    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    seat = find_seat(manager, session->seat_id);
    if (seat == NULL) {
        return DL_ERROR_INVALID_STATE;
    }
    device = find_device(seat, major_number, minor_number);
    if (device == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (session_has_claim(session, major_number, minor_number)) {
        return DL_ERROR_ALREADY_EXISTS;
    }

    for (other = manager->sessions; other != NULL; other = other->next) {
        if (other != session &&
            session_has_claim(other, major_number, minor_number)) {
            return DL_ERROR_BUSY;
        }
    }

    claim = malloc(sizeof(*claim));
    if (claim == NULL) {
        return DL_ERROR_NO_MEMORY;
    }
    claim->major_number = major_number;
    claim->minor_number = minor_number;
    claim->pause_state = DL_DEVICE_ACTIVE;
    claim->next = session->claims;
    session->claims = claim;

    *out_session_inactive = seat->active_session_id == NULL ||
        strcmp(seat->active_session_id, session_id) != 0;
    return DL_OK;
}

dl_error dl_manager_release_device(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number)
{
    dl_session *session;
    dl_claim **cursor;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }

    cursor = &session->claims;
    while (*cursor != NULL) {
        dl_claim *claim = *cursor;
        if (claim->major_number == major_number &&
            claim->minor_number == minor_number) {
            *cursor = claim->next;
            free(claim);
            return DL_OK;
        }
        cursor = &claim->next;
    }
    return DL_ERROR_NOT_FOUND;
}

 
static dl_claim *find_claim(
    dl_session *session,
    uint32_t major_number,
    uint32_t minor_number)
{
    dl_claim *claim;

    for (claim = session->claims; claim != NULL; claim = claim->next) {
        if (claim->major_number == major_number &&
            claim->minor_number == minor_number) {
            return claim;
        }
    }
    return NULL;
}

dl_error dl_manager_pause_device(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number,
    bool force)
{
    dl_session *session;
    dl_claim *claim;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    claim = find_claim(session, major_number, minor_number);
    if (claim == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (claim->pause_state != DL_DEVICE_ACTIVE &&
        !(force && claim->pause_state == DL_DEVICE_PAUSE_PENDING)) {
        return DL_ERROR_INVALID_STATE;
    }

    claim->pause_state = force ? DL_DEVICE_PAUSED : DL_DEVICE_PAUSE_PENDING;
    return DL_OK;
}

dl_error dl_manager_complete_device_pause(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number)
{
    dl_session *session;
    dl_claim *claim;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    claim = find_claim(session, major_number, minor_number);
    if (claim == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (claim->pause_state != DL_DEVICE_PAUSE_PENDING) {
        return DL_ERROR_INVALID_STATE;
    }

    claim->pause_state = DL_DEVICE_PAUSED;
    return DL_OK;
}

dl_error dl_manager_resume_device(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number)
{
    dl_session *session;
    dl_claim *claim;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    claim = find_claim(session, major_number, minor_number);
    if (claim == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (!dl_manager_session_is_active(manager, session_id)) {
        return DL_ERROR_INVALID_STATE;
    }
    if (claim->pause_state != DL_DEVICE_PAUSED) {
        return DL_ERROR_INVALID_STATE;
    }

    claim->pause_state = DL_DEVICE_ACTIVE;
    return DL_OK;
}

dl_device_pause_state dl_manager_get_device_pause_state(
    const dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number)
{
    dl_session *session;
    dl_claim *claim;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0') {
        return DL_DEVICE_PAUSED;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_DEVICE_PAUSED;
    }
    claim = find_claim(session, major_number, minor_number);
    return claim == NULL ? DL_DEVICE_PAUSED : claim->pause_state;
}

uint64_t dl_manager_device_generation(
    const dl_manager *manager,
    const char *seat_id,
    uint32_t major_number,
    uint32_t minor_number)
{
    const dl_seat *seat;
    const dl_device *device;

    if (manager == NULL || seat_id == NULL || seat_id[0] == '\0') {
        return 0U;
    }
    seat = find_seat(manager, seat_id);
    if (seat == NULL) {
        return 0U;
    }
    device = find_device(seat, major_number, minor_number);
    return device == NULL ? 0U : device->generation;
}

const char *dl_error_string(dl_error error)
{
    switch (error) {
    case DL_OK:
        return "success";
    case DL_ERROR_INVALID_ARGUMENT:
        return "invalid argument";
    case DL_ERROR_NO_MEMORY:
        return "out of memory";
    case DL_ERROR_NOT_FOUND:
        return "not found";
    case DL_ERROR_ALREADY_EXISTS:
        return "already exists";
    case DL_ERROR_INVALID_STATE:
        return "invalid state";
    case DL_ERROR_BUSY:
        return "resource busy";
    case DL_ERROR_LIMIT:
        return "generation limit reached";
    }
    return "unknown error";
}
