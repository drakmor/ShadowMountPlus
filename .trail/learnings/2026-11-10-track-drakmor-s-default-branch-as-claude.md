---
title: "Track drakmor's default branch as upstream, not the other branches"
author: Claude
date: 2026-11-10
branch: claude/quirky-galileo-re3ahw
cost: { minutes: 1, tokens: 1167 }
id: l-05da9c04
scope: repo
status: active
src: agent
touches:
  - BACKLOG.md
  - CLAUDE.md
---

## Decided
"Latest ShadowMount" means drakmor's default branch (currently 1.7). This fork merges only that branch, and quer3q's ampr-emu-ng support is carried on top of it as a merge, not rebased.

## Tried and ruled out
drakmor's other branches (1.8, dev, main, no-pause) were checked on 2026-11-10: all are older side tracks. 1.8's last commit is from 2026-11-02 and it lacks 15 commits that are in 1.7; dev/main/no-pause stopped between June and August 2026. Merging them would pull in stale or diverged code.

## Watch out
quer3q's ampr-emu-ng branch was cut from an older 1.7 and predates the new INI config format. Its new key was silently ignored until it was added to the section/key table, so when re-applying quer3q changes, check every new config key is mapped.

In the code:
- Find the default branch: git ls-remote --symref https://github.com/drakmor/ShadowMountPlus.git HEAD
- Config key table: sm_config_keys[] in src/sm_config_file.c; the test asserting ampr_ng_update_url is in tests/test_runtime_config.c
- A daily routine (07:13 Europe/Stockholm) merges new default-branch commits when host tests pass
