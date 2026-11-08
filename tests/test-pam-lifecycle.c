#include "pam/lifecycle.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct fake { int fail; unsigned int opens,closes; char last_id[65]; } fake;
static int transport(void *data,const dl_session_registration_request *r)
{
    fake *f=data;
    if(f->fail)return -1;
    if(r->operation==DL_SESSION_REGISTRATION_OPEN)f->opens++;
    else if(r->operation==DL_SESSION_REGISTRATION_CLOSE)f->closes++;
    else return -1;
    memcpy(f->last_id,r->session_id,sizeof(f->last_id)); return 0;
}
static void make_open(dl_session_registration_request *r,const char *id)
{
    memset(r,0,sizeof(*r));r->magic=DL_SESSION_REGISTRATION_MAGIC;
    r->version=DL_SESSION_REGISTRATION_VERSION;r->operation=DL_SESSION_REGISTRATION_OPEN;
    (void)snprintf(r->session_id,sizeof(r->session_id),"%s",id);
}
static void test_open_state(void)
{
    dl_pam_session_state s={{0}};dl_session_registration_request r;fake f={0};
    make_open(&r,"pam-session-1");f.fail=1;
    assert(dl_pam_session_open(&s,&r,transport,&f)!=0&&s.session_id[0]=='\0');
    f.fail=0;assert(dl_pam_session_open(&s,&r,transport,&f)==0);
    assert(strcmp(s.session_id,"pam-session-1")==0&&f.opens==1U);
    assert(dl_pam_session_open(&s,&r,transport,&f)!=0&&f.opens==1U);
}
static void test_close_retry(void)
{
    dl_pam_session_state s={{0}};dl_session_registration_request r;fake f={0};
    make_open(&r,"pam-session-2");assert(dl_pam_session_open(&s,&r,transport,&f)==0);
    f.fail=1;assert(dl_pam_session_close(&s,transport,&f)!=0);
    assert(strcmp(s.session_id,"pam-session-2")==0);
    f.fail=0;assert(dl_pam_session_close(&s,transport,&f)==0);
    assert(s.session_id[0]=='\0'&&f.closes==1U);
    assert(strcmp(f.last_id,"pam-session-2")==0);
    assert(dl_pam_session_close(&s,transport,&f)==0&&f.closes==1U);
}
static void test_invalid_id(void)
{
    dl_pam_session_state s={{0}};dl_session_registration_request r;fake f={0};
    make_open(&r,"../bad");assert(dl_pam_session_open(&s,&r,transport,&f)!=0);
    assert(f.opens==0U);
}
int main(void)
{
    test_open_state();test_close_retry();test_invalid_id();
    puts("PAM lifecycle tests passed");return 0;
}
