#include "sm_platform.h"

#include <pthread.h>

#include "sm_appinst.h"
#include "sm_log.h"

#define SM_APPINST_SHELLCORE_AUTHID UINT64_C(0x3800000000000010)
#define SM_APPINST_SYSTEM_AUTHID UINT64_C(0x4801000000000013)

typedef char sm_playgo_scenario_id_t[3];
typedef char sm_language_t[8];
typedef char sm_content_id_t[0x30];

typedef struct {
  sm_content_id_t content_id;
  int content_type;
  int content_platform;
} sm_app_install_pkg_info_t;

typedef struct {
  const char *uri;
  const char *ex_uri;
  const char *playgo_scenario_id;
  const char *content_id;
  const char *content_name;
  const char *icon_url;
  uint64_t unknown_30;
  uint64_t reserved[7];
} sm_appinst_meta_info_t;

_Static_assert(sizeof(sm_appinst_meta_info_t) == 0x70,
               "InstallByPackage MetaInfo layout mismatch");

typedef struct {
  sm_language_t languages[30];
  sm_playgo_scenario_id_t playgo_scenario_ids[64];
  sm_content_id_t content_ids[64];
  unsigned char unknown[6480];
} sm_playgo_info_t;

extern int sceAppInstUtilInstallByPackage(sm_appinst_meta_info_t *meta,
                                          sm_app_install_pkg_info_t *pkg_info,
                                          sm_playgo_info_t *playgo);

static pthread_mutex_t g_appinst_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_appinst_initialized;

static uint64_t acquire_authid(uint64_t target, bool *changed) {
  uint64_t saved = kernel_get_ucred_authid(getpid());
  *changed = false;
  if (saved == 0 || saved == target)
    return saved;
  if (kernel_set_ucred_authid(getpid(), target) == 0)
    *changed = true;
  return saved;
}

static bool restore_authid(uint64_t saved, bool changed) {
  if (!changed)
    return true;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (kernel_set_ucred_authid(getpid(), saved) == 0 &&
        kernel_get_ucred_authid(getpid()) == saved)
      return true;
  }
  return false;
}

int sm_appinst_initialize(void) {
  int result;
  pthread_mutex_lock(&g_appinst_mutex);
  if (g_appinst_initialized) {
    pthread_mutex_unlock(&g_appinst_mutex);
    return 0;
  }
  bool changed = false;
  uint64_t saved = acquire_authid(SM_APPINST_SYSTEM_AUTHID, &changed);
  result = sceAppInstUtilInitialize();
  if (!restore_authid(saved, changed))
    log_debug("  [APPINST] failed to restore authid after initialize");
  if (result == 0)
    g_appinst_initialized = true;
  pthread_mutex_unlock(&g_appinst_mutex);
  return result;
}

int sm_appinst_install_by_package(const char *uri, const char *title_name) {
  int result;
  sm_appinst_meta_info_t meta;
  sm_app_install_pkg_info_t pkg_info;
  sm_playgo_info_t playgo;
  bool changed = false;
  uint64_t target = sm_firmware_major_version() < 11u
                        ? SM_APPINST_SHELLCORE_AUTHID
                        : SM_APPINST_SYSTEM_AUTHID;
  uint64_t saved = 0;
  if (!uri || uri[0] == '\0')
    return EINVAL;

  pthread_mutex_lock(&g_appinst_mutex);
  if (!g_appinst_initialized) {
    bool init_changed = false;
    uint64_t init_saved = acquire_authid(SM_APPINST_SYSTEM_AUTHID,
                                         &init_changed);
    int init_result = sceAppInstUtilInitialize();
    if (!restore_authid(init_saved, init_changed))
      log_debug("  [APPINST] failed to restore authid after initialize");
    if (init_result != 0) {
      pthread_mutex_unlock(&g_appinst_mutex);
      return init_result;
    }
    g_appinst_initialized = true;
  }

  memset(&meta, 0, sizeof(meta));
  memset(&pkg_info, 0, sizeof(pkg_info));
  memset(&playgo, 0, sizeof(playgo));
  meta.uri = uri;
  meta.ex_uri = "";
  meta.playgo_scenario_id = "";
  meta.content_id = "";
  meta.content_name = title_name ? title_name : "";
  meta.icon_url = "";

  saved = acquire_authid(target, &changed);
  result = sceAppInstUtilInstallByPackage(&meta, &pkg_info, &playgo);
  if (!restore_authid(saved, changed))
    log_debug("  [APPINST] failed to restore authid after InstallByPackage");
  sceKernelUsleep(200000);
  pthread_mutex_unlock(&g_appinst_mutex);
  return result;
}

int sm_appinst_install_all(void *reserved) {
  int result;
  pthread_mutex_lock(&g_appinst_mutex);
  result = sceAppInstUtilAppInstallAll(reserved);
  sceKernelUsleep(200000);
  pthread_mutex_unlock(&g_appinst_mutex);
  return result;
}

int sm_appinst_uninstall(const char *title_id) {
  int result;
  pthread_mutex_lock(&g_appinst_mutex);
  result = sceAppInstUtilAppUnInstall(title_id);
  sceKernelUsleep(200000);
  pthread_mutex_unlock(&g_appinst_mutex);
  return result;
}
