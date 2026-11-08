#ifndef DINIT_LOGIND_DBUS_LOGIN1_H
#define DINIT_LOGIND_DBUS_LOGIN1_H
#include "core/manager.h"
typedef struct dl_login1 dl_login1;
int dl_login1_open(dl_login1 **out_login1, dl_manager *manager);
int dl_login1_get_fd(const dl_login1 *login1);
short dl_login1_get_events(const dl_login1 *login1);
int dl_login1_dispatch(dl_login1 *login1);
void dl_login1_close(dl_login1 *login1);
#endif
