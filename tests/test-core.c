#include "core/manager.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_seat_session_and_device_lifecycle(void)
{
    dl_manager *manager = NULL;
    uint64_t first_generation = 0U;
    uint64_t second_generation = 0U;
    bool inactive = true;

    assert(dl_manager_create(&manager) == DL_OK);
    assert(manager != NULL);

    assert(dl_manager_add_seat(manager, "seat0") == DL_OK);
    assert(dl_manager_add_seat(manager, "seat0") == DL_ERROR_ALREADY_EXISTS);
    assert(dl_manager_add_session(manager, "c1", 1000U, "seat0") == DL_OK);
    assert(dl_manager_add_session(manager, "c2", 1001U, "seat0") == DL_OK);

    assert(dl_manager_add_device(
        manager, "seat0", 226U, 0U, &first_generation) == DL_OK);
    assert(first_generation != 0U);
    assert(dl_manager_add_device(
        manager, "seat0", 226U, 0U, &second_generation) ==
        DL_ERROR_ALREADY_EXISTS);

    assert(dl_manager_set_active_session(manager, "seat0", "c1") == DL_OK);
    assert(dl_manager_session_is_active(manager, "c1"));
    assert(!dl_manager_session_is_active(manager, "c2"));

    assert(dl_manager_claim_device(manager, "c2", 226U, 0U, &inactive) == DL_OK);
    assert(inactive);
    assert(dl_manager_claim_device(manager, "c1", 226U, 0U, &inactive) ==
        DL_ERROR_BUSY);
    assert(dl_manager_release_device(manager, "c2", 226U, 0U) == DL_OK);

    assert(dl_manager_claim_device(manager, "c1", 226U, 0U, &inactive) == DL_OK);
    assert(!inactive);
    assert(dl_manager_get_device_pause_state(
        manager, "c1", 226U, 0U) == DL_DEVICE_ACTIVE);
    assert(dl_manager_pause_device(
        manager, "c1", 226U, 0U, false) == DL_OK);
    assert(dl_manager_get_device_pause_state(
        manager, "c1", 226U, 0U) == DL_DEVICE_PAUSE_PENDING);
    assert(dl_manager_complete_device_pause(
        manager, "c1", 226U, 0U) == DL_OK);
    assert(dl_manager_complete_device_pause(
        manager, "c1", 226U, 0U) == DL_ERROR_INVALID_STATE);
    assert(dl_manager_get_device_pause_state(
        manager, "c1", 226U, 0U) == DL_DEVICE_PAUSED);
    assert(dl_manager_resume_device(
        manager, "c1", 226U, 0U) == DL_OK);
    assert(dl_manager_pause_device(
        manager, "c1", 226U, 0U, true) == DL_OK);
    assert(dl_manager_get_device_pause_state(
        manager, "c1", 226U, 0U) == DL_DEVICE_PAUSED);
    assert(dl_manager_remove_device(manager, "seat0", 226U, 0U) == DL_OK);
    assert(dl_manager_get_device_pause_state(
        manager, "c1", 226U, 0U) == DL_DEVICE_UNCLAIMED);
    assert(dl_manager_device_generation(manager, "seat0", 226U, 0U) == 0U);

    assert(dl_manager_add_device(
        manager, "seat0", 226U, 0U, &second_generation) == DL_OK);
    assert(second_generation > first_generation);

    assert(dl_manager_remove_seat(manager, "seat0") == DL_ERROR_BUSY);
    assert(dl_manager_remove_session(manager, "c1") == DL_OK);
    assert(!dl_manager_session_is_active(manager, "c1"));
    assert(dl_manager_remove_session(manager, "c2") == DL_OK);
    assert(dl_manager_remove_device(manager, "seat0", 226U, 0U) == DL_OK);
    assert(dl_manager_remove_seat(manager, "seat0") == DL_OK);

    dl_manager_destroy(manager);
}

static void test_argument_validation(void)
{
    dl_manager *manager = NULL;

    assert(dl_manager_create(NULL) == DL_ERROR_INVALID_ARGUMENT);
    assert(dl_manager_create(&manager) == DL_OK);
    assert(dl_manager_add_seat(manager, NULL) == DL_ERROR_INVALID_ARGUMENT);
    assert(dl_manager_set_active_session(manager, "missing", NULL) ==
        DL_ERROR_NOT_FOUND);
    assert(dl_manager_remove_session(manager, "missing") == DL_ERROR_NOT_FOUND);
    assert(dl_manager_remove_device(manager, "missing", 1U, 2U) ==
        DL_ERROR_NOT_FOUND);
    dl_manager_destroy(manager);
}

int main(void)
{
    test_seat_session_and_device_lifecycle();
    test_argument_validation();
    puts("core model tests passed");
    return 0;
}
