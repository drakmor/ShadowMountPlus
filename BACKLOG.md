# BACKLOG.md

Found bugs, stubs, ideas and multi-session work. Keep `> RESUME HERE` current.

> **RESUME HERE**
> - Last done: merged quer3q `ampr-emu-ng` support (commit `53feb3a`) and added memory files.
> - Next: build on PS5 with the SDK and test a zstd game with ampr-emu-ng in `fakelib/`
>   (and the auto-updater with `<emulators_path>/ampr-emu-ng/libSceAmpr.sprx`).
> - Open question: what "trailcode" refers to (user asked to fetch it; not identified yet).

## Open / to verify
- [ ] PS5 build + on-console test of the ampr-emu-ng merge (not possible in cloud container).
- [ ] Web UI / API don't expose `ampr_update_url` / `ampr_ng_update_url`; only `auto_update_ampr`.
  Consider adding to settings API (`docs/api.md`, `docs/openapi.yaml`, `web/index.html`).
- [ ] `docs/openapi.yaml` `auto_update_ampr` description doesn't mention ampr-emu-ng.

## Ideas
- Keep in sync with upstream `drakmor/ShadowMountPlus` `1.7` and quer3q's `ampr-emu-ng` branch.
- Show the detected AMPR runtime (upstream / ng, version) per game in the web UI.

## Done
- 2026-11-09: Merged quer3q/ShadowMountPlus `ampr-emu-ng` (3663a32); mapped
  `[emulators] ampr_ng_update_url` in `sm_config_keys[]`; host test covers it.
