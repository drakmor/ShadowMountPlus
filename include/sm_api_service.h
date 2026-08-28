#ifndef SM_API_SERVICE_H
#define SM_API_SERVICE_H

#include <stdbool.h>

bool sm_api_service_start(void);
void sm_api_service_stop(void);
bool sm_api_service_reconfigure(void);
// Close active sockets on sleep entry and wake the listener on resume.
void sm_api_service_on_sleep_change(bool active);

#endif
