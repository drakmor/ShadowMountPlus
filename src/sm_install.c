#include "sm_platform.h"
#include "sm_runtime.h"
#include "sm_install.h"
#include "sm_types.h"
#include "sm_game_cache.h"
#include "sm_log.h"
#include "sm_filesystem.h"
#include "sm_limits.h"
#include "sm_path_utils.h"
#include "sm_appdb.h"
#include "sm_title_state.h"
#include "sm_image_cache.h"

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

// --- Install/Remount Action ---
static bool mount_and_install(const char *src_path, const char *title_id,
                              const char *title_name, bool is_remount,
                              bool should_register) {
  char user_app_dir[MAX_PATH];
  char user_sce_sys[MAX_PATH];
  char src_sce_sys[MAX_PATH];
  char image_source_path[MAX_PATH];
  bool has_image_source = false;

  snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);

  if (is_under_image_mount_base(src_path)) {
    has_image_source = resolve_image_source_from_mount_cache(
        src_path, image_source_path, sizeof(image_source_path));
    if (!has_image_source) {
      log_debug("  [LINK] image source lookup failed for %s: %s", title_id,
                src_path);
    }
  }

  // COPY FILES
  if (!is_remount) {
    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_app_dir, 0777);
    mkdir(user_sce_sys, 0777);

    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", src_path);
    if (copy_dir(src_sce_sys, user_sce_sys) != 0) {
      log_debug("  [COPY] Failed to copy sce_sys: %s -> %s", src_sce_sys,
                user_sce_sys);
      return false;
    }

    char icon_src[MAX_PATH], icon_dst[MAX_PATH];
    snprintf(icon_src, sizeof(icon_src), "%s/sce_sys/icon0.png", src_path);
    snprintf(icon_dst, sizeof(icon_dst), "/user/app/%s/icon0.png", title_id);
    if (copy_file(icon_src, icon_dst) != 0) {
      log_debug("  [COPY] Failed to copy icon: %s -> %s", icon_src, icon_dst);
      return false;
    }
  } else {
    log_debug("  [SPEED] Skipping file copy (Assets already exist)");
  }

  // WRITE TRACKER
  char lnk_path[MAX_PATH];
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

  if (!mount_title_nullfs(title_id, src_path)) {
    log_debug("  [LINK] mount.lnk created but nullfs mount failed: title=%s "
              "src=%s",
              title_id, src_path);
    return false;
  }

  if (!should_register) {
    log_debug("  [REG] Skip (already present in app.db)");
    return true;
  }

  // REGISTER
  char src_snd0[MAX_PATH];
  snprintf(src_snd0, sizeof(src_snd0), "%s/sce_sys/snd0.at9", src_path);
  bool has_src_snd0 = (access(src_snd0, F_OK) == 0);

  mark_register_attempted(title_id);
  int res = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);
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
  for (int i = 0; i < candidate_count; i++) {
    if (should_stop_requested())
      return;

    const scan_candidate_t *c = &candidates[i];
    if (c->installed) {
      log_debug("  [ACTION] Remounting: %s", c->title_name);
    } else {
      log_debug("  [ACTION] Installing: %s (%s)", c->title_name, c->title_id);
      notify_system_info("Installing: %s (%s)...", c->title_name,
                         c->title_id);
    }

    if (mount_and_install(c->path, c->title_id, c->title_name, c->installed,
                          !c->in_app_db)) {
      clear_failed_mount_attempts(c->title_id);
      cache_game_entry(c->path, c->title_id, c->title_name);
    } else {
      uint8_t failed_attempts = bump_failed_mount_attempts(c->title_id);
      if (failed_attempts == MAX_FAILED_MOUNT_ATTEMPTS) {
        log_debug("  [RETRY] limit reached (%u/%u): %s (%s)",
                  (unsigned)failed_attempts,
                  (unsigned)MAX_FAILED_MOUNT_ATTEMPTS, c->title_name,
                  c->title_id);
      }
    }
  }
}
