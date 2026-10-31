/* Copyright (C) 2025 John Törnblom
 * SMB2/3 connection setup follows ps5-payload-dev/websrv
 * src/smb.c (GPL-3.0-or-later). Named connection storage is owned by sm_config_mount.
 * See licenses/SMB-NOTICES.txt. */
#include "sm_platform.h"
#include "sm_smb.h"
#include "sm_log.h"
#include "sm_config_mount.h"
#include "sm_runtime.h"
#include <ctype.h>
#include <pthread.h>
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/smb2-errors.h>
#include <strings.h>
#include <sys/socket.h>

static pthread_mutex_t client_mutex = PTHREAD_MUTEX_INITIALIZER;

void sm_smb_lock(void) {
  pthread_mutex_lock(&client_mutex);
}
void sm_smb_unlock(void) {
  pthread_mutex_unlock(&client_mutex);
}
int sm_smb_result(int result) {
  if (result < 0) {
    errno = -result;
    return -1;
  }
  return result;
}

static bool safe_components(const char *path) {
  for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
    if (*p < 32 || *p == 127 || *p == '\\') return false;
  for (const char *p = path; *p;) {
    const char *end = strchr(p, '/');
    size_t size = end ? (size_t)(end - p) : strlen(p);
    if (!size || (size == 1 && *p == '.') || (size == 2 && !strncmp(p, "..", 2))) return false;
    if (!end) break;
    p = end + 1;
  }
  return true;
}

static int base_address(const sm_smb_connection_t *c, sm_smb_address_t *a) {
  memset(a, 0, sizeof(*a));
  a->read_only = c->read_only;
  if (!sm_smb_path(c->path) || !safe_components(c->path + 6)) return EINVAL;
  const char *server = c->path + 6, *share = strchr(server, '/');
  if (!share || share == server || !share[1]) return EINVAL;
  size_t length = (size_t)(share - server);
  if (length >= sizeof(a->server)) return ENAMETOOLONG;
  memcpy(a->server, server, length);
  for (char *p = a->server; *p; ++p) {
    if (isspace((unsigned char)*p) || *p == '@' || *p == '?' || *p == '#') return EINVAL;
    *p = (char)tolower((unsigned char)*p);
  }
  ++share;
  const char *relative = strchr(share, '/');
  length = relative ? (size_t)(relative - share) : strlen(share);
  if (!length || length >= sizeof(a->share)) return EINVAL;
  memcpy(a->share, share, length);
  if (relative && strlcpy(a->relative, relative + 1, sizeof(a->relative)) >= sizeof(a->relative))
    return ENAMETOOLONG;
  size_t size = strlen(a->relative);
  if (size && a->relative[size - 1] == '/') a->relative[size - 1] = 0;
  const char *user = c->username;
  const char *domain = strchr(user, '\\');
  if (domain) {
    length = (size_t)(domain - user);
    if (!length || length >= sizeof(a->domain) || !domain[1]) return EINVAL;
    memcpy(a->domain, user, length);
    user = domain + 1;
  }
  strlcpy(a->user, user, sizeof(a->user));
  strlcpy(a->password, c->password, sizeof(a->password));
  return 0;
}

int sm_smb_validate(const sm_smb_connection_t *c) {
  if (!c || !memchr(c->name, 0, sizeof(c->name)) || !memchr(c->path, 0, sizeof(c->path)) ||
      !memchr(c->username, 0, sizeof(c->username)) || !memchr(c->password, 0, sizeof(c->password)))
    return EINVAL;
  if (!c->name[0] || !strcmp(c->name, ".") || !strcmp(c->name, "..") ||
      isspace((unsigned char)c->name[0]) || isspace((unsigned char)c->name[strlen(c->name) - 1]))
    return EINVAL;
  for (const unsigned char *p = (const unsigned char *)c->name; *p; ++p)
    if (*p < 32 || *p == 127 || strchr("/\\:@?#%", *p)) return EINVAL;
  const char *credentials[] = {c->username, c->password};
  for (size_t i = 0; i < 2; ++i)
    for (const unsigned char *p = (const unsigned char *)credentials[i]; *p; ++p)
      if (*p < 32 || *p == 127) return EINVAL;
  sm_smb_address_t address;
  return base_address(c, &address);
}

size_t sm_smb_snapshot(sm_smb_connection_t out[SM_SMB_CONNECTIONS_MAX]) {
  return sm_config_smb_snapshot(out);
}

int sm_smb_get(const char *name, sm_smb_connection_t *out) {
  return sm_config_smb_get(name, out);
}

int sm_smb_save(const char *previous, const sm_smb_connection_t *c) {
  if (!c) return EINVAL;
  return sm_config_smb_update(previous, c);
}

int sm_smb_remove(const char *name) {
  return sm_config_smb_update(name, NULL);
}

int sm_smb_parse_address(const char *path, sm_smb_address_t *a) {
  if (!path || !sm_smb_path(path) || strlen(path) >= MAX_PATH) {
    errno = EINVAL;
    return -1;
  }
  const char *name = path + 6, *slash = strchr(name, '/');
  size_t size = slash ? (size_t)(slash - name) : strlen(name);
  if (!size || size >= 64 || (slash && !safe_components(slash + 1))) {
    errno = EINVAL;
    return -1;
  }
  char alias[64];
  memcpy(alias, name, size);
  alias[size] = 0;
  sm_smb_connection_t c;
  int status = sm_smb_get(alias, &c);
  if (!status && !c.active) status = ENODEV;
  if (!status) status = base_address(&c, a);
  memset(&c, 0, sizeof(c));
  if (status) {
    errno = status;
    return -1;
  }
  char relative[MAX_PATH];
  int length = snprintf(relative, sizeof(relative), "%s%s%s", a->relative,
                        a->relative[0] && slash && slash[1] ? "/" : "", slash ? slash + 1 : "");
  if (length < 0 || (size_t)length >= sizeof(relative)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  size_t end = strlen(relative);
  if (end && relative[end - 1] == '/') relative[end - 1] = 0;
  strlcpy(a->relative, relative, sizeof(a->relative));
  length = snprintf(a->path, sizeof(a->path), "smb://%s%s%s", alias, slash && slash[1] ? "/" : "",
                    slash ? slash + 1 : "");
  if (length < 0 || (size_t)length >= sizeof(a->path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  end = strlen(a->path);
  if (end && a->path[end - 1] == '/') a->path[end - 1] = 0;
  return 0;
}

int sm_smb_normalize(const char *path, char out[MAX_PATH]) {
  sm_smb_address_t address;
  if (sm_smb_parse_address(path, &address)) return errno;
  strlcpy(out, address.path, MAX_PATH);
  return 0;
}

bool sm_smb_path_within(const char *path, const char *root) {
  sm_smb_address_t a, b;
  if (sm_smb_parse_address(path, &a) || sm_smb_parse_address(root, &b)) return false;
  size_t length = strlen(b.relative);
  return !strcasecmp(a.server, b.server) && !strcasecmp(a.share, b.share) &&
         !strncasecmp(a.relative, b.relative, length) &&
         (!length || !a.relative[length] || a.relative[length] == '/');
}

static struct smb2_context *connect_address_attempt(const sm_smb_address_t *a, bool anonymous,
                                                   uint32_t *native_status) {
  if (native_status) *native_status = 0;
  if (should_stop_requested() || runtime_sleep_mode_active()) {
    errno = ECANCELED;
    return NULL;
  }
  struct smb2_context *ctx = smb2_init_context();
  if (!ctx) {
    errno = ENOMEM;
    return NULL;
  }
  smb2_set_version(ctx, SMB2_VERSION_ANY);
  smb2_set_timeout(ctx, 5);
  smb2_set_user(ctx, anonymous ? "" : a->user[0] ? a->user : "Guest");
  smb2_set_domain(ctx, anonymous ? "" : a->domain[0] ? a->domain : "WORKGROUP");
  // NULL selects anonymous NTLM; an empty string authenticates an account.
  // Set the password last and do not reload it through connect_share's user.
  smb2_set_password(ctx, anonymous ? NULL : a->password);
  smb2_set_security_mode(ctx, a->user[0] || a->password[0] ? SMB2_NEGOTIATE_SIGNING_ENABLED : 0);
  int status = smb2_connect_share(ctx, a->server, a->share, NULL);
  if (should_stop_requested() || runtime_sleep_mode_active()) {
    smb2_destroy_context(ctx);
    errno = ECANCELED;
    return NULL;
  }
  if (status < 0) {
    if (native_status) *native_status = (uint32_t)smb2_get_nterror(ctx);
    smb2_destroy_context(ctx);
    errno = -status;
    return NULL;
  }
  // Console defaults are about 32/64 KiB. Grow small buffers for sustained
  // transfers without reducing larger system defaults or making tuning fatal.
  int saved_errno = errno;
  int options[] = {SO_SNDBUF, SO_RCVBUF};
  int fd = smb2_get_fd(ctx);
  for (size_t i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
    int current = 0, desired = 512 * 1024;
    socklen_t size = sizeof(current);
    if (getsockopt(fd, SOL_SOCKET, options[i], &current, &size) == 0 && current < desired)
      (void)setsockopt(fd, SOL_SOCKET, options[i], &desired, sizeof(desired));
  }
  errno = saved_errno;
  return ctx;
}

static struct smb2_context *connect_address(const sm_smb_address_t *a) {
  uint32_t status = 0;
  struct smb2_context *ctx = connect_address_attempt(a, false, &status);
  if (ctx || a->user[0] || a->password[0]) return ctx;
  if (status != SMB2_STATUS_ACCESS_DENIED && status != SMB2_STATUS_LOGON_FAILURE &&
      status != SMB2_STATUS_ACCOUNT_DISABLED && status != SMB2_STATUS_ACCOUNT_RESTRICTION &&
      status != SMB2_STATUS_LOGON_TYPE_NOT_GRANTED) return NULL;
  int saved_errno = errno;
  ctx = connect_address_attempt(a, true, NULL);
  if (!ctx && errno != ECANCELED) errno = saved_errno;
  return ctx;
}

struct smb2_context *sm_smb_connect_path(const char *path, sm_smb_address_t *a) {
  if (sm_smb_parse_address(path, a)) return NULL;
  return connect_address(a);
}

int sm_smb_check(const sm_smb_connection_t *c, uint16_t *dialect) {
  int status = sm_smb_validate(c);
  if (status) return status;
  sm_smb_address_t a;
  status = base_address(c, &a);
  if (status) return status;
  sm_smb_lock();
  struct smb2_context *ctx = connect_address(&a);
  struct smb2_stat_64 st;
  if (!ctx)
    status = errno;
  else {
    int result = smb2_stat(ctx, a.relative, &st);
    status = result < 0 ? -result : st.smb2_type != SMB2_TYPE_DIRECTORY ? ENOTDIR : 0;
    if (!status && dialect) *dialect = smb2_get_dialect(ctx);
    smb2_destroy_context(ctx);
  }
  sm_smb_unlock();
  return status;
}
