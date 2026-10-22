#ifndef SM_APPINST_H
#define SM_APPINST_H

#include <stdbool.h>

int sm_appinst_initialize(void);
int sm_appinst_install_by_package(const char *uri, const char *title_name);
int sm_appinst_install_all(void *reserved);
int sm_appinst_uninstall(const char *title_id);

#endif
