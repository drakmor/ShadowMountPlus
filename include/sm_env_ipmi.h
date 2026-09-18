/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The environment service: what ShadowMountPlus tells a sandboxed title about
 * the console it is running on. A backported title needs to know whether the
 * loaded kstuff carries the getSceSysDirPath and trophy patches its NP
 * registrations depend on, and it cannot find that out from inside the sandbox.
 */
#ifndef SM_ENV_IPMI_H
#define SM_ENV_IPMI_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The wire contract. Every client carries its own copy of this block, and a
 * copy that drifts from this one does not fail at build time -- it fails on
 * hardware, as a struct one side fills and the other misreads. Change this and
 * you change every client with it.
 */

/* Two rules, both learned by breaking them.
 *
 * The name must start with "Sce". IPMIMGR kills the caller from inside create()
 * otherwise, having logged:
 *
 *   [IPMIMGR] ERROR: [Bug #140942] IPMI server name created by the system
 *   process must be given "Sce" prefix.
 *
 * A payload started by the ELF loader counts as a system process. Spelled as a
 * concatenation so the prefix cannot be dropped without deleting it from this
 * line.
 *
 * It must also fit Config::name[16] including the NUL. The server truncates
 * with strncpy(size - 1) and the client with a flat 16-byte memcpy, so a name
 * at the boundary registers under one spelling and is looked up under another,
 * and every connect answers ESRCH against a service that is registered and
 * serving. 14 is the budget, asserted on both sides. */
#define SMP_ENV_IPMI_SERVICE_PREFIX "Sce"
#define SMP_ENV_IPMI_SERVICE SMP_ENV_IPMI_SERVICE_PREFIX "ShadowMnt"

/* The one command. A gate that needs a conversation is a gate that can hang
 * halfway through one. */
#define SMP_ENV_IPMI_CMD_QUERY 0x534D5001u

/* Bumped when the layout or a field's meaning changes. A client that meets a
 * reply_version it does not know refuses to judge rather than guessing.
 * 3: kstuff reported as measured capabilities, not a variant plus version. */
#define SMP_ENV_REPLY_VERSION 3u

/* Which optional SceShellCore patches the loaded kstuff actually applied. These
 * are the difference between kstuff-lite and full kstuff, and they are read out
 * of ShellCore's live text -- see sm_kstuff_caps.c. */
#define SMP_KSTUFF_CAP_SYSDIRPATH (1u << 0)
#define SMP_KSTUFF_CAP_TROPHY (1u << 1)

/* PRESENT: the sysentvec probe recognised kstuff's toggle, so it really is
 * loaded. ENABLED: both sysentvecs are on -- this server toggles them itself
 * around some launches. CAPS_VALID: kstuff_caps was measured; without it the
 * capabilities are unknown, which is not the same as absent. */
#define SMP_ENV_FLAG_KSTUFF_PRESENT (1u << 0)
#define SMP_ENV_FLAG_KSTUFF_ENABLED (1u << 1)
#define SMP_ENV_FLAG_CAPS_VALID (1u << 2)

/* major*1000000 + minor*1000 + patch, compared component-wise. */
#define SMP_ENV_VERSION(maj, min, pat)                                        \
  ((uint32_t)(maj) * 1000000u + (uint32_t)(min) * 1000u + (uint32_t)(pat))

/* This build's own version, hand-maintained: bump on every release and never
 * let it go backwards. Not derived from SHADOWMOUNT_VERSION because that is
 * `git describe` output and tags like `1.6beta16` have no orderable parse.
 * A pre-release takes the previous minor with a high patch (1.5.916). */
#define SMP_ENV_SMP_VERSION SMP_ENV_VERSION(1, 6, 0) /* 1.6 */

/* Fixed size, no pointers: this crosses a process boundary as raw bytes. */
typedef struct SmpEnvReply {
  uint32_t reply_version;
  uint32_t flags;
  uint32_t kstuff_caps;
  uint32_t smp_version_num;
  char smp_version[32];
} SmpEnvReply;

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve libSceIpmi, register the handler and start the dispatcher thread.
 * -> false having logged why. Never fatal: a console with no backported title
 * does not need this service at all, so main() logs and carries on. */
bool sm_env_ipmi_serve(void);

/* Stop the dispatcher and, only if it really left, destroy the registration.
 * A registration must never outlive its dispatcher: a client probing a name
 * held by a process that no longer answers reads it free and is killed inside
 * create(). Nothing detects that state and only a reboot clears it. */
void sm_env_ipmi_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SM_ENV_IPMI_H */
