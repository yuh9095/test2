#define _POSIX_C_SOURCE 200809L
#include "core/manager.h"
#include "dbus/login1.h"
#include <systemd/sd-bus.h>
#include <assert.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct service_state {
    dl_login1 *login1;
    atomic_bool stopping;
} service_state;

static void *dispatch_thread(void *userdata)
{
    service_state *state=userdata;
    while (!atomic_load(&state->stopping)) {
        struct pollfd pfd;
        int r=dl_login1_dispatch(state->login1);
        assert(r==0);
        pfd.fd=dl_login1_get_fd(state->login1);
        pfd.events=dl_login1_get_events(state->login1);
        pfd.revents=0;
        (void)poll(&pfd,1,20);
    }
    return NULL;
}

static bool saw_pause_device = false;
static bool saw_force_device = false;
static bool saw_gone_device = false;
static unsigned int resume_device_count = 0U;
static bool resume_device_fd_valid = false;

static int resume_device_signal(sd_bus_message *message, void *userdata,
    sd_bus_error *error)
{
    uint32_t major_number = 0U, minor_number = 0U;
    int fd = -1;
    struct stat st;
    (void)userdata;
    (void)error;
    if (sd_bus_message_read(message, "uuh", &major_number, &minor_number,
            &fd) >= 0 && fd >= 0 && fstat(fd, &st) == 0 &&
            S_ISCHR(st.st_mode)) {
        resume_device_fd_valid = true;
        resume_device_count++;
    }
    return 0;
}

static int pause_device_signal(sd_bus_message *message, void *userdata,
    sd_bus_error *error)
{
    uint32_t major_number = 0U, minor_number = 0U;
    const char *type = NULL;
    (void)userdata;
    (void)error;
    if (sd_bus_message_read(message, "uus", &major_number,
            &minor_number, &type) >= 0 && type != NULL) {
        if (strcmp(type, "pause") == 0) saw_pause_device = true;
        else if (strcmp(type, "force") == 0) saw_force_device = true;
        else if (strcmp(type, "gone") == 0) saw_gone_device = true;
    }
    return 0;
}

static void call_ok(sd_bus *bus,const char *path,const char *interface,
    const char *method,const char *signature,...)
{
    sd_bus_error error=SD_BUS_ERROR_NULL;
    sd_bus_message *reply=NULL;
    va_list ap;
    int r;
    va_start(ap,signature);
    r=sd_bus_call_methodv(bus,"org.freedesktop.login1",path,interface,
        method,&error,&reply,signature,ap);
    va_end(ap);
    if(r<0) {
        fprintf(stderr,"%s.%s failed: %s\n",interface,method,
            error.message?error.message:"unknown D-Bus error");
    }
    assert(r>=0);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
}


static void call_error_name(sd_bus *bus,const char *path,
    const char *interface,const char *method,const char *expected,
    const char *signature,...)
{
    sd_bus_error error=SD_BUS_ERROR_NULL;
    sd_bus_message *reply=NULL;
    va_list ap;
    int r;
    va_start(ap,signature);
    r=sd_bus_call_methodv(bus,"org.freedesktop.login1",path,interface,
        method,&error,&reply,signature,ap);
    va_end(ap);
    assert(r<0);
    if (error.name == NULL || strcmp(error.name, expected) != 0) {
        fprintf(stderr, "D-Bus %s.%s expected error %s, got %s (r=%d, message=%s)\n",
            interface, method, expected,
            error.name == NULL ? "(null)" : error.name, r,
            error.message == NULL ? "(none)" : error.message);
    }
    assert(error.name!=NULL && strcmp(error.name,expected)==0);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
}

static void call_not_supported(sd_bus *bus,const char *path,
    const char *interface,const char *method,const char *signature,...)
{
    sd_bus_error error=SD_BUS_ERROR_NULL;
    sd_bus_message *reply=NULL;
    va_list ap;
    int r;
    va_start(ap,signature);
    r=sd_bus_call_methodv(bus,"org.freedesktop.login1",path,interface,
        method,&error,&reply,signature,ap);
    va_end(ap);
    assert(r<0);
    assert(error.name!=NULL);
    assert(strcmp(error.name,SD_BUS_ERROR_NOT_SUPPORTED)==0);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
}

static void get_session_path(sd_bus *bus, const char *session_id,
    char out_path[256])
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *path = NULL;
    int r = sd_bus_call_method(bus, "org.freedesktop.login1",
        "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
        "GetSession", &error, &reply, "s", session_id);
    assert(r >= 0);
    assert(sd_bus_message_read(reply, "o", &path) >= 0);
    assert(path != NULL);
    assert(snprintf(out_path, 256U, "%s", path) > 0);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
}

int main(void)
{
    const char *address=getenv("DBUS_SESSION_BUS_ADDRESS");
    dl_manager *manager=NULL;
    dl_login1 *login1=NULL;
    sd_bus *client=NULL, *controller_client=NULL;
    pthread_t thread;
    service_state state={0};
    sd_bus_error error=SD_BUS_ERROR_NULL;
    sd_bus_message *reply=NULL;
    const char *object_path_value=NULL;
    char object_path[256];
    int r;
    pid_t kill_child, session_kill_child, terminate_child, manager_terminate_child, seat_child, switch_child;
    sd_bus_slot *pause_slot = NULL, *resume_slot = NULL;
    struct stat null_stat, zero_stat;
    uint64_t device_generation = 0U;
    dl_session_properties kill_properties = {0};
    dl_session_properties session_kill_properties = {0};
    dl_session_properties session_properties = {0};
    dl_session_properties termination_properties = {0};
    dl_session_properties disconnect_properties = {0};
    assert(address!=NULL && address[0] != 0);
    assert(dl_manager_create(&manager)==DL_OK);
    assert(dl_manager_add_seat(manager,"seat0")==DL_OK);
    assert(dl_manager_add_seat(manager,"seat-test")==DL_OK);
    kill_child = fork();
    assert(kill_child >= 0);
    if (kill_child == 0) { for (;;) pause(); }
    kill_properties.leader_pid = (uint32_t)kill_child;
    kill_properties.class_name = "user";
    kill_properties.service = "test";
    assert(dl_manager_add_session_with_properties(manager,"kill-test",
        (uint32_t)getuid(),&kill_properties)==DL_OK);
    session_kill_child = fork();
    assert(session_kill_child >= 0);
    if (session_kill_child == 0) { for (;;) pause(); }
    session_kill_properties.leader_pid = (uint32_t)session_kill_child;
    session_kill_properties.class_name = "user";
    session_kill_properties.service = "test";
    assert(dl_manager_add_session_with_properties(manager,"session-kill-test",
        (uint32_t)getuid(),&session_kill_properties)==DL_OK);
    terminate_child = fork();
    assert(terminate_child >= 0);
    if (terminate_child == 0) { for (;;) pause(); }
    termination_properties.class_name = "user";
    termination_properties.service = "test";
    termination_properties.leader_pid = (uint32_t) terminate_child;
    assert(dl_manager_add_session_with_properties(manager,"term-test",
        (uint32_t)getuid(),&termination_properties)==DL_OK);
    manager_terminate_child = fork();
    assert(manager_terminate_child >= 0);
    if (manager_terminate_child == 0) {
        (void) signal(SIGTERM, SIG_IGN);
        for (;;) pause();
    }
    termination_properties.leader_pid = (uint32_t) manager_terminate_child;
    assert(dl_manager_add_session_with_properties(manager,"manager-term-test",
        (uint32_t)getuid(),&termination_properties)==DL_OK);
    {
        seat_child = fork();
        assert(seat_child >= 0);
        if (seat_child == 0) { for (;;) pause(); }
        termination_properties.seat_id = "seat-test";
        termination_properties.leader_pid = (uint32_t) seat_child;
        assert(dl_manager_add_session_with_properties(manager,"seat-term-test",
            (uint32_t)getuid(),&termination_properties)==DL_OK);
    }
    session_properties.seat_id = "seat0";
    session_properties.type = "tty";
    session_properties.class_name = "user";
    session_properties.service = "test";
    session_properties.leader_pid = (uint32_t)getpid();
    session_properties.cgroup_path = "test.slice/test-session.scope";
    assert(dl_manager_add_session_with_properties(manager,"test-session",
        (uint32_t)getuid(),&session_properties)==DL_OK);
    switch_child = fork();
    assert(switch_child >= 0);
    if (switch_child == 0) { for (;;) pause(); }
    session_properties.leader_pid = (uint32_t)switch_child;
    session_properties.type = "wayland";
    assert(dl_manager_add_session_with_properties(manager,"switch-test",
        (uint32_t)getuid(),&session_properties)==DL_OK);
    disconnect_properties.seat_id = "seat0";
    disconnect_properties.class_name = "user";
    disconnect_properties.service = "disconnect-test";
    assert(dl_manager_add_session_with_properties(manager,"disconnect-test",
        (uint32_t)getuid(),&disconnect_properties)==DL_OK);
    assert(stat("/dev/null", &null_stat) == 0);
    assert(dl_manager_register_device_node(manager, "seat0",
        (uint32_t)major(null_stat.st_rdev), (uint32_t)minor(null_stat.st_rdev),
        "/dev/null", false, DL_DEVICE_CAP_GRAPHICAL,
        &device_generation) == DL_OK);
    assert(stat("/dev/zero", &zero_stat) == 0);
    assert(dl_manager_register_device_node(manager, "seat0",
        (uint32_t)major(zero_stat.st_rdev), (uint32_t)minor(zero_stat.st_rdev),
        "/dev/zero", false, DL_DEVICE_CAP_NONE,
        &device_generation) == DL_OK);
    assert(dl_login1_open_address(&login1,manager,address)==0);
    state.login1=login1;
    atomic_init(&state.stopping,false);
    assert(pthread_create(&thread,NULL,dispatch_thread,&state)==0);
    assert(setenv("DBUS_SYSTEM_BUS_ADDRESS",address,1)==0);
    assert(sd_bus_open_system(&client)==0);

    /* Power capabilities are explicitly unavailable, and requests fail
     * safely instead of silently pretending a reboot/suspend occurred. */
    {
        static const char *const capability_methods[] = {
            "CanPowerOff", "CanReboot", "CanSuspend", "CanHibernate",
            "CanHybridSleep", "CanSuspendThenHibernate"
        };
        static const char *const action_methods[] = {
            "PowerOff", "Reboot", "Suspend", "Hibernate",
            "HybridSleep", "SuspendThenHibernate"
        };
        size_t i;
        for (i = 0U; i < sizeof(capability_methods)/sizeof(capability_methods[0]); i++) {
            const char *capability = NULL;
            r = sd_bus_call_method(client, "org.freedesktop.login1",
                "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
                capability_methods[i], &error, &reply, "");
            assert(r >= 0);
            assert(sd_bus_message_read(reply, "s", &capability) >= 0);
            assert(capability != NULL && strcmp(capability, "na") == 0);
            sd_bus_message_unref(reply); reply = NULL;
        }
        for (i = 0U; i < sizeof(action_methods)/sizeof(action_methods[0]); i++) {
            call_not_supported(client, "/org/freedesktop/login1",
                "org.freedesktop.login1.Manager", action_methods[i], "b", 0);
        }
    }

    r=sd_bus_call_method(client,"org.freedesktop.login1",
        "/org/freedesktop/login1","org.freedesktop.login1.Manager",
        "GetSession",&error,&reply,"s","test-session");
    assert(r>=0);
    assert(sd_bus_message_read(reply,"o",&object_path_value)>=0);
    assert(object_path_value!=NULL);
    assert(snprintf(object_path,sizeof(object_path),"%s",object_path_value)>0);
    sd_bus_message_unref(reply);
    reply=NULL;

    call_ok(client,object_path,"org.freedesktop.login1.Session","Activate","");
    assert(dl_manager_session_is_active(manager,"test-session"));

    r=sd_bus_get_property(client,"org.freedesktop.login1",object_path,
        "org.freedesktop.login1.Session","VTNr",&error,&reply,"u");
    assert(r>=0);
    {
        uint32_t vtnr=99U;
        assert(sd_bus_message_read(reply,"u",&vtnr)>=0);
        assert(vtnr==0U);
    }
    sd_bus_message_unref(reply);
    reply=NULL;
    r=sd_bus_get_property(client,"org.freedesktop.login1",object_path,
        "org.freedesktop.login1.Session","CGroup",&error,&reply,"s");
    assert(r>=0);
    {
        const char *cgroup_path=NULL;
        assert(sd_bus_message_read(reply,"s",&cgroup_path)>=0);
        assert(cgroup_path!=NULL &&
            strcmp(cgroup_path,"/test.slice/test-session.scope")==0);
    }
    sd_bus_message_unref(reply);
    reply=NULL;
    call_ok(client,"/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.login1.Seat","ActivateSession","s","test-session");
    /* Relative VT switching is privileged. This integration test normally
     * runs as an unprivileged CI user, so verify the authorization boundary
     * rather than incorrectly expecting the post-authorization capability
     * error. */
    if (geteuid() == 0) {
        call_not_supported(client,"/org/freedesktop/login1/seat/seat0",
            "org.freedesktop.login1.Seat","SwitchToNext","");
        call_not_supported(client,"/org/freedesktop/login1/seat/seat0",
            "org.freedesktop.login1.Seat","SwitchToPrevious","");
    } else {
        call_error_name(client,"/org/freedesktop/login1/seat/seat0",
            "org.freedesktop.login1.Seat","SwitchToNext",
            SD_BUS_ERROR_ACCESS_DENIED,"");
        call_error_name(client,"/org/freedesktop/login1/seat/seat0",
            "org.freedesktop.login1.Seat","SwitchToPrevious",
            SD_BUS_ERROR_ACCESS_DENIED,"");
    }
    call_ok(client,object_path,"org.freedesktop.login1.Session","Lock","");
    call_ok(client,object_path,"org.freedesktop.login1.Session","Unlock","");
    call_ok(client,object_path,"org.freedesktop.login1.Session","SetType","s","wayland");
    r=sd_bus_get_property(client,"org.freedesktop.login1",object_path,
        "org.freedesktop.login1.Session","Type",&error,&reply,"s");
    assert(r>=0);
    {
        const char *session_type=NULL;
        assert(sd_bus_message_read(reply,"s",&session_type)>=0);
        assert(session_type!=NULL && strcmp(session_type,"wayland")==0);
    }
    sd_bus_message_unref(reply); reply=NULL;

    /* libseat's logind backend expects PauseDevice and a fresh TakeDevice
     * after the Session becomes active again. */
    call_ok(client,object_path,"org.freedesktop.login1.Session","TakeControl","b",0);
    assert(sd_bus_match_signal(client,&pause_slot,"org.freedesktop.login1",
        object_path,"org.freedesktop.login1.Session","PauseDevice",
        pause_device_signal,NULL)>=0);
    assert(sd_bus_match_signal(client,&resume_slot,"org.freedesktop.login1",
        object_path,"org.freedesktop.login1.Session","ResumeDevice",
        resume_device_signal,NULL)>=0);
    r=sd_bus_call_method(client,"org.freedesktop.login1",object_path,
        "org.freedesktop.login1.Session","TakeDevice",&error,&reply,"uu",
        (uint32_t)major(null_stat.st_rdev),(uint32_t)minor(null_stat.st_rdev));
    assert(r>=0);
    {
        int device_fd=-1; int inactive=1;
        assert(sd_bus_message_read(reply,"hb",&device_fd,&inactive)>=0);
        assert(device_fd>=0 && inactive==0);
    }
    sd_bus_message_unref(reply); reply=NULL;
    call_ok(client,"/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.login1.Seat","ActivateSession","s","switch-test");
    {
        unsigned int attempt;
        for(attempt=0;attempt<100U && !saw_pause_device;attempt++) {
            struct pollfd pfd={sd_bus_get_fd(client),POLLIN,0};
            (void)sd_bus_process(client,NULL);
            if(!saw_pause_device) (void)poll(&pfd,1,10);
        }
        assert(saw_pause_device);
    }
    assert(dl_manager_get_device_pause_state(manager,"test-session",
        (uint32_t)major(null_stat.st_rdev),(uint32_t)minor(null_stat.st_rdev))==
        DL_DEVICE_PAUSE_PENDING);
    call_ok(client,object_path,"org.freedesktop.login1.Session",
        "PauseDeviceComplete","uu",(uint32_t)major(null_stat.st_rdev),
        (uint32_t)minor(null_stat.st_rdev));
    assert(dl_manager_get_device_pause_state(manager,"test-session",
        (uint32_t)major(null_stat.st_rdev),(uint32_t)minor(null_stat.st_rdev))==
        DL_DEVICE_PAUSED);
    call_ok(client,"/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.login1.Seat","ActivateSession","s","test-session");
    {
        unsigned int attempt;
        for(attempt=0;attempt<100U && resume_device_count<1U;attempt++) {
            struct pollfd pfd={sd_bus_get_fd(client),POLLIN,0};
            (void)sd_bus_process(client,NULL);
            if(resume_device_count<1U) (void)poll(&pfd,1,10);
        }
        assert(resume_device_count==1U && resume_device_fd_valid);
    }
    assert(dl_manager_get_device_pause_state(manager,"test-session",
        (uint32_t)major(null_stat.st_rdev),(uint32_t)minor(null_stat.st_rdev))==
        DL_DEVICE_ACTIVE);
    /* If the client does not acknowledge a later pause, the daemon emits
     * the force notification at the one-second deadline. */
    call_ok(client,"/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.login1.Seat","ActivateSession","s","switch-test");
    {
        unsigned int attempt;
        for(attempt=0;attempt<220U && !saw_force_device;attempt++) {
            struct pollfd pfd={sd_bus_get_fd(client),POLLIN,0};
            (void)sd_bus_process(client,NULL);
            if(!saw_force_device) (void)poll(&pfd,1,10);
        }
        assert(saw_force_device);
    }
    assert(dl_manager_get_device_pause_state(manager,"test-session",
        (uint32_t)major(null_stat.st_rdev),(uint32_t)minor(null_stat.st_rdev))==
        DL_DEVICE_PAUSED);
    call_ok(client,"/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.login1.Seat","ActivateSession","s","test-session");
    {
        unsigned int attempt;
        for(attempt=0;attempt<100U && resume_device_count<2U;attempt++) {
            struct pollfd pfd={sd_bus_get_fd(client),POLLIN,0};
            (void)sd_bus_process(client,NULL);
            if(resume_device_count<2U) (void)poll(&pfd,1,10);
        }
        assert(resume_device_count==2U && resume_device_fd_valid);
    }
    assert(dl_manager_get_device_pause_state(manager,"test-session",
        (uint32_t)major(null_stat.st_rdev),(uint32_t)minor(null_stat.st_rdev))==
        DL_DEVICE_ACTIVE);

    /* A udev removal notifies the owning Session with type=gone and drops
     * the associated server-side claim. */
    r=sd_bus_call_method(client,"org.freedesktop.login1",object_path,
        "org.freedesktop.login1.Session","TakeDevice",&error,&reply,"uu",
        (uint32_t)major(zero_stat.st_rdev),(uint32_t)minor(zero_stat.st_rdev));
    assert(r>=0);
    sd_bus_message_unref(reply); reply=NULL;
    assert(dl_manager_remove_device(manager,"seat0",
        (uint32_t)major(zero_stat.st_rdev),(uint32_t)minor(zero_stat.st_rdev))==
        DL_OK);
    {
        unsigned int attempt;
        for(attempt=0;attempt<100U && !saw_gone_device;attempt++) {
            struct pollfd pfd={sd_bus_get_fd(client),POLLIN,0};
            (void)sd_bus_process(client,NULL);
            if(!saw_gone_device) (void)poll(&pfd,1,10);
        }
        assert(saw_gone_device);
    }
    call_ok(client,object_path,"org.freedesktop.login1.Session",
        "ReleaseDevice","uu",(uint32_t)major(null_stat.st_rdev),
        (uint32_t)minor(null_stat.st_rdev));
    call_ok(client,object_path,"org.freedesktop.login1.Session","ReleaseControl","");
    pause_slot=sd_bus_slot_unref(pause_slot);
    resume_slot=sd_bus_slot_unref(resume_slot);

    /* A vanished D-Bus controller must not leave ownership or device claims. */
    {
        char disconnect_path[256], controller_name[128];
        const char *unique_name=NULL;
        unsigned int attempt;
        assert(sd_bus_open_system(&controller_client)==0);
        get_session_path(controller_client,"disconnect-test",disconnect_path);
        call_ok(controller_client,disconnect_path,
            "org.freedesktop.login1.Session","TakeControl","b",0);
        r=sd_bus_call_method(controller_client,"org.freedesktop.login1",
            disconnect_path,"org.freedesktop.login1.Session","TakeDevice",
            &error,&reply,"uu",(uint32_t)major(null_stat.st_rdev),
            (uint32_t)minor(null_stat.st_rdev));
        assert(r>=0);
        sd_bus_message_unref(reply); reply=NULL;
        assert(sd_bus_get_unique_name(controller_client,&unique_name)>=0);
        assert(unique_name!=NULL);
        assert(snprintf(controller_name,sizeof(controller_name),"%s",
            unique_name)>0);
        sd_bus_close_unref(controller_client);
        controller_client=NULL;
        for(attempt=0;attempt<100U;attempt++) {
            if(dl_manager_session_is_controller(manager,"disconnect-test",
                    controller_name)!=DL_OK)
                break;
            {
                struct timespec delay={0,10000000L};
                (void)nanosleep(&delay,NULL);
            }
        }
        assert(attempt<100U);
        assert(dl_manager_get_device_pause_state(manager,"disconnect-test",
            (uint32_t)major(null_stat.st_rdev),
            (uint32_t)minor(null_stat.st_rdev))==DL_DEVICE_UNCLAIMED);
    }
    call_ok(client,"/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","ListSessions","");

    call_ok(client,"/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","GetUser","u",(uint32_t)getuid());
    call_ok(client,"/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","GetUserByPID","u",(uint32_t)getpid());
    call_ok(client,"/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","ListUsers","");

    /* KillSession(leader) must signal the exact pidfd-captured process. */
    {
        char kill_path[256];
        int status = 0;
        const char *kill_object = NULL;
        r = sd_bus_call_method(client,"org.freedesktop.login1",
            "/org/freedesktop/login1","org.freedesktop.login1.Manager",
            "GetSession",&error,&reply,"s","kill-test");
        assert(r >= 0);
        assert(sd_bus_message_read(reply,"o",&kill_object) >= 0);
        assert(snprintf(kill_path,sizeof(kill_path),"%s",kill_object) > 0);
        sd_bus_message_unref(reply); reply = NULL;
        {
            const char *by_pid_path = NULL;
            r = sd_bus_call_method(client,"org.freedesktop.login1",
                "/org/freedesktop/login1","org.freedesktop.login1.Manager",
                "GetSessionByPID",&error,&reply,"u",(uint32_t)kill_child);
            assert(r >= 0);
            assert(sd_bus_message_read(reply,"o",&by_pid_path) >= 0);
            assert(by_pid_path != NULL && strcmp(by_pid_path,kill_path)==0);
            sd_bus_message_unref(reply); reply = NULL;
        }
        call_ok(client,"/org/freedesktop/login1",
            "org.freedesktop.login1.Manager","KillSession","ssi",
            "kill-test","leader",SIGTERM);
        assert(waitpid(kill_child,&status,0)==kill_child);
        assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGTERM);
        call_not_supported(client,kill_path,
            "org.freedesktop.login1.Session","Kill","si","all",SIGTERM);
        call_error_name(client,"/org/freedesktop/login1",
            "org.freedesktop.login1.Manager","KillSession",
            SD_BUS_ERROR_INVALID_ARGS,"ssi","kill-test","unknown",SIGTERM);
        call_error_name(client,"/org/freedesktop/login1",
            "org.freedesktop.login1.Manager","KillSession",
            SD_BUS_ERROR_INVALID_ARGS,"ssi","kill-test","leader",0);
    }
    /* Session.Kill(leader) must use the same pidfd-safe path as Manager.KillSession. */
    {
        char session_kill_path[256];
        int status = 0;
        get_session_path(client, "session-kill-test", session_kill_path);
        call_ok(client, session_kill_path, "org.freedesktop.login1.Session",
            "Kill", "si", "leader", SIGTERM);
        assert(waitpid(session_kill_child, &status, 0) == session_kill_child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    }

    /* Session.Terminate sends SIGTERM immediately and completes without
     * blocking the D-Bus dispatch thread. */
    {
        char terminate_path[256];
        int status = 0;
        get_session_path(client, "term-test", terminate_path);
        call_ok(client, terminate_path, "org.freedesktop.login1.Session",
            "Terminate", "");
        assert(waitpid(terminate_child, &status, 0) == terminate_child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    }

    /* Seat.Terminate is root-only and targets only the selected Seat. */
    if (geteuid() == 0) {
        int status = 0;
        dl_session_properties p = {0};
        call_ok(client, "/org/freedesktop/login1/seat/seat_2dtest",
            "org.freedesktop.login1.Seat", "Terminate", "");
        assert(dl_manager_get_session_properties(manager,
            "seat-term-test", &p) == DL_OK);
        assert(waitpid((pid_t)p.leader_pid, &status, 0) == (pid_t)p.leader_pid);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    } else {
        call_error_name(client, "/org/freedesktop/login1/seat/seat_2dtest",
            "org.freedesktop.login1.Seat", "Terminate",
            SD_BUS_ERROR_ACCESS_DENIED, "");
        {
            int status = 0;
            assert(kill(seat_child, SIGKILL) == 0);
            assert(waitpid(seat_child, &status, 0) == seat_child);
        }
    }

    /* Manager.TerminateSession escalates to SIGKILL after the grace period
     * when the registered Leader ignores SIGTERM. */
    {
        int status = 0;
        call_ok(client, "/org/freedesktop/login1",
            "org.freedesktop.login1.Manager", "TerminateSession", "s",
            "manager-term-test");
        assert(waitpid(manager_terminate_child, &status, 0) ==
            manager_terminate_child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    }

    { int status=0; (void)kill(switch_child,SIGTERM); assert(waitpid(switch_child,&status,0)==switch_child); }
    sd_bus_close_unref(client);
    atomic_store(&state.stopping,true);
    assert(pthread_join(thread,NULL)==0);
    dl_login1_close(login1);
    dl_manager_destroy(manager);
    sd_bus_error_free(&error);
    puts("private login1 D-Bus integration tests passed");
    return 0;
}
