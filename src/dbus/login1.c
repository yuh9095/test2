#define _GNU_SOURCE
#include "dbus/login1.h"
#include "platform/linux/vt.h"
#include "session/process.h"
#include <systemd/sd-bus.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.login1"
#define ROOT_PATH "/org/freedesktop/login1"
#define SESSION_PATH ROOT_PATH "/session"
#define SEAT_PATH ROOT_PATH "/seat"

struct dl_login1 {
    sd_bus *bus;
    sd_bus_slot *manager_slot;
    sd_bus_slot *session_slot;
    sd_bus_slot *seat_slot;
    sd_bus_slot *user_slot;
};

static int activate_session_on_seat(sd_bus *bus, dl_manager *manager,
    const char *seat_id, const char *session_id);
static int seat_lookup(const char *path, void *userdata, char *id, size_t cap);

static int make_path(const char *prefix, const char *id, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = strlen(prefix), i;
    if (!id || !*id || n + 2U >= cap) return -EINVAL;
    memcpy(out, prefix, n); out[n++] = '/';
    for (i = 0; id[i]; i++) {
        unsigned char c = (unsigned char)id[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            if (n + 1U >= cap) return -ENAMETOOLONG;
            out[n++] = (char)c;
        } else {
            if (n + 3U >= cap) return -ENAMETOOLONG;
            out[n++] = '_'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15U];
        }
    }
    out[n] = '\0';
    return 0;
}

static int unhex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int path_id(const char *path, const char *prefix, char *out, size_t cap)
{
    const char *p;
    size_t n = 0;
    if (!path || !prefix || !out || strncmp(path, prefix, strlen(prefix)) ||
        path[strlen(prefix)] != '/') return -EINVAL;
    p = path + strlen(prefix) + 1U;
    if (!*p || strchr(p, '/')) return -EINVAL;
    while (*p) {
        unsigned char c;
        if (*p == '_') {
            int a, b;
            if (!p[1] || !p[2] || (a = unhex(p[1])) < 0 ||
                (b = unhex(p[2])) < 0) return -EINVAL;
            c = (unsigned char)((a << 4) | b); p += 3;
        } else c = (unsigned char)*p++;
        if (!c || n + 1U >= cap) return -ENAMETOOLONG;
        out[n++] = (char)c;
    }
    out[n] = '\0';
    return 0;
}

static int user_name(uint32_t uid, char *buf, size_t cap)
{
    struct passwd pw, *found = NULL;
    int r = getpwuid_r((uid_t)uid, &pw, buf, cap, &found);
    if (r) return -r;
    if (!found || !pw.pw_name || !*pw.pw_name)
        (void)snprintf(buf, cap, "%u", uid);
    return 0;
}

static int send_reply(sd_bus_message *call, sd_bus_message *reply)
{
    int r = sd_bus_send(sd_bus_message_get_bus(call), reply, NULL);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

static int list_sessions(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    dl_manager *mgr = userdata;
    sd_bus_message *reply = NULL;
    size_t i, count = dl_manager_get_session_count(mgr);
    int r;
    (void)error;
    if ((r = sd_bus_message_new_method_return(m, &reply)) < 0) return r;
    if ((r = sd_bus_message_open_container(reply, 'a', "(susso)")) < 0) goto fail;
    for (i = 0; i < count; i++) {
        dl_session_properties p = {0};
        char obj[256], name[16384];
        if (dl_manager_get_session_at(mgr, i, &p) != DL_OK) { r = -EIO; goto fail; }
        if ((r = make_path(SESSION_PATH, p.session_id, obj, sizeof(obj))) < 0) goto fail;
        if ((r = user_name(p.uid, name, sizeof(name))) < 0) goto fail;
        if ((r = sd_bus_message_append(reply, "(susso)", p.session_id,
                p.uid, name, p.seat_id ? p.seat_id : "", obj)) < 0) goto fail;
    }
    if ((r = sd_bus_message_close_container(reply)) < 0) goto fail;
    return send_reply(m, reply);
fail:
    sd_bus_message_unref(reply);
    return r;
}

static int get_session(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    const char *id = NULL;
    dl_session_properties p = {0};
    char obj[256];
    int r;
    (void)error;
    if ((r = sd_bus_message_read(m, "s", &id)) < 0) return r;
    if (dl_manager_get_session_properties(userdata, id, &p) != DL_OK)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_UNKNOWN_OBJECT,
            "Session '%s' does not exist", id);
    if ((r = make_path(SESSION_PATH, id, obj, sizeof(obj))) < 0) return r;
    return sd_bus_reply_method_return(m, "o", obj);
}

static int get_session_pid(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    uint32_t pid = 0;
    dl_session_properties p = {0};
    char obj[256];
    int r;
    (void)error;
    if ((r = sd_bus_message_read(m, "u", &pid)) < 0) return r;
    if (dl_manager_find_session_by_leader_pid(userdata, pid, &p) != DL_OK)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_UNKNOWN_OBJECT,
            "No registered Session has leader PID %u", pid);
    if ((r = make_path(SESSION_PATH, p.session_id, obj, sizeof(obj))) < 0) return r;
    return sd_bus_reply_method_return(m, "o", obj);
}


static bool user_has_session(const dl_manager *manager, uint32_t uid)
{
    size_t i;
    for (i = 0; i < dl_manager_get_session_count(manager); i++) {
        dl_session_properties p = {0};
        if (dl_manager_get_session_at(manager, i, &p) == DL_OK &&
            p.uid == uid) return true;
    }
    return false;
}

static int user_object_path(uint32_t uid, char *out, size_t cap)
{
    int n = snprintf(out, cap, ROOT_PATH "/user/_%u", uid);
    return n < 0 ? -EIO : ((size_t)n >= cap ? -ENAMETOOLONG : 0);
}

static int get_user(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    uint32_t uid = 0;
    char object[128];
    int r;
    (void)error;
    if ((r = sd_bus_message_read(m, "u", &uid)) < 0) return r;
    if (!user_has_session(userdata, uid))
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_UNKNOWN_OBJECT,
            "No active User object for UID %u", uid);
    if ((r = user_object_path(uid, object, sizeof(object))) < 0) return r;
    return sd_bus_reply_method_return(m, "o", object);
}

static int get_user_by_pid(sd_bus_message *m, void *userdata,
    sd_bus_error *error)
{
    uint32_t pid = 0;
    dl_session_properties p = {0};
    char object[128];
    int r;
    (void)error;
    if ((r = sd_bus_message_read(m, "u", &pid)) < 0) return r;
    if (dl_manager_find_session_by_leader_pid(userdata, pid, &p) != DL_OK)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_UNKNOWN_OBJECT,
            "No registered Session has leader PID %u", pid);
    if ((r = user_object_path(p.uid, object, sizeof(object))) < 0) return r;
    return sd_bus_reply_method_return(m, "o", object);
}

static int list_users(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    dl_manager *manager = userdata;
    sd_bus_message *reply = NULL;
    size_t i, j, count = dl_manager_get_session_count(manager);
    int r;
    (void)error;
    if ((r = sd_bus_message_new_method_return(m, &reply)) < 0) return r;
    if ((r = sd_bus_message_open_container(reply, 'a', "(uso)")) < 0)
        goto fail;
    for (i = 0; i < count; i++) {
        dl_session_properties p = {0};
        char name[16384], object[128];
        bool seen = false;
        if (dl_manager_get_session_at(manager, i, &p) != DL_OK) {
            r = -EIO;
            goto fail;
        }
        for (j = 0; j < i; j++) {
            dl_session_properties previous = {0};
            if (dl_manager_get_session_at(manager, j, &previous) == DL_OK &&
                previous.uid == p.uid) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        if ((r = user_name(p.uid, name, sizeof(name))) < 0) goto fail;
        if ((r = user_object_path(p.uid, object, sizeof(object))) < 0)
            goto fail;
        if ((r = sd_bus_message_append(reply, "(uso)", p.uid, name,
                object)) < 0) goto fail;
    }
    if ((r = sd_bus_message_close_container(reply)) < 0) goto fail;
    return send_reply(m, reply);
fail:
    sd_bus_message_unref(reply);
    return r;
}

static int list_seats(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    dl_manager *mgr = userdata;
    sd_bus_message *reply = NULL;
    size_t i, count = dl_manager_get_seat_count(mgr);
    int r;
    (void)error;
    if ((r = sd_bus_message_new_method_return(m, &reply)) < 0) return r;
    if ((r = sd_bus_message_open_container(reply, 'a', "(so)")) < 0) goto fail;
    for (i = 0; i < count; i++) {
        const char *id = NULL;
        char obj[256];
        if (dl_manager_get_seat_at(mgr, i, &id) != DL_OK) { r = -EIO; goto fail; }
        if ((r = make_path(SEAT_PATH, id, obj, sizeof(obj))) < 0) goto fail;
        if ((r = sd_bus_message_append(reply, "(so)", id, obj)) < 0) goto fail;
    }
    if ((r = sd_bus_message_close_container(reply)) < 0) goto fail;
    return send_reply(m, reply);
fail:
    sd_bus_message_unref(reply);
    return r;
}

static int get_seat(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    const char *id = NULL, *known = NULL;
    char obj[256];
    size_t i;
    int r;
    (void)error;
    if ((r = sd_bus_message_read(m, "s", &id)) < 0) return r;
    for (i = 0; i < dl_manager_get_seat_count(userdata); i++) {
        if (dl_manager_get_seat_at(userdata, i, &known) == DL_OK &&
            strcmp(id, known) == 0) {
            if ((r = make_path(SEAT_PATH, id, obj, sizeof(obj))) < 0) return r;
            return sd_bus_reply_method_return(m, "o", obj);
        }
    }
    return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_UNKNOWN_OBJECT,
        "Seat '%s' does not exist", id);
}

static int session_lookup(const char *path, void *userdata,
    dl_session_properties *p)
{
    char id[128];
    int r = path_id(path, SESSION_PATH, id, sizeof(id));
    if (r < 0) return r;
    return dl_manager_get_session_properties(userdata, id, p) == DL_OK ? 0 : -ENOENT;
}

static int prop_session_id(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    dl_session_properties s={0}; int r;
    (void)b;(void)i;(void)n;(void)e;
    if ((r=session_lookup(p,u,&s))<0) return r;
    return sd_bus_message_append(m,"s",s.session_id);
}
static int prop_session_user(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    dl_session_properties s={0}; char obj[128]; int r;
    (void)b;(void)i;(void)n;(void)e;
    if ((r=session_lookup(p,u,&s))<0) return r;
    (void)snprintf(obj,sizeof(obj),ROOT_PATH "/user/_%u",s.uid);
    return sd_bus_message_append(m,"(uo)",s.uid,obj);
}
static int prop_session_name(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    dl_session_properties s={0}; char buf[16384]; int r;
    (void)b;(void)i;(void)n;(void)e;
    if ((r=session_lookup(p,u,&s))<0) return r;
    if ((r=user_name(s.uid,buf,sizeof(buf)))<0) return r;
    return sd_bus_message_append(m,"s",buf);
}
static int prop_session_text(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    dl_session_properties s={0}; const char *v=""; int r;
    (void)b;(void)i;(void)e;
    if ((r=session_lookup(p,u,&s))<0) return r;
    if (!strcmp(n,"TTY")) v=s.tty;
    else if (!strcmp(n,"Display")) v=s.display;
    else if (!strcmp(n,"Service")) v=s.service;
    else if (!strcmp(n,"Desktop")) v=s.desktop;
    else if (!strcmp(n,"Type")) v=s.type;
    else if (!strcmp(n,"Class")) v=s.class_name;
    if (!v) v="";
    return sd_bus_message_append(m,"s",v);
}
static int prop_session_seat(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    dl_session_properties s={0}; char obj[256]="/"; int r;
    (void)b;(void)i;(void)n;(void)e;
    if ((r=session_lookup(p,u,&s))<0) return r;
    if (s.seat_id && (r=make_path(SEAT_PATH,s.seat_id,obj,sizeof(obj)))<0) return r;
    return sd_bus_message_append(m,"(so)",s.seat_id?s.seat_id:"",obj);
}
static int prop_session_bool(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    dl_session_properties s={0}; int r;
    (void)b;(void)i;(void)e;
    if ((r=session_lookup(p,u,&s))<0) return r;
    return sd_bus_message_append(m,"b",!strcmp(n,"Remote") ? s.remote :
        dl_manager_session_is_active(u,s.session_id));
}
static int prop_session_leader(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    dl_session_properties s={0}; int r;
    (void)b;(void)i;(void)n;(void)e;
    if ((r=session_lookup(p,u,&s))<0) return r;
    return sd_bus_message_append(m,"u",s.leader_pid);
}

static int prop_session_cgroup(sd_bus *b, const char *path,
    const char *interface, const char *property, sd_bus_message *reply,
    void *userdata, sd_bus_error *error)
{
    dl_session_properties p = {0};
    char cgroup[4097];
    int r;
    (void)b; (void)interface; (void)property; (void)error;
    if ((r = session_lookup(path, userdata, &p)) < 0) return r;
    if (p.cgroup_path == NULL) return sd_bus_message_append(reply, "s", "");
    r = snprintf(cgroup, sizeof(cgroup), "/%s", p.cgroup_path);
    if (r < 0 || (size_t)r >= sizeof(cgroup)) return -EOVERFLOW;
    return sd_bus_message_append(reply, "s", cgroup);
}

static int parse_vt_number(const char *tty, unsigned int *out_number)
{
    const char *digits;
    char *end = NULL;
    unsigned long value;
    if (tty == NULL || out_number == NULL) return -EINVAL;
    if (strncmp(tty, "/dev/tty", 8U) == 0) digits = tty + 8U;
    else if (strncmp(tty, "tty", 3U) == 0) digits = tty + 3U;
    else return -ENOENT;
    if (*digits == '\0') return -EINVAL;
    value = strtoul(digits, &end, 10);
    if (end == digits || *end != '\0' || value == 0U || value > 63U)
        return -EINVAL;
    *out_number = (unsigned int)value;
    return 0;
}

static int prop_session_vtnr(sd_bus *b, const char *path,
    const char *interface, const char *property, sd_bus_message *reply,
    void *userdata, sd_bus_error *error)
{
    dl_session_properties p = {0};
    unsigned int number = 0U;
    int r;
    (void)b; (void)interface; (void)property; (void)error;
    if ((r = session_lookup(path, userdata, &p)) < 0) return r;
    if (p.tty != NULL) (void)parse_vt_number(p.tty, &number);
    return sd_bus_message_append(reply, "u", number);
}

static int switch_to_vt(sd_bus *bus, dl_manager *manager,
    const char *seat_id, unsigned int vt_number)
{
    dl_vt *vt = NULL;
    dl_vt_result vr;
    size_t i, count;
    char old_id[128] = "";
    const char *old_active = NULL;
    int r;
    if (strcmp(seat_id, "seat0") != 0) return -EOPNOTSUPP;
    vr = dl_vt_open("/dev/tty0", &vt);
    if (vr != DL_VT_OK) return -EOPNOTSUPP;
    vr = dl_vt_activate(vt, vt_number);
    if (vr == DL_VT_OK) vr = dl_vt_wait_active(vt, vt_number);
    dl_vt_close(vt);
    if (vr != DL_VT_OK) return -EOPNOTSUPP;

    if (dl_manager_get_active_session(manager, seat_id, &old_active) == DL_OK &&
        old_active != NULL) {
        if (snprintf(old_id, sizeof(old_id), "%s", old_active) >=
            (int)sizeof(old_id)) return -ENAMETOOLONG;
    }
    count = dl_manager_get_session_count(manager);
    for (i = 0; i < count; i++) {
        dl_session_properties p = {0};
        unsigned int candidate = 0U;
        if (dl_manager_get_session_at(manager, i, &p) != DL_OK)
            return -EIO;
        if (p.seat_id == NULL || strcmp(p.seat_id, seat_id) != 0 ||
            parse_vt_number(p.tty, &candidate) < 0 ||
            candidate != vt_number) continue;
        return activate_session_on_seat(bus, manager, seat_id, p.session_id);
    }
    r = dl_manager_set_active_session(manager, seat_id, NULL);
    if (r != DL_OK) return -EINVAL;
    {
        char seat_path[256], session_path[256];
        if ((r = make_path(SEAT_PATH, seat_id, seat_path,
                sizeof(seat_path))) < 0) return r;
        if ((r = sd_bus_emit_properties_changed(bus, seat_path,
                BUS_NAME ".Seat", "ActiveSession", NULL)) < 0) return r;
        if (old_id[0] != '\0') {
            if ((r = make_path(SESSION_PATH, old_id, session_path,
                    sizeof(session_path))) < 0) return r;
            return sd_bus_emit_properties_changed(bus, session_path,
                BUS_NAME ".Session", "Active", NULL);
        }
    }
    return 0;
}

static int method_seat_switch_to(sd_bus_message *m, void *userdata,
    sd_bus_error *error)
{
    char seat_id[128];
    uint32_t vt_number = 0U;
    sd_bus_creds *creds = NULL;
    uid_t euid;
    int r;
    (void)error;
    if ((r = seat_lookup(sd_bus_message_get_path(m), userdata, seat_id,
            sizeof(seat_id))) < 0)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_UNKNOWN_OBJECT,
            "Seat does not exist");
    if ((r = sd_bus_message_read(m, "u", &vt_number)) < 0) return r;
    if (vt_number == 0U || vt_number > 63U)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS,
            "VT number must be between 1 and 63");
    if ((r = sd_bus_query_sender_creds(m, SD_BUS_CREDS_EUID, &creds)) < 0)
        return r;
    if ((r = sd_bus_creds_get_euid(creds, &euid)) < 0) {
        sd_bus_creds_unref(creds);
        return r;
    }
    sd_bus_creds_unref(creds);
    if (euid != 0)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_ACCESS_DENIED,
            "Only root may switch virtual terminals");
    r = switch_to_vt(sd_bus_message_get_bus(m), userdata, seat_id,
        (unsigned int)vt_number);
    if (r == -EOPNOTSUPP)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_NOT_SUPPORTED,
            "Virtual terminal switching is unavailable on this Seat");
    if (r < 0)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED,
            "VT switch failed (%d)", r);
    return sd_bus_reply_method_return(m, "");
}


static int seat_lookup(const char *path, void *userdata, char *id, size_t cap)
{
    size_t i; const char *known = NULL; int r;
    if ((r=path_id(path,SEAT_PATH,id,cap))<0) return r;
    for (i=0;i<dl_manager_get_seat_count(userdata);i++)
        if (dl_manager_get_seat_at(userdata,i,&known)==DL_OK && !strcmp(id,known)) return 0;
    return -ENOENT;
}
static int prop_seat_id(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    char id[128]; int r; (void)b;(void)i;(void)n;(void)e;
    if ((r=seat_lookup(p,u,id,sizeof(id)))<0) return r;
    return sd_bus_message_append(m,"s",id);
}
static int prop_seat_active(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    char id[128],obj[256]="/"; const char *active=NULL; int r;
    (void)b;(void)i;(void)n;(void)e;
    if ((r=seat_lookup(p,u,id,sizeof(id)))<0) return r;
    r=dl_manager_get_active_session(u,id,&active);
    if (r==DL_OK && (r=make_path(SESSION_PATH,active,obj,sizeof(obj)))<0) return r;
    return sd_bus_message_append(m,"(so)",r==DL_OK?active:"",obj);
}
static int prop_seat_capability(sd_bus *b,const char *p,const char *i,const char *n,
    sd_bus_message *m,void *u,sd_bus_error *e)
{
    char id[128]; dl_seat_capabilities c={0}; int r;
    (void)b;(void)i;(void)e;
    if ((r=seat_lookup(p,u,id,sizeof(id)))<0) return r;
    if ((r=dl_manager_get_seat_capabilities(u,id,&c))!=DL_OK) return -ENOENT;
    return sd_bus_message_append(m,"b",!strcmp(n,"CanTTY")?c.has_tty:c.has_graphical);
}


static int prop_seat_sessions(sd_bus *b, const char *path,
    const char *interface, const char *property, sd_bus_message *reply,
    void *userdata, sd_bus_error *error)
{
    char seat_id[128];
    size_t i, count;
    int r;
    (void)b; (void)interface; (void)property; (void)error;
    if ((r = seat_lookup(path, userdata, seat_id, sizeof(seat_id))) < 0)
        return r;
    count = dl_manager_get_session_count(userdata);
    if ((r = sd_bus_message_open_container(reply, 'a', "(so)")) < 0)
        return r;
    for (i = 0; i < count; i++) {
        dl_session_properties p = {0};
        char object[256];
        if (dl_manager_get_session_at(userdata, i, &p) != DL_OK)
            return -EIO;
        if (p.seat_id == NULL || strcmp(p.seat_id, seat_id) != 0)
            continue;
        if ((r = make_path(SESSION_PATH, p.session_id, object,
                sizeof(object))) < 0) return r;
        if ((r = sd_bus_message_append(reply, "(so)", p.session_id,
                object)) < 0) return r;
    }
    return sd_bus_message_close_container(reply);
}

static int activate_session_on_seat(sd_bus *bus, dl_manager *manager,
    const char *seat_id, const char *session_id)
{
    const char *old_active = NULL;
    char old_id[128] = "";
    char seat_path[256], session_path[256];
    int r;
    if (dl_manager_get_active_session(manager, seat_id, &old_active) == DL_OK &&
        old_active != NULL) {
        if (snprintf(old_id, sizeof(old_id), "%s", old_active) >=
            (int)sizeof(old_id)) return -ENAMETOOLONG;
    }
    r = dl_manager_set_active_session(manager, seat_id, session_id);
    if (r != DL_OK) return -EINVAL;
    if ((r = make_path(SEAT_PATH, seat_id, seat_path,
            sizeof(seat_path))) < 0) return r;
    if ((r = sd_bus_emit_properties_changed(bus, seat_path,
            BUS_NAME ".Seat", "ActiveSession", NULL)) < 0) return r;
    if (old_id[0] != '\0' && strcmp(old_id, session_id) != 0) {
        if ((r = make_path(SESSION_PATH, old_id, session_path,
                sizeof(session_path))) < 0) return r;
        if ((r = sd_bus_emit_properties_changed(bus, session_path,
                BUS_NAME ".Session", "Active", NULL)) < 0) return r;
    }
    if ((r = make_path(SESSION_PATH, session_id, session_path,
            sizeof(session_path))) < 0) return r;
    return sd_bus_emit_properties_changed(bus, session_path,
        BUS_NAME ".Session", "Active", NULL);
}

static int method_seat_activate_session(sd_bus_message *m, void *userdata,
    sd_bus_error *error)
{
    char seat_id[128];
    const char *session_id = NULL;
    dl_session_properties p = {0};
    sd_bus_creds *creds = NULL;
    uid_t euid;
    int r;
    (void)error;
    if ((r = seat_lookup(sd_bus_message_get_path(m), userdata, seat_id,
            sizeof(seat_id))) < 0)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_UNKNOWN_OBJECT,
            "Seat does not exist");
    if ((r = sd_bus_message_read(m, "s", &session_id)) < 0) return r;
    if (dl_manager_get_session_properties(userdata, session_id, &p) != DL_OK ||
        p.seat_id == NULL || strcmp(p.seat_id, seat_id) != 0)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_UNKNOWN_OBJECT,
            "Session does not belong to this Seat");
    if ((r = sd_bus_query_sender_creds(m, SD_BUS_CREDS_EUID, &creds)) < 0)
        return r;
    if ((r = sd_bus_creds_get_euid(creds, &euid)) < 0) {
        sd_bus_creds_unref(creds);
        return r;
    }
    sd_bus_creds_unref(creds);
    if (euid != 0 && (uint32_t)euid != p.uid)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_ACCESS_DENIED,
            "Only root or the Session owner may activate this Session");
    r = activate_session_on_seat(sd_bus_message_get_bus(m), userdata,
        seat_id, session_id);
    if (r < 0) return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED,
        "Could not activate Session (%d)", r);
    return sd_bus_reply_method_return(m, "");
}


static int auth_session(sd_bus_message *m,dl_manager *mgr,char *id,size_t cap,
 uint32_t *uid,const char **sender,sd_bus_creds **creds)
{
 dl_session_properties p={0}; uid_t euid; int r;
 if(path_id(sd_bus_message_get_path(m),SESSION_PATH,id,cap)<0)return -ENOENT;
 if(dl_manager_get_session_properties(mgr,id,&p)!=DL_OK)return -ENOENT;
 *sender=sd_bus_message_get_sender(m); if(!*sender)return -EACCES;
 r=sd_bus_query_sender_creds(m,SD_BUS_CREDS_EUID,creds); if(r<0)return r;
 r=sd_bus_creds_get_euid(*creds,&euid); if(r<0){sd_bus_creds_unref(*creds);*creds=NULL;return r;}
 *uid=(uint32_t)euid;
 if(*uid!=0U&&*uid!=p.uid){sd_bus_creds_unref(*creds);*creds=NULL;return -EACCES;}
 return 0;
}
static int method_activate(sd_bus_message*m,void*u,sd_bus_error*e)
{
 char id[128];uint32_t uid;const char*sender;sd_bus_creds*c=NULL;dl_session_properties p={0};int r;(void)e;(void)sender;
 r=auth_session(m,u,id,sizeof(id),&uid,&sender,&c);if(r<0)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"Not authorized");
 r=dl_manager_get_session_properties(u,id,&p);
 if(r==DL_OK&&p.seat_id)r=activate_session_on_seat(sd_bus_message_get_bus(m),u,p.seat_id,id);else r=-EINVAL;
 sd_bus_creds_unref(c);if(r<0)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_FAILED,"Activate failed (%d)",r);
 return sd_bus_reply_method_return(m,"");
}
static int method_lock_signal(sd_bus_message*m,void*u,bool lock)
{
 char id[128];uint32_t uid;const char*sender;sd_bus_creds*c=NULL;int r;
 r=auth_session(m,u,id,sizeof(id),&uid,&sender,&c);if(r<0)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"Not authorized");
 sd_bus_creds_unref(c);
 r=sd_bus_emit_signal(sd_bus_message_get_bus(m),sd_bus_message_get_path(m),BUS_NAME ".Session",lock?"Lock":"Unlock","");
 if(r<0)return r;
 return sd_bus_reply_method_return(m,"");
}
static int method_lock(sd_bus_message*m,void*u,sd_bus_error*e){(void)e;return method_lock_signal(m,u,true);}
static int method_unlock(sd_bus_message*m,void*u,sd_bus_error*e){(void)e;return method_lock_signal(m,u,false);}
static int method_take_control(sd_bus_message*m,void*u,sd_bus_error*e)
{
 char id[128];uint32_t uid;const char*sender;sd_bus_creds*c=NULL;int force,r;(void)e;
 if((r=sd_bus_message_read(m,"b",&force))<0)return r;
 if((r=auth_session(m,u,id,sizeof(id),&uid,&sender,&c))<0)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"Not authorized");
 r=dl_manager_take_control(u,id,sender,uid,force!=0);sd_bus_creds_unref(c);
 if(r!=DL_OK)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_FAILED,"TakeControl failed (%d)",r);
 return sd_bus_reply_method_return(m,"");
}
static int method_release_control(sd_bus_message*m,void*u,sd_bus_error*e)
{
 char id[128];uint32_t uid;const char*sender;sd_bus_creds*c=NULL;int r;(void)e;
 if((r=auth_session(m,u,id,sizeof(id),&uid,&sender,&c))<0)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"Not authorized");
 r=dl_manager_release_control(u,id,sender);sd_bus_creds_unref(c);
 if(r!=DL_OK)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_FAILED,"ReleaseControl failed (%d)",r);
 return sd_bus_reply_method_return(m,"");
}
static int method_take_device(sd_bus_message*m,void*u,sd_bus_error*e)
{
 char id[128];uint32_t uid,maj,min;const char*sender;sd_bus_creds*c=NULL;int fd=-1,r;bool inactive=false;(void)e;
 if((r=sd_bus_message_read(m,"uu",&maj,&min))<0)return r;
 if((r=auth_session(m,u,id,sizeof(id),&uid,&sender,&c))<0)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"Not authorized");
 r=dl_manager_take_device_fd(u,id,sender,maj,min,&fd,&inactive);sd_bus_creds_unref(c);
 if(r!=DL_OK)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_FAILED,"TakeDevice failed (%d)",r);
 r=sd_bus_reply_method_return(m,"hb",fd,inactive);(void)close(fd);return r;
}
static int method_release_device(sd_bus_message*m,void*u,sd_bus_error*e)
{
 char id[128];uint32_t uid,maj,min;const char*sender;sd_bus_creds*c=NULL;int r;(void)e;
 if((r=sd_bus_message_read(m,"uu",&maj,&min))<0)return r;
 if((r=auth_session(m,u,id,sizeof(id),&uid,&sender,&c))<0)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"Not authorized");
 r=dl_manager_session_is_controller(u,id,sender);
 if(r==DL_OK)r=dl_manager_release_device(u,id,maj,min);
 sd_bus_creds_unref(c);if(r!=DL_OK)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_FAILED,"ReleaseDevice failed (%d)",r);
 return sd_bus_reply_method_return(m,"");
}
static int method_pause_complete(sd_bus_message*m,void*u,sd_bus_error*e)
{
 char id[128];uint32_t uid,maj,min;const char*sender;sd_bus_creds*c=NULL;int r;(void)e;
 if((r=sd_bus_message_read(m,"uu",&maj,&min))<0)return r;
 if((r=auth_session(m,u,id,sizeof(id),&uid,&sender,&c))<0)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_ACCESS_DENIED,"Not authorized");
 r=dl_manager_session_is_controller(u,id,sender);
 if(r==DL_OK)r=dl_manager_complete_device_pause(u,id,maj,min);
 sd_bus_creds_unref(c);if(r!=DL_OK)return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_FAILED,"PauseDeviceComplete failed (%d)",r);
 return sd_bus_reply_method_return(m,"");
}
static int kill_session_processes(dl_manager *manager, const char *session_id,
    const char *who, int signal_number)
{
    dl_session_properties p = {0};
    int pidfd = -1, root_fd = -1, cgroup_fd = -1, r = -1;
    if (who == NULL || (strcmp(who, "leader") != 0 && strcmp(who, "all") != 0) ||
        signal_number <= 0 || signal_number > SIGRTMAX) {
        errno = EINVAL;
        return -1;
    }
    if (dl_manager_get_session_properties(manager, session_id, &p) != DL_OK) {
        errno = ENOENT;
        return -1;
    }
    if (dl_manager_dup_session_pidfd(manager, session_id, &pidfd) != DL_OK) {
        errno = ENOTSUP;
        return -1;
    }
    if (strcmp(who, "leader") == 0) {
        r = dl_session_pidfd_send_signal(pidfd, signal_number);
        goto done;
    }
    /* cgroup.kill is an unconditional SIGKILL operation, not a general
     * signal API. Never translate another signal into it. */
    if (signal_number != SIGKILL || p.cgroup_path == NULL ||
        p.cgroup_path[0] == '\0') {
        errno = ENOTSUP;
        goto done;
    }
    root_fd = open("/sys/fs/cgroup", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0) goto done;
    cgroup_fd = dl_session_cgroup_openat(root_fd, p.cgroup_path);
    if (cgroup_fd < 0) goto done;
    r = dl_session_cgroup_kill_all(cgroup_fd, p.leader_pid, pidfd);
done:
    {
        int saved_errno = errno;
        if (cgroup_fd >= 0) (void) close(cgroup_fd);
        if (root_fd >= 0) (void) close(root_fd);
        (void) close(pidfd);
        errno = saved_errno;
    }
    return r;
}

static int kill_error_reply(sd_bus_message *m, int error_number)
{
    if (error_number == EINVAL)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS,
            "Invalid process target or signal");
    if (error_number == ENOENT || error_number == ESRCH)
        return sd_bus_reply_method_errorf(m, "org.freedesktop.login1.NoSuchSession",
            "Session leader or registered cgroup membership no longer exists");
    if (error_number == EACCES || error_number == EPERM)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_ACCESS_DENIED,
            "Permission denied for the requested process operation");
    if (error_number == ENOTSUP || error_number == EOPNOTSUPP ||
        error_number == ENOSYS)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_NOT_SUPPORTED,
            "Requested process operation is not supported safely");
    return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_FAILED,
        "Process operation failed (%d)", error_number);
}

static int method_session_kill(sd_bus_message *m, void *userdata, sd_bus_error *error)
{
    char id[128];
    uint32_t uid;
    const char *sender;
    const char *who = NULL;
    sd_bus_creds *creds = NULL;
    int signal_number, r;
    (void) error;
    if ((r = auth_session(m, userdata, id, sizeof(id), &uid, &sender, &creds)) < 0)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_ACCESS_DENIED,
            "Not authorized to kill this Session");
    r = sd_bus_message_read(m, "si", &who, &signal_number);
    sd_bus_creds_unref(creds);
    if (r < 0) return r;
    if (kill_session_processes(userdata, id, who, signal_number) < 0)
        return kill_error_reply(m, errno);
    return sd_bus_reply_method_return(m, "");
}

static int method_manager_kill_session(sd_bus_message *m, void *userdata,
    sd_bus_error *error)
{
    const char *session_id = NULL, *who = NULL;
    dl_session_properties p = {0};
    sd_bus_creds *creds = NULL;
    uid_t euid;
    int signal_number, r;
    (void) error;
    if ((r = sd_bus_message_read(m, "ssi", &session_id, &who,
            &signal_number)) < 0) return r;
    if (dl_manager_get_session_properties(userdata, session_id, &p) != DL_OK)
        return sd_bus_reply_method_errorf(m,
            "org.freedesktop.login1.NoSuchSession", "Session does not exist");
    if ((r = sd_bus_query_sender_creds(m, SD_BUS_CREDS_EUID, &creds)) < 0)
        return r;
    r = sd_bus_creds_get_euid(creds, &euid);
    sd_bus_creds_unref(creds);
    if (r < 0) return r;
    if (euid != 0 && (uint32_t)euid != p.uid)
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_ACCESS_DENIED,
            "Only root or the Session owner may kill this Session");
    if (kill_session_processes(userdata, session_id, who, signal_number) < 0)
        return kill_error_reply(m, errno);
    return sd_bus_reply_method_return(m, "");
}

static int method_termination_unsupported(sd_bus_message*m,void*u,sd_bus_error*e)
{(void)u;(void)e;return sd_bus_reply_method_errorf(m,SD_BUS_ERROR_NOT_SUPPORTED,"Graceful asynchronous Session termination is not implemented");}
static int user_path_uid(const char*p,uint32_t*uid)
{
 const char*prefix=ROOT_PATH "/user/_";char*end=NULL;unsigned long v;
 if(strncmp(p,prefix,strlen(prefix))||!p[strlen(prefix)]||strchr(p+strlen(prefix),'/'))return -EINVAL;
 v=strtoul(p+strlen(prefix),&end,10);if(!end||*end||v>UINT32_MAX)return -EINVAL;*uid=(uint32_t)v;return 0;
}
static int prop_user_uid(sd_bus*b,const char*p,const char*i,const char*n,sd_bus_message*m,void*u,sd_bus_error*e)
{uint32_t uid;(void)b;(void)i;(void)n;(void)u;(void)e;if(user_path_uid(p,&uid)<0)return -ENOENT;return sd_bus_message_append(m,"u",uid);}
static int prop_user_name(sd_bus*b,const char*p,const char*i,const char*n,sd_bus_message*m,void*u,sd_bus_error*e)
{uint32_t uid;char buf[16384];int r;(void)b;(void)i;(void)n;(void)u;(void)e;if(user_path_uid(p,&uid)<0)return -ENOENT;if((r=user_name(uid,buf,sizeof(buf)))<0)return r;return sd_bus_message_append(m,"s",buf);}

static int prop_user_sessions(sd_bus *b, const char *path, const char *interface,
    const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *error)
{
    uint32_t uid;
    size_t i, count;
    int r;
    (void)b; (void)interface; (void)property; (void)error;
    if (user_path_uid(path, &uid) < 0) return -ENOENT;
    count = dl_manager_get_session_count(userdata);
    if ((r = sd_bus_message_open_container(reply, 'a', "(so)")) < 0)
        return r;
    for (i = 0; i < count; i++) {
        dl_session_properties p = {0};
        char object[256];
        if (dl_manager_get_session_at(userdata, i, &p) != DL_OK)
            return -EIO;
        if (p.uid != uid) continue;
        if ((r = make_path(SESSION_PATH, p.session_id, object,
                sizeof(object))) < 0) return r;
        if ((r = sd_bus_message_append(reply, "(so)", p.session_id,
                object)) < 0) return r;
    }
    return sd_bus_message_close_container(reply);
}


static const sd_bus_vtable manager_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ListSessions","","a(susso)",list_sessions,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetSession","s","o",get_session,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetSessionByPID","u","o",get_session_pid,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetUser","u","o",get_user,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetUserByPID","u","o",get_user_by_pid,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ListUsers","","a(uso)",list_users,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ListSeats","","a(so)",list_seats,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetSeat","s","o",get_seat,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("TerminateSession","s","",method_termination_unsupported,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("KillSession","ssi","",method_manager_kill_session,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable session_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Id","s",prop_session_id,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("User","(uo)",prop_session_user,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Name","s",prop_session_name,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Seat","(so)",prop_session_seat,0,SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("TTY","s",prop_session_text,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Display","s",prop_session_text,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Remote","b",prop_session_bool,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Service","s",prop_session_text,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Desktop","s",prop_session_text,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Leader","u",prop_session_leader,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("CGroup","s",prop_session_cgroup,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("VTNr","u",prop_session_vtnr,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Type","s",prop_session_text,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Class","s",prop_session_text,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Active","b",prop_session_bool,0,SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("Activate","","",method_activate,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Lock","","",method_lock,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Unlock","","",method_unlock,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Terminate","","",method_termination_unsupported,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Kill","si","",method_session_kill,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("TakeControl","b","",method_take_control,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ReleaseControl","","",method_release_control,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("TakeDevice","uu","hb",method_take_device,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ReleaseDevice","uu","",method_release_device,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("PauseDeviceComplete","uu","",method_pause_complete,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable user_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("UID","u",prop_user_uid,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Name","s",prop_user_name,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Sessions","a(so)",prop_user_sessions,0,SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END
};
static const sd_bus_vtable seat_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Id","s",prop_seat_id,0,SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("ActiveSession","(so)",prop_seat_active,0,SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanTTY","b",prop_seat_capability,0,SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("CanGraphical","b",prop_seat_capability,0,SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Sessions","a(so)",prop_seat_sessions,0,SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_METHOD("ActivateSession","s","",method_seat_activate_session,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SwitchTo","u","",method_seat_switch_to,SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

static int login1_open_internal(dl_login1 **out, dl_manager *manager,
    const char *address)
{
    dl_login1 *s; int r;
    if (!out || !manager) return -EINVAL;
    *out=NULL;
    s=calloc(1,sizeof(*s)); if (!s) return -ENOMEM;
    if (address == NULL) {
        r=sd_bus_open_system(&s->bus);
    } else {
        r=sd_bus_new(&s->bus);
        if (r >= 0) r=sd_bus_set_address(s->bus,address);
        if (r >= 0) r=sd_bus_set_bus_client(s->bus,1);
        if (r >= 0) r=sd_bus_start(s->bus);
    }
    if (r<0) goto fail;
    r=sd_bus_add_object_vtable(s->bus,&s->manager_slot,ROOT_PATH,
        BUS_NAME ".Manager",manager_vtable,manager); if (r<0) goto fail;
    r=sd_bus_add_fallback_vtable(s->bus,&s->session_slot,SESSION_PATH,
        BUS_NAME ".Session",session_vtable,NULL,manager); if (r<0) goto fail;
    r=sd_bus_add_fallback_vtable(s->bus,&s->user_slot,ROOT_PATH "/user",BUS_NAME ".User",user_vtable,NULL,manager); if(r<0) goto fail;
    r=sd_bus_add_fallback_vtable(s->bus,&s->seat_slot,SEAT_PATH,
        BUS_NAME ".Seat",seat_vtable,NULL,manager); if (r<0) goto fail;
    r=sd_bus_request_name(s->bus,BUS_NAME,0); if (r<0) goto fail;
    *out=s; return 0;
fail:
    dl_login1_close(s); return r;
}
int dl_login1_open(dl_login1 **out, dl_manager *manager)
{
    return login1_open_internal(out, manager, NULL);
}
int dl_login1_open_address(dl_login1 **out, dl_manager *manager,
    const char *address)
{
    if (address == NULL || address[0] == '\0') return -EINVAL;
    return login1_open_internal(out, manager, address);
}
int dl_login1_get_fd(const dl_login1 *s)
{ return s && s->bus ? sd_bus_get_fd(s->bus) : -1; }
short dl_login1_get_events(const dl_login1 *s)
{ int r; if (!s||!s->bus) return 0; r=sd_bus_get_events(s->bus); return r<0?0:(short)r; }
int dl_login1_dispatch(dl_login1 *s)
{
    int r; if (!s||!s->bus) return -EINVAL;
    do { r=sd_bus_process(s->bus,NULL); } while (r>0);
    return r<0?r:0;
}
void dl_login1_close(dl_login1 *s)
{
    if (!s) return;
    s->seat_slot=sd_bus_slot_unref(s->seat_slot);
    s->user_slot=sd_bus_slot_unref(s->user_slot);
    s->session_slot=sd_bus_slot_unref(s->session_slot);
    s->manager_slot=sd_bus_slot_unref(s->manager_slot);
    s->bus=sd_bus_flush_close_unref(s->bus);
    free(s);
}
