/* Host integration smoke check: production SMP functions, platform calls mocked. */
#include "host_platform.h"
#include "sm_appdb.h"
#include "sm_config_mount.h"
#include "sm_game_cache.h"
#include "sm_fpkg_cache.h"
#include "sm_fpkg.h"
#include "sm_fakelib.h"
#include "sm_image.h"
#include "sm_filesystem.h"
#include "sm_image_cache.h"
#include "sm_image_index.h"
#include "sm_install.h"
#include "sm_install_queue.h"
#include "sm_l10n.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_manual.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_pkg.h"
#include "sm_runtime.h"
#include "sm_shellcore_hooks.h"
#include "sm_shellcore_service.h"
#include "sm_scan.h"
#include "sm_title_state.h"
#include "sm_types.h"
#include <strings.h>

#ifndef MNT_RDONLY
#define MNT_RDONLY 0x00000001
#endif

typedef int (*app_install_title_dir_fn_t)(const char *title_id,
                                          const char *install_path,
                                          void *reserved);
typedef struct {
  const char *label;
} fakelib_layer_t;
typedef struct {
  char title_id[MAX_TITLE_ID];
  char sandbox_app0_path[MAX_PATH];
  char fpkg_generation_path[MAX_PATH];
  char fpkg_source_directory[MAX_PATH];
  fakelib_layer_t layers[8];
  size_t layer_count;
} fakelib_session_t;

static const char *const k_source =
    "/data/shadowmount/fpkg-cache/PPSA00001/generation-host-test";
static const char *const k_title = "PPSA00001";
static char events[16][32];
static size_t event_count;
static bool fail_mount_link;
static bool bridge_enabled;
static bool fail_bridge;
static int api_result;
static int api_calls;
static int bridge_calls;
static char api_title[MAX_TITLE_ID];
static char api_install_path[MAX_PATH];
static char sandbox_target[MAX_PATH];
static char sandbox_source[MAX_PATH];
static char sandbox_label[32];
static int sandbox_flags;
static int sandbox_calls;
static fakelib_session_t g_fakelib_mount;
static bool fpkg_feature_enabled;
static sm_fpkg_status_t generation_info_status;
static sm_fpkg_cache_generation_info_t generation_info;
static unsigned generation_info_calls;
static char expected_generation_path[MAX_PATH] =
    "/data/shadowmount/fpkg-cache/PPSA00001/generation-host-test";
static char mount_link_path[MAX_PATH];
static char expected_cache_root[MAX_PATH];
static char image_mount_root[MAX_PATH];
static char synthetic_image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
static size_t synthetic_image_chain_count;
static bool synthetic_image_mount_succeeds;
static bool synthetic_remount_changes_md_identity;
static bool synthetic_remounted_snapshot_matches;
static bool image_index_record_succeeds = true;
static bool backport_matches = true;
static const char *test_backport_path;
static sm_fpkg_status_t fake_find_status = SM_FPKG_UNSUPPORTED;
static bool fake_installed, fake_data_mounted;
static unsigned image_index_record_calls;
static unsigned prepare_image_source_calls;
static bool prepare_image_source_succeeds = true;
static unsigned runtime_unmount_calls;
static char expected_source_path[MAX_PATH];
static unsigned fake_fpkg_open_count;
static unsigned fake_fpkg_close_count;
static unsigned fake_cache_build_count;
static sm_fpkg_status_t fake_fpkg_open_status = SM_FPKG_OK;
static bool external_mutation_active;

static bool copy_sce_sys_to_appmeta(const char *src, const char *dst);
static bool update_trophy_metadata(const char *title_id,
                                   const char *src_sce_sys);
static bool prepare_image_source(const char *title_id,
                                 const char *runtime_source,
                                 const bool *mount_read_only_override);
static void update_registered_title_snd0info(const char *title_id);
bool mount_sandbox_directory_at(const char *title_id, const char *app0_path,
                                const char *target_suffix,
                                const char *source_path, const char *label,
                                int flags);

static void record_event(const char *event) {
  assert(event_count < sizeof(events) / sizeof(events[0]));
  (void)strlcpy(events[event_count++], event, sizeof(events[0]));
}

static void reset_events(void) {
  event_count = 0;
  memset(events, 0, sizeof(events));
  api_calls = 0;
  bridge_calls = 0;
}

static int fake_install_title_dir(const char *title_id,
                                  const char *install_path,
                                  void *reserved) {
  assert(reserved == NULL);
  ++api_calls;
  (void)strlcpy(api_title, title_id, sizeof(api_title));
  (void)strlcpy(api_install_path, install_path, sizeof(api_install_path));
  record_event("AppInstallTitleDir");
  return api_result;
}

static app_install_title_dir_fn_t resolve_app_install_title_dir(void) {
  return fake_install_title_dir;
}

static int test_mkdir(const char *path, mode_t mode) {
  (void)path;
  (void)mode;
  return 0;
}

static int test_unlink(const char *path) {
  (void)path;
  errno = ENOENT;
  return -1;
}

#define mkdir test_mkdir
#define unlink test_unlink
#include "install_lifecycle.inc"
#undef mkdir
#undef unlink

bool path_matches_root_or_child(const char *path, const char *root);
bool is_under_image_mount_base(const char *path) {
  return image_mount_root[0] &&
         path_matches_root_or_child(path, image_mount_root);
}
bool path_matches_root_or_child(const char *path, const char *root) {
  if (!path || !root)
    return false;
  size_t root_length = strlen(root);
  if (strcmp(path, root) == 0 ||
      (strncmp(path, root, root_length) == 0 && path[root_length] == '/'))
    return true;
  if (strcmp(root, SM_FPKG_CACHE_DEFAULT_ROOT) != 0 ||
      !expected_cache_root[0])
    return false;
  root_length = strlen(expected_cache_root);
  return strcmp(path, expected_cache_root) == 0 ||
         (strncmp(path, expected_cache_root, root_length) == 0 &&
          path[root_length] == '/');
}
bool maybe_mount_image_file(const char *full_path, const char *display_name,
                            bool *unstable_out) {
  assert(full_path && full_path[0]);
  (void)display_name;
  (void)unstable_out;
  record_event("image mount");
  if (synthetic_image_mount_succeeds && generation_info_status == SM_FPKG_OK &&
      !synthetic_remount_changes_md_identity)
    generation_info.source_matches_snapshot = true;
  return synthetic_image_mount_succeeds;
}
bool sm_fpkg_cache_source_matches_remounted_image(
    const sm_fpkg_cache_generation_info_t *info) {
  return info && synthetic_remounted_snapshot_matches;
}
bool read_mount_image_chain(
    const char *title_id,
    char paths[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH], size_t *count_out) {
  assert(strcmp(title_id, k_title) == 0);
  if (!count_out || synthetic_image_chain_count == 0)
    return false;
  *count_out = synthetic_image_chain_count;
  memcpy(paths, synthetic_image_chain, sizeof(synthetic_image_chain));
  return true;
}
const char *get_filename_component(const char *path) {
  const char *separator = strrchr(path, '/');
  return separator ? separator + 1 : path;
}
void notify_duplicate_title_once(const char *title_id, const char *path_a,
                                 const char *path_b) {
  (void)title_id;
  (void)path_a;
  (void)path_b;
}
size_t resolve_image_source_chain_from_mount_cache(
    const char *path, char chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH]) {
  (void)path;
  (void)chain;
  return 0;
}
bool resolve_sce_sys_file(const char *root, const char *relative,
                          char out[MAX_PATH]) {
  int written = snprintf(out, MAX_PATH, "%s/%s", root, relative);
  return written >= 0 && written < MAX_PATH;
}
bool has_appmeta_data(const char *title_id) {
  (void)title_id;
  return true;
}
bool should_stop_requested(void) { return false; }
bool runtime_sleep_mode_active(void) { return false; }
int copy_sce_sys_dir(const char *src, const char *dst) {
  (void)src;
  (void)dst;
  return 0;
}
static bool copy_sce_sys_to_appmeta(const char *src, const char *dst) {
  (void)src;
  (void)dst;
  return true;
}
int copy_file(const char *src, const char *dst) {
  (void)src;
  (void)dst;
  return 0;
}
static bool update_trophy_metadata(const char *title_id,
                                   const char *src_sce_sys) {
  (void)title_id;
  (void)src_sce_sys;
  return true;
}
bool mount_title_nullfs(const char *title_id, const char *src_path) {
  assert(strcmp(title_id, k_title) == 0);
  assert(strcmp(src_path, k_source) == 0 ||
         strcmp(src_path, expected_generation_path) == 0);
  record_event("nullfs mount");
  return true;
}
void runtime_mount_state_lock(void) {}
void runtime_mount_state_unlock(void) {}
bool rollback_title_nullfs_mount(const char *title_id, const char *src_path) {
  assert(strcmp(title_id, k_title) == 0);
  assert(strcmp(src_path, k_source) == 0 ||
         strcmp(src_path, expected_generation_path) == 0);
  record_event("nullfs rollback");
  return true;
}
bool write_mount_link(const char *title_id, const char *src_path) {
  assert(strcmp(title_id, k_title) == 0);
  assert(strcmp(src_path, k_source) == 0 ||
         strcmp(src_path, expected_generation_path) == 0);
  record_event("mount.lnk");
  if (fail_mount_link) {
    errno = EIO;
    return false;
  }
  (void)strlcpy(mount_link_path, src_path, sizeof(mount_link_path));
  return true;
}
bool write_mount_image_chain(
    const char *title_id,
    const char paths[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH], size_t count) {
  (void)title_id;
  (void)paths;
  (void)count;
  return true;
}
bool cache_image_source_mapping(const char *path, const char *mount_point) {
  (void)path;
  (void)mount_point;
  return true;
}
int update_snd0info(const char *title_id) {
  (void)title_id;
  return 0;
}
bool path_exists(const char *path) {
  (void)path;
  return false;
}
bool sm_shellcore_install_bridge_enabled(void) { return bridge_enabled; }
bool sm_shellcore_install_title_dir(const char *title_id,
                                    const char *install_dir,
                                    int *result_out) {
  ++bridge_calls;
  (void)strlcpy(api_title, title_id, sizeof(api_title));
  (void)strlcpy(api_install_path, install_dir, sizeof(api_install_path));
  record_event("ShellCore TitleDir bridge");
  if (fail_bridge)
    return false;
  *result_out = api_result;
  return true;
}
bool sm_config_fpkg_direct_mount_enabled(void) {
  return fpkg_feature_enabled;
}
bool app_db_title_list_contains(const struct AppDbTitleList *list,
                                const char *title_id) {
  (void)list;
  (void)title_id;
  return fake_installed;
}
bool get_appmeta_present_for_scan_cycle(const char *title_id) {
  (void)title_id;
  return fake_installed;
}
void note_found_title(const char *title_id) { (void)title_id; }
bool request_blocked_ppsa_uninstall(const char *title_id,
                                    const char *source_path) {
  (void)title_id;
  (void)source_path;
  return false;
}
bool wait_for_stability_fast(const char *path, const char *name) {
  (void)path;
  (void)name;
  return true;
}
bool is_installed(const char *title_id) {
  (void)title_id;
  return fake_installed;
}
bool read_mount_link(const char *title_id, char *out, size_t out_size) {
  assert(strcmp(title_id, k_title) == 0);
  if (!mount_link_path[0] || out_size == 0)
    return false;
  return strlcpy(out, mount_link_path, out_size) < out_size;
}
bool directory_has_game_executable(const char *dir_path) {
  (void)dir_path;
  return false;
}
bool is_data_mounted(const char *title_id) {
  (void)title_id;
  return fake_data_mounted;
}
bool read_mount_image_link(const char *title_id, char *out, size_t out_size) {
  (void)title_id;
  (void)out;
  (void)out_size;
  return false;
}
bool sm_image_index_source_has_title(const char *path, const char *title_id) {
  (void)path;
  (void)title_id;
  return false;
}
bool sm_image_index_record_game(const char *path, const char *title_id) {
  assert(path && path[0] && strcmp(title_id, k_title) == 0);
  ++image_index_record_calls;
  return image_index_record_succeeds;
}
void note_game_cache_source_seen(const char *path, const char *title_id,
                                 const char *title_name) {
  (void)path;
  (void)title_id;
  (void)title_name;
}
bool is_title_install_pending(const char *title_id) {
  (void)title_id;
  return false;
}
bool was_register_attempted(const char *title_id) {
  (void)title_id;
  return false;
}
uint8_t get_failed_mount_attempts(const char *title_id) {
  (void)title_id;
  return 0;
}
void cache_game_entry(const char *path, const char *title_id,
                      const char *title_name) {
  (void)path;
  (void)title_id;
  (void)title_name;
}
void sm_manual_note_installed(const char *manual_source_path,
                              const char *title_id,
                              const char *title_name) {
  (void)manual_source_path;
  (void)title_id;
  (void)title_name;
}
bool resolve_backport_path_for_title(const char *title_id,
                                     const char *root,
                                     char path[MAX_PATH]) {
  (void)title_id; (void)root;
  if (test_backport_path) {
    (void)strlcpy(path, test_backport_path, MAX_PATH);
    return true;
  }
  path[0] = '\0';
  return false;
}
sm_fpkg_status_t sm_fpkg_cache_find_current_generation_with_backport(
    const char *cache_root, const sm_pkg_info_t *source_info,
    const char *backport_path, char generation_path_out[MAX_PATH]) {
  assert(strcmp(cache_root, SM_FPKG_CACHE_DEFAULT_ROOT) == 0);
  assert((!backport_path && !test_backport_path) ||
         (backport_path && test_backport_path &&
          !strcmp(backport_path, test_backport_path)));
  assert(source_info && strcmp(source_info->title_id, k_title) == 0);
  (void)generation_path_out;
  return fake_find_status;
}
sm_fpkg_status_t sm_fpkg_cache_read_generation_info(
    const char *cache_root, const char *title_id, const char *generation_path,
    sm_fpkg_cache_generation_info_t *info_out) {
  assert(strcmp(cache_root, SM_FPKG_CACHE_DEFAULT_ROOT) == 0);
  assert(strcmp(title_id, k_title) == 0);
  if (strcmp(generation_path, expected_generation_path) != 0)
    return SM_FPKG_UNSUPPORTED;
  ++generation_info_calls;
  record_event("cache info");
  if (generation_info_status == SM_FPKG_OK)
    *info_out = generation_info;
  return generation_info_status;
}
bool sm_fpkg_cache_backport_matches(
    const sm_fpkg_cache_generation_info_t *info, const char *backport_path) {
  assert(info && ((!backport_path && !test_backport_path) ||
                  (backport_path && test_backport_path &&
                   !strcmp(backport_path, test_backport_path))));
  return backport_matches;
}
static bool prepare_image_source(const char *title_id,
                                 const char *runtime_source,
                                 const bool *mount_read_only_override) {
  assert(strcmp(title_id, k_title) == 0);
  assert(strcmp(runtime_source, expected_generation_path) == 0);
  (void)mount_read_only_override;
  ++prepare_image_source_calls;
  return prepare_image_source_succeeds;
}
bool mount_backport_overlay_for_title(const char *source_path,
                                      const char *title_id,
                                      const char *owning_scan_root,
                                      bool *unstable_found_out) {
  assert(strcmp(source_path, expected_generation_path) == 0);
  assert(strcmp(title_id, k_title) == 0);
  (void)owning_scan_root;
  (void)unstable_found_out;
  return true;
}
void sm_fakelib_prepare_title_cache(const char *title_id,
                                    const char *game_path) {
  assert(strcmp(title_id, k_title) == 0);
  assert(strcmp(game_path, expected_generation_path) == 0);
}
bool unmount_title_runtime_layers(const char *title_id) {
  assert(strcmp(title_id, k_title) == 0);
  ++runtime_unmount_calls;
  return true;
}
bool sm_pkg_parse_fd(const char *path, int fd, sm_pkg_info_t *info_out) {
  struct stat st;
  assert(strcmp(path, expected_source_path) == 0);
  assert(fd >= 0 && fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
  memset(info_out, 0, sizeof(*info_out));
  (void)strlcpy(info_out->title_id, k_title, sizeof(info_out->title_id));
  (void)strlcpy(info_out->path, path, sizeof(info_out->path));
  (void)strlcpy(info_out->title_name, "Synthetic debug FPKG",
                sizeof(info_out->title_name));
  info_out->pkg_kind = PKG_KIND_BASE;
  info_out->pkg_classification_confident = true;
  record_event("pkg parse");
  return true;
}
sm_fpkg_status_t sm_fpkg_open_fd(const char *path, int fd,
                                 const sm_pkg_info_t *metadata,
                                 sm_fpkg_t **fpkg_out) {
  assert(strcmp(path, expected_source_path) == 0);
  assert(fd >= 0 && metadata && strcmp(metadata->title_id, k_title) == 0);
  ++fake_fpkg_open_count;
  record_event("FPKG reader");
  *fpkg_out = fake_fpkg_open_status == SM_FPKG_OK
                  ? (sm_fpkg_t *)(uintptr_t)1u
                  : NULL;
  return fake_fpkg_open_status;
}
void sm_fpkg_close(sm_fpkg_t *fpkg) {
  assert(fpkg == (sm_fpkg_t *)(uintptr_t)1u);
  ++fake_fpkg_close_count;
}
sm_fpkg_status_t sm_fpkg_cache_recover_title(
    const char *cache_root, const char *title_id,
    sm_fpkg_cache_recovery_report_t *report_out) {
  assert(strcmp(cache_root, SM_FPKG_CACHE_DEFAULT_ROOT) == 0);
  assert(strcmp(title_id, k_title) == 0);
  if (report_out)
    memset(report_out, 0, sizeof(*report_out));
  record_event("cache recover");
  return SM_FPKG_OK;
}
sm_fpkg_status_t sm_fpkg_cache_build_generation(
    const char *cache_root, const char *source_path, int source_fd,
    const sm_pkg_info_t *metadata, sm_fpkg_t *fpkg,
    sm_fpkg_index_extract_fn should_extract_fn, void *extract_ctx,
    const sm_fpkg_cache_options_t *options, char generation_path_out[MAX_PATH]) {
  assert(strcmp(cache_root, SM_FPKG_CACHE_DEFAULT_ROOT) == 0);
  assert(strcmp(source_path, expected_source_path) == 0);
  assert(source_fd >= 0 && metadata && strcmp(metadata->title_id, k_title) == 0);
  assert(fpkg == (sm_fpkg_t *)(uintptr_t)1u);
  assert(should_extract_fn == NULL && extract_ctx == NULL);
  assert(options && options->max_extract_bytes ==
                          SM_FPKG_CACHE_DEFAULT_EXTRACT_LIMIT);
  assert((!options->backport_path && !test_backport_path) ||
         (options->backport_path && test_backport_path &&
          !strcmp(options->backport_path, test_backport_path)));
  assert(options->is_cancelled &&
         !options->is_cancelled(options->cancel_ctx));
  ++fake_cache_build_count;
  record_event("cache build");
  (void)strlcpy(generation_path_out, expected_generation_path, MAX_PATH);
  return SM_FPKG_OK;
}
bool sm_shellcore_try_begin_external_mutation(void) {
  assert(!external_mutation_active);
  external_mutation_active = true;
  record_event("mutation begin");
  return true;
}
void sm_shellcore_end_external_mutation(void) {
  assert(external_mutation_active);
  external_mutation_active = false;
  record_event("mutation end");
}
bool sm_shellcore_title_is_protected(const char *title_id) {
  assert(strcmp(title_id, k_title) == 0);
  return false;
}
void mark_register_attempted(const char *title_id) { (void)title_id; }
void invalidate_app_db_title_cache(void) {}
void clear_register_attempts(const char *title_id) { (void)title_id; }
void notify_game_installed_rich(const char *title_id) { (void)title_id; }
static void update_registered_title_snd0info(const char *title_id) {
  (void)title_id;
}
void notify_system_l10n(sm_l10n_key_t key, ...) { (void)key; }
void log_debug(const char *format, ...) { (void)format; }
bool mount_sandbox_directory_at(const char *title_id, const char *app0_path,
                                const char *target_suffix,
                                const char *source_path, const char *label,
                                int flags) {
  assert(strcmp(title_id, k_title) == 0);
  assert(strcmp(app0_path, "/mnt/sandbox/PPSA00001_000/app0") == 0);
  ++sandbox_calls;
  record_event("sandbox mount");
  (void)strlcpy(sandbox_target, target_suffix, sizeof(sandbox_target));
  (void)strlcpy(sandbox_source, source_path, sizeof(sandbox_source));
  (void)strlcpy(sandbox_label, label, sizeof(sandbox_label));
  sandbox_flags = flags;
  return true;
}

static void test_title_dir_registration_routes(void) {
  int result = 123;
  bridge_enabled = false;
  api_result = (int)0x80990002u;
  reset_events();
  assert(sm_install_register_title_dir(k_title, APP_BASE "/", &result));
  assert(result == api_result && api_calls == 1 && bridge_calls == 0);
  assert(strcmp(api_title, k_title) == 0);
  assert(strcmp(api_install_path, APP_BASE "/") == 0);

  bridge_enabled = true;
  api_result = -42;
  result = 0;
  reset_events();
  assert(sm_install_register_title_dir(k_title, APP_BASE "/", &result));
  assert(result == -42 && bridge_calls == 1 && api_calls == 0);
  assert(strcmp(events[0], "ShellCore TitleDir bridge") == 0);

  fail_bridge = true;
  assert(!sm_install_register_title_dir(k_title, APP_BASE "/", &result));
  assert(sm_install_register_title_dir(NULL, APP_BASE "/", &result) == false);
  fail_bridge = false;
}

static void test_install_path_component_bounds(void) {
  char path[MAX_PATH];
  assert(append_install_path_component(path, "/user/app", "PPSA00001_000"));
  assert(strcmp(path, "/user/app/PPSA00001_000") == 0);

  char oversized_base[MAX_PATH];
  memset(oversized_base, 'x', sizeof(oversized_base));
  oversized_base[sizeof(oversized_base) - 1u] = '\0';
  memset(path, 'y', sizeof(path));
  assert(!append_install_path_component(path, oversized_base, "sce_sys"));
  assert(path[0] == 'y');
}

static void test_install_publication_and_registration_order(void) {
  reset_events();
  fpkg_feature_enabled = true;
  bridge_enabled = false;
  api_result = 0;
  fail_mount_link = false;
  assert(mount_and_install(k_source, k_title, "Synthetic FPKG", false, true,
                           false, NULL));
  assert(event_count == 4);
  assert(strcmp(events[0], "cache info") == 0);
  assert(strcmp(events[1], "nullfs mount") == 0);
  assert(strcmp(events[2], "mount.lnk") == 0);
  assert(strcmp(events[3], "AppInstallTitleDir") == 0);
  assert(api_calls == 1 && bridge_calls == 0);
  assert(strcmp(api_title, k_title) == 0);
  assert(strcmp(api_install_path, APP_BASE "/") == 0);

  reset_events();
  fail_mount_link = true;
  assert(!mount_and_install(k_source, k_title, "Synthetic FPKG", false, true,
                            false, NULL));
  assert(event_count == 4);
  assert(strcmp(events[0], "cache info") == 0);
  assert(strcmp(events[1], "nullfs mount") == 0);
  assert(strcmp(events[2], "mount.lnk") == 0);
  assert(strcmp(events[3], "nullfs rollback") == 0);
  assert(api_calls == 0 && bridge_calls == 0);
  fail_mount_link = false;

  reset_events();
  assert(mount_and_install(k_source, k_title, "Synthetic FPKG", false, true,
                           true, NULL));
  assert(event_count == 3 && api_calls == 0 && bridge_calls == 0);
  assert(strcmp(events[0], "cache info") == 0);
  assert(strcmp(events[1], "nullfs mount") == 0);
  assert(strcmp(events[2], "mount.lnk") == 0);

  reset_events();
  assert(mount_and_install(k_source, k_title, "Synthetic FPKG", true, false,
                           false, NULL));
  assert(event_count == 3 && api_calls == 0 && bridge_calls == 0);

  reset_events();
  api_result = (int)0x80990002u;
  assert(mount_and_install(k_source, k_title, "Synthetic FPKG", true, true,
                           false, NULL));
  assert(event_count == 4 && api_calls == 1);
  assert(strcmp(events[3], "AppInstallTitleDir") == 0);

  reset_events();
  api_result = -1;
  assert(!mount_and_install(k_source, k_title, "Synthetic FPKG", false, true,
                            false, NULL));
  assert(event_count == 4 && api_calls == 1);
  assert(strcmp(events[3], "AppInstallTitleDir") == 0);

  bool batch_snd0 = true;
  reset_events();
  api_result = 0;
  assert(mount_and_install(k_source, k_title, "Synthetic FPKG", false, true,
                           true, &batch_snd0));
  assert(!batch_snd0);
}

static void test_fpkg_sandbox_mount_route(void) {
  sandbox_calls = 0;
  fpkg_feature_enabled = true;
  memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
  assert(mount_fpkg_source_if_needed());
  assert(sandbox_calls == 0);

  generation_info_status = SM_FPKG_OK;
  generation_info = (sm_fpkg_cache_generation_info_t){0};
  (void)strlcpy(generation_info.source_path, "/tmp/synthetic.pkg",
                sizeof(generation_info.source_path));
  generation_info.source_matches_snapshot = true;
  (void)strlcpy(mount_link_path, expected_generation_path,
                sizeof(mount_link_path));

  (void)strlcpy(g_fakelib_mount.title_id, k_title,
                sizeof(g_fakelib_mount.title_id));
  (void)strlcpy(g_fakelib_mount.sandbox_app0_path,
                "/mnt/sandbox/PPSA00001_000/app0",
                sizeof(g_fakelib_mount.sandbox_app0_path));
  (void)strlcpy(g_fakelib_mount.fpkg_generation_path,
                expected_generation_path,
                sizeof(g_fakelib_mount.fpkg_generation_path));
  (void)strlcpy(g_fakelib_mount.fpkg_source_directory, "/tmp",
                sizeof(g_fakelib_mount.fpkg_source_directory));
  backport_matches = false;
  errno = 0;
  assert(!mount_fpkg_source_if_needed() && errno == ESTALE);
  assert(sandbox_calls == 0);
  backport_matches = true;
  assert(mount_fpkg_source_if_needed());
  assert(sandbox_calls == 1);

  g_fakelib_mount.layers[0].label = "fpkg";
  g_fakelib_mount.layer_count = 1;
  assert(mount_fpkg_source_if_needed());
  assert(sandbox_calls == 1);

  (void)strlcpy(mount_link_path, "/data/shadowmount/fpkg-cache/PPSA00001/"
                                 "gen-replaced",
                sizeof(mount_link_path));
  errno = 0;
  assert(!mount_fpkg_source_if_needed());
  assert(errno == ESTALE && sandbox_calls == 1);
  (void)strlcpy(mount_link_path, expected_generation_path,
                sizeof(mount_link_path));

  assert(mount_fpkg_source_directory(
      k_title, "/mnt/sandbox/PPSA00001_000/app0",
      "/data/shadowmount/fpkg-cache/PPSA00001/generation-host-test"));
  assert(sandbox_calls == 2);
  assert(strcmp(sandbox_target, "/app0/kraken") == 0);
  assert(strcmp(sandbox_source,
                "/data/shadowmount/fpkg-cache/PPSA00001/generation-host-test")
         == 0);
  assert(strcmp(sandbox_label, "fpkg") == 0);
  assert(sandbox_flags == MNT_RDONLY);
}

static void test_fpkg_source_parent_resolution(void) {
  char temporary[] = "/private/tmp/sm-fpkg-source-parent.XXXXXX";
  if (access("/private/tmp", W_OK) != 0)
    (void)strlcpy(temporary, "/tmp/sm-fpkg-source-parent.XXXXXX",
                  sizeof(temporary));
  char *parent = mkdtemp(temporary);
  assert(parent != NULL);
  char source_path[MAX_PATH];
  int length = snprintf(source_path, sizeof(source_path), "%s/source.PKG",
                        parent);
  assert(length > 0 && (size_t)length < sizeof(source_path));

  const char *generation =
      "/data/shadowmount/fpkg-cache/PPSA00001/generation-host-test";
  char directory[MAX_PATH];
  fpkg_feature_enabled = false;
  generation_info_calls = 0;
  errno = 0;
  assert(!resolve_fpkg_source_directory(k_title, generation, directory));
  assert(errno == ENOTSUP && directory[0] == '\0' &&
         generation_info_calls == 0);

  fpkg_feature_enabled = true;
  generation_info_status = SM_FPKG_OK;
  generation_info = (sm_fpkg_cache_generation_info_t){0};
  (void)strlcpy(generation_info.source_path, source_path,
                sizeof(generation_info.source_path));
  generation_info.source_matches_snapshot = false;
  errno = 0;
  assert(!resolve_fpkg_source_directory(k_title, generation, directory));
  assert(errno == ESTALE && directory[0] == '\0' &&
         generation_info_calls == 1);

  generation_info.source_matches_snapshot = true;
  assert(resolve_fpkg_source_directory(k_title, generation, directory));
  assert(strcmp(directory, parent) == 0);

  length = snprintf(generation_info.source_path,
                    sizeof(generation_info.source_path),
                    "%s/missing/source.pkg", parent);
  assert(length > 0 && (size_t)length < sizeof(generation_info.source_path));
  errno = 0;
  assert(!resolve_fpkg_source_directory(k_title, generation, directory));
  assert(errno == ENOENT && directory[0] == '\0');
  assert(rmdir(parent) == 0);
}

static void test_fpkg_source_image_chain_restore(void) {
  char root_template[] = "/private/tmp/sm-fpkg-image-chain.XXXXXX";
  if (access("/private/tmp", W_OK) != 0)
    (void)strlcpy(root_template, "/tmp/sm-fpkg-image-chain.XXXXXX",
                  sizeof(root_template));
  char *root = mkdtemp(root_template);
  assert(root != NULL);
  char inner[MAX_PATH], source_path[MAX_PATH], image_path[MAX_PATH];
  int length = snprintf(inner, sizeof(inner), "%s/inner", root);
  assert(length > 0 && (size_t)length < sizeof(inner));
  assert(mkdir(inner, 0700) == 0);
  length = snprintf(source_path, sizeof(source_path), "%s/source.pkg", inner);
  assert(length > 0 && (size_t)length < sizeof(source_path));
  int source_fd = open(source_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                       0600);
  assert(source_fd >= 0 && close(source_fd) == 0);
  length = snprintf(image_path, sizeof(image_path), "%s/outer.img", root);
  assert(length > 0 && (size_t)length < sizeof(image_path));
  int image_fd = open(image_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                      0600);
  assert(image_fd >= 0 && close(image_fd) == 0);

  (void)strlcpy(image_mount_root, root, sizeof(image_mount_root));
  (void)strlcpy(synthetic_image_chain[0], image_path,
                sizeof(synthetic_image_chain[0]));
  synthetic_image_chain_count = 1;
  synthetic_image_mount_succeeds = true;
  synthetic_remount_changes_md_identity = true;
  synthetic_remounted_snapshot_matches = true;
  fpkg_feature_enabled = true;
  generation_info_status = SM_FPKG_OK;
  generation_info = (sm_fpkg_cache_generation_info_t){0};
  (void)strlcpy(generation_info.source_path, source_path,
                sizeof(generation_info.source_path));
  generation_info.source_matches_snapshot = false;
  generation_info_calls = 0;
  reset_events();

  char directory[MAX_PATH];
  assert(resolve_fpkg_source_directory(
      k_title, expected_generation_path, directory));
  assert(strcmp(directory, inner) == 0);
  assert(generation_info_calls == 2 && event_count == 3);
  assert(strcmp(events[0], "cache info") == 0);
  assert(strcmp(events[1], "image mount") == 0);
  assert(strcmp(events[2], "cache info") == 0);

  generation_info.source_matches_snapshot = false;
  generation_info_calls = 0;
  reset_events();
  prepare_image_source_calls = 0;
  runtime_unmount_calls = 0;
  assert(prepare_title_runtime(k_title, expected_generation_path, NULL));
  assert(generation_info_calls == 3 && prepare_image_source_calls == 1);
  assert(runtime_unmount_calls == 0);

  synthetic_remounted_snapshot_matches = false;
  generation_info.source_matches_snapshot = false;
  generation_info_calls = 0;
  reset_events();
  unsigned prepare_calls_before = prepare_image_source_calls;
  errno = 0;
  assert(!prepare_title_runtime(k_title, expected_generation_path, NULL));
  assert(errno == ESTALE && generation_info_calls == 3 &&
         prepare_image_source_calls == prepare_calls_before);

  image_mount_root[0] = '\0';
  synthetic_image_chain_count = 0;
  synthetic_image_mount_succeeds = false;
  synthetic_remount_changes_md_identity = false;
  synthetic_remounted_snapshot_matches = false;
  assert(unlink(source_path) == 0);
  assert(unlink(image_path) == 0);
  assert(rmdir(inner) == 0);
  assert(rmdir(root) == 0);
}

static void test_candidate_to_registered_launch_flow(void) {
  char parent_template[] = "/private/tmp/sm-fpkg-flow.XXXXXX";
  if (access("/private/tmp", W_OK) != 0)
    (void)strlcpy(parent_template, "/tmp/sm-fpkg-flow.XXXXXX",
                  sizeof(parent_template));
  char *parent = mkdtemp(parent_template);
  assert(parent != NULL);
  int cache_root_length = snprintf(expected_cache_root,
                                   sizeof(expected_cache_root), "%s/cache",
                                   parent);
  assert(cache_root_length > 0 &&
         (size_t)cache_root_length < sizeof(expected_cache_root));
  int length = snprintf(expected_source_path, sizeof(expected_source_path),
                        "%s/source.pkg", parent);
  assert(length > 0 && (size_t)length < sizeof(expected_source_path));
  int source_fd = open(expected_source_path,
                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(source_fd >= 0);
  assert(write(source_fd, "synthetic", 9u) == 9);
  assert(close(source_fd) == 0);

  length = snprintf(expected_generation_path, sizeof(expected_generation_path),
                    "%s/cache/PPSA00001/generation-flow", parent);
  assert(length > 0 && (size_t)length < sizeof(expected_generation_path));
  generation_info = (sm_fpkg_cache_generation_info_t){0};
  (void)strlcpy(generation_info.source_path, expected_source_path,
                sizeof(generation_info.source_path));
  generation_info.source_matches_snapshot = true;
  generation_info_status = SM_FPKG_OK;
  test_backport_path = "/tmp/synthetic-backport";
  fpkg_feature_enabled = true;
  generation_info_calls = 0;
  fake_fpkg_open_count = 0;
  fake_fpkg_close_count = 0;
  fake_cache_build_count = 0;
  fake_fpkg_open_status = SM_FPKG_OK;
  reset_events();

  assert(is_native_fpkg_path(expected_source_path));
  assert(is_native_fpkg_path("/source/UPPER.PKG"));
  assert(!is_native_fpkg_path("/source/game.pkg.bak"));
  scan_app_db_context_t app_db = {.titles_ready = true};
  scan_candidate_t candidates[2] = {0};
  int candidate_count = 0;
  bool unstable_found = false;
  fpkg_feature_enabled = false;
  assert(try_collect_candidate_for_fpkg(
      expected_source_path, candidates, 2, &candidate_count, &app_db, NULL,
      &unstable_found));
  assert(candidate_count == 0 && fake_fpkg_open_count == 0);

  fpkg_feature_enabled = true;
  fake_fpkg_open_status = SM_FPKG_UNSUPPORTED;
  assert(try_collect_candidate_for_fpkg(
      expected_source_path, candidates, 2, &candidate_count, &app_db, NULL,
      &unstable_found));
  assert(candidate_count == 0 && fake_fpkg_open_count == 1 &&
         fake_fpkg_close_count == 0);

  fake_fpkg_open_status = SM_FPKG_OK;
  image_index_record_calls = 0;
  image_index_record_succeeds = true;
  assert(try_collect_candidate_for_fpkg(
      expected_source_path, candidates, 2, &candidate_count, &app_db, NULL,
      &unstable_found));
  assert(candidate_count == 1 && !unstable_found);
  scan_candidate_t *candidate = &candidates[0];
  assert(candidate->fpkg && !candidate->installed && !candidate->in_app_db &&
         strcmp(candidate->path, expected_source_path) == 0 &&
         strcmp(candidate->title_id, k_title) == 0);
  assert(fake_fpkg_open_count == 2 && fake_fpkg_close_count == 1);

  image_index_record_succeeds = false;
  unstable_found = false;
  assert(try_collect_candidate_for_fpkg(
      expected_source_path, candidates, 2, &candidate_count, &app_db, NULL,
      &unstable_found));
  assert(candidate_count == 1 && unstable_found &&
         image_index_record_calls == 2);
  image_index_record_succeeds = true;

  reset_events();
  char previous_generation[MAX_PATH];
  assert(prepare_fpkg_candidate(candidate, previous_generation));
  assert(!candidate->fpkg &&
         strcmp(candidate->path, expected_generation_path) == 0);
  assert(previous_generation[0] == '\0' && !external_mutation_active);
  assert(fake_fpkg_open_count == 4 && fake_fpkg_close_count == 3 &&
         fake_cache_build_count == 1 && generation_info_calls == 1);
  assert(event_count == 7);
  assert(strcmp(events[0], "mutation begin") == 0);
  assert(strcmp(events[1], "pkg parse") == 0);
  assert(strcmp(events[2], "FPKG reader") == 0);
  assert(strcmp(events[3], "cache recover") == 0);
  assert(strcmp(events[4], "cache build") == 0);
  assert(strcmp(events[5], "cache info") == 0);
  assert(strcmp(events[6], "mutation end") == 0);

  reset_events();
  bridge_enabled = false;
  api_result = 0;
  assert(mount_and_install(candidate->path, k_title, candidate->title_name,
                           false, true, false, NULL));
  assert(event_count == 4 && api_calls == 1);
  assert(strcmp(events[0], "cache info") == 0);
  assert(strcmp(events[1], "nullfs mount") == 0);
  assert(strcmp(events[2], "mount.lnk") == 0);
  assert(strcmp(events[3], "AppInstallTitleDir") == 0);

  reset_events();
  generation_info_calls = 0;
  char source_directory[MAX_PATH];
  assert(resolve_fpkg_source_directory(k_title, candidate->path,
                                       source_directory));
  assert(strcmp(source_directory, parent) == 0 && generation_info_calls == 1);
  memset(&g_fakelib_mount, 0, sizeof(g_fakelib_mount));
  (void)strlcpy(g_fakelib_mount.title_id, k_title,
                sizeof(g_fakelib_mount.title_id));
  (void)strlcpy(g_fakelib_mount.sandbox_app0_path,
                "/mnt/sandbox/PPSA00001_000/app0",
                sizeof(g_fakelib_mount.sandbox_app0_path));
  (void)strlcpy(g_fakelib_mount.fpkg_source_directory, source_directory,
                sizeof(g_fakelib_mount.fpkg_source_directory));
  (void)strlcpy(g_fakelib_mount.fpkg_generation_path,
                expected_generation_path,
                sizeof(g_fakelib_mount.fpkg_generation_path));
  assert(mount_fpkg_source_if_needed());
  assert(sandbox_calls == 3 && strcmp(sandbox_source, parent) == 0);
  assert(strcmp(events[0], "cache info") == 0);
  assert(strcmp(events[1], "cache info") == 0);
  assert(strcmp(events[2], "sandbox mount") == 0);

  fake_find_status = SM_FPKG_IO;
  candidate_count = 0;
  unstable_found = false;
  assert(try_collect_candidate_for_fpkg(
      expected_source_path, candidates, 2, &candidate_count, &app_db, NULL,
      &unstable_found));
  assert(candidate_count == 0 && unstable_found);
  fake_find_status = SM_FPKG_UNSUPPORTED;
  fake_installed = true;
  fake_data_mounted = true;
  (void)strlcpy(mount_link_path, expected_generation_path, sizeof(mount_link_path));
  candidate_count = 0;
  unstable_found = false;
  assert(try_collect_candidate_for_fpkg(
      expected_source_path, candidates, 2, &candidate_count, &app_db, NULL,
      &unstable_found));
  assert(candidate_count == 1 && !unstable_found && candidates[0].fpkg);
  fake_installed = false;
  fake_data_mounted = false;

  assert(unlink(expected_source_path) == 0);
  assert(rmdir(parent) == 0);
  test_backport_path = NULL;
}

int main(void) {
  generation_info_status = SM_FPKG_OK;
  (void)strlcpy(generation_info.source_path, "/tmp/synthetic.pkg",
                sizeof(generation_info.source_path));
  generation_info.source_matches_snapshot = true;
  test_title_dir_registration_routes();
  test_install_path_component_bounds();
  test_install_publication_and_registration_order();
  test_fpkg_sandbox_mount_route();
  test_fpkg_source_parent_resolution();
  test_fpkg_source_image_chain_restore();
  test_candidate_to_registered_launch_flow();
  puts("FPKG scan-to-launch routing: candidate selection, source preparation, registration, rollback, and read-only sandbox target passed");
  return 0;
}
