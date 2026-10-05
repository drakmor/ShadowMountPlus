#ifndef SM_PS5SX2_H
#define SM_PS5SX2_H

#include <stdbool.h>

// Starts PS5SX2 (the PCSX2 port, title PPSA99203) straight into one PS2 disc
// image, for home screen shortcuts. Released PS5SX2 builds take no launch
// arguments, so the image is handed over the way PS5SX2 itself remembers a
// game: /data/PCSX2/lastgame.txt names it, and the nomenu flag skips the shelf
// until PS5SX2 has picked it up. The image also goes in argv ("--boot <path>")
// for builds that read it.

// Checks the request and starts the launch on a worker thread, so the caller
// (usually the shortcut app that is about to be closed) gets its answer first.
// Returns 0, or an errno value; *reason_out is set to a reason code for
// errors the caller can fix, and stays NULL otherwise.
int sm_ps5sx2_request_launch(const char *image_path, const char **reason_out);

// Removes a nomenu flag left behind by an interrupted launch.
void sm_ps5sx2_recover(void);

#endif
