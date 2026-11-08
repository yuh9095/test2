#include "core/manager.h"

#include <assert.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>


static void test_device_fd_and_controller_lifecycle(void)
{
    dl_manager *manager = NULL;
    dl_seat_capabilities caps;
    uint64_t generation = 0U;
    int fd = -1;
    bool inactive = true;

    assert(dl_manager_create(&manager) == DL_OK);
    assert(dl_manager_add_seat(manager, "seat0") == DL_OK);
    assert(dl_manager_add_session(manager, "s1", 1000U, "seat0") == DL_OK);
    assert(dl_manager_set_active_session(manager, "seat0", "s1") == DL_OK);
    assert(dl_manager_register_device_node(
        manager, "seat0", 1U, 3U, "/dev/null", true,
        DL_DEVICE_CAP_TTY | DL_DEVICE_CAP_GRAPHICAL | DL_DEVICE_CAP_VT,
        &generation) == DL_OK);
    assert(generation != 0U);
    assert(dl_manager_get_seat_capabilities(manager, "seat0", &caps) == DL_OK);
    assert(caps.has_tty && caps.has_graphical && caps.has_vt);

    assert(dl_manager_take_control(manager, "s1", ":1.42", 1000U, false) == DL_OK);
    assert(dl_manager_take_control(manager, "s1", ":1.43", 1000U, false) ==
        DL_ERROR_BUSY);
    assert(dl_manager_take_device_fd(
        manager, "s1", ":1.42", 1U, 3U, &fd, &inactive) == DL_OK);
    assert(fd >= 0 && !inactive);
    assert((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    assert(close(fd) == 0);

    assert(dl_manager_pause_device_timeout(
        manager, "s1", 1U, 3U, 60000U) == DL_OK);
    assert(dl_manager_complete_device_pause(manager, "s1", 1U, 3U) == DL_OK);
    assert(dl_manager_resume_device_fd(
        manager, "s1", ":1.42", 1U, 3U, &fd) == DL_OK);
    assert(fd >= 0);
    assert(close(fd) == 0);

    assert(dl_manager_pause_device_timeout(
        manager, "s1", 1U, 3U, 60000U) == DL_OK);
    assert(dl_manager_expire_device_pauses(manager, UINT64_MAX) == 1U);
    assert(dl_manager_get_device_pause_state(
        manager, "s1", 1U, 3U) == DL_DEVICE_PAUSED);
    assert(dl_manager_resume_device_fd(
        manager, "s1", ":1.42", 1U, 3U, &fd) == DL_OK);
    assert(close(fd) == 0);

    assert(dl_manager_register_device_node(
        manager, "seat0", 1U, 5U, "/dev/null", false,
        DL_DEVICE_CAP_NONE, &generation) == DL_OK);
    assert(dl_manager_take_device_fd(
        manager, "s1", ":1.42", 1U, 5U, &fd, &inactive) ==
        DL_ERROR_INVALID_STATE);
    assert(dl_manager_remove_device(manager, "seat0", 1U, 5U) == DL_OK);

    assert(dl_manager_release_control(manager, "s1", ":1.42") == DL_OK);
    assert(dl_manager_take_device_fd(
        manager, "s1", ":1.42", 1U, 3U, &fd, &inactive) ==
        DL_ERROR_INVALID_STATE);
    assert(dl_manager_remove_session(manager, "s1") == DL_OK);
    assert(dl_manager_remove_device(manager, "seat0", 1U, 3U) == DL_OK);
    assert(dl_manager_remove_seat(manager, "seat0") == DL_OK);
    dl_manager_destroy(manager);
}

static void test_device_node_identity_and_master_capabilities(void)
{
    dl_manager *manager = NULL;
    dl_seat_capabilities caps;
    uint64_t generation = 0U;
    int fd = -1;
    bool inactive = false;

    assert(dl_manager_create(&manager) == DL_OK);
    assert(dl_manager_add_seat(manager, "seat0") == DL_OK);
    assert(dl_manager_add_session(manager, "s1", 1000U, "seat0") == DL_OK);
    assert(dl_manager_set_active_session(manager, "seat0", "s1") == DL_OK);
    assert(dl_manager_register_device_node(
        manager, "seat0", 1U, 3U, "/dev/null", false,
        DL_DEVICE_CAP_GRAPHICAL, &generation) == DL_OK);
    assert(dl_manager_get_seat_capabilities(manager, "seat0", &caps) == DL_OK);
    assert(!caps.has_graphical);

    assert(dl_manager_take_control(manager, "s1", ":1.1", 1000U, false) == DL_OK);
    assert(dl_manager_take_device_fd(
        manager, "s1", ":1.1", 1U, 3U, &fd, &inactive) == DL_OK);
    assert(close(fd) == 0);
    assert(dl_manager_release_device(manager, "s1", 1U, 3U) == DL_OK);
    assert(dl_manager_release_control(manager, "s1", ":1.1") == DL_OK);
    assert(dl_manager_remove_session(manager, "s1") == DL_OK);
    assert(dl_manager_remove_device(manager, "seat0", 1U, 3U) == DL_OK);
    assert(dl_manager_remove_seat(manager, "seat0") == DL_OK);
    dl_manager_destroy(manager);
}

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

static void test_unseated_remote_session_metadata(void)
{
    dl_manager *manager = NULL;
    dl_session_properties input = {0};
    dl_session_properties output = {0};
    bool inactive = false;

    input.type = "tty";
    input.class_name = "user";
    input.service = "sshd";
    input.tty = "pts/3";
    input.remote = true;
    input.leader_pid = 4321U;

    assert(dl_manager_create(&manager) == DL_OK);
    assert(dl_manager_add_session_with_properties(
        manager, "ssh-1", 1000U, &input) == DL_OK);
    assert(dl_manager_get_session_properties(
        manager, "ssh-1", &output) == DL_OK);
    assert(output.seat_id == NULL);
    assert(output.remote);
    assert(output.leader_pid == 4321U);
    assert(output.type != NULL && strcmp(output.type, "tty") == 0);
    assert(output.class_name != NULL &&
        strcmp(output.class_name, "user") == 0);
    assert(output.service != NULL && strcmp(output.service, "sshd") == 0);
    assert(output.tty != NULL && strcmp(output.tty, "pts/3") == 0);
    assert(!dl_manager_session_is_active(manager, "ssh-1"));
    assert(dl_manager_claim_device(
        manager, "ssh-1", 226U, 0U, &inactive) == DL_ERROR_INVALID_STATE);
    assert(dl_manager_remove_session(manager, "ssh-1") == DL_OK);
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
    test_device_fd_and_controller_lifecycle();
    test_device_node_identity_and_master_capabilities();
    test_seat_session_and_device_lifecycle();
    test_unseated_remote_session_metadata();
    test_argument_validation();
    puts("core model tests passed");
    return 0;
}
