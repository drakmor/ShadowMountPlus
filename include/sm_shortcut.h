#ifndef SM_SHORTCUT_H
#define SM_SHORTCUT_H

#include <stdbool.h>

// Home screen shortcuts: small apps that start another installed app with
// arguments, such as an emulator straight into one game. A shortcut is a
// folder game holding smp-launch.json:
//   {"title_id": "PPSA99203", "args": ["--boot", "/data/..."], "adapter": "ps5sx2"}
// Its eboot.bin is ShadowMountPlus's launcher, which only asks the local API
// to launch; an app cannot start another app itself. ShadowMountPlus puts the
// launcher and the app loader's libc.prx shim into the folder.
#define SM_SHORTCUT_FILE "smp-launch.json"

// Called for each folder game the scanner finds: when the folder is a
// shortcut, puts the launcher in place as eboot.bin and sce_module/libc.prx.
void sm_shortcut_ensure_launcher(const char *folder);

// Launches the target of the shortcut that is running as the big app. The
// request names nothing: the shortcut is found from the running app, so a
// caller can only start what an installed shortcut declares. Checks run
// first; the launch itself runs on a worker after the caller is answered.
// Returns 0, or an errno value with *reason_out set to a reason code for
// errors the user can fix.
int sm_shortcut_request_launch(const char **reason_out);

#endif
