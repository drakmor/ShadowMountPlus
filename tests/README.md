Host regression tests (Linux/WSL; no console connection):

Metadata filename regression (case-insensitive USB lookup and case-sensitive
internal destinations):

```sh
cc -std=gnu11 -O0 -Wall -Wextra -Werror -Wno-error=format-truncation -ffunction-sections \
  -Wl,--gc-sections -Iinclude tests/test_install_metadata.c \
  -o /tmp/shadowmount-test-metadata
/tmp/shadowmount-test-metadata
```

The test compiles the production install implementation with a file-copy stub
that emulates USB case-insensitive source lookup. It checks canonical icon
staging in appmeta, retries over old uppercase metadata, unchanged custom
artwork names, deterministic exact-case source selection, and copy failures.
Mounting and the PS5 installer are not emulated.
GCC's existing path-truncation warnings in the included console implementation
remain visible without failing this host-only test build.

Runtime config regression:

```sh
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -ffunction-sections \
  -Wl,--gc-sections -Iinclude tests/test_runtime_config.c \
  -o /tmp/shadowmount-test-config
timeout 30s /tmp/shadowmount-test-config
```

The config test compiles the production implementation with host stubs for
logging, localization and backend names. It checks concurrent initialization,
snapshot lifetime, coherent reads during repeated file reloads, path/rule
getters, unchanged settings and restoration of defaults after file removal.
Add `-fsanitize=thread -g` to the compiler command to check for data races on
hosts that support ThreadSanitizer.
If WSL reports `unexpected memory mapping`, run the instrumented binary with
`setarch x86_64 -R /tmp/shadowmount-test-config` to disable ASLR for that process.
