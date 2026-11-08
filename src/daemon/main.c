#define _GNU_SOURCE
#include "core/manager.h"
#include "platform/linux/udev-manager.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define DL_VERSION "0.1.0"

static void print_usage(const char *program)
{
    (void) printf(
        "Usage: %s [--help] [--version]\n"
        "\n"
        "Run dinit-logind in the foreground.\n"
        "\n"
        "Options:\n"
        "  --help       Show this help and exit\n"
        "  --version    Show version and exit\n",
        program);
}

static bool select_device_seat(
    const dl_udev_device_info *device,
    const char **seat_id,
    unsigned int *capabilities,
    void *userdata)
{
    (void) userdata;

    if (device == NULL || device->seat_id == NULL ||
        device->seat_id[0] == '\0') {
        return false;
    }

    *seat_id = device->seat_id;
    *capabilities = DL_DEVICE_CAP_NONE;

    if (device->subsystem != NULL &&
        strcmp(device->subsystem, "tty") == 0) {
        *capabilities |= DL_DEVICE_CAP_TTY | DL_DEVICE_CAP_VT;
    } else if (device->subsystem != NULL &&
               strcmp(device->subsystem, "drm") == 0) {
        *capabilities |= DL_DEVICE_CAP_GRAPHICAL;
    }

    return true;
}

static int create_signal_fd(void)
{
    sigset_t mask;
    int fd;

    if (sigemptyset(&mask) < 0 ||
        sigaddset(&mask, SIGINT) < 0 ||
        sigaddset(&mask, SIGTERM) < 0 ||
        sigaddset(&mask, SIGHUP) < 0) {
        return -1;
    }
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        return -1;
    }
    fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    return fd;
}

static int handle_signal_fd(int signal_fd, bool *running)
{
    struct signalfd_siginfo info;

    for (;;) {
        ssize_t count = read(signal_fd, &info, sizeof(info));
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0;
            }
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (count == 0) {
            return 0;
        }
        if ((size_t) count != sizeof(info)) {
            errno = EIO;
            return -1;
        }

        if (info.ssi_signo == SIGINT || info.ssi_signo == SIGTERM) {
            *running = false;
        } else if (info.ssi_signo == SIGHUP) {
            (void) fprintf(stderr,
                "dinit-logind: SIGHUP received; no reloadable configuration yet\n");
        }
    }
}

static int run_event_loop(dl_udev_seat_monitor *monitor, int signal_fd)
{
    struct pollfd descriptors[2];
    bool running = true;

    descriptors[0].fd = dl_udev_seat_monitor_get_fd(monitor);
    descriptors[0].events = POLLIN;
    descriptors[1].fd = signal_fd;
    descriptors[1].events = POLLIN;

    if (descriptors[0].fd < 0) {
        return -1;
    }

    while (running) {
        int result;

        descriptors[0].revents = 0;
        descriptors[1].revents = 0;
        result = poll(descriptors, 2U, -1);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            errno = EIO;
            return -1;
        }
        if ((descriptors[1].revents & POLLIN) != 0) {
            if (handle_signal_fd(signal_fd, &running) != 0) {
                return -1;
            }
        }

        if (!running) {
            break;
        }

        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            errno = EIO;
            return -1;
        }
        if ((descriptors[0].revents & POLLIN) != 0) {
            unsigned int dispatched = 0U;

            while (dispatched < 64U) {
                result = dl_udev_seat_monitor_dispatch(monitor);
                if (result < 0) {
                    return -1;
                }
                if (result == 0) {
                    break;
                }
                dispatched++;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    dl_manager *manager = NULL;
    dl_udev_seat_monitor *monitor = NULL;
    int signal_fd = -1;
    int exit_status = EXIT_FAILURE;
    int index;

    for (index = 1; index < argc; index++) {
        if (strcmp(argv[index], "--help") == 0) {
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        }
        if (strcmp(argv[index], "--version") == 0) {
            (void) printf("dinit-logind %s\n", DL_VERSION);
            return EXIT_SUCCESS;
        }
        print_usage(argv[0]);
        (void) fprintf(stderr, "dinit-logind: unknown option: %s\n",
            argv[index]);
        return EXIT_FAILURE;
    }

    signal_fd = create_signal_fd();
    if (signal_fd < 0) {
        (void) fprintf(stderr, "dinit-logind: cannot create signal fd: %s\n",
            strerror(errno));
        goto cleanup;
    }

    if (dl_manager_create(&manager) != DL_OK) {
        (void) fprintf(stderr, "dinit-logind: cannot create core manager\n");
        goto cleanup;
    }

    if (dl_udev_seat_monitor_open(
            &monitor, manager, select_device_seat, NULL) != 0) {
        (void) fprintf(stderr, "dinit-logind: cannot open libudev monitor: %s\n",
            strerror(errno));
        goto cleanup;
    }

    if (dl_udev_seat_monitor_enumerate(monitor) != 0) {
        (void) fprintf(stderr, "dinit-logind: device enumeration failed: %s\n",
            strerror(errno));
        goto cleanup;
    }

    (void) fprintf(stderr,
        "dinit-logind: started; monitoring eudev events (foreground)\n");

    if (run_event_loop(monitor, signal_fd) != 0) {
        (void) fprintf(stderr, "dinit-logind: event loop failed: %s\n",
            strerror(errno));
        goto cleanup;
    }

    exit_status = EXIT_SUCCESS;
    (void) fprintf(stderr, "dinit-logind: stopping\n");

cleanup:
    dl_udev_seat_monitor_close(monitor);
    dl_manager_destroy(manager);
    if (signal_fd >= 0) {
        (void) close(signal_fd);
    }
    return exit_status;
}
