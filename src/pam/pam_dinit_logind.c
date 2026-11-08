#define _GNU_SOURCE
#include "session/registration.h"
#include <security/pam_modules.h>
#include <errno.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define DATA_KEY "dinit-logind-session-id"
#define DEFAULT_PATH "/run/dinit-logind/session.sock"

static void free_id(pam_handle_t *p, void *d, int s)
{ (void)p; (void)s; free(d); }

static const char *socket_path(int n, const char **v)
{
    int i;
    for (i=0;i<n;i++) if (strncmp(v[i],"socket=",7)==0) {
        const char *p=v[i]+7;
        if (p[0]!='/' || strlen(p)>=sizeof(((struct sockaddr_un *)0)->sun_path)) return NULL;
        return p;
    }
    return DEFAULT_PATH;
}

static int request_socket(const char *path, const dl_session_registration_request *q)
{
    struct sockaddr_un a;
    dl_session_registration_response r;
    socklen_t len;
    int fd;
    ssize_t n;
    struct timeval timeout;
    fd=socket(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0);
    if(fd<0)return -1;
    timeout.tv_sec=2; timeout.tv_usec=0;
    if(setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))!=0 ||
       setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))!=0) {
        close(fd); return -1;
    }
    memset(&a,0,sizeof(a)); a.sun_family=AF_UNIX;
    memcpy(a.sun_path,path,strlen(path)+1U);
    len=(socklen_t)(offsetof(struct sockaddr_un,sun_path)+strlen(path)+1U);
    if(connect(fd,(struct sockaddr *)&a,len)<0){close(fd);return -1;}
    n=send(fd,q,sizeof(*q),MSG_NOSIGNAL);
    if(n!=(ssize_t)sizeof(*q)){close(fd);return -1;}
    n=recv(fd,&r,sizeof(r),0); close(fd);
    if(n!=(ssize_t)sizeof(r)||r.magic!=DL_SESSION_REGISTRATION_MAGIC||
       r.version!=DL_SESSION_REGISTRATION_VERSION||r.reserved!=0U||r.status!=DL_OK){
        errno=EPROTO;return -1;
    }
    return 0;
}

static int user_uid(const char *name, uid_t *uid)
{
    struct passwd pw,*found=NULL;
    long hint=sysconf(_SC_GETPW_R_SIZE_MAX);
    size_t size=hint>0?(size_t)hint:16384U;
    char *buf; int e;
    if(size>1048576U)size=1048576U;
    buf=malloc(size); if(!buf)return -1;
    e=getpwnam_r(name,&pw,buf,size,&found);
    if(e==0&&found)*uid=pw.pw_uid; else if(e==0)e=ENOENT;
    free(buf); if(e){errno=e;return -1;} return 0;
}

static int new_id(char out[65])
{
    unsigned char b[16]; static const char h[]="0123456789abcdef";
    size_t done=0,i;
    while(done<sizeof(b)){
        ssize_t n=getrandom(b+done,sizeof(b)-done,0);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return -1; done+=(size_t)n;
    }
    memcpy(out,"pam-",4);
    for(i=0;i<sizeof(b);i++){out[4+2*i]=h[b[i]>>4];out[5+2*i]=h[b[i]&15];}
    out[36]='\0'; return 0;
}

static void close_request(dl_session_registration_request *q)
{
    q->operation=DL_SESSION_REGISTRATION_CLOSE;q->uid=0;q->leader_pid=0;q->flags=0;
    memset(q->seat_id,0,sizeof(q->seat_id));memset(q->type,0,sizeof(q->type));
    memset(q->class_name,0,sizeof(q->class_name));memset(q->service,0,sizeof(q->service));
    memset(q->desktop,0,sizeof(q->desktop));memset(q->tty,0,sizeof(q->tty));
    memset(q->display,0,sizeof(q->display));
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t *p,int flags,int argc,const char **argv)
{
    const char *path=socket_path(argc,argv),*user=NULL,*svc=NULL,*tty=NULL,*host=NULL;
    const void *old=NULL; uid_t uid; dl_session_registration_request q; char *saved; int st;
    (void)flags;
    if(!path)return PAM_SESSION_ERR;
    if(pam_get_data(p,DATA_KEY,&old)==PAM_SUCCESS&&old)return PAM_SESSION_ERR;
    if(pam_get_user(p,&user,NULL)!=PAM_SUCCESS||!user||!user[0]||user_uid(user,&uid)<0)
        return PAM_USER_UNKNOWN;
    (void)pam_get_item(p,PAM_SERVICE,(const void **)&svc);
    (void)pam_get_item(p,PAM_TTY,(const void **)&tty);
    (void)pam_get_item(p,PAM_RHOST,(const void **)&host);
    memset(&q,0,sizeof(q));q.magic=DL_SESSION_REGISTRATION_MAGIC;
    q.version=DL_SESSION_REGISTRATION_VERSION;q.operation=DL_SESSION_REGISTRATION_OPEN;
    q.uid=(uint32_t)uid;q.leader_pid=(uint32_t)getpid();
    if(new_id(q.session_id)<0)return PAM_SESSION_ERR;
    snprintf(q.class_name,sizeof(q.class_name),"user");
    snprintf(q.type,sizeof(q.type),"%s",tty&&tty[0]?"tty":"unspecified");
    if(svc)snprintf(q.service,sizeof(q.service),"%s",svc);
    if(tty)snprintf(q.tty,sizeof(q.tty),"%s",tty);
    if(host&&host[0])q.flags|=DL_SESSION_REGISTRATION_FLAG_REMOTE;
    if(request_socket(path,&q)<0)return PAM_SESSION_ERR;
    saved=strdup(q.session_id);
    if(!saved){close_request(&q);(void)request_socket(path,&q);return PAM_BUF_ERR;}
    st=pam_set_data(p,DATA_KEY,saved,free_id);
    if(st!=PAM_SUCCESS){close_request(&q);(void)request_socket(path,&q);free(saved);return st;}
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t *p,int flags,int argc,const char **argv)
{
    const char *path=socket_path(argc,argv);const void *data=NULL;
    dl_session_registration_request q;int st;(void)flags;
    if(!path)return PAM_SESSION_ERR;
    st=pam_get_data(p,DATA_KEY,&data);if(st!=PAM_SUCCESS||!data)return PAM_SUCCESS;
    if(strlen((const char *)data)>=sizeof(q.session_id))return PAM_SESSION_ERR;
    memset(&q,0,sizeof(q));q.magic=DL_SESSION_REGISTRATION_MAGIC;
    q.version=DL_SESSION_REGISTRATION_VERSION;q.operation=DL_SESSION_REGISTRATION_CLOSE;
    snprintf(q.session_id,sizeof(q.session_id),"%s",(const char *)data);
    if(request_socket(path,&q)<0)return PAM_SESSION_ERR;
    return pam_set_data(p,DATA_KEY,NULL,NULL)==PAM_SUCCESS?PAM_SUCCESS:PAM_SESSION_ERR;
}
