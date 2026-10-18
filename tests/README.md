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

Exit-release ownership test:

```sh
cc -std=gnu11 -O2 -Wall -Wextra -Werror -pthread -ffunction-sections \
  -Wl,--gc-sections -Iinclude tests/test_exit_release_ownership.c \
  -o /tmp/shadowmount-test-exit-release
timeout 30s /tmp/shadowmount-test-exit-release
```

It compiles the production ShellCore service with console I/O stubbed and drives
the exit reaper and runtime reservation through a fixed two-thread schedule. The
sandbox lookup is redirected to a temporary directory containing one stale title
sandbox, so the linger deadline path is exercised without a console.

Four scenarios run:

1. a relaunch is claimed before the deadline; the reaper must defer
2. no relaunch; the exited owner is released once past the deadline
3. a replacement process is already bound; the owner is left alone
4. a relaunch is claimed and prepared but not yet bound at the deadline; the
   reaper must not release the runtime

Scenario 4 fails on the parent branch and passes with the ownership check.

