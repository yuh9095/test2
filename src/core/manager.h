#ifndef DINIT_LOGIND_CORE_MANAGER_H
#define DINIT_LOGIND_CORE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dl_manager dl_manager;

typedef enum dl_device_capability {
    DL_DEVICE_CAP_NONE = 0U,
    DL_DEVICE_CAP_TTY = 1U << 0,
    DL_DEVICE_CAP_GRAPHICAL = 1U << 1,
    DL_DEVICE_CAP_VT = 1U << 2
} dl_device_capability;

typedef struct dl_seat_capabilities {
    bool has_tty;
    bool has_graphical;
    bool has_vt;
} dl_seat_capabilities;

typedef enum dl_device_pause_state {
    DL_DEVICE_UNCLAIMED = 0,
    DL_DEVICE_ACTIVE,
    DL_DEVICE_PAUSE_PENDING,
    DL_DEVICE_PAUSED
} dl_device_pause_state;

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
 * Session metadata is copied on registration. seat_id may be NULL for SSH,
 * headless, and other sessions that do not belong to a local graphical seat.
 * Pointers returned by get_session_properties() are borrowed until removal.
 */
typedef struct dl_session_properties {
    const char *seat_id;
    const char *type;
    const char *class_name;
    const char *service;
    const char *desktop;
    const char *tty;
    const char *display;
    uint32_t leader_pid;
    bool remote;
} dl_session_properties;

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
/* Register a trusted device-node path supplied by the device-enumeration adapter. */
dl_error dl_manager_register_device_node(
    dl_manager *manager,
    const char *seat_id,
    uint32_t major_number,
    uint32_t minor_number,
    const char *devnode,
    bool master_of_seat,
    unsigned int capabilities,
    uint64_t *out_generation);
dl_error dl_manager_get_seat_capabilities(
    const dl_manager *manager,
    const char *seat_id,
    dl_seat_capabilities *out_capabilities);
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
dl_error dl_manager_add_session_with_properties(
    dl_manager *manager,
    const char *session_id,
    uint32_t uid,
    const dl_session_properties *properties);
dl_error dl_manager_get_session_properties(
    const dl_manager *manager,
    const char *session_id,
    dl_session_properties *out_properties);
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

/* Controller ownership is tied to a D-Bus connection identity. */
dl_error dl_manager_take_control(
    dl_manager *manager,
    const char *session_id,
    const char *connection_id,
    uint32_t caller_uid,
    bool force);
dl_error dl_manager_release_control(
    dl_manager *manager,
    const char *session_id,
    const char *connection_id);
dl_error dl_manager_take_device_fd(
    dl_manager *manager,
    const char *session_id,
    const char *connection_id,
    uint32_t major_number,
    uint32_t minor_number,
    int *out_fd,
    bool *out_session_inactive);
dl_error dl_manager_resume_device_fd(
    dl_manager *manager,
    const char *session_id,
    const char *connection_id,
    uint32_t major_number,
    uint32_t minor_number,
    int *out_fd);
dl_error dl_manager_pause_device_timeout(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number,
    uint64_t timeout_milliseconds);
size_t dl_manager_expire_device_pauses(
    dl_manager *manager,
    uint64_t monotonic_now_milliseconds);

/*
 * A graceful pause must be acknowledged with complete_device_pause().
 * A forced pause takes effect immediately. Device removal is represented
 * by remove_device(), which also drops any outstanding claim.
 */
dl_error dl_manager_pause_device(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number,
    bool force);
dl_error dl_manager_complete_device_pause(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number);
dl_error dl_manager_resume_device(
    dl_manager *manager,
    const char *session_id,
    uint32_t major_number,
    uint32_t minor_number);
dl_device_pause_state dl_manager_get_device_pause_state(
    const dl_manager *manager,
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
