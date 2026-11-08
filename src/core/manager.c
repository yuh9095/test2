#define _POSIX_C_SOURCE 200809L
#include "core/manager.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

typedef struct dl_device {
    uint32_t major_number;
    uint32_t minor_number;
    uint64_t generation;
    char *devnode;
    bool master_of_seat;
    unsigned int capabilities;
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
    int manager_fd;
    uint64_t pause_deadline_ms;
    struct dl_claim *next;
} dl_claim;

typedef struct dl_session {
    char *id;
    char *seat_id;
    uint32_t uid;
    uint32_t leader_pid;
    bool remote;
    char *type;
    char *class_name;
    char *service;
    char *desktop;
    char *tty;
    char *display;
    char *controller_id;
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

static dl_seat *find_session_seat(
    const dl_manager *manager,
    const dl_session *session)
{
    if (session == NULL || session->seat_id == NULL) {
        return NULL;
    }
    return find_seat(manager, session->seat_id);
}

static char *copy_optional_string(const char *value)
{
    return value == NULL ? NULL : copy_string(value);
}

static dl_session *find_session_by_leader_pid(
    const dl_manager *manager,
    uint32_t leader_pid)
{
    dl_session *session;

    if (leader_pid == 0U) {
        return NULL;
    }
    for (session = manager->sessions; session != NULL; session = session->next) {
        if (session->leader_pid == leader_pid) {
            return session;
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

    if (seat == NULL) {
        return NULL;
    }
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
        if (claim->manager_fd >= 0) {
            (void) close(claim->manager_fd);
        }
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
                if (claim->manager_fd >= 0) {
                    (void) close(claim->manager_fd);
                }
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
            free(device->devnode);
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
        free(session->type);
        free(session->class_name);
        free(session->service);
        free(session->desktop);
        free(session->tty);
        free(session->display);
        free(session->controller_id);
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
                if (session->seat_id != NULL &&
                    strcmp(session->seat_id, seat_id) == 0) {
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

size_t dl_manager_get_seat_count(const dl_manager *manager)
{
    const dl_seat *seat;
    size_t count = 0U;

    if (manager == NULL) {
        return 0U;
    }
    for (seat = manager->seats; seat != NULL; seat = seat->next) {
        if (count == SIZE_MAX) {
            return SIZE_MAX;
        }
        count++;
    }
    return count;
}

dl_error dl_manager_get_seat_at(
    const dl_manager *manager,
    size_t index,
    const char **out_seat_id)
{
    const dl_seat *seat;
    size_t current = 0U;

    if (manager == NULL || out_seat_id == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    *out_seat_id = NULL;
    for (seat = manager->seats; seat != NULL; seat = seat->next) {
        if (current == index) {
            *out_seat_id = seat->id;
            return DL_OK;
        }
        current++;
    }
    return DL_ERROR_NOT_FOUND;
}

dl_error dl_manager_get_active_session(
    const dl_manager *manager,
    const char *seat_id,
    const char **out_session_id)
{
    const dl_seat *seat;

    if (manager == NULL || seat_id == NULL || out_session_id == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    *out_session_id = NULL;
    seat = find_seat(manager, seat_id);
    if (seat == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (seat->active_session_id == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    *out_session_id = seat->active_session_id;
    return DL_OK;
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
            free(device->devnode);
            free(device);
            return DL_OK;
        }
        cursor = &device->next;
    }
    return DL_ERROR_NOT_FOUND;
}

dl_error dl_manager_add_session_with_properties(
    dl_manager *manager,
    const char *session_id,
    uint32_t uid,
    const dl_session_properties *properties)
{
    dl_session *session;
    const char *seat_id;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0' ||
        properties == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    seat_id = properties->seat_id;
    if (seat_id != NULL && seat_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    if (find_session(manager, session_id) != NULL ||
        (properties->leader_pid != 0U &&
         find_session_by_leader_pid(manager, properties->leader_pid) != NULL)) {
        return DL_ERROR_ALREADY_EXISTS;
    }
    if (seat_id != NULL && find_seat(manager, seat_id) == NULL) {
        return DL_ERROR_NOT_FOUND;
    }

    session = calloc(1U, sizeof(*session));
    if (session == NULL) {
        return DL_ERROR_NO_MEMORY;
    }
    session->id = copy_string(session_id);
    session->seat_id = copy_optional_string(seat_id);
    session->type = copy_optional_string(properties->type);
    session->class_name = copy_optional_string(properties->class_name);
    session->service = copy_optional_string(properties->service);
    session->desktop = copy_optional_string(properties->desktop);
    session->tty = copy_optional_string(properties->tty);
    session->display = copy_optional_string(properties->display);
    if (session->id == NULL ||
        (seat_id != NULL && session->seat_id == NULL) ||
        (properties->type != NULL && session->type == NULL) ||
        (properties->class_name != NULL && session->class_name == NULL) ||
        (properties->service != NULL && session->service == NULL) ||
        (properties->desktop != NULL && session->desktop == NULL) ||
        (properties->tty != NULL && session->tty == NULL) ||
        (properties->display != NULL && session->display == NULL)) {
        free(session->id);
        free(session->seat_id);
        free(session->type);
        free(session->class_name);
        free(session->service);
        free(session->desktop);
        free(session->tty);
        free(session->display);
        free(session);
        return DL_ERROR_NO_MEMORY;
    }

    session->uid = uid;
    session->leader_pid = properties->leader_pid;
    session->remote = properties->remote;
    session->next = manager->sessions;
    manager->sessions = session;
    return DL_OK;
}

dl_error dl_manager_add_session(
    dl_manager *manager,
    const char *session_id,
    uint32_t uid,
    const char *seat_id)
{
    dl_session_properties properties = {0};

    properties.seat_id = seat_id;
    properties.type = "unspecified";
    properties.class_name = "user";
    return dl_manager_add_session_with_properties(
        manager, session_id, uid, &properties);
}

dl_error dl_manager_get_session_properties(
    const dl_manager *manager,
    const char *session_id,
    dl_session_properties *out_properties)
{
    const dl_session *session;

    if (manager == NULL || session_id == NULL || session_id[0] == '\0' ||
        out_properties == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }

    out_properties->session_id = session->id;
    out_properties->uid = session->uid;
    out_properties->seat_id = session->seat_id;
    out_properties->type = session->type;
    out_properties->class_name = session->class_name;
    out_properties->service = session->service;
    out_properties->desktop = session->desktop;
    out_properties->tty = session->tty;
    out_properties->display = session->display;
    out_properties->leader_pid = session->leader_pid;
    out_properties->remote = session->remote;
    return DL_OK;
}

dl_error dl_manager_find_session_by_leader_pid(
    const dl_manager *manager,
    uint32_t leader_pid,
    dl_session_properties *out_properties)
{
    const dl_session *session;

    if (manager == NULL || leader_pid == 0U || out_properties == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    session = find_session_by_leader_pid(manager, leader_pid);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    return dl_manager_get_session_properties(
        manager, session->id, out_properties);
}

size_t dl_manager_get_session_count(const dl_manager *manager)
{
    const dl_session *session;
    size_t count = 0U;

    if (manager == NULL) {
        return 0U;
    }
    for (session = manager->sessions; session != NULL; session = session->next) {
        if (count == SIZE_MAX) {
            return SIZE_MAX;
        }
        count++;
    }
    return count;
}

dl_error dl_manager_get_session_at(
    const dl_manager *manager,
    size_t index,
    dl_session_properties *out_properties)
{
    const dl_session *session;
    size_t current = 0U;

    if (manager == NULL || out_properties == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    for (session = manager->sessions; session != NULL; session = session->next) {
        if (current == index) {
            return dl_manager_get_session_properties(
                manager, session->id, out_properties);
        }
        current++;
    }
    return DL_ERROR_NOT_FOUND;
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
            dl_seat *seat = find_session_seat(manager, session);
            if (seat != NULL && seat->active_session_id != NULL &&
                strcmp(seat->active_session_id, session_id) == 0) {
                free(seat->active_session_id);
                seat->active_session_id = NULL;
            }
            *cursor = session->next;
            free_claims(session->claims);
            free(session->id);
            free(session->seat_id);
            free(session->type);
            free(session->class_name);
            free(session->service);
            free(session->desktop);
            free(session->tty);
            free(session->display);
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
        if (session == NULL || session->seat_id == NULL ||
            strcmp(session->seat_id, seat_id) != 0) {
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
    seat = find_session_seat(manager, session);
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
    seat = find_session_seat(manager, session);
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
    claim->manager_fd = -1;
    claim->pause_deadline_ms = 0U;
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
            if (claim->manager_fd >= 0) {
                (void) close(claim->manager_fd);
            }
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
    claim->pause_deadline_ms = 0U;
    if (force && claim->manager_fd >= 0) {
        (void) close(claim->manager_fd);
        claim->manager_fd = -1;
    }
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
    claim->pause_deadline_ms = 0U;
    if (claim->manager_fd >= 0) {
        (void) close(claim->manager_fd);
        claim->manager_fd = -1;
    }
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
        return DL_DEVICE_UNCLAIMED;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_DEVICE_UNCLAIMED;
    }
    claim = find_claim(session, major_number, minor_number);
    return claim == NULL ? DL_DEVICE_UNCLAIMED : claim->pause_state;
}


static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0U;
    }
    return (uint64_t) now.tv_sec * 1000U +
        (uint64_t) now.tv_nsec / 1000000U;
}

static int open_verified_device(const dl_device *device)
{
    struct stat st;
    int fd;

    if (device == NULL || device->devnode == NULL) {
        errno = ENODEV;
        return -1;
    }
    fd = open(device->devnode, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;
        (void) close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISCHR(st.st_mode) || major(st.st_rdev) != device->major_number ||
        minor(st.st_rdev) != device->minor_number) {
        (void) close(fd);
        errno = ENODEV;
        return -1;
    }
    return fd;
}

static int duplicate_cloexec(int fd)
{
    return fcntl(fd, F_DUPFD_CLOEXEC, 3);
}

dl_error dl_manager_register_device_node(
    dl_manager *manager,
    const char *seat_id,
    uint32_t major_number,
    uint32_t minor_number,
    const char *devnode,
    bool master_of_seat,
    unsigned int capabilities,
    uint64_t *out_generation)
{
    dl_error result;
    dl_device *device;
    char *path;

    if (manager == NULL || seat_id == NULL || devnode == NULL ||
        devnode[0] != '/' || out_generation == NULL ||
        (capabilities & ~(unsigned int) (DL_DEVICE_CAP_TTY |
            DL_DEVICE_CAP_GRAPHICAL | DL_DEVICE_CAP_VT)) != 0U) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    path = copy_string(devnode);
    if (path == NULL) {
        return DL_ERROR_NO_MEMORY;
    }
    result = dl_manager_add_device(
        manager, seat_id, major_number, minor_number, out_generation);
    if (result != DL_OK) {
        free(path);
        return result;
    }
    device = find_device(find_seat(manager, seat_id), major_number, minor_number);
    device->devnode = path;
    device->master_of_seat = master_of_seat;
    device->capabilities = capabilities;
    return DL_OK;
}

dl_error dl_manager_get_seat_capabilities(
    const dl_manager *manager,
    const char *seat_id,
    dl_seat_capabilities *out_capabilities)
{
    const dl_seat *seat;
    const dl_device *device;
    if (manager == NULL || seat_id == NULL || out_capabilities == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    seat = find_seat(manager, seat_id);
    if (seat == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    out_capabilities->has_tty = false;
    out_capabilities->has_graphical = false;
    out_capabilities->has_vt = false;
    for (device = seat->devices; device != NULL; device = device->next) {
        if (!device->master_of_seat) {
            continue;
        }
        out_capabilities->has_tty |=
            (device->capabilities & DL_DEVICE_CAP_TTY) != 0U;
        out_capabilities->has_graphical |=
            (device->capabilities & DL_DEVICE_CAP_GRAPHICAL) != 0U;
        out_capabilities->has_vt |=
            (device->capabilities & DL_DEVICE_CAP_VT) != 0U;
    }
    return DL_OK;
}

dl_error dl_manager_take_control(
    dl_manager *manager,
    const char *session_id,
    const char *connection_id,
    uint32_t caller_uid,
    bool force)
{
    dl_session *session;
    char *replacement;
    if (manager == NULL || session_id == NULL || connection_id == NULL ||
        session_id[0] == '\0' || connection_id[0] == '\0') {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (caller_uid != 0U && caller_uid != session->uid) {
        return DL_ERROR_INVALID_STATE;
    }
    if (session->controller_id != NULL) {
        if (strcmp(session->controller_id, connection_id) == 0) {
            return DL_ERROR_ALREADY_EXISTS;
        }
        if (!force || caller_uid != 0U) {
            return DL_ERROR_BUSY;
        }
        free_claims(session->claims);
        session->claims = NULL;
    }
    replacement = copy_string(connection_id);
    if (replacement == NULL) {
        return DL_ERROR_NO_MEMORY;
    }
    free(session->controller_id);
    session->controller_id = replacement;
    return DL_OK;
}

dl_error dl_manager_session_is_controller(const dl_manager *manager, const char *session_id, const char *connection_id)
{
 const dl_session *session;
 if (!manager || !session_id || !connection_id) return DL_ERROR_INVALID_ARGUMENT;
 session=find_session(manager,session_id);
 if (!session) return DL_ERROR_NOT_FOUND;
 return session->controller_id && strcmp(session->controller_id,connection_id)==0 ? DL_OK : DL_ERROR_INVALID_STATE;
}
dl_error dl_manager_release_control(
    dl_manager *manager,
    const char *session_id,
    const char *connection_id)
{
    dl_session *session;
    if (manager == NULL || session_id == NULL || connection_id == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (session->controller_id == NULL ||
        strcmp(session->controller_id, connection_id) != 0) {
        return DL_ERROR_INVALID_STATE;
    }
    free_claims(session->claims);
    session->claims = NULL;
    free(session->controller_id);
    session->controller_id = NULL;
    return DL_OK;
}

dl_error dl_manager_take_device_fd(
    dl_manager *manager,
    const char *session_id,
    const char *connection_id,
    uint32_t major_number,
    uint32_t minor_number,
    int *out_fd,
    bool *out_session_inactive)
{
    dl_session *session;
    dl_device *device;
    dl_claim *claim;
    dl_error result;
    int fd, client_fd;

    if (manager == NULL || session_id == NULL || connection_id == NULL ||
        out_fd == NULL || out_session_inactive == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    *out_fd = -1;
    *out_session_inactive = false;
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (session->controller_id == NULL ||
        strcmp(session->controller_id, connection_id) != 0) {
        return DL_ERROR_INVALID_STATE;
    }
    device = find_device(find_session_seat(manager, session),
        major_number, minor_number);
    if (device == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (session_has_claim(session, major_number, minor_number)) {
        return DL_ERROR_ALREADY_EXISTS;
    }
    fd = open_verified_device(device);
    if (fd < 0) {
        return DL_ERROR_INVALID_STATE;
    }
    result = dl_manager_claim_device(
        manager, session_id, major_number, minor_number, out_session_inactive);
    if (result != DL_OK) {
        (void) close(fd);
        return result;
    }
    claim = find_claim(session, major_number, minor_number);
    claim->manager_fd = fd;
    client_fd = duplicate_cloexec(fd);
    if (client_fd < 0) {
        (void) dl_manager_release_device(
            manager, session_id, major_number, minor_number);
        return DL_ERROR_INVALID_STATE;
    }
    *out_fd = client_fd;
    return DL_OK;
}

dl_error dl_manager_resume_device_fd(
    dl_manager *manager,
    const char *session_id,
    const char *connection_id,
    uint32_t major_number,
    uint32_t minor_number,
    int *out_fd)
{
    dl_session *session;
    dl_claim *claim;
    dl_device *device;
    int fd, client_fd;

    if (manager == NULL || session_id == NULL || connection_id == NULL ||
        out_fd == NULL) {
        return DL_ERROR_INVALID_ARGUMENT;
    }
    *out_fd = -1;
    session = find_session(manager, session_id);
    if (session == NULL) {
        return DL_ERROR_NOT_FOUND;
    }
    if (session->controller_id == NULL ||
        strcmp(session->controller_id, connection_id) != 0 ||
        !dl_manager_session_is_active(manager, session_id)) {
        return DL_ERROR_INVALID_STATE;
    }
    claim = find_claim(session, major_number, minor_number);
    device = find_device(find_session_seat(manager, session),
        major_number, minor_number);
    if (claim == NULL || device == NULL ||
        claim->pause_state != DL_DEVICE_PAUSED) {
        return DL_ERROR_INVALID_STATE;
    }
    fd = open_verified_device(device);
    if (fd < 0) {
        return DL_ERROR_INVALID_STATE;
    }
    client_fd = duplicate_cloexec(fd);
    if (client_fd < 0) {
        (void) close(fd);
        return DL_ERROR_INVALID_STATE;
    }
    if (claim->manager_fd >= 0) {
        (void) close(claim->manager_fd);
    }
    claim->manager_fd = fd;
    claim->pause_state = DL_DEVICE_ACTIVE;
    claim->pause_deadline_ms = 0U;
    *out_fd = client_fd;
    return DL_OK;
}

dl_error dl_manager_pause_device_timeout(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number,
    uint64_t timeout_milliseconds)
{
    dl_error result;
    dl_session *session;
    dl_claim *claim;
    uint64_t now;
    if (timeout_milliseconds == 0U) {
        return dl_manager_pause_device(
            manager, session_id, major_number, minor_number, true);
    }
    now = monotonic_milliseconds();
    if (now == 0U || UINT64_MAX - now < timeout_milliseconds) {
        return DL_ERROR_LIMIT;
    }
    result = dl_manager_pause_device(
        manager, session_id, major_number, minor_number, false);
    if (result != DL_OK) {
        return result;
    }
    session = find_session(manager, session_id);
    claim = find_claim(session, major_number, minor_number);
    claim->pause_deadline_ms = now + timeout_milliseconds;
    return DL_OK;
}

size_t dl_manager_expire_device_pauses(
    dl_manager *manager,
    uint64_t monotonic_now_milliseconds)
{
    dl_session *session;
    size_t expired = 0U;
    if (manager == NULL || monotonic_now_milliseconds == 0U) {
        return 0U;
    }
    for (session = manager->sessions; session != NULL; session = session->next) {
        dl_claim *claim;
        for (claim = session->claims; claim != NULL; claim = claim->next) {
            if (claim->pause_state == DL_DEVICE_PAUSE_PENDING &&
                claim->pause_deadline_ms != 0U &&
                claim->pause_deadline_ms <= monotonic_now_milliseconds) {
                claim->pause_state = DL_DEVICE_PAUSED;
                claim->pause_deadline_ms = 0U;
                if (claim->manager_fd >= 0) {
                    (void) close(claim->manager_fd);
                    claim->manager_fd = -1;
                }
                expired++;
            }
        }
    }
    return expired;
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
