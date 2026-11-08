#define _POSIX_C_SOURCE 200809L
#include "core/manager.h"
#include "dbus/login1.h"
#include <systemd/sd-bus.h>
#include <assert.h>
#include <pthread.h>
#include <poll.h>
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
    assert(address!=NULL && address[0] != 0);
    assert(dl_manager_create(&manager)==DL_OK);
    assert(dl_manager_add_seat(manager,"seat0")==DL_OK);
    assert(dl_manager_add_session(manager,"test-session",(uint32_t)getuid(),
        "seat0")==DL_OK);
    assert(dl_login1_open_address(&login1,manager,address)==0);
    state.login1=login1;
    atomic_init(&state.stopping,false);
    assert(pthread_create(&thread,NULL,dispatch_thread,&state)==0);
    assert(sd_bus_new(&client)==0);
    assert(sd_bus_set_address(client,address)==0);
    assert(sd_bus_set_bus_client(client,1)==0);
    assert(sd_bus_start(client)==0);

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
    call_ok(client,object_path,"org.freedesktop.login1.Session","Lock","");
    call_ok(client,object_path,"org.freedesktop.login1.Session","Unlock","");
    call_ok(client,object_path,"org.freedesktop.login1.Session","TakeControl","b",0);
    call_ok(client,object_path,"org.freedesktop.login1.Session","ReleaseControl","");
    call_ok(client,"/org/freedesktop/login1",
        "org.freedesktop.login1.Manager","ListSessions","");

    sd_bus_close_unref(client);
    atomic_store(&state.stopping,true);
    assert(pthread_join(thread,NULL)==0);
    dl_login1_close(login1);
    dl_manager_destroy(manager);
    sd_bus_error_free(&error);
    puts("private login1 D-Bus integration tests passed");
    return 0;
}
