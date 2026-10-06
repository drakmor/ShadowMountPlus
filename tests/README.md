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

```sh
cc -std=gnu11 -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
  tests/test_kstuff_caps.c -o /tmp/shadowmount-test-kstuff-caps -lpthread
timeout 30s /tmp/shadowmount-test-kstuff-caps
```

The kstuff capability test compiles the production prober with the three
kernel reads stubbed, so a case can decide what ShellCore's text looks like.
It does not check the offset table against a console, which only a console can
do; it checks the decisions around it. Which row a firmware selects, and that
every row is reachable with its own two offsets and no duplicated firmware
stamp. That a whole patch signature must match rather than its first byte, so
the retail offsets landing on unrelated code on a devkit cannot report a
capability kstuff does not have. That each firmware's own signature from
its own row reads as patched while a neighbour's does not, so the 6-byte and
2-byte getSceSysDirPath NOPs are each rejected on the wrong side of 7.00. And what may be cached: a
zero or a one-bit reading must not latch, because kstuff may load after this
payload, while a complete reading must not be re-read or downgraded by a later
failed probe. The last of those is also checked under concurrency, because the
route answers on API worker threads: eight threads probing at once must all
read the same complete answer, and one probe must serve them all. Run that case
under `-fsanitize=thread`, where removing the cache lock reports a data race in
`sm_kstuff_probe_caps`. A table regenerated against a newer kstuff-lite is checked by the
same cases, so that is the whole review after running

```sh
tools/generate_kstuff_caps_offsets.py <kstuff-lite>/ps5-kstuff/shellcore_patches \
  --output src/sm_kstuff_caps_offsets.inc
```

Add `-fsanitize=address,undefined -g` to the compiler command on
hosts that support it.
