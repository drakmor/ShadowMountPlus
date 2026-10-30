# Changelog

## Unreleased

- Stage `icon0.png` with its canonical lowercase filename in both `sce_sys`
  and appmeta. USB sources can enumerate the same file as `ICON0.PNG`; keeping
  that spelling on case-sensitive internal storage causes title registration
  to fail with `0x80A30008`. Adding the canonical copies restored registration
  of ProsperoStore and Playden on firmware 11.40 with ShadowMountPlus 1.7beta11.
  The patched payload still requires its own PS5 build and console validation.
