# Upstream synchronization

`FATEx0/slssteam-ronin` is a direct fork of `AceSLS/SLSsteam`.

The branch contract is:

- `main` mirrors `upstream/main` and contains no Ronin commits.
- `ronin/main` contains the ordered downstream patch series and complete Ronin
  package.
- Release tags are immutable points on `ronin/main`.

Configured remotes:

```text
origin    https://github.com/FATEx0/slssteam-ronin.git
upstream  https://github.com/AceSLS/SLSsteam.git
```

## Update the mirror

Fetch first and inspect the incoming commits:

```sh
git fetch upstream
git log --oneline main..upstream/main
git diff --stat main..upstream/main
```

Update `main` only from a clean worktree. `main` is an upstream mirror, so a
fast-forward is required:

```sh
git switch main
git merge --ff-only upstream/main
git push origin main
```

Never resolve an upstream conflict on `main`, and never add a local commit
there.

## Reapply the Ronin patch series

Before rebasing:

1. Finish or checkpoint all work on `ronin/main`.
2. Confirm `git status --short` is empty.
3. Create an immutable backup tag or branch for the last tested revision.
4. Read every incoming upstream commit that touches a ledger-owned subsystem.

Then:

```sh
git switch ronin/main
git rebase main
```

Resolve conflicts in the downstream patch that owns the requirement. Preserve
AceSLS's current SDK, decompiler, VFT discovery, hook lifecycle, and generic
feature implementation. Adapt the Ronin extension to those interfaces rather
than copying an older Moon core file over them.

After each affected ledger group, run its focused tests. After the rebase, run
the complete offline suite and controlled live acceptance before publishing:

```sh
git push --force-with-lease origin ronin/main
```

`--force-with-lease` is appropriate only because `ronin/main` is a maintained
patch-series branch. Never rewrite a release tag.

## Route new changes

- Generic SLSsteam engine fix: submit to AceSLS and carry temporarily only if
  Ronin needs it before acceptance.
- Lua ecosystem behavior: add a focused Ronin patch and ledger entry.
- Standard module lifecycle or packaging behavior: implement through the
  Ronin specification/SDK.
- SLS-specific UI: keep in the package-owned view.
- Temporary workaround: document its removal condition and do not move it into
  Tsuki.

When AceSLS accepts a carried generic fix, drop the duplicate downstream commit
during the next rebase.

## Provenance

Every package build must record:

```text
ace_base
ronin_revision
payload_sha256
build_environment
```

The same identity must be available in the package `SOURCE` file and startup
or health diagnostics. A package must never combine metadata and payloads from
different revisions.

