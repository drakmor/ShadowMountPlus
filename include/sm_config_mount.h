#ifndef SM_CONFIG_MOUNT_H
#define SM_CONFIG_MOUNT_H

#include <stdbool.h>
#include <stdint.h>

typedef struct runtime_config runtime_config_t;

// Ensure runtime configuration is loaded before use.
void ensure_runtime_config_ready(void);
// Load runtime configuration from disk and apply defaults.
bool load_runtime_config(void);
// Reload runtime configuration from disk when config.ini changed.
bool reload_runtime_config_if_changed(bool *reloaded_out);
// Return the current runtime configuration.
const runtime_config_t *runtime_config(void);
// Return the number of configured scan roots.
int get_scan_path_count(void);
// Return a scan root by index, or NULL if out of range.
const char *get_scan_path(int index);
// Resolve a per-image read-only override from the file name.
bool get_image_mode_override(const char *filename, bool *mount_read_only_out);
// Resolve a per-image sector-size override from autotune.ini or config.ini.
bool get_image_sector_size_override(const char *filename,
                                    uint32_t *sector_size_out);
// Return true when kstuff auto-pause is disabled for the given title ID.
bool is_kstuff_pause_disabled_for_title(const char *title_id);
// Resolve a per-title kstuff pause-delay override in seconds.
bool get_kstuff_pause_delay_override_for_title(const char *title_id,
                                               uint32_t *delay_seconds_out);
// Resolve a per-title autotuned kstuff pause-delay override in seconds.
bool get_kstuff_autotune_pause_delay_for_title(const char *title_id,
                                               uint32_t *delay_seconds_out);
// Upsert an autotuned pause-delay override for the title.
bool upsert_kstuff_autotune_pause_delay(const char *title_id,
                                        uint32_t current_delay_seconds,
                                        uint32_t *delay_seconds_out);
// Upsert an autotuned per-image sector-size override.
bool upsert_image_sector_size_autotune(const char *filename,
                                       uint32_t sector_size,
                                       uint32_t *sector_size_out);

#endif
