#ifndef DINIT_LOGIND_PAM_LIFECYCLE_H
#define DINIT_LOGIND_PAM_LIFECYCLE_H
#include "session/registration.h"
typedef int (*dl_pam_session_transport_fn)(void *, const dl_session_registration_request *);
typedef struct dl_pam_session_state { char session_id[65]; } dl_pam_session_state;
int dl_pam_session_open(dl_pam_session_state *, const dl_session_registration_request *, dl_pam_session_transport_fn, void *);
int dl_pam_session_close(dl_pam_session_state *, dl_pam_session_transport_fn, void *);
#endif
