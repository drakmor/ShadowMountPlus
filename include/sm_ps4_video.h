#ifndef SM_PS4_VIDEO_H
#define SM_PS4_VIDEO_H
#include <stdint.h>
#include <sys/types.h>
// Called only by the lifecycle owner. No extra worker or permanent timer.
void sm_ps4_video_on_exec(pid_t pid, const char *title_id);
void sm_ps4_video_note_module_ready(pid_t pid);
void sm_ps4_video_poll(void);
void sm_ps4_video_reset(void);
uint64_t sm_ps4_video_next_wake_us(void);
#endif
