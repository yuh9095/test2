#define _POSIX_C_SOURCE 200809L
#include "core/manager.h"
#include "dbus/login1.h"
#include <systemd/sd-bus.h>
#include <assert.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

int main(void)
{
    const char *address=getenv("DBUS_SESSION_BUS_ADDRESS");
    dl_manager *manager=NULL;
    dl_login1 *login1=NULL;
    sd_bus *client=NULL;
    pthread_t thread;
    service_state state={0};
    sd_bus_error error=SD_BUS_ERROR_NULL;
    sd_bus_message *reply=NULL;
    const char *object_path_value=NULL;
    char object_path[256];
    int r;
    pid_t kill_child;
    dl_session_properties kill_properties = {0};
    dl_session_properties session_properties = {0};
    assert(address!=NULL && address[0] != 0);
    assert(dl_manager_create(&manager)==DL_OK);
    assert(dl_manager_add_seat(manager,"seat0")==DL_OK);
    kill_child = fork();
    assert(kill_child >= 0);
    if (kill_child == 0) { for (;;) pause(); }
    kill_properties.leader_pid = (uint32_t)kill_child;
    kill_properties.class_name = "user";
    kill_properties.service = "test";
    assert(dl_manager_add_session_with_properties(manager,"kill-test",
        (uint32_t)getuid(),&kill_properties)==DL_OK);
    session_properties.seat_id = "seat0";
    session_properties.type = "tty";
    session_properties.class_name = "user";
    session_properties.service = "test";
    session_properties.leader_pid = (uint32_t)getpid();
    session_properties.cgroup_path = "test.slice/test-session.scope";
    assert(dl_manager_add_session_with_properties(manager,"test-session",
        (uint32_t)getuid(),&session_properties)==DL_OK);
    assert(dl_login1_open_address(&login1,manager,address)==0);
    state.login1=login1;
    atomic_init(&state.stopping,false);
    assert(pthread_create(&thread,NULL,dispatch_thread,&state)==0);
    assert(setenv("DBUS_SYSTEM_BUS_ADDRESS",address,1)==0);
    assert(sd_bus_open_system(&client)==0);

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
    call_not_supported(client,"/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.login1.Seat","SwitchToNext","");
    call_not_supported(client,"/org/freedesktop/login1/seat/seat0",
        "org.freedesktop.login1.Seat","SwitchToPrevious","");
    call_ok(client,object_path,"org.freedesktop.login1.Session","Lock","");
    call_ok(client,object_path,"org.freedesktop.login1.Session","Unlock","");
    call_ok(client,object_path,"org.freedesktop.login1.Session","TakeControl","b",0);
    call_ok(client,object_path,"org.freedesktop.login1.Session","ReleaseControl","");
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
    call_not_supported(client,object_path,
        "org.freedesktop.login1.Session","Terminate","");
    call_not_supported(client,"/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","TerminateSession","s",
        "test-session");

    sd_bus_close_unref(client);
    atomic_store(&state.stopping,true);
    assert(pthread_join(thread,NULL)==0);
    dl_login1_close(login1);
    dl_manager_destroy(manager);
    sd_bus_error_free(&error);
    puts("private login1 D-Bus integration tests passed");
    return 0;
}
