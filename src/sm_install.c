#include "sm_platform.h"
#include "sm_shellcore_service.h"
#include "sm_game_lifecycle.h"
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
#include "sm_manual.h"
#include "sm_shellcore_hooks.h"
#include "sm_fpkg.h"
#include "sm_fpkg_cache.h"
#include "sm_pkg.h"
#include "sm_fakelib.h"
#include "sm_scan.h"

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

bool sm_install_register_title_dir(const char *title_id,
                                   const char *install_dir,
                                   int *result_out) {
  if (!title_id || !install_dir || !result_out)
    return false;

  if (sm_shellcore_install_bridge_enabled())
    return sm_shellcore_install_title_dir(title_id, install_dir, result_out);

  app_install_title_dir_fn_t install_title_dir =
      resolve_app_install_title_dir();
  if (!install_title_dir)
    return false;

  *result_out = install_title_dir(title_id, install_dir, NULL);
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
  if (should_stop_requested() || runtime_sleep_mode_active()) {
    errno = ECANCELED;
    return false;
  }
  DIR *dir = opendir(src_sce_sys);
  if (!dir)
    return false;

  int error = 0;
  char src_path[MAX_PATH];
  char dst_path[MAX_PATH];
  struct stat st;
  for (;;) {
    if (should_stop_requested() || runtime_sleep_mode_active()) {
      error = ECANCELED;
      break;
    }
    errno = 0;
    const struct dirent *e = readdir(dir);
    if (!e) {
      error = errno;
      break;
    }
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
      continue;
    if (!is_appmeta_file(e->d_name))
      continue;

    int written = snprintf(src_path, sizeof(src_path), "%s/%s", src_sce_sys,
                            e->d_name);
    if (written < 0 || (size_t)written >= sizeof(src_path)) {
      error = ENAMETOOLONG;
      break;
    }
    if (stat(src_path, &st) != 0) {
      error = errno;
      break;
    }
    if (!S_ISREG(st.st_mode))
      continue;

    written = snprintf(dst_path, sizeof(dst_path), "%s/%s", user_appmeta_dir,
                        e->d_name);
    if (written < 0 || (size_t)written >= sizeof(dst_path)) {
      error = ENAMETOOLONG;
      break;
    }
    lowercase_sce_sys_name(dst_path + strlen(user_appmeta_dir) + 1);
    errno = 0;
    if (copy_file(src_path, dst_path) != 0) {
      error = errno != 0 ? errno : EIO;
      break;
    }
  }

  if (closedir(dir) != 0 && error == 0)
    error = errno;

  if (error != 0)
    errno = error;
  return error == 0;
}

static bool copy_optional_trophy_metadata_file(const char *src_sce_sys,
                                               const char *dst_base,
                                               const char *relative_path) {
  if (should_stop_requested() || runtime_sleep_mode_active()) {
    errno = ECANCELED;
    return false;
  }
  char src_path[MAX_PATH];
  char dst_path[MAX_PATH];

  if (!resolve_sce_sys_file(src_sce_sys, relative_path, src_path))
    return errno == ENOENT || errno == ENOTDIR;
  struct stat st;
  if (stat(src_path, &st) != 0)
    return errno == ENOENT || errno == ENOTDIR;
  if (!S_ISREG(st.st_mode)) {
    errno = EINVAL;
    return false;
  }

  int written = snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_base,
                          relative_path);
  if (written < 0 || (size_t)written >= sizeof(dst_path)) {
    errno = ENAMETOOLONG;
    return false;
  }
  if (stat(dst_path, &st) == 0) {
    if (S_ISREG(st.st_mode))
      return true;
    errno = EINVAL;
    return false;
  }
  if (errno != ENOENT)
    return false;

  errno = 0;
  if (copy_file(src_path, dst_path) == 0)
    return true;

  int error = errno != 0 ? errno : EIO;
  log_debug("  [COPY] Failed to copy trophy metadata: %s -> %s", src_path,
            dst_path);
  errno = error;
  return false;
}

static bool update_trophy_metadata(const char *title_id,
                                   const char *src_sce_sys) {
  if (should_stop_requested() || runtime_sleep_mode_active()) {
    errno = ECANCELED;
    return false;
  }
  char dst_base[MAX_PATH];
  char dst_dir[MAX_PATH];
  int written = snprintf(dst_base, sizeof(dst_base),
                          "/system_data/priv/appmeta/%s", title_id);
  if (written < 0 || (size_t)written >= sizeof(dst_base)) {
    errno = ENAMETOOLONG;
    return false;
  }
  mkdir("/system_data/priv/appmeta", 0755);
  mkdir(dst_base, 0755);

  if (strncmp(title_id, "CUSA", 4u) == 0) {
    return copy_optional_trophy_metadata_file(src_sce_sys, dst_base,
                                              "npbind.dat") &&
           copy_optional_trophy_metadata_file(src_sce_sys, dst_base,
                                              "nptitle.dat") &&
           copy_optional_trophy_metadata_file(src_sce_sys, dst_base,
                                              "param.sfo");
  }

  written = snprintf(dst_dir, sizeof(dst_dir), "%s/trophy2", dst_base);
  if (written < 0 || (size_t)written >= sizeof(dst_dir)) {
    errno = ENAMETOOLONG;
    return false;
  }
  mkdir(dst_dir, 0755);
  written = snprintf(dst_dir, sizeof(dst_dir), "%s/uds", dst_base);
  if (written < 0 || (size_t)written >= sizeof(dst_dir)) {
    errno = ENAMETOOLONG;
    return false;
  }
  mkdir(dst_dir, 0755);

  return copy_optional_trophy_metadata_file(src_sce_sys, dst_base,
                                            "trophy2/npbind.dat") &&
         copy_optional_trophy_metadata_file(src_sce_sys, dst_base,
                                            "uds/npbind.dat") &&
         copy_optional_trophy_metadata_file(src_sce_sys, dst_base, "param.json");
}

static void update_registered_title_snd0info(const char *title_id) {
  sceKernelUsleep(200000);
  int snd0_updates = update_snd0info(title_id);
  if (snd0_updates >= 0)
    log_debug("  [DB] snd0info updated rows=%d", snd0_updates);
}

typedef struct {
  const char *title_id;
} fpkg_cache_cancel_context_t;

static bool fpkg_cache_build_cancelled(void *ctx) {
  const fpkg_cache_cancel_context_t *cancel = ctx;
  return should_stop_requested() || runtime_sleep_mode_active() ||
         (cancel && sm_shellcore_title_is_protected(cancel->title_id));
}

static bool prepare_fpkg_candidate(scan_candidate_t *candidate,
                                   char previous_generation[MAX_PATH]) {
  previous_generation[0] = '\0';
  if (!candidate || !candidate->fpkg ||
      !sm_config_fpkg_direct_mount_enabled()) {
    errno = ENOTSUP;
    return false;
  }
  if (!sm_shellcore_try_begin_external_mutation()) {
    errno = EBUSY;
    return false;
  }

  bool prepared = false;
  int source_fd = -1;
  sm_fpkg_t *fpkg = NULL;
  do {
    if (should_stop_requested() || runtime_sleep_mode_active() ||
        sm_shellcore_title_is_protected(candidate->title_id)) {
      errno = EBUSY;
      break;
    }
    source_fd = open(candidate->path,
                     O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (source_fd < 0)
      break;

    sm_pkg_info_t metadata = {0};
    if (!sm_pkg_parse_fd(candidate->path, source_fd, &metadata) ||
        strcmp(metadata.title_id, candidate->title_id) != 0 ||
        metadata.pkg_kind != PKG_KIND_BASE ||
        !metadata.pkg_classification_confident) {
      errno = EINVAL;
      break;
    }
    sm_fpkg_status_t fpkg_status = sm_fpkg_open_fd(
        candidate->path, source_fd, &metadata, &fpkg);
    if (fpkg_status != SM_FPKG_OK) {
      errno = fpkg_status == SM_FPKG_IO ? EIO : EINVAL;
      break;
    }

    sm_fpkg_cache_recovery_report_t recovery = {0};
    sm_fpkg_status_t recovery_status = sm_fpkg_cache_recover_title(
        SM_FPKG_CACHE_DEFAULT_ROOT, candidate->title_id, &recovery);
    if (recovery_status != SM_FPKG_OK) {
      errno = EIO;
      break;
    }
    if (recovery.active_generation[0])
      (void)strlcpy(previous_generation, recovery.active_generation,
                    MAX_PATH);

    fpkg_cache_cancel_context_t cancel = {.title_id = candidate->title_id};
    char backport_path[MAX_PATH] = {0};
    (void)resolve_backport_path_for_title(candidate->title_id, NULL,
                                          backport_path);
    sm_fpkg_cache_options_t options = {
        .max_extract_bytes = SM_FPKG_CACHE_DEFAULT_EXTRACT_LIMIT,
        .is_cancelled = fpkg_cache_build_cancelled,
        .cancel_ctx = &cancel,
        .backport_path = backport_path[0] ? backport_path : NULL,
    };
    char generation_path[MAX_PATH];
    fpkg_status = sm_fpkg_cache_build_generation(
        SM_FPKG_CACHE_DEFAULT_ROOT, candidate->path, source_fd, &metadata,
        fpkg, NULL, NULL, &options, generation_path);
    if (fpkg_status != SM_FPKG_OK) {
      errno = fpkg_status == SM_FPKG_CANCELLED ? ECANCELED : EIO;
      log_debug("  [FPKG] cache build failed: title=%s source=%s status=%d",
                candidate->title_id, candidate->path, (int)fpkg_status);
      break;
    }

    sm_fpkg_cache_generation_info_t generation_info;
    if (sm_fpkg_cache_read_generation_info(
            SM_FPKG_CACHE_DEFAULT_ROOT, candidate->title_id, generation_path,
            &generation_info) != SM_FPKG_OK ||
        !generation_info.source_matches_snapshot ||
        strcmp(generation_info.source_path, candidate->path) != 0) {
      errno = ESTALE;
      break;
    }
    (void)strlcpy(candidate->path, generation_path,
                  sizeof(candidate->path));
    candidate->fpkg = false;
    prepared = true;
  } while (0);

  if (fpkg)
    sm_fpkg_close(fpkg);
  if (source_fd >= 0)
    close(source_fd);
  sm_shellcore_end_external_mutation();
  return prepared;
}

static bool begin_fpkg_cache_cleanup(const char *title_id,
                                     const char *generation_path, void *ctx) {
  (void)ctx;
  if (!sm_shellcore_try_begin_external_mutation())
    return false;
  char current_source[MAX_PATH];
  bool in_use = sm_shellcore_title_is_protected(title_id) ||
                (read_mount_link(title_id, current_source,
                                 sizeof(current_source)) &&
                 paths_overlap(generation_path, current_source));
  if (in_use) {
    sm_shellcore_end_external_mutation();
    return false;
  }
  return true;
}

static void end_fpkg_cache_cleanup(void *ctx) {
  (void)ctx;
  sm_shellcore_end_external_mutation();
}

bool sm_install_release_replaced_fpkg_generation(
    const char *title_id, const char *previous_generation_name,
    const char *current_path) {
  if (!title_id || !previous_generation_name ||
      !previous_generation_name[0] || !current_path || !current_path[0])
    return true;
  char previous_path[MAX_PATH];
  int path_length = snprintf(previous_path, sizeof(previous_path), "%s/%s/%s",
                             SM_FPKG_CACHE_DEFAULT_ROOT, title_id,
                             previous_generation_name);
  if (path_length < 0 || (size_t)path_length >= sizeof(previous_path))
    return false;
  if (strcmp(previous_path, current_path) == 0)
    return true;
  sm_fpkg_status_t status = sm_fpkg_cache_remove_generation(
      SM_FPKG_CACHE_DEFAULT_ROOT, title_id, previous_generation_name,
      begin_fpkg_cache_cleanup, end_fpkg_cache_cleanup, NULL);
  if (status != SM_FPKG_OK && status != SM_FPKG_CANCELLED)
    log_debug("  [FPKG] old cache retained: title=%s path=%s status=%d",
              title_id, previous_path, (int)status);
  return status == SM_FPKG_OK;
}

// --- Install/Remount Action ---
static bool append_install_path_component(char out[MAX_PATH],
                                          const char *base,
                                          const char *component) {
  if (!out || !base || !component)
    return false;
  size_t base_len = strnlen(base, MAX_PATH);
  size_t component_len = strnlen(component, MAX_PATH);
  if (base_len == MAX_PATH || component_len == MAX_PATH)
    return false;
  bool add_separator = base_len > 0 && base[base_len - 1] != '/';
  size_t separator_len = add_separator ? 1u : 0u;
  if (base_len > MAX_PATH - 1u - separator_len ||
      component_len > MAX_PATH - 1u - base_len - separator_len)
    return false;

  memcpy(out, base, base_len);
  if (add_separator)
    out[base_len] = '/';
  memcpy(out + base_len + separator_len, component, component_len + 1u);
  return true;
}

static bool mount_and_install(const char *src_path, const char *title_id,
                              const char *title_name, bool is_remount,
                              bool should_register,
                              bool use_app_install_all,
                              bool *has_src_snd0_out) {
  char user_appmeta_dir[MAX_PATH];
  char user_app_dir[MAX_PATH];
  char user_sce_sys[MAX_PATH];
  char icon_src[MAX_PATH];
  char icon_dst[MAX_PATH];
  char lnk_path[MAX_PATH];
  char img_lnk_path[MAX_PATH];
  char src_sce_sys[MAX_PATH];
  char src_snd0[MAX_PATH];
  char image_source_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t image_source_count = 0;
  bool has_image_source = false;
  bool appmeta_missing = false;
  bool metadata_restaged = false;
  bool restage_staging = false;
  bool restage_appmeta = false;
  bool has_src_snd0 = false;

  if (!append_install_path_component(user_appmeta_dir, APPMETA_BASE,
                                     title_id) ||
      !append_install_path_component(user_app_dir, APP_BASE, title_id) ||
      !append_install_path_component(user_sce_sys, user_app_dir, "sce_sys") ||
      !append_install_path_component(icon_src, user_sce_sys, "icon0.png") ||
      !append_install_path_component(icon_dst, user_app_dir, "icon0.png") ||
      !append_install_path_component(lnk_path, user_app_dir, "mount.lnk") ||
      !append_install_path_component(img_lnk_path, user_app_dir,
                                     "mount_img.lnk")) {
    log_debug("  [LINK] title path is too long: %s", title_id);
    return false;
  }

  const char *image_chain_source = src_path;
  char fpkg_source_path[MAX_PATH];
  if (path_matches_root_or_child(src_path,
                                 SM_FPKG_CACHE_DEFAULT_ROOT)) {
    if (!sm_fakelib_resolve_fpkg_generation_source(
            title_id, src_path, fpkg_source_path)) {
      errno = ESTALE;
      return false;
    }
    image_chain_source = fpkg_source_path;
  }
  if (is_under_image_mount_base(image_chain_source)) {
    image_source_count = resolve_image_source_chain_from_mount_cache(
        image_chain_source, image_source_chain);
    has_image_source = image_source_count > 0;
    if (!has_image_source) {
      log_debug("  [LINK] image source lookup failed for %s: %s", title_id,
                src_path);
    }
  }

  if (!resolve_sce_sys_file(src_path, "sce_sys", src_sce_sys))
    return false;
  appmeta_missing = !has_appmeta_data(title_id);
  if (is_remount && appmeta_missing) {
    log_debug("  [REG] appmeta missing, restaging metadata only: %s", title_id);
  }

  restage_staging = (!is_remount || should_register);
  restage_appmeta = (!is_remount || appmeta_missing);

  if (should_stop_requested() || runtime_sleep_mode_active())
    return false;

  // COPY FILES
  has_src_snd0 = resolve_sce_sys_file(src_sce_sys, "snd0.at9", src_snd0) &&
                 path_exists(src_snd0);
  if (!restage_staging && !restage_appmeta)
    log_debug("  [SPEED] Skipping file copy (Assets already exist)");

  if (restage_staging) {
    mkdir(APP_BASE, 0777);
    mkdir(user_app_dir, 0777);
    mkdir(user_sce_sys, 0777);
    if (copy_sce_sys_dir(src_sce_sys, user_sce_sys) != 0) {
      log_debug("  [COPY] Failed to copy sce_sys staging: %s -> %s", src_sce_sys,
                user_sce_sys);
      return false;
    }

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

  if (!update_trophy_metadata(title_id, src_sce_sys))
    return false;

  if (should_stop_requested() || runtime_sleep_mode_active())
    return false;

  if (!mount_title_nullfs(title_id, src_path)) {
    log_debug("  [LINK] nullfs mount failed: title=%s src=%s", title_id,
              src_path);
    return false;
  }

  // WRITE TRACKER
  runtime_mount_state_lock();
  if (runtime_sleep_mode_active()) {
    (void)rollback_title_nullfs_mount(title_id, src_path);
    runtime_mount_state_unlock();
    return false;
  }

  mkdir(APP_BASE, 0777);
  mkdir(user_app_dir, 0777);
  if (!write_mount_link(title_id, src_path)) {
    log_debug("  [LINK] write failed for %s: %s", lnk_path, strerror(errno));
    (void)rollback_title_nullfs_mount(title_id, src_path);
    runtime_mount_state_unlock();
    return false;
  }

  log_debug("  [LINK] mount.lnk created: %s -> %s", lnk_path, src_path);

  if (has_image_source) {
    if (!write_mount_image_chain(title_id, image_source_chain,
                                 image_source_count)) {
      (void)unlink(lnk_path);
      (void)rollback_title_nullfs_mount(title_id, src_path);
      runtime_mount_state_unlock();
      return false;
    }
    log_debug("  [LINK] mount_img.lnk created: %s -> %s layers=%zu",
              img_lnk_path, image_source_chain[0], image_source_count);
    if (!cache_image_source_mapping(image_source_chain[0], src_path)) {
      log_debug("  [LINK] image source cache update failed: %s -> %s",
                image_source_chain[0], src_path);
    }
  } else if (unlink(img_lnk_path) != 0 && errno != ENOENT) {
    log_debug("  [LINK] remove failed for %s: %s", img_lnk_path,
              strerror(errno));
  }
  bool sleep_started = runtime_sleep_mode_active();
  runtime_mount_state_unlock();
  if (sleep_started || runtime_sleep_mode_active())
    return true;

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

  if (use_app_install_all) {
    if (has_src_snd0_out)
      *has_src_snd0_out = has_src_snd0;
    log_debug("  [REG] Prepared for batch install: %s (%s)", title_name,
              title_id);
    return true;
  }

  // REGISTER
  int res = 0;
  bool bridge_enabled = sm_shellcore_install_bridge_enabled();
  if (!sm_install_register_title_dir(title_id, APP_BASE "/", &res)) {
    if (bridge_enabled) {
      log_debug("  [REG] internal AppInstallTitleDir bridge unavailable");
      notify_system_l10n(SM_L10N_REGISTER_BRIDGE_UNAVAILABLE, title_name,
                         title_id);
    } else {
      log_debug("  [REG] sceAppInstUtilAppInstallTitleDir unavailable");
      notify_system_l10n(SM_L10N_REGISTER_FAILED_UNAVAILABLE, title_name,
                         title_id);
    }
    return false;
  }
  mark_register_attempted(title_id);
  if (res == 0) {
    invalidate_app_db_title_cache();
    clear_register_attempts(title_id);
    log_debug("  [REG] Installed NEW!");
    notify_game_installed_rich(title_id);
    if (has_src_snd0)
      update_registered_title_snd0info(title_id);
  } else if ((uint32_t)res == 0x80990002u) {
    invalidate_app_db_title_cache();
    clear_register_attempts(title_id);
    log_debug("  [REG] Restored.");
    if (has_src_snd0)
      update_registered_title_snd0info(title_id);
    // Silent on restore/remount to avoid spam
  } else {
    log_debug("  [REG] FAIL: 0x%x", res);
    notify_system_l10n(SM_L10N_REGISTER_FAILED_CODE, title_name, title_id,
                       (uint32_t)res);
    return false;
  }

  return true;
}

// --- Execution (per discovered candidate) ---
void process_scan_candidates(const scan_candidate_t *candidates,
                             int candidate_count) {
  if (runtime_sleep_mode_active())
    return;

  sm_install_poll_pending_for_scan();

  for (int i = 0; i < candidate_count; i++) {
    if (should_stop_requested() || runtime_sleep_mode_active() ||
        !sm_game_lifecycle_background_title(NULL))
      return;

    scan_candidate_t prepared_candidate = candidates[i];
    const scan_candidate_t *c = &prepared_candidate;
    if (sm_shellcore_title_is_protected(c->title_id))
      continue;
    char previous_fpkg_generation[MAX_PATH] = {0};
    if (c->fpkg && !prepare_fpkg_candidate(&prepared_candidate,
                                           previous_fpkg_generation)) {
      if (errno == EBUSY || errno == ECANCELED)
        continue;
      uint8_t failed_attempts = bump_failed_mount_attempts(c->title_id);
      log_debug("  [FPKG] candidate preparation failed: %s (%s): %s",
                c->title_name, c->title_id, strerror(errno));
      if (failed_attempts == MAX_FAILED_MOUNT_ATTEMPTS)
        log_debug("  [RETRY] limit reached (%u/%u): %s (%s)",
                  (unsigned)failed_attempts,
                  (unsigned)MAX_FAILED_MOUNT_ATTEMPTS, c->title_name,
                  c->title_id);
      continue;
    }
    bool has_src_snd0 = false;
    bool use_app_install_all =
        runtime_config().app_install_all_enabled && !c->in_app_db;
    const char *previous_generation_name = NULL;
    if (candidates[i].fpkg && previous_fpkg_generation[0]) {
      const char *slash = strrchr(previous_fpkg_generation, '/');
      previous_generation_name = slash ? slash + 1 : previous_fpkg_generation;
    }
    if (c->installed) {
      log_debug("  [ACTION] Remounting: %s", c->title_name);
    } else {
      log_debug("  [ACTION] Installing: %s (%s)", c->title_name, c->title_id);
      if (!use_app_install_all) {
        notify_system_info_l10n(SM_L10N_INSTALLING, c->title_name,
                                c->title_id);
      }
    }

    bool mounted = mount_and_install(c->path, c->title_id, c->title_name,
                                     c->installed, !c->in_app_db,
                                     use_app_install_all, &has_src_snd0);
    if (runtime_sleep_mode_active())
      return;

    if (mounted) {
      if (c->manual && !use_app_install_all) {
        sm_manual_note_installed(c->manual_source_path, c->title_id,
                                 c->title_name);
      }
      if (use_app_install_all) {
        const char *previous_generation = candidates[i].fpkg
            ? previous_generation_name
            : NULL;
        if (!sm_install_queue_candidate(c, has_src_snd0,
                                        previous_generation)) {
          log_debug("  [REG] Failed to queue staged install: %s (%s)",
                    c->title_name, c->title_id);
          if (should_stop_requested() || runtime_sleep_mode_active())
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
        cache_game_entry(candidates[i].fpkg ? candidates[i].path : c->path,
                         c->title_id, c->title_name);
      }
      if (candidates[i].fpkg && !use_app_install_all)
        if (!sm_install_release_replaced_fpkg_generation(
                c->title_id, previous_generation_name, c->path) &&
            !sm_install_defer_fpkg_generation_cleanup(
                c->title_id, previous_generation_name, c->path))
          log_debug("  [FPKG] unable to queue old cache cleanup: %s (%s)",
                    c->title_name, c->title_id);
    } else {
      if (should_stop_requested() || runtime_sleep_mode_active())
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
