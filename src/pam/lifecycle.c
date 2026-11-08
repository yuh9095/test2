#include "pam/lifecycle.h"
#include <string.h>
static int valid_id(const char *id)
{
    size_t i;
    for (i=0U;i<65U;i++) {
        unsigned char ch=(unsigned char)id[i];
        if (ch=='\0') return i!=0U;
        if (!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||
              (ch>='0'&&ch<='9')||ch=='_'||ch=='-')) return 0;
    }
    return 0;
}
int dl_pam_session_open(dl_pam_session_state *state,
    const dl_session_registration_request *request,
    dl_pam_session_transport_fn transport, void *userdata)
{
    if (!state||!request||!transport||state->session_id[0]!='\0'||
        request->operation!=DL_SESSION_REGISTRATION_OPEN||
        !valid_id(request->session_id)) return -1;
    if (transport(userdata,request)!=0) return -1;
    memcpy(state->session_id,request->session_id,sizeof(state->session_id));
    return 0;
}
int dl_pam_session_close(dl_pam_session_state *state,
    dl_pam_session_transport_fn transport, void *userdata)
{
    dl_session_registration_request request;
    if (!state||!transport) return -1;
    if (state->session_id[0]=='\0') return 0;
    if (!valid_id(state->session_id)) return -1;
    memset(&request,0,sizeof(request));
    request.magic=DL_SESSION_REGISTRATION_MAGIC;
    request.version=DL_SESSION_REGISTRATION_VERSION;
    request.operation=DL_SESSION_REGISTRATION_CLOSE;
    memcpy(request.session_id,state->session_id,sizeof(request.session_id));
    if (transport(userdata,&request)!=0) return -1;
    memset(state->session_id,0,sizeof(state->session_id));
    return 0;
}
