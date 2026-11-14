/* Async registration must finish before an old FPKG cache is reclaimed. */
#include "host_platform.h"

#include "sm_appdb.h"
#include "sm_config_mount.h"
#include "sm_game_cache.h"
#include "sm_install.h"
#include "sm_install_queue.h"
#include "sm_l10n.h"
#include "sm_log.h"
#include "sm_manual.h"
#include "sm_runtime.h"
#include "sm_time.h"
#include "sm_title_state.h"

static runtime_config_t test_config = {.app_install_all_enabled = true};
static bool app_db_has_title;
static bool app_db_has_failed_title;
static bool cleanup_allowed = true;
static unsigned cleanup_calls;
static unsigned scan_runtime_release_calls;
static uint64_t fake_clock = 100;
static char cached_path[MAX_PATH];
static char cleaned_generations[32][112];

runtime_config_t runtime_config(void) { return test_config; }
uint64_t monotonic_time_us(void) { return ++fake_clock; }
bool runtime_sleep_mode_active(void) { return false; }

bool get_app_db_title_list_cached(struct AppDbTitleList *list_out) {
  (void)list_out;
  return true;
}
bool app_db_title_list_contains(const struct AppDbTitleList *list,
                                const char *title_id) {
  (void)list;
  return (app_db_has_title && strcmp(title_id, "PPSA00001") == 0) ||
         (app_db_has_failed_title && strcmp(title_id, "PPSA00002") == 0);
}
void free_app_db_title_list(struct AppDbTitleList *list) { (void)list; }
void invalidate_app_db_title_cache(void) {}

void log_debug(const char *format, ...) { (void)format; }
void notify_system_info(const char *format, ...) { (void)format; }
void notify_system_l10n(sm_l10n_key_t id, ...) { (void)id; }
const char *sm_l10n_get(sm_l10n_key_t id) {
  (void)id;
  return "queued %d";
}
void notify_game_installed_rich(const char *title_id) {
  assert(strcmp(title_id, "PPSA00001") == 0 ||
         strcmp(title_id, "PPSA00002") == 0);
}
int sceAppInstUtilAppInstallAll(void *reserved) {
  (void)reserved;
  return 0;
}

void cache_game_entry(const char *path, const char *title_id,
                      const char *title_name) {
  assert(strcmp(title_id, "PPSA00001") == 0 ||
         strcmp(title_id, "PPSA00002") == 0);
  assert((strcmp(title_id, "PPSA00001") == 0 &&
          strcmp(title_name, "Synthetic Game") == 0) ||
         (strcmp(title_id, "PPSA00002") == 0 &&
          strcmp(title_name, "Failed Synthetic Game") == 0));
  (void)strlcpy(cached_path, path, sizeof(cached_path));
}
bool release_scan_runtime_mounts(void) {
  ++scan_runtime_release_calls;
  return true;
}
void clear_register_attempts(const char *title_id) { (void)title_id; }
void clear_failed_mount_attempts(const char *title_id) { (void)title_id; }
uint8_t bump_failed_mount_attempts(const char *title_id) {
  (void)title_id;
  return 1;
}
uint8_t get_failed_mount_attempts(const char *title_id) {
  (void)title_id;
  return 0;
}
int update_snd0info(const char *title_id) {
  (void)title_id;
  return 0;
}
void sm_manual_note_installed(const char *source_path, const char *title_id,
                              const char *title_name) {
  (void)source_path;
  (void)title_id;
  (void)title_name;
}

bool sm_install_release_replaced_fpkg_generation(
    const char *title_id, const char *previous_path, const char *current_path) {
  assert(strcmp(title_id, "PPSA00001") == 0 ||
         strcmp(title_id, "PPSA00002") == 0 ||
         strcmp(title_id, "PPSA00003") == 0 ||
         strcmp(title_id, "PPSA00004") == 0 ||
         strcmp(title_id, "PPSA00005") == 0);
  assert(cleanup_calls < sizeof(cleaned_generations) /
                             sizeof(cleaned_generations[0]));
  (void)strlcpy(cleaned_generations[cleanup_calls], previous_path,
                sizeof(cleaned_generations[cleanup_calls]));
  if (strcmp(title_id, "PPSA00001") == 0)
    assert(strcmp(current_path, "/cache/PPSA00001/gen-newest") == 0);
  else if (strcmp(title_id, "PPSA00002") == 0)
    assert(strcmp(current_path, "/cache/PPSA00002/gen-newer") == 0);
  else if (strcmp(title_id, "PPSA00003") == 0)
    assert(strcmp(current_path, "/cache/PPSA00003/gen-current") == 0);
  else if (strcmp(title_id, "PPSA00004") == 0)
    assert(strcmp(current_path, "/cache/PPSA00004/gen-current") == 0);
  else
    assert(strncmp(current_path, "/cache/PPSA00005/gen-current-",
                   strlen("/cache/PPSA00005/gen-current-")) == 0);
  ++cleanup_calls;
  return cleanup_allowed;
}

int main(void) {
  scan_candidate_t candidate = {0};
  (void)strlcpy(candidate.path, "/cache/PPSA00001/gen-new",
                sizeof(candidate.path));
  (void)strlcpy(candidate.title_id, "PPSA00001",
                sizeof(candidate.title_id));
  (void)strlcpy(candidate.title_name, "Synthetic Game",
                sizeof(candidate.title_name));

  assert(sm_install_queue_candidate(
      &candidate, false, "gen-old"));
  (void)strlcpy(candidate.path, "/cache/PPSA00001/gen-newest",
                sizeof(candidate.path));
  assert(sm_install_queue_candidate(
      &candidate, false, "gen-middle"));
  assert(cleanup_calls == 0);
  assert(sm_install_has_pending_work());
  assert(sm_install_next_wake_us(fake_clock) != 0);
  assert(sm_install_submit_queued());
  assert(cleanup_calls == 0);
  assert(sm_install_has_pending_work());

  app_db_has_title = true;
  cleanup_allowed = false;
  sm_install_poll_pending();
  assert(cleanup_calls == 2);
  assert(strcmp(cleaned_generations[0], "gen-old") == 0);
  assert(strcmp(cleaned_generations[1], "gen-middle") == 0);
  assert(strcmp(cached_path, "/cache/PPSA00001/gen-newest") == 0);
  assert(!sm_install_has_pending_work());
  assert(sm_install_next_wake_us(fake_clock) != 0);
  cleanup_allowed = true;
  sm_install_poll_pending();
  assert(cleanup_calls == 4);
  assert(strcmp(cleaned_generations[2], "gen-old") == 0);
  assert(strcmp(cleaned_generations[3], "gen-middle") == 0);
  assert(!sm_install_has_pending_work());

  cleanup_allowed = false;
  assert(sm_install_defer_fpkg_generation_cleanup(
      "PPSA00003", "gen-sync-old", "/cache/PPSA00003/gen-current"));
  assert(!sm_install_has_pending_work());
  assert(sm_install_next_wake_us(fake_clock) != 0);
  sm_install_poll_pending();
  assert(cleanup_calls == 5);
  assert(strcmp(cleaned_generations[4], "gen-sync-old") == 0);
  assert(!sm_install_has_pending_work());
  cleanup_allowed = true;
  sm_install_poll_pending();
  assert(cleanup_calls == 6);
  assert(strcmp(cleaned_generations[5], "gen-sync-old") == 0);

  scan_candidate_t forgotten_candidate = {0};
  (void)strlcpy(forgotten_candidate.title_id, "PPSA00004",
                sizeof(forgotten_candidate.title_id));
  (void)strlcpy(forgotten_candidate.title_name, "Forgotten Synthetic Game",
                sizeof(forgotten_candidate.title_name));
  (void)strlcpy(forgotten_candidate.path,
                "/cache/PPSA00004/gen-current",
                sizeof(forgotten_candidate.path));
  assert(sm_install_queue_candidate(
      &forgotten_candidate, false, "gen-forgotten-old"));
  assert(is_title_install_pending("PPSA00004"));
  sm_install_forget_pending_title("PPSA00004");
  assert(!is_title_install_pending("PPSA00004"));
  assert(!sm_install_has_pending_work());
  cleanup_allowed = false;
  sm_install_poll_pending();
  assert(cleanup_calls == 7);
  assert(strcmp(cleaned_generations[6], "gen-forgotten-old") == 0);
  cleanup_allowed = true;
  sm_install_poll_pending();
  assert(cleanup_calls == 8);
  assert(strcmp(cleaned_generations[7], "gen-forgotten-old") == 0);

  scan_candidate_t failed_candidate = {0};
  (void)strlcpy(failed_candidate.path, "/cache/PPSA00002/gen-new",
                sizeof(failed_candidate.path));
  (void)strlcpy(failed_candidate.title_id, "PPSA00002",
                sizeof(failed_candidate.title_id));
  (void)strlcpy(failed_candidate.title_name, "Failed Synthetic Game",
                sizeof(failed_candidate.title_name));
  assert(sm_install_queue_candidate(
      &failed_candidate, false, "gen-old-failed"));
  assert(sm_install_submit_queued());
  fake_clock += UINT64_C(301000000);
  sm_install_poll_pending();
  assert(cleanup_calls == 8);
  assert(!sm_install_has_pending_work());
  assert(!is_title_install_pending("PPSA00002"));

  (void)strlcpy(failed_candidate.path, "/cache/PPSA00002/gen-newer",
                sizeof(failed_candidate.path));
  assert(sm_install_queue_candidate(
      &failed_candidate, false, "gen-current-failed"));
  assert(is_title_install_pending("PPSA00002"));
  app_db_has_failed_title = true;
  assert(sm_install_submit_queued());
  sm_install_poll_pending();
  assert(cleanup_calls == 10);
  assert(strcmp(cleaned_generations[8], "gen-old-failed") == 0);
  assert(strcmp(cleaned_generations[9], "gen-current-failed") == 0);
  assert(!sm_install_has_pending_work());

  scan_candidate_t many_generations_candidate = {0};
  (void)strlcpy(many_generations_candidate.title_id, "PPSA00005",
                sizeof(many_generations_candidate.title_id));
  (void)strlcpy(many_generations_candidate.title_name,
                "Many Generations Synthetic Game",
                sizeof(many_generations_candidate.title_name));
  unsigned first_many_generation_cleanup = cleanup_calls;
  for (unsigned i = 0; i < 10; ++i) {
    char generation_name[112];
    int written = snprintf(generation_name, sizeof(generation_name),
                           "gen-displaced-%02u", i);
    assert(written > 0 && (size_t)written < sizeof(generation_name));
    written = snprintf(many_generations_candidate.path,
                       sizeof(many_generations_candidate.path),
                       "/cache/PPSA00005/gen-current-%02u", i);
    assert(written > 0 &&
           (size_t)written < sizeof(many_generations_candidate.path));
    assert(sm_install_queue_candidate(&many_generations_candidate, false,
                                      generation_name));
  }
  sm_install_forget_pending_title("PPSA00005");
  assert(!is_title_install_pending("PPSA00005"));
  sm_install_poll_pending();
  assert(cleanup_calls == first_many_generation_cleanup + 10);
  for (unsigned i = 0; i < 10; ++i) {
    char expected[112];
    int written = snprintf(expected, sizeof(expected), "gen-displaced-%02u", i);
    assert(written > 0 && (size_t)written < sizeof(expected));
    assert(strcmp(cleaned_generations[first_many_generation_cleanup + i],
                  expected) == 0);
  }
  unsigned releases_before_scan_poll = scan_runtime_release_calls;
  sm_install_poll_pending_for_scan();
  assert(scan_runtime_release_calls == releases_before_scan_poll);
  sm_install_service_pending();
  assert(scan_runtime_release_calls == releases_before_scan_poll + 1);
  puts("async FPKG cache cleanup waits for confirmed app.db registration");
  return 0;
}
