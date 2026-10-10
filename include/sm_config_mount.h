#ifndef SM_CONFIG_MOUNT_H
#define SM_CONFIG_MOUNT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sm_types.h"

// Ensure runtime configuration is loaded before use.
void ensure_runtime_config_ready(void);
// Load runtime configuration from disk and apply defaults.
bool load_runtime_config(void);
// Reload runtime configuration from disk when config.ini changed.
bool reload_runtime_config_if_changed(bool *reloaded_out);
// Atomically persist the settings exposed by the HTTP API while preserving
// unrelated config.ini keys and comments. Runtime reload remains scanner-owned.
bool sm_config_write_web_settings(bool debug_enabled, bool quiet_mode,
                                  bool update_emulators_enabled,
                                  bool auto_update_ampr_enabled,
                                  bool auto_remove_missing_games,
                                  uint32_t auto_remove_missing_delay_seconds,
                                  bool allow_lan_access,
                                  uint32_t fan_target_temperature_c,
                                  const char *const *scan_paths,
                                  size_t scan_path_count);
// Return a coherent caller-owned copy of the current runtime configuration.
runtime_config_t runtime_config(void);
// Return the number of configured scan roots.
int get_scan_path_count(void);
// Copy a scan root; return false and an empty path if the index is invalid.
bool get_scan_path(int index, char path_out[MAX_PATH]);
// Backport lookup uses scan roots followed by the permanent internal fallback.
// These paths do not add roots to normal game scanning.
int get_backport_scan_path_count(void);
bool get_backport_scan_path(int index, char path_out[MAX_PATH]);
// Return only scan roots explicitly configured through scanpath entries.
int get_custom_scan_path_count(void);
bool get_custom_scan_path(int index, char path_out[MAX_PATH]);
// Return scan depth for a root, including managed container-root expansion.
uint32_t get_scan_depth_for_root(const char *scan_path);
// Resolve a per-image read-only override from the file name.
bool get_image_mode_override(const char *filename, bool *mount_read_only_out);
// Resolve a per-image sector-size override from autotune.ini or config.ini.
bool get_image_sector_size_override(const char *filename,
                                    uint32_t *sector_size_out);
// Upsert an autotuned per-image sector-size override.
bool upsert_image_sector_size_autotune(const char *filename,
                                       uint32_t sector_size,
                                       uint32_t *sector_size_out);
// Return true when the global fakelib overlay is disabled for this title.
bool is_global_fakelib_excluded_for_title(const char *title_id);
// Return true when all fakelib overlays are disabled for this title.
bool is_fakelib_excluded_for_title(const char *title_id);
// Resolve a mode from a snapshot or the live config; absent rules mean full.
sm_fakelib_mode_t sm_config_title_fakelib_mode(const runtime_config_t *cfg,
                                             const char *title_id);
sm_fakelib_mode_t get_fakelib_mode_for_title(const char *title_id);
const char *sm_config_fakelib_mode_name(sm_fakelib_mode_t mode);
bool sm_config_parse_fakelib_mode(const char *value, sm_fakelib_mode_t *mode);
// Persist the mode atomically without changing active mounts.
bool sm_config_set_title_fakelib_mode(const char *title_id, sm_fakelib_mode_t mode);
// Compatibility API: enabled selects full, false selects disabled.
bool sm_config_set_title_fakelib_enabled(const char *title_id, bool enabled);

#endif
