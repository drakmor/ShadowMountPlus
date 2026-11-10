# BACKLOG.md

Found bugs, stubs, ideas and multi-session work. Keep `> RESUME HERE` current.

> **RESUME HERE**
> - Last done: Trail-code (trailhq/Trail-Code) installed in the cloud session, code map built,
>   `.gitignore`/`.ignore` updated, usage documented in `CLAUDE.md`.
> - Not done: `trail init` agent wiring (hooks/statusline in `.claude/` and `~/.claude/`) was
>   blocked by the session's permission classifier; the user can run
>   `trail init --agents claude` locally, or allow it here.
> - Next: build on PS5 with the SDK and test a zstd game with ampr-emu-ng in `fakelib/`
>   (and the auto-updater with `<emulators_path>/ampr-emu-ng/libSceAmpr.sprx`).

## Open / to verify
- [ ] Wire Trail-code into Claude Code (`trail init --agents claude`) and commit `.claude/`,
  `.trail/`. Cloud sessions also need `trail` installed (environment setup script).
- [ ] PS5 build + on-console test of the ampr-emu-ng merge (not possible in cloud container).
- [ ] Web UI / API don't expose `ampr_update_url` / `ampr_ng_update_url`; only `auto_update_ampr`.
  Consider adding to settings API (`docs/api.md`, `docs/openapi.yaml`, `web/index.html`).
- [ ] `docs/openapi.yaml` `auto_update_ampr` description doesn't mention ampr-emu-ng.

## Ideas
- Keep in sync with upstream `drakmor/ShadowMountPlus` `1.7` and quer3q's `ampr-emu-ng` branch.
- Show the detected AMPR runtime (upstream / ng, version) per game in the web UI.

## Done
- 2026-11-10: Trail-code v0.29.0 code map built (136 files, 2414 symbols, 3266 edges);
  `graft/` git-ignored, `.ignore` keeps it searchable.
- 2026-11-09: Merged quer3q/ShadowMountPlus `ampr-emu-ng` (3663a32); mapped
  `[emulators] ampr_ng_update_url` in `sm_config_keys[]`; host test covers it.
