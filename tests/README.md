Host regression tests (Linux/WSL; no console connection):

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
