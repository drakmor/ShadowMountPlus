#include "sm_platform.h"

#include "sm_kstuff_caps.h"
#include "sm_log.h"
#include "sm_runtime.h"

// Measured: the kernel reports "SceShellCore", with no ".elf" -- unlike its
// siblings SceSysCore.elf and mini-syscore.elf. Getting this wrong makes the
// probe report "not running", which is indistinguishable from a real answer of
// "no patches" unless you check.
static const char *const k_shellcore_name = "SceShellCore";
#define SHELLCORE_MAIN_MODULE_HANDLE 0u

// The two SceShellCore patches kstuff-lite applies and full kstuff does not.
// Offsets are kstuff-lite's own retail tables; on a testkit or devkit they miss
// and the signature match below then correctly reports "not patched".
typedef struct {
  uint32_t fw;
  uint32_t sysdir_off;
  uint32_t trophy_off;
  uint8_t sysdir_len;
} shellcore_cap_row_t;

// getSceSysDirPath is NOP-ed: a 6-byte NOP before 7.00, a 2-byte one after.
// The trophy fix flips one conditional jump to an unconditional 0xEB.
static const uint8_t k_sysdir_wide[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};
static const uint8_t k_sysdir_short[2] = {0x66, 0x90};
#define TROPHY_PATCH_BYTE 0xEBu

static const shellcore_cap_row_t g_cap_rows[] = {
    {0x02500000, 0x3b271c, 0x7d8584, 6}, // 2.50
    {0x03000000, 0x3fc24c, 0x8b5634, 6}, // 3.00
    {0x03100000, 0x3fc28c, 0x8b5674, 6}, // 3.10
    {0x03200000, 0x3fc33c, 0x8b5924, 6}, // 3.20
    {0x03210000, 0x3fc33c, 0x8b5924, 6}, // 3.21
    {0x04000000, 0x43db4c, 0x8337a7, 6}, // 4.00
    {0x04020000, 0x43db4c, 0x8337a7, 6}, // 4.02
    {0x04030000, 0x43db4c, 0x8337a7, 6}, // 4.03
    {0x04500000, 0x43e29c, 0x834117, 6}, // 4.50
    {0x04510000, 0x43e29c, 0x834127, 6}, // 4.51
    {0x05000000, 0x4a3e7c, 0x8e2c87, 6}, // 5.00
    {0x05020000, 0x4a3e6c, 0x8e2c77, 6}, // 5.02
    {0x05100000, 0x4a5d9c, 0x8e5647, 6}, // 5.10
    {0x05500000, 0x4a5d9c, 0x8e6057, 6}, // 5.50
    {0x06000000, 0x4d3bec, 0x92e937, 6}, // 6.00
    {0x06020000, 0x4d3bec, 0x92e8d7, 6}, // 6.02
    {0x06500000, 0x4d3c5c, 0x92f107, 6}, // 6.50
    {0x07000000, 0x579656, 0x9e7e96, 2}, // 7.00
    {0x07010000, 0x579656, 0x9e7e96, 2}, // 7.01
    {0x07200000, 0x579676, 0x9e8776, 2}, // 7.20
    {0x07400000, 0x57e166, 0x9f3e66, 2}, // 7.40
    {0x07600000, 0x57e166, 0x9f7446, 2}, // 7.60
    {0x07610000, 0x57e166, 0x9f7446, 2}, // 7.61
    {0x08000000, 0x5a5f53, 0xa4969d, 2}, // 8.00
    {0x08200000, 0x5a7023, 0xa50b0d, 2}, // 8.20
    {0x08400000, 0x5a7023, 0xa50b0d, 2}, // 8.40
    {0x08600000, 0x5a6e03, 0xa5099d, 2}, // 8.60
    {0x09000000, 0x5da91a, 0xab2c91, 2}, // 9.00
    {0x09050000, 0x5da91a, 0xab2c91, 2}, // 9.05
    {0x09200000, 0x5da63a, 0xab29d1, 2}, // 9.20
    {0x09400000, 0x5dad0a, 0xab3121, 2}, // 9.40
    {0x09600000, 0x5dad7a, 0xabb4f1, 2}, // 9.60
    {0x10000000, 0x5d8511, 0xaa9cc1, 2}, // 10.00
    {0x10010000, 0x5d8511, 0xaa9cc1, 2}, // 10.01
    {0x10200000, 0x5d8511, 0xaadf81, 2}, // 10.20
    {0x10400000, 0x5d8461, 0xaadfa1, 2}, // 10.40
    {0x10600000, 0x5d9cf1, 0xaaf831, 2}, // 10.60
    {0x11000000, 0x638caa, 0xaec74a, 2}, // 11.00
    {0x11200000, 0x638e1a, 0xaecada, 2}, // 11.20
    {0x11400000, 0x639dfa, 0xaee68a, 2}, // 11.40
    {0x11600000, 0x64186a, 0xaf63ea, 2}, // 11.60
    {0x12000000, 0x6557aa, 0xb1b02a, 2}, // 12.00
    {0x12020000, 0x6557aa, 0xb1b02a, 2}, // 12.02
    {0x12200000, 0x6557aa, 0xb1beba, 2}, // 12.20
    {0x12400000, 0x6557aa, 0xb1beba, 2}, // 12.40
    {0x12600000, 0x6569fa, 0xb21d1a, 2}, // 12.60
    {0x12700000, 0x6569fa, 0xb21d1a, 2}, // 12.70
};

static bool g_caps_probed = false;
static bool g_caps_valid = false;
static uint32_t g_caps = 0;

static const shellcore_cap_row_t *find_cap_row(void) {
  uint32_t fw = kernel_get_fw_version() & 0xffff0000u;
  for (size_t i = 0; i < sizeof(g_cap_rows) / sizeof(g_cap_rows[0]); i++) {
    if (g_cap_rows[i].fw == fw)
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

  // Match the whole patch signature, not just its first byte: on a testkit or
  // devkit these retail offsets land somewhere unrelated, and a partial match
  // there would report a capability the running kstuff does not have.
  uint8_t sysdir[6];
  uint8_t trophy = 0;
  if (kernel_proc_copyout(pid, base + row->sysdir_off, sysdir, row->sysdir_len) ||
      kernel_proc_copyout(pid, base + row->trophy_off, &trophy, 1)) {
    log_debug("  [KCAPS] copyout failed from pid=%ld", (long)pid);
    return false;
  }

  const uint8_t *want = row->sysdir_len == 6 ? k_sysdir_wide : k_sysdir_short;
  uint32_t caps = 0;
  if (memcmp(sysdir, want, row->sysdir_len) == 0)
    caps |= SM_KSTUFF_CAP_SYSDIRPATH;
  if (trophy == TROPHY_PATCH_BYTE)
    caps |= SM_KSTUFF_CAP_TROPHY;

  log_debug("  [KCAPS] sysdirpath=%s trophy=%s",
            (caps & SM_KSTUFF_CAP_SYSDIRPATH) ? "yes" : "no",
            (caps & SM_KSTUFF_CAP_TROPHY) ? "yes" : "no");
  *caps_out = caps;
  return true;
}

bool sm_kstuff_probe_caps(uint32_t *caps) {
  // Only a complete answer is cached.
  //
  // ShellCore is patched once at kstuff load and never un-patched, so a
  // positive can never go stale. A zero is different: it may only mean kstuff
  // has not run yet, which depends on autoload ordering we do not control. This
  // used to latch whichever answer came first, and a console with kstuff-lite
  // correctly installed then refused to launch a backported title, the gate
  // reporting sysdirpath=no trophy=no with CAPS_VALID set -- a probe that
  // succeeded and honestly saw an unpatched ShellCore, cached for the boot.
  //
  // Confirmed with kstuff-lite loading after this payload: the startup probe
  // reads caps=0x0, and the next probe 11s later reads caps=0x3. Load order no
  // longer matters.
  //
  // A partial reading is cached no more than a zero is. patch_shellcore()
  // writes the sysdir and trophy entries one after the other, so a probe that
  // lands between them sees one bit and would otherwise latch it for the boot
  // -- the same failure as caps=0, one bit over.
  //
  // Deliberately not time-throttled: the callers are our own startup and the
  // one query a title makes during module init, so a retry costs one
  // find_pid_by_name plus two small copyouts, and only until the answer turns
  // complete.
  const uint32_t kCapsAll = SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY;
  if (!g_caps_probed || !g_caps_valid || g_caps != kCapsAll) {
    uint32_t probed = 0;
    if (run_probe(&probed)) {
      g_caps = probed;
      g_caps_valid = true;
    } else if (!g_caps_probed) {
      // Keep any earlier successful reading rather than downgrading it: a probe
      // that fails later (ShellCore momentarily unfindable) is not evidence
      // that the patches went away.
      g_caps_valid = false;
    }
    g_caps_probed = true;
  }

  if (caps && g_caps_valid)
    *caps = g_caps;
  return g_caps_valid;
}
