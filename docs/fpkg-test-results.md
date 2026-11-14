# Native FPKG test and validation record

Date: 2026-11-14. This is an execution record, not a claim of FPKG compatibility.

## Environment

- Host: macOS 27.2, Apple Silicon (`arm64`), Apple Clang 21.0.0.
- Python: 3.14.3.
- PS5 SDK: unavailable; `/opt/ps5-payload-sdk` does not exist.
- Docker: unavailable.
- Test FPKGs: user-supplied plaintext debug packages for `PPSA12544`
  (2,430,510,116 bytes) and `PPSA21703` (1,031,609,089 bytes) were opened
  read-only from Downloads. No package data was copied into the repository;
  no game-file payload was extracted to disk.
- PS5 console: unavailable in this task environment.
- Local Linux containers: unavailable (`docker` and `podman` are not installed).
- GitHub Actions: current run `43612405370` passed at code commit
  `9bb105717fed06948ed48fa102158e8574594eb6`. It passed the FPKG host and
  lifecycle checks, built the Prospero SDK and payload, and uploaded the
  commit-specific ELF artifact. Earlier failures exposed and fixed a missing
  writer-fixture stub, nondeterministic synthetic-file mtime, and Linux
  `-Werror=format-truncation` diagnostics in install-path composition.
- PS5 ELF artifact: `ShadowKraken_PS5_9bb105717fed06948ed48fa102158e8574594eb6`,
  4,452,776 bytes, SHA-256
  `a4a74417e7d7c1721672bb47c50aa0d3cd0013baa24699093ef152ed3874c0af`.
  The downloaded file identifies as ELF64 x86-64 PIE and contains PS5
  `libSce*.sprx` imports. This verifies the artifact format and build, not a
  console load or launch.

## Baseline build and regression suite

| Command | Result | Evidence / reason |
| --- | --- | --- |
| `make -n help` on macOS | BLOCKED (environment) | The local host lacks `/opt/ps5-payload-sdk/toolchain/prospero.mk`; Linux GitHub Actions separately built the actual PS5 payload with Prospero. |
| `CC=clang python3 tools/test_runtime_config.py --sanitize` | PASS (targeted, macOS arm64) | The test harness supplies Darwin stat/linker aliases and a test-only barrier shim; production config reload, concurrency and watcher checks pass. |
| `python3 tools/test_shellcore.py --sanitize` | PARTIAL/FAIL (platform) | ptrace unit portion passed; remaining test assembly uses x86-64 GNU syntax and cannot assemble for this arm64 macOS host. |
| `python3 tools/test_scan_permissions.py` | PASS (targeted) | Confirms explicit `direct_mount=off` suppresses uppercase `.PKG` callbacks and enabling the gate admits them; the production default is on. Host shim now overrides the macOS `strlcpy` macro portably. |
| `python3 tools/test_shellcore_launch.py --sanitize` | PASS | `Launch ownership: abandoned switch recovery, retry identity, fallback, live owner and mutation guards passed`. |
| `python3 tools/test_sandbox_cleanup.py` | PASS (targeted) | Checks owned nullfs/unionfs cleanup, stale identities, overmount and borrowed-layer protection, real failures, `fpkg`-labelled nullfs cleanup, and refusal to detach a borrowed FPKG layer during media removal. |
| Targeted cache harness: `clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-macro-redefined -Dst_mtim=st_mtimespec -Dst_ctim=st_ctimespec -Iinclude -Isrc -Itests -include tests/host_platform.h -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_fpkg_cache.c src/sm_fpkg_cache.c -o /private/tmp/sm-fpkg-cache-test && /private/tmp/sm-fpkg-cache-test` | PASS (targeted) | Synthetic active generation, source identity, symlinked active pointer, interrupted stage, orphaned generation, quarantine report, active-generation cleanup refusal, foreign-file refusal and ownership-aware quarantine/removal passed under ASan/UBSan. |
| Cache runtime image-reference validation: `tests/test_fpkg_cache_build.c` and `tests/test_fpkg_cache.c` linked with the cache/reader modules under ASan/UBSan | PASS (targeted, macOS arm64) | A same-size replacement of `kraken.image` with a different package filename is rejected by published-generation validation; restoring the expected relative filename makes the generation readable again. The hand-built recovery generation now also names the source package exactly. |
| Targeted end-to-end synthetic cache build: `tests/test_fpkg_cache_build.c` linked with the native FPKG reader/tree/index/cache modules and host shims under ASan/UBSan | PASS (targeted, macOS arm64) | A generated plaintext fixture with a 256 KiB inner PFS block size and 64 KiB inode-table pages passed parsing, loose `eboot.bin`/`sce_sys/param.json` extraction, AMPRIDX3/AMPRPAK4 creation, `kraken.image` path generation, cancellation after partial extraction, recovery, and reuse. A second-source rebuild cancelled after artifact generation preserved the prior active pointer. No commercial package was opened. |
| Targeted async registration cleanup: `tests/test_install_queue_fpkg.c` linked with `src/sm_install_queue.c` under ASan/UBSan | PASS (targeted, macOS arm64) | Confirms the old FPKG generation is retained while an AppInstallAll title is queued/submitted, released only after the title appears in app.db, and retained on registration timeout. |
| AMPR NG runtime selection/bootstrap: `tests/test_ampr_runtime.c` linked with `src/sm_ampr_runtime.c` and the generated pinned runtime asset under ASan/UBSan | PASS (targeted, macOS arm64) | Selects a user-provided NG runtime before the bundled fallback, finds a marker split across a 64 KiB read boundary, rejects upstream AMPR without overwriting it, atomically installs and reuses the pinned runtime, preserves/rejects a wrong-family fallback, and rejects symlinked fallback roots/files. This does not test PS5 library loading. |
| Pinned AMPR runtime artifact | PASS (targeted) | `vendor/ampr-emu-ng-v0.4.0/libSceAmpr.sprx` matches the SHA-256 in its vendored `SHA256SUMS`; the Linux workflow repeats that verification. |
| Updated FPKG reader and tree checks: `tests/test_fpkg_header.c` and `tests/test_fpkg_tree.c` under ASan/UBSan | PASS (targeted, macOS arm64) | The synthetic full-reader fixture now verifies that the superblock's PFS block size positions the inode table, while inode records remain paged in 64 KiB units. The focused test-header harness supplies a false fd-identity stub because it exercises callback-backed readers only. |
| Targeted syntax checks: `clang -std=gnu11 -Wall -Wextra -Werror -Wno-macro-redefined ... -fsyntax-only src/sm_scan.c src/sm_scan_tree.c src/sm_install.c src/sm_shellcore_service.c src/sm_fakelib.c src/sm_ampr_runtime.c` with the macOS host shim and Darwin stat-time aliases | PASS (targeted) | Changed scanner, install, runtime, Fakelib cache integration and AMPR identity translation units pass host syntax checking with declarations for PS5 SDK calls supplied by the test shim. This does not validate PS5 ABI or behavior. |
| Targeted API syntax check: `clang -std=gnu11 -Wall -Wextra -Werror ... -fsyntax-only src/sm_api_service.c` with the host shim and Homebrew json-c/libmicrohttpd headers | PASS (targeted, macOS arm64) | The settings GET/update integration compiles with its host dependencies available. This does not validate PS5 API behavior. |
| `python3 tools/test_fpkg_install_lifecycle.py` | PASS (targeted, macOS arm64) | Production `.pkg` classification and `try_collect_candidate_for_fpkg`, `prepare_fpkg_candidate`, install/registration and Fakelib source-resolution helpers were compiled and executed in sequence. It checks extension classification, feature-off and unsupported-input rejection, candidate-to-generation transition, lifecycle mutation guard, stale source-snapshot rejection, `.PKG` source-parent resolution, nullfs → `mount.lnk` → TitleDir ordering, rollback on link publication failure, bridge/API result handling, batch/restore/failure registration paths, launch-layer reuse and `/app0/kraken` read-only flags. It also covers bounded install-path construction. The orchestration harness stubs the PKG parser, FPKG reader/cache APIs and platform calls; the separate cache-build fixture exercises the real C reader/cache with synthetic bytes. This is host routing evidence, not console behavior. |
| `clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_fpkg_header.c src/sm_fpkg.c src/sm_fpkg_layout.c src/sm_fpkg_image.c src/sm_fpkg_decoder.c src/sm_fpkg_tree.c src/sm_fpkg_index.c -o /tmp/sm-fpkg-header-test && /tmp/sm-fpkg-header-test` | PASS (focused safety test) | Synthetic FIH/PFS eligibility, bounded CNT table and payload extents, outer direct/indirect maps, mapped descriptor parse, three-block image reads, retained file enumeration and `eboot.bin` range read, package block mapping for aligned inner and CNT files, unaligned-block rejection, package-to-AMPR index construction for packed and caller-selected extracted files, streaming extraction success and cancellation rollback, CNT `icon0.png` fallback enumeration/read, and inner tree validation pass under ASan/UBSan. This is not a game or real-package compatibility fixture. |
| `clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_fpkg_index.c src/sm_fpkg_index.c src/sm_fpkg.c src/sm_fpkg_layout.c src/sm_fpkg_image.c src/sm_fpkg_decoder.c src/sm_fpkg_tree.c -o /tmp/sm-fpkg-index-test && /tmp/sm-fpkg-index-test` | PASS (focused safety test) | Synthetic AMPRIDX3/AMPRPAK4 layout, RAW and codec-2 descriptors, CRCs, duplicate/case-folded paths, traversal, invalid UTF-8 and package-bound checks passed under ASan/UBSan. |
| `clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_fpkg_layout.c src/sm_fpkg_layout.c -o /private/tmp/sm-fpkg-layout-overlap-test && /private/tmp/sm-fpkg-layout-overlap-test` | PASS (focused safety test) | Synthetic raw 256 KiB Kraken descriptor, a legal 62-byte stored-range overlap, resource bounds, compression/shuffle rejection, sparse rejection and short input passed under ASan/UBSan. |
| `clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_fpkg_decoder.c src/sm_fpkg_decoder.c -o /private/tmp/sm-fpkg-decoder-test && /private/tmp/sm-fpkg-decoder-test` | PASS (focused safety test, macOS arm64) | Synthetic raw block, LZ literal and match commands, escaped match length, delta literals, single-symbol and multi-symbol Huffman arrays, plus 16,384 deterministic malformed headers/arrays/modes passed under ASan/UBSan. |
| `python3 tools/test_fpkg_decoder_reference.py /private/tmp/kraken-mount-reference` | PASS | The pinned Kraken reader at `4b420d579bd381796d773875e36d14c9acaabf39` independently decoded matching synthetic literal, delta, match and Huffman vectors. |
| `python3 tools/test_fpkg_decoder_differential.py /private/tmp/kraken-mount-reference` | PASS (targeted differential, macOS arm64) | 16,384 deterministic malformed inputs with full-width descriptor mode bytes plus sparse-block and block-boundary cases (16,410 total) matched the pinned Kraken decoder on acceptance and every decoded byte for accepted inputs. |
| `clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_fpkg_tree.c src/sm_fpkg_tree.c -o /private/tmp/sm-fpkg-tree-64k-test && /private/tmp/sm-fpkg-tree-64k-test` | PASS (focused safety test) | Synthetic inner-PFS tree bounds, 64 KiB inode-table pages, UTF-8/path traversal rejection, inode-count cap and directory-cycle rejection passed under ASan/UBSan. |
| `clang -std=gnu11 -O1 -g -Wall -Wextra -Werror -Iinclude -Isrc tests/fpkg_ampr_oracle/generate.c src/sm_fpkg_index.c src/sm_fpkg.c src/sm_fpkg_layout.c src/sm_fpkg_image.c src/sm_fpkg_decoder.c src/sm_fpkg_tree.c -o /private/tmp/fpkg-ampr-generate`; `/private/tmp/fpkg-ampr-generate /private/tmp/sm-fpkg-ampr-oracle`; `CARGO_TARGET_DIR=/private/tmp/fpkg-ampr-cargo cargo run --offline --locked --manifest-path tests/fpkg_ampr_oracle/Cargo.toml -- /private/tmp/sm-fpkg-ampr-oracle/ampr_emu.index /private/tmp/sm-fpkg-ampr-oracle/ampr_assets.index /private/tmp/sm-fpkg-ampr-oracle/kraken.pkg` | PASS (targeted differential, macOS arm64) | The synthetic fixture contains three files and four mixed RAW/KRAKEN chunks, including a compressed full-block tail for a shorter logical final chunk. C and pinned Kraken indexes match byte-for-byte; pinned AMPR v0.4.0 validates the manifests and reads back all RAW and codec-2 bytes. The AMPR source remains test-only and is not included in SMP. |
| Read-only fixture probe: temporary C harness linked `sm_pkg.c` and all FPKG modules, parsed package metadata, then called `sm_fpkg_open_stream` | EXPECTED UNSUPPORTED | SMP metadata classification accepted `PPSA12544` as a base package. The native reader returned `SM_FPKG_UNSUPPORTED` because the layout boundary table contains a sparse extent at mount offset `0x6b52db7b`. No game files were read or extracted. The temporary harness and executable are in `/private/tmp`, not the repository. |
| Pinned reference cross-check: `CARGO_TARGET_DIR=/private/tmp/kraken-cargo cargo run --offline --locked --manifest-path /private/tmp/kraken-mount-reference/crates/kraken-mount-pkg/Cargo.toml --example ls -- '<supplied package path>'` | EXPECTED UNSUPPORTED | Kraken revision `4b420d579bd381796d773875e36d14c9acaabf39` returned `unsupported package: the layout has a sparse extent at mount 0x6b52db7b; repack with --kraken stored`. The native reader and pinned reference agree on this unsupported boundary. This is not a full-tree differential test or compatibility pass. |
| Read-only fixture probe: user-supplied Vampire Survivors package, `PPSA21703`, 1,031,609,089 bytes | PASS (host reader/tree) | SMP metadata accepted it as a base package (`content_type=0x20`). After fixing legal overlapping stored extents, escaped Kraken lengths and the inner PFS 64 KiB block size, the native C reader opened it and enumerated 1,336 entries. The pinned Kraken reader at `4b420d579bd381796d773875e36d14c9acaabf39` enumerated the same 1,336 paths with identical file/directory types and sizes. Both report `eboot.bin` at 37,792,703 bytes (145 raw blocks) and `sce_sys/param.json` at 2,651 bytes. One actual 256 KiB metadata block decoded byte-for-byte identically with C and Rust. File payloads were streamed and hashed separately (next row), not saved to disk; this is not a PS5 launch test. |
| Real file read differential: temporary host harness using `sm_fpkg_open_fd`/`sm_fpkg_read_file` compared to pinned Kraken `ls --cat`, streamed to SHA-256 | PASS | `eboot.bin` (37,792,703 bytes) and `sce_sys/param.json` (2,651 bytes) matched byte-for-byte by SHA-256: `b72dd8a1c6ea1cd118a087773e8a4075d725415d9701fc70fb6bad2386e9828a` and `7e98ed267fae1b337d6af82e87f11ab52587c646fe7c50f53ccb5e4351d4d380`, respectively. Contents were streamed through memory and pipes, not saved to disk. |
| Temporary focused layout oracle: `ps5upload_fpkg::kraken_image::describe` and the C parser consumed the same synthetic descriptor bytes | PASS | Both reported one raw block at logical/stored offset zero, length 256 KiB. The descriptor and harness remained under `/private/tmp`; this is not an FPKG fixture. |

The existing suite outcomes above are macOS baseline results; the FPKG commands
are focused implementation checks. No test was reported as passing based on
source inspection.

## FPKG matrix status

All T01–T38 remain **NOT MET**. The supplied `PPSA12544` package is a real debug
FPKG, but both the native reader and pinned Kraken reject it because of a sparse
extent that the pinned reference MVP does not support. A second real package,
`PPSA21703`, now passes native metadata parsing and full tree enumeration with
an exact path/type/size match against pinned Kraken; one real metadata block
also decodes byte-for-byte identically, and its `eboot.bin` and
`sce_sys/param.json` reads match pinned Kraken by SHA-256. Broader file payload
coverage, package-to-AMPR readback, the full legal golden corpus and PS5 launch
test remain unverified. The PS5 payload build is now verified in Linux CI, but
that does not verify package acceptance or runtime behavior on a console. Both
packages remain in Downloads and were not copied into this repository.

## Validation gates

| Gate | Status |
| --- | --- |
| G0 repository/source/license audit | Source and license audit recorded. Local macOS lacks the PS5 SDK; Linux CI successfully built the PS5 payload at `9bb1057`. |
| G1 legal-fixture differential reader tests | PARTIAL; synthetic outer-to-inner reads, retained file paths, bounded file reads, CNT fallback entries, package-to-AMPR index construction and streaming extraction exist. The `PPSA21703` tree matches pinned Kraken across all 1,336 entries, and its `eboot.bin` and `param.json` readbacks match by SHA-256. The broader legal fixture corpus remains. `PPSA12544` remains unsupported by both readers due to its sparse extent. |
| G2 decoder differential/sanitizer/fuzz gates | PARTIAL; native decoder and synthetic differential vectors pass, including an escaped-length regression under ASan/UBSan. A 16,384-case deterministic malformed-input corpus passes sanitizers and matches pinned Kraken acceptance/output in a 16,410-case differential check. One real metadata block and two real file readbacks match pinned Rust. The broader legal golden corpus, longer fuzzing and worst-case CPU/memory measurements remain. |
| G3 byte-identical indexes and runtime readback | PARTIAL; the C and pinned Kraken writers produce byte-identical AMPRIDX3 and AMPRPAK4 output for a three-file, four-chunk RAW/KRAKEN fixture, including a compressed short tail. Pinned AMPR readers validate the manifests and read back all synthetic chunks. Broader manifest coverage and real-package AMPR vectors remain. |
| G4 safe cache crash/recovery tests | PARTIAL; synthetic recovery, symlink rejection, staging/orphan/quarantine reporting, active-generation refusal and ownership-manifest cleanup pass. Power-loss injection during a real FPKG build is not available in the host fixture. |
| G5 PS5 launch/mount lifecycle | PS5-only; NOT RUN. |
| G6 Linux CI and release build | PASS for commit `9bb1057`; workflow `43612405370` built the PS5 ELF and uploaded its commit-specific artifact. |

Host-verified functionality includes the bounded FIH/PFS/CNT and outer metadata
reader through descriptor mapping and parsing, retained inner-tree paths and
bounded file reads (including the three reference CNT fallback paths), package
block mapping, package-to-AMPR index construction and a callback-based streaming
extraction path against synthetic fixtures, full synthetic reader-to-cache
generation construction with cancellation recovery and generation reuse, cache
recovery/cleanup fixtures,
feature-gated scanner callback behavior, the sandbox layer ownership tests, exact
path/type/size tree matching for the historical read-only `PPSA21703` fixture,
byte-identical streaming readback of its `eboot.bin` and `sce_sys/param.json`,
one byte-identical real metadata block decode, the standalone Kraken descriptor
parser against a synthetic descriptor also accepted by ps5-dump-forge, byte
comparison of multi-file writer output against pinned Kraken and AMPR RAW/KRAKEN
readback, the
native Kraken decoder against synthetic reference vectors and a deterministic
malformed-input differential corpus, and AMPR NG identity,
user-runtime precedence and bundled fallback installation. Host syntax checks
cover the changed scanner, install, runtime, Fakelib cache integration and
AMPR identity translation units and
`sm_api_service.c`; the API syntax check uses Homebrew json-c and
libmicrohttpd headers. Cache generation against a full FPKG,
AppInstallTitleDir registration, the PS5 `/app0/kraken` mount and game launch
remain unverified on console. Fakelib cache composition with the NG runtime
also remains unverified on PS5. SMP uses a compatible runtime at
`<emulators_path>/ampr-emu-ng/libSceAmpr.sprx` when present; otherwise it installs
the pinned v0.4.0 binary into `/data/shadowmount/fpkg-runtime/`. A wrong-family
file at either location blocks FPKG launch preparation and is not overwritten.
The existing updater still targets the top-level upstream AMPR file. The
production config writer's FPKG setting roundtrip is covered by the host config
test.
