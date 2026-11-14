# ampr-emu-ng runtime notice

ShadowMountPlus embeds the unmodified `libSceAmpr.sprx` release runtime from
ampr-emu-ng v0.4.0. The corresponding source is available at the pinned
[ampr-emu-ng revision](https://github.com/quer3q/ampr-emu-ng/tree/920e9172b6bf99bd9210361b545e2953659d1e9b).

- Runtime SHA-256: `8320eb65c16565f42031b9d88b15a447039f0fd7e5694faf5f1cb84cafdba218`
- License: GNU GPL version 3 or later; see `vendor/ampr-emu-ng-v0.4.0/LICENSE`.
- Vendored runtime and checksum: `vendor/ampr-emu-ng-v0.4.0/`.

The payload installs this runtime only when the configured NG runtime is
missing. It preserves an existing file and fails closed if that file does not
identify as ampr-emu-ng. The binary is not modified by ShadowMountPlus.
