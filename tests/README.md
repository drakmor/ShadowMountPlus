Host regression tests (Linux/WSL; no console connection):

```sh
python3 tools/test_runtime_config.py --sanitize
python3 tools/test_shellcore.py --sanitize
python3 tools/test_scan_permissions.py
python3 tools/test_shellcore_launch.py --sanitize
python3 tools/test_sandbox_cleanup.py
python3 tools/test_storage_paths.py --sanitize
python3 tools/verify_l10n.py
```

CI runs all of these in the `host-tests` job of `.github/workflows/ps5.yml`;
tagged releases are only published when they pass.

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
