# FPKG cache transaction contract

Issue #221. The cache implementation follows this contract; host evidence and
remaining failure-injection gaps are recorded below.

## State machine

```text
idle -> locked -> prepared -> staging -> verified -> published -> registered
                   |            |           |            |
                   +------------+-----------+------------+-> failed/quarantined
```

The lock is per title and acquired under SMP's existing global lifecycle/mount
state ordering. A title that is launching, active, mounted, in rest-mode recovery,
or has ambiguous ownership cannot be scanned for rebuild, mutated, or purged.

## Prepare

- Open the source without following a final symlink; retain the opened file and
  source snapshot (device/inode, size, ctime/mtime at minimum).
- Validate the existing SMP PKG metadata and then the separate full plaintext
  debug-FPKG format eligibility. Reject PS4 CNT-only, LIH, retail, AES-XTS,
  shuffled/unsupported revisions and malformed geometry.
- Build a bounded plan of files and output sizes. Reject path collisions,
  traversal, excess depth/count/size, overflow, unsupported file types and any
  source change observed before publication.
- Check free space and exact per-title collision/registration state before writes.

## Stage

- Create a unique, mode-restricted staging generation inside the cache filesystem
  using exclusive creation and no-follow directory-relative operations.
- Extract bounded chunks to new files. Do not truncate or replace existing files.
- Write the relative `kraken.image`, AMPR indexes, owned-file manifest and journal
  into staging only. Never follow symlink components.
- Flush file data and staging directory metadata where the platform supports it.

## Verify

- Re-read the source snapshot and fail if replaced or modified.
- Validate all generated paths and sizes, executable metadata, index bounds,
  relative image target, CRCs and hashes where specified.
- Compare index/manifest bytes with the pinned Kraken writer after normalizing
  only documented nondeterministic fields.
- Ask the pinned AMPR runtime/reader to read selected codec-2 chunks and compare
  decoded bytes when that oracle is available.
- A missing/corrupt `.built`, journal or `.done` is quarantine/manual-recovery
  state, never evidence that the whole cache is owned.

## Publish and register

- Keep the last known-good generation available until the new generation passes
  validation and no active session references it.
- Publish a single generation pointer/marker last; a sequence of renames across
  cache, appdb and mount state is not a transaction.
- Where the kernel lacks no-replace rename, first pointer publication uses a
  hard link and synchronizes the directory before removing the temporary name.
  Recovery removes only a verified, backend-named alias of the same owned inode.
  Directory transitions reserve and verify an empty destination inside the
  private, title-locked cache directory before replacing that reservation.
- Update SMP registration only after generation publication. If registration
  fails, retain the prior registration and last known-good generation; record
  the journal state for recovery.
- Mount the containing directory of the original `.pkg` at
  `<sandbox>/app0/kraken`, then verify the relative `kraken.image` resolves to the
  expected filename. File-to-file nullfs is not accepted as a substitute.

## Recovery, rebuild and removal

- Startup recovery inspects journal state and mount identity. It may complete or
  roll back only paths created by the backend and positively identified by the
  journal/generation identity.
- Never copy Kraken's `clear_build -> empty_dir` fallback. Never recursively
  remove a cache based only on a corrupt or missing ownership manifest.
- Rebuild replaces backend-owned immutable generations. It preserves unknown and
  game-created files, and never mutates a live writable `app0` blindly.
- Safe cleanup removes only known inactive generated generations after validating
  their identity and symlink-free path. Destructive purge is a separate explicit
  action, scope-limited and blocked while title/mount ownership is active.
- Game-created `/app0` files live in a separate writable layer. A generation
  switch copies non-owned paths into a fresh layer and publishes its pointer
  only after the copy is synchronized. Prior layers and conflicting files are
  retained. Unsupported special entries stop migration without deleting data.
- Failure at extract, index write, flush, rename, journal update, registration or
  power-loss recovery must leave the last known-good generation reachable.

## Failure-injection coverage and gaps

The synthetic cache fixtures currently cover cancellation during extraction and
replacement, recovery of interrupted staging/orphan generations, invalid active
pointers, symlink rejection, ownership-manifest cleanup, refusal to remove the
active generation, and preservation of the previous active generation after a
cancelled replacement. Runtime generation validation also checks that
`kraken.image` contains the exact relative filename recorded by the source
identity; a same-size altered target is rejected.

The host fixtures do not inject failure after every individual `fsync`, rename,
journal transition, and active-pointer publication syscall, and they do not
simulate an actual power cut or PS5 filesystem behavior. Those remain separate
validation work; see `docs/fpkg-test-results.md` for the current gate status.
