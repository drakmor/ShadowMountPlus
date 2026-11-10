# BACKLOG.md

Found bugs, stubs, ideas and multi-session work. Keep `> RESUME HERE` current.

> **RESUME HERE**
> - Last done: Trail-code wired into Claude Code (`trail init --agents claude`): SessionStart
>   hook in `.claude/settings.json`, `.trail/` committed (learnings go to `.trail/learnings/`).
> - Daily routine `trig_0111Hk6SaZPNxxerHwwTd5mw` (07:13 Europe/Stockholm) merges new commits
>   from drakmor's default branch (`1.7`) into this branch when tests pass, else reports.
> - Next: build on PS5 with the SDK and test a zstd game with ampr-emu-ng in `fakelib/`
>   (and the auto-updater with `<emulators_path>/ampr-emu-ng/libSceAmpr.sprx`).

## Open / to verify
- [ ] Cloud sessions need `trail` installed each time (environment setup script:
  `npm install -g @trailhq/trail-code`); the hook is a silent no-op without it.
- [ ] PS5 build + on-console test of the ampr-emu-ng merge (not possible in cloud container).
- [ ] Web UI / API don't expose `ampr_update_url` / `ampr_ng_update_url`; only `auto_update_ampr`.
  Consider adding to settings API (`docs/api.md`, `docs/openapi.yaml`, `web/index.html`).
- [ ] `docs/openapi.yaml` `auto_update_ampr` description doesn't mention ampr-emu-ng.

- [ ] Verify the first daily routine run can push (its fresh session has no repo attached
  as a source; if the push is refused, add the repo to the routine in claude.ai Routines).

## Ideas
- Keep in sync with upstream `drakmor/ShadowMountPlus` `1.7` and quer3q's `ampr-emu-ng` branch.
- Show the detected AMPR runtime (upstream / ng, version) per game in the web UI.

## Done
- 2026-11-10: `trail init --agents claude` run; `.claude/settings.json` + `.trail/` committed.
- 2026-11-10: Trail-code v0.29.0 code map built (136 files, 2414 symbols, 3266 edges);
  `graft/` git-ignored, `.ignore` keeps it searchable.
- 2026-11-09: Merged quer3q/ShadowMountPlus `ampr-emu-ng` (3663a32); mapped
  `[emulators] ampr_ng_update_url` in `sm_config_keys[]`; host test covers it.
