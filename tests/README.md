Host regression tests (Linux/WSL; no console connection):

```sh
python3 tools/test_runtime_config.py --sanitize
python3 tools/test_shellcore.py --sanitize
python3 tools/test_scan_permissions.py
python3 tools/test_shellcore_launch.py --sanitize
python3 tools/test_sandbox_cleanup.py
```

The standalone FPKG envelope, outer-metadata and AMPR index checks are also run
by the Linux PS5 build workflow. Run them locally with:

```sh
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_fpkg_header.c src/sm_fpkg.c src/sm_fpkg_layout.c \
  src/sm_fpkg_image.c src/sm_fpkg_decoder.c src/sm_fpkg_tree.c \
  src/sm_fpkg_index.c -o /tmp/sm-fpkg-header-test
/tmp/sm-fpkg-header-test
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_fpkg_index.c src/sm_fpkg_index.c src/sm_fpkg.c \
  src/sm_fpkg_layout.c src/sm_fpkg_image.c src/sm_fpkg_decoder.c \
  src/sm_fpkg_tree.c -o /tmp/sm-fpkg-index-test
/tmp/sm-fpkg-index-test
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_fpkg_layout.c src/sm_fpkg_layout.c -o /tmp/sm-fpkg-layout-test
/tmp/sm-fpkg-layout-test
```

These tests use synthetic bytes and are not real FPKG compatibility tests.

The config test compiles the production implementation with host stubs for
logging, localization and backend names. It checks concurrent initialization,
snapshot lifetime, coherent reads during repeated file reloads, path/rule
getters, unchanged settings and restoration of defaults after file removal.
The ShellCore tests cover actual ptrace results, credential restoration, hook
repair, partial writes, failed rollback, detach failures and shutdown ownership.
If ASan cannot initialize under WSL, prefix either Python command with
`setarch x86_64 -R` to disable ASLR for that process.

The scan permission test checks inaccessible roots and descendants in watcher
discovery while keeping content scans and real I/O failures strict.

The native FPKG cache transaction fixture is a targeted host check for active
generation validation, interrupted publication recovery, symlink rejection,
ownership-aware cleanup and quarantine:

```sh
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -Itests \
  -include tests/host_platform.h \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_fpkg_cache.c src/sm_fpkg_backport.c src/sm_fpkg_cache.c src/sm_fpkg_index.c \
  -o /tmp/sm-fpkg-cache-test
/tmp/sm-fpkg-cache-test
```

It uses synthetic cache records and generated AMPR indexes, verifies damaged
index rejection without a size change, and does not open an FPKG fixture.

Backport snapshot checks cover source replacement, regular-file enumeration,
fakelib exclusion and symlink rejection. The cache build fixture additionally
checks loose-file overrides, added paths, case differences, source mutation
during staging, and cache invalidation when the backport changes or disappears.
The cache transaction fixture checks writable-layer migration, retained
conflicting files, recovery after migration failure and pointer symlink rejection.

```sh
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -Itests \
  -include tests/host_platform.h -fsanitize=address,undefined \
  -fno-omit-frame-pointer tests/test_fpkg_backport.c \
  src/sm_fpkg_backport.c -o /tmp/sm-fpkg-backport-test
/tmp/sm-fpkg-backport-test
```

The AMPR runtime selector verifies the distinct NG runtime path, scans across
read boundaries, preserves a selected custom runtime, installs the pinned
fallback when needed, and rejects wrong-family files and symlinked fallback
paths:

```sh
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_ampr_runtime.c src/sm_ampr_runtime.c \
  src/ampr_emu_ng_runtime_asset.c \
  -o /tmp/sm-ampr-runtime-test
/tmp/sm-ampr-runtime-test
```

Generate the asset source first with:

```sh
xxd -i -n sm_ampr_ng_runtime vendor/ampr-emu-ng-v0.4.0/libSceAmpr.sprx \
  src/ampr_emu_ng_runtime_asset.c
```

This checks runtime selection and provisioning only; it cannot establish PS5
loader or codec-2 behavior.

The decoder differential check runs the deterministic malformed corpus and
boundary vectors through native C and pinned Kraken. It compares acceptance
and every byte of each accepted output:

```sh
python3 tools/test_fpkg_decoder_differential.py \
  /path/to/kraken-mount-reference
```

The reference checkout must be at Kraken Mount revision
`4b420d579bd381796d773875e36d14c9acaabf39`.

The cache builder also has an end-to-end synthetic plaintext FPKG fixture. It
checks the FIH/PFS reader, loose-file extraction, AMPR index/manifest creation,
cancelled staging recovery, atomic generation publication and reuse:

```sh
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -Itests \
  -include tests/host_platform.h \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_fpkg_cache_build.c src/sm_fpkg_backport.c src/sm_fpkg_cache.c src/sm_fpkg.c \
  src/sm_fpkg_layout.c src/sm_fpkg_image.c src/sm_fpkg_decoder.c \
  src/sm_fpkg_tree.c src/sm_fpkg_index.c \
  -o /tmp/sm-fpkg-cache-build-test
/tmp/sm-fpkg-cache-build-test
```

The package bytes are generated by the fixture and contain no game content.
The separate AMPR writer oracle in the Linux workflow uses a three-file,
four-chunk synthetic package to compare C output byte-for-byte with Kraken and
to read back both RAW and codec-2 chunks through the pinned AMPR readers.

The async registration queue check verifies that replaced FPKG generations are
released only after app.db confirms installation; an install timeout retains
the prior generation:

```sh
clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -Itests \
  -include tests/host_platform.h -ffunction-sections -fdata-sections \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  tests/test_install_queue_fpkg.c src/sm_install_queue.c \
  -Wl,--gc-sections -o /tmp/sm-install-queue-fpkg-test
/tmp/sm-install-queue-fpkg-test
```

The FPKG install lifecycle smoke check compiles the production SMP functions
for `.pkg` candidate selection and preparation, TitleDir registration, title
publication, and the `/app0/kraken` sandbox layer. It verifies extension
classification, disabled-feature and unsupported-input rejection, candidate
transition to a cache generation, nullfs → `mount.lnk` → registration ordering,
rollback when link publication fails, the batch registration route, bridge/API
result preservation, source-parent resolution, reuse/creation of the launch-time
FPKG layer, and the read-only FPKG sandbox mount:

```sh
python3 tools/test_fpkg_install_lifecycle.py
```

The candidate flow uses a temporary placeholder package and stubs the PKG
parser, FPKG reader/cache APIs, and platform calls. The separate cache-build
fixture exercises the real C reader and cache with a synthetic plaintext FPKG.
Together these provide host evidence for scanner-to-launch routing and rollback,
not proof that PS5 nullfs, AppInstallTitleDir, AMPR loading, or game launch works
on-console.
