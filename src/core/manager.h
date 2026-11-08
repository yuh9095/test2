#ifndef DINIT_LOGIND_CORE_MANAGER_H
#define DINIT_LOGIND_CORE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dl_manager dl_manager;

typedef enum dl_error {
    DL_OK = 0,
    DL_ERROR_INVALID_ARGUMENT,
    DL_ERROR_NO_MEMORY,
    DL_ERROR_NOT_FOUND,
    DL_ERROR_ALREADY_EXISTS,
    DL_ERROR_INVALID_STATE,
    DL_ERROR_BUSY,
    DL_ERROR_LIMIT
} dl_error;

/*
 * The core owns its model objects and copied identifiers.
 * This layer deliberately has no D-Bus, PAM, udev, seatd, or systemd
 * dependency. Adapters will be added around this single state model.
 */
dl_error dl_manager_create(dl_manager **out_manager);
void dl_manager_destroy(dl_manager *manager);

dl_error dl_manager_add_seat(dl_manager *manager, const char *seat_id);
dl_error dl_manager_remove_seat(dl_manager *manager, const char *seat_id);

dl_error dl_manager_add_device(
    dl_manager *manager,
    const char *seat_id,
    uint32_t major_number,
    uint32_t minor_number,
    uint64_t *out_generation);
dl_error dl_manager_remove_device(
    dl_manager *manager,
    const char *seat_id,
    uint32_t major_number,
    uint32_t minor_number);

dl_error dl_manager_add_session(
    dl_manager *manager,
    const char *session_id,
    uint32_t uid,
    const char *seat_id);
dl_error dl_manager_remove_session(dl_manager *manager, const char *session_id);
dl_error dl_manager_set_active_session(
    dl_manager *manager,
    const char *seat_id,
    const char *session_id);

bool dl_manager_session_is_active(
    const dl_manager *manager,
    const char *session_id);

/*
 * Claim/release records the core's logical device ownership only.
 * It does not open a device or pass a file descriptor; that belongs
 * to the later seatd/libseat adapter.
 */
dl_error dl_manager_claim_device(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number,
    bool *out_session_inactive);
dl_error dl_manager_release_device(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number);

uint64_t dl_manager_device_generation(
    const dl_manager *manager,
    const char *seat_id,
    uint32_t major_number,
    uint32_t minor_number);

const char *dl_error_string(dl_error error);

#ifdef __cplusplus
}
#endif

#endif
