#ifndef SM_PS5SX2_H
#define SM_PS5SX2_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

// The shortcut adapter for PS5SX2 (the PCSX2 port, title PPSA99203).
// Released PS5SX2 builds take no launch arguments, so a shortcut's
// "--boot <image>" is handed over the way PS5SX2 itself remembers a game:
// /data/PCSX2/lastgame.txt names it, and the nomenu flag skips the shelf until
// PS5SX2 has picked it up. The arguments still reach PS5SX2 for builds that
// read them.

typedef struct {
  char file[NAME_MAX + 1];
  struct timespec written;
  bool flag_ours;
} sm_ps5sx2_launch_t;

// Checks a shortcut's target and arguments for this adapter. Returns 0, or an
// errno value with *reason_out set to a reason code.
int sm_ps5sx2_check(const char *target_title_id, const char *const *args,
                    size_t arg_count, sm_ps5sx2_launch_t *launch,
                    const char **reason_out);

// Names the image in lastgame.txt and sets the nomenu flag, before the launch.
bool sm_ps5sx2_prepare(sm_ps5sx2_launch_t *launch);

// After the launch: waits for PS5SX2 to pick the game up, then removes the
// flag; removes it at once when the launch failed.
void sm_ps5sx2_finish(sm_ps5sx2_launch_t *launch, bool launched);

// Removes a nomenu flag left behind by an interrupted launch.
void sm_ps5sx2_recover(void);

#endif
