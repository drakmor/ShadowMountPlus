#include "sm_platform.h"
#include "sm_config_mount.h"
#include "sm_runtime.h"
#include "sm_install.h"
#include "sm_install_queue.h"
#include "sm_types.h"
#include "sm_game_cache.h"
#include "sm_log.h"
#include "sm_filesystem.h"
#include "sm_limits.h"
#include "sm_path_utils.h"
#include "sm_appdb.h"
#include "sm_title_state.h"
#include "sm_image_cache.h"
#include "sm_paths.h"

#include <dlfcn.h>

typedef int (*app_install_title_dir_fn_t)(const char *title_id,
                                          const char *install_path,
                                          void *reserved);

static const char *const k_app_inst_util_sprx_path =
    "/system/common/lib/libSceAppInstUtil.sprx";
static const char *const k_app_install_title_dir_symbol =
    "sceAppInstUtilAppInstallTitleDir";

static app_install_title_dir_fn_t g_app_install_title_dir_fn = NULL;
static bool g_app_install_title_dir_resolve_attempted = false;

static app_install_title_dir_fn_t resolve_app_install_title_dir(void) {
  if (g_app_install_title_dir_fn)
    return g_app_install_title_dir_fn;
  if (g_app_install_title_dir_resolve_attempted)
    return NULL;
  if (sm_firmware_major_version() >= 12u) {
    g_app_install_title_dir_resolve_attempted = true;
    return NULL;
  }

  g_app_install_title_dir_resolve_attempted = true;
  void *app_inst_util_handle = dlopen(k_app_inst_util_sprx_path, RTLD_LAZY);
  if (!app_inst_util_handle) {
    const char *error = dlerror();
    log_debug("  [REG] dlopen(%s) failed: %s", k_app_inst_util_sprx_path,
              error ? error : "unknown");
    return NULL;
  }

  dlerror();
  g_app_install_title_dir_fn = (app_install_title_dir_fn_t)dlsym(
      app_inst_util_handle, k_app_install_title_dir_symbol);
  if (g_app_install_title_dir_fn)
    return g_app_install_title_dir_fn;

  const char *error = dlerror();
  log_debug("  [REG] %s lookup in %s failed: %s",
            k_app_install_title_dir_symbol, k_app_inst_util_sprx_path,
            error ? error : "unknown");
  return NULL;
}

static bool write_link_file(const char *path, const char *value) {
  FILE *f = fopen(path, "w");
  if (!f) {
    log_debug("  [LINK] open failed for %s: %s", path, strerror(errno));
    return false;
  }

  int saved_errno = 0;
  if (fprintf(f, "%s", value) < 0)
    saved_errno = errno;
  if (fflush(f) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (fclose(f) != 0 && saved_errno == 0)
    saved_errno = errno;

  if (saved_errno != 0) {
    errno = saved_errno;
    log_debug("  [LINK] write failed for %s: %s", path, strerror(errno));
    return false;
  }
  return true;
}

static bool is_appmeta_file(const char *name) {
  if (!name)
    return false;
  if (strcasecmp(name, "param.json") == 0 || strcasecmp(name, "param.sfo") == 0)
    return true;

  const char *ext = strrchr(name, '.');
  if (!ext)
    return false;

  return (strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".dds") == 0 ||
          strcasecmp(ext, ".at9") == 0);
}

static bool copy_sce_sys_to_appmeta(const char *src_sce_sys,
                                    const char *user_appmeta_dir) {
  DIR *d = opendir(src_sce_sys);
  if (!d)
    return false;

  bool ok = true;
  struct dirent *e;
  char src_path[MAX_PATH];
  char dst_path[MAX_PATH];
  struct stat st;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (!is_appmeta_file(e->d_name))
      continue;

    snprintf(src_path, sizeof(src_path), "%s/%s", src_sce_sys, e->d_name);
    if (stat(src_path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    snprintf(dst_path, sizeof(dst_path), "%s/%s", user_appmeta_dir, e->d_name);
    if (copy_file(src_path, dst_path) != 0) {
      ok = false;
      break;
    }
  }

  if (closedir(d) != 0)
    ok = false;
  return ok;
}

// --- Install/Remount Action ---
static bool mount_and_install(const char *src_path, const char *title_id,
                              const char *title_name, bool is_remount,
                              bool should_register,
                              bool use_app_install_all,
                              bool *has_src_snd0_out) {
  char user_appmeta_dir[MAX_PATH];
  char user_app_dir[MAX_PATH];
  char user_sce_sys[MAX_PATH];
  char src_sce_sys[MAX_PATH];
  char src_snd0[MAX_PATH];
  char image_source_path[MAX_PATH];
  bool has_image_source = false;
  bool appmeta_missing = false;
  bool metadata_restaged = false;
  bool restage_staging = false;
  bool restage_appmeta = false;
  bool has_src_snd0 = false;

  snprintf(user_appmeta_dir, sizeof(user_appmeta_dir), "%s/%s", APPMETA_BASE,
           title_id);
  snprintf(user_app_dir, sizeof(user_app_dir), "%s/%s", APP_BASE, title_id);

  if (is_under_image_mount_base(src_path)) {
    has_image_source = resolve_image_source_from_mount_cache(
        src_path, image_source_path, sizeof(image_source_path));
    if (!has_image_source) {
      log_debug("  [LINK] image source lookup failed for %s: %s", title_id,
                src_path);
    }
  }

  appmeta_missing = !has_appmeta_data(title_id);
  if (is_remount && appmeta_missing) {
    log_debug("  [REG] appmeta missing, restaging metadata only: %s", title_id);
  }

  restage_staging = (!is_remount || should_register);
  restage_appmeta = (!is_remount || appmeta_missing);

  if (should_pause_work())
    return false;

  // COPY FILES
  if (restage_staging || restage_appmeta) {
    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", src_path);
    snprintf(src_snd0, sizeof(src_snd0), "%s/snd0.at9", src_sce_sys);
    has_src_snd0 = path_exists(src_snd0);
  } else {
    log_debug("  [SPEED] Skipping file copy (Assets already exist)");
  }

  if (restage_staging) {
    mkdir(APP_BASE, 0777);
    mkdir(user_app_dir, 0777);
    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_sce_sys, 0777);
    if (copy_dir(src_sce_sys, user_sce_sys) != 0) {
      log_debug("  [COPY] Failed to copy sce_sys staging: %s -> %s", src_sce_sys,
                user_sce_sys);
      return false;
    }

    char icon_src[MAX_PATH];
    char icon_dst[MAX_PATH];
    snprintf(icon_src, sizeof(icon_src), "%s/icon0.png", src_sce_sys);
    snprintf(icon_dst, sizeof(icon_dst), "%s/icon0.png", user_app_dir);
    if (copy_file(icon_src, icon_dst) != 0) {
      log_debug("  [COPY] Failed to copy staged icon: %s -> %s", icon_src,
                icon_dst);
      return false;
    }
  }

  if (restage_appmeta) {
    mkdir(APPMETA_BASE, 0777);
    mkdir(user_appmeta_dir, 0777);
    if (!copy_sce_sys_to_appmeta(src_sce_sys, user_appmeta_dir)) {
      log_debug("  [COPY] Failed to copy appmeta files: %s -> %s", src_sce_sys,
                user_appmeta_dir);
      return false;
    }
    metadata_restaged = true;
  }

  if (should_pause_work())
    return false;

  if (!mount_title_nullfs(title_id, src_path)) {
    log_debug("  [LINK] nullfs mount failed: title=%s src=%s", title_id,
              src_path);
    return false;
  }

  // WRITE TRACKER
  char lnk_path[MAX_PATH];
  mkdir(APP_BASE, 0777);
  mkdir(user_app_dir, 0777);
  snprintf(lnk_path, sizeof(lnk_path), "%s/mount.lnk", user_app_dir);
  if (!write_link_file(lnk_path, src_path))
    return false;

  log_debug("  [LINK] mount.lnk created: %s -> %s", lnk_path, src_path);

  char img_lnk_path[MAX_PATH];
  snprintf(img_lnk_path, sizeof(img_lnk_path), "%s/mount_img.lnk",
           user_app_dir);
  if (has_image_source) {
    if (!write_link_file(img_lnk_path, image_source_path))
      return false;
    log_debug("  [LINK] mount_img.lnk created: %s -> %s", img_lnk_path,
              image_source_path);
    if (!cache_image_source_mapping(image_source_path, src_path)) {
      log_debug("  [LINK] image source cache update failed: %s -> %s",
                src_path, image_source_path);
    }
  } else if (unlink(img_lnk_path) != 0 && errno != ENOENT) {
    log_debug("  [LINK] remove failed for %s: %s", img_lnk_path,
              strerror(errno));
  }

  if (!should_register) {
    if (metadata_restaged && has_src_snd0) {
      int snd0_updates = update_snd0info(title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info force-updated after appmeta refresh rows=%d",
                  snd0_updates);
    }
    log_debug("  [REG] Skip (already present in app.db)");
    return true;
  }

  if (!metadata_restaged) {
    snprintf(src_snd0, sizeof(src_snd0), "%s/sce_sys/snd0.at9", src_path);
    has_src_snd0 = path_exists(src_snd0);
  }

  if (use_app_install_all) {
    if (should_pause_work())
      return false;
    if (has_src_snd0_out)
      *has_src_snd0_out = has_src_snd0;
    log_debug("  [REG] Prepared for batch install: %s (%s)", title_name,
              title_id);
    return true;
  }

  // REGISTER
  if (should_pause_work())
    return false;

  app_install_title_dir_fn_t app_install_title_dir_fn =
      resolve_app_install_title_dir();
  if (!app_install_title_dir_fn) {
    log_debug("  [REG] sceAppInstUtilAppInstallTitleDir unavailable");
    notify_system("Register failed: %s (%s)\nAppInstallTitleDir unavailable",
                  title_name, title_id);
    return false;
  }

  mark_register_attempted(title_id);
  int res = app_install_title_dir_fn(title_id, APP_BASE "/", 0);
  sceKernelUsleep(200000);

  if (res == 0) {
    invalidate_app_db_title_cache();
    log_debug("  [REG] Installed NEW!");
    notify_game_installed_rich(title_id);
    if (has_src_snd0) {
      int snd0_updates = update_snd0info(title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
    }
  } else if ((uint32_t)res == 0x80990002u) {
    invalidate_app_db_title_cache();
    log_debug("  [REG] Restored.");
    if (has_src_snd0) {
      int snd0_updates = update_snd0info(title_id);
      if (snd0_updates >= 0)
        log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
    }
    // Silent on restore/remount to avoid spam
  } else {
    log_debug("  [REG] FAIL: 0x%x", res);
    notify_system("Register failed: %s (%s)\ncode=0x%08X", title_name, title_id,
                  (uint32_t)res);
    return false;
  }

  return true;
}

// --- Execution (per discovered candidate) ---
void process_scan_candidates(const scan_candidate_t *candidates,
                             int candidate_count) {
  sm_install_poll_pending();

  for (int i = 0; i < candidate_count; i++) {
    if (should_pause_work())
      return;

    const scan_candidate_t *c = &candidates[i];
    bool has_src_snd0 = false;
    bool use_app_install_all =
        runtime_config()->app_install_all_enabled && !c->in_app_db;
    if (c->installed) {
      log_debug("  [ACTION] Remounting: %s", c->title_name);
    } else {
      log_debug("  [ACTION] Installing: %s (%s)", c->title_name, c->title_id);
      if (!use_app_install_all) {
        notify_system_info("Installing: %s (%s)...", c->title_name,
                           c->title_id);
      }
    }

    if (mount_and_install(c->path, c->title_id, c->title_name, c->installed,
                          !c->in_app_db, use_app_install_all,
                          &has_src_snd0)) {
      if (use_app_install_all) {
        if (!sm_install_queue_candidate(c, has_src_snd0)) {
          log_debug("  [REG] Failed to queue staged install: %s (%s)",
                    c->title_name, c->title_id);
          if (should_stop_requested())
            return;

          uint8_t failed_attempts = bump_failed_mount_attempts(c->title_id);
          if (failed_attempts == MAX_FAILED_MOUNT_ATTEMPTS) {
            log_debug("  [RETRY] limit reached (%u/%u): %s (%s)",
                      (unsigned)failed_attempts,
                      (unsigned)MAX_FAILED_MOUNT_ATTEMPTS, c->title_name,
                      c->title_id);
          }
          continue;
        }
        clear_failed_mount_attempts(c->title_id);
      } else {
        clear_failed_mount_attempts(c->title_id);
        cache_game_entry(c->path, c->title_id, c->title_name);
      }
    } else {
      if (should_stop_requested())
        return;

      uint8_t failed_attempts = bump_failed_mount_attempts(c->title_id);
      if (failed_attempts == MAX_FAILED_MOUNT_ATTEMPTS) {
        log_debug("  [RETRY] limit reached (%u/%u): %s (%s)",
                  (unsigned)failed_attempts,
                  (unsigned)MAX_FAILED_MOUNT_ATTEMPTS, c->title_name,
                  c->title_id);
      }
    }
  }

  if (!sm_install_submit_queued())
    sm_install_note_submit_failure();
}
