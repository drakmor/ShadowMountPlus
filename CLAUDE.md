# CLAUDE.md

Instructions for Claude on every turn in this repo. Keep this file short; tune it over time.

## Start of every session
1. Read `SNAPSHOT.md` (architecture map) instead of scanning the whole repo.
2. Read `BACKLOG.md`, especially the `> RESUME HERE` block, to pick up unfinished work.
3. Only open the source files the task actually touches.

## End of every task
Update all three memory files before the final commit:
- `SNAPSHOT.md`: architecture/module changes, new config keys, new files.
- `BACKLOG.md`: move finished items to Done, add found bugs/ideas/stubs, rewrite `> RESUME HERE`.
- `CLAUDE.md`: only if a durable working rule or preference was learned.

## Working style
- The user writes in Swedish; answer in Swedish. Code, comments and commit messages in English.
- Be brief. Give a recommendation, not a survey of options.
- Develop on the branch named by the session; never push elsewhere. No PRs unless asked.
- Don't put model names in commits, code or docs.

## Project facts
- PS5 homebrew payload (C, gnu11), built with the PS5 payload SDK (`prospero-clang`).
  The SDK is NOT available in the cloud container: `make` won't work there.
- Upstream: `drakmor/ShadowMountPlus` (branch `1.7`). Extra remote for ampr-emu-ng work:
  `quer3q/ShadowMountPlus` (branch `ampr-emu-ng`).
- zstd decompression lives in the external `quer3q/ampr-emu-ng` (`libSceAmpr.sprx`),
  not in this repo. This repo only mounts games and manages that emulator file.

## Checks before pushing
Host tests (need `libjson-c-dev` for the config test: `apt-get install -y libjson-c-dev`):
```sh
python3 tools/test_runtime_config.py
python3 tools/test_storage_paths.py
python3 tools/test_scan_permissions.py
python3 tools/test_sandbox_cleanup.py
python3 tools/test_shellcore.py
python3 tools/test_shellcore_launch.py
```
Without the SDK, compare `clang -fsyntax-only -std=gnu11 -Wall -Wextra -Iinclude -Itests
-include tests/host_platform.h <file>` diagnostics before/after a change; PS5-only APIs
always produce errors, so only new diagnostics matter.

## Gotchas
- New config keys must be added to `sm_config_keys[]` in `src/sm_config_file.c`
  (section/key → internal name) or they are silently ignored in the INI format,
  and parsed in `src/sm_config_mount.c`. Add a check to `tests/test_runtime_config.c`.
- `config.ini.example` and `web/index.html` are embedded into the binary as assets.
