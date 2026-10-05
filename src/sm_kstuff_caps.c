#include "sm_platform.h"

#include "sm_kstuff_caps.h"
#include "sm_log.h"
#include "sm_runtime.h"

// The kernel reports "SceShellCore", with no ".elf" unlike SceSysCore.elf.
static const char *const k_shellcore_name = "SceShellCore";
#define SHELLCORE_MAIN_MODULE_HANDLE 0u

// The two SceShellCore patches kstuff-lite applies and full kstuff does not,
// read back at kstuff-lite's own retail offsets. The table is generated from
// kstuff-lite's own patch headers by tools/generate_kstuff_caps_offsets.py, so
// a firmware it gains is picked up by re-running that rather than by hand.
#define CAP_SIG_MAX 6u
typedef struct {
  uint16_t firmware;
  const char *name;
  uint32_t sysdir_off;
  uint8_t sysdir_len;
  uint8_t sysdir_sig[CAP_SIG_MAX];
  uint32_t trophy_off;
  uint8_t trophy_len;
  uint8_t trophy_sig[CAP_SIG_MAX];
} shellcore_cap_row_t;

static const shellcore_cap_row_t g_cap_rows[] = {
#include "sm_kstuff_caps_offsets.inc"
};

static bool g_caps_probed = false;
static bool g_caps_valid = false;
static uint32_t g_caps = 0;

static const shellcore_cap_row_t *find_cap_row(void) {
  uint16_t fw = (uint16_t)((kernel_get_fw_version() >> 16) & 0xffffu);
  for (size_t i = 0; i < sizeof(g_cap_rows) / sizeof(g_cap_rows[0]); i++) {
    if (g_cap_rows[i].firmware == fw)
      return &g_cap_rows[i];
  }
  return NULL;
}

static bool run_probe(uint32_t *caps_out) {
  const shellcore_cap_row_t *row = find_cap_row();
  if (!row) {
    log_debug("  [KCAPS] no patch offsets for fw 0x%08x", kernel_get_fw_version());
    return false;
  }

  pid_t pid = find_pid_by_name(k_shellcore_name, false);
  if (pid <= 0) {
    log_debug("  [KCAPS] SceShellCore not running");
    return false;
  }

  intptr_t base = kernel_dynlib_mapbase_addr(pid, SHELLCORE_MAIN_MODULE_HANDLE);
  if (base <= 0) {
    log_debug("  [KCAPS] no mapbase for pid=%ld", (long)pid);
    return false;
  }

  // Match the whole signature, not its first byte: on a devkit these retail
  // offsets land somewhere unrelated and a partial match would report a
  // capability the running kstuff does not have.
  uint8_t sysdir[CAP_SIG_MAX];
  uint8_t trophy[CAP_SIG_MAX];
  if (kernel_proc_copyout(pid, base + row->sysdir_off, sysdir, row->sysdir_len) ||
      kernel_proc_copyout(pid, base + row->trophy_off, trophy, row->trophy_len)) {
    log_debug("  [KCAPS] copyout failed from pid=%ld", (long)pid);
    return false;
  }

  uint32_t caps = 0;
  if (memcmp(sysdir, row->sysdir_sig, row->sysdir_len) == 0)
    caps |= SM_KSTUFF_CAP_SYSDIRPATH;
  if (memcmp(trophy, row->trophy_sig, row->trophy_len) == 0)
    caps |= SM_KSTUFF_CAP_TROPHY;

  log_debug("  [KCAPS] fw=%s sysdirpath=%s trophy=%s", row->name,
            (caps & SM_KSTUFF_CAP_SYSDIRPATH) ? "yes" : "no",
            (caps & SM_KSTUFF_CAP_TROPHY) ? "yes" : "no");
  *caps_out = caps;
  return true;
}

bool sm_kstuff_probe_caps(uint32_t *caps) {
  // Only a complete answer is cached. A positive cannot go stale -- ShellCore
  // is patched once at kstuff load -- but a zero may only mean kstuff has not
  // autoloaded yet, and one bit may mean the probe landed between the two
  // writes patch_shellcore() makes. Not throttled: a retry is one
  // find_pid_by_name and two small copyouts, and only until caps turn complete.
  const uint32_t kCapsAll = SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY;
  if (!g_caps_probed || !g_caps_valid || g_caps != kCapsAll) {
    uint32_t probed = 0;
    if (run_probe(&probed)) {
      g_caps = probed;
      g_caps_valid = true;
    } else if (!g_caps_probed) {
      // A later failure -- ShellCore momentarily unfindable -- is not evidence
      // the patches went away, so an earlier reading is kept.
      g_caps_valid = false;
    }
    g_caps_probed = true;
  }

  if (caps && g_caps_valid)
    *caps = g_caps;
  return g_caps_valid;
}
