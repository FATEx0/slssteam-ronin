# SLSsteam Ronin

SLSsteam Ronin is an upstream-first SLSsteam variant and a self-contained
[Ronin module](module/) for Tsuki.

It keeps [AceSLS/SLSsteam](https://github.com/AceSLS/SLSsteam) as its
authoritative engine while carrying a small, documented downstream patch
series for Lua-managed applications, content provisioning, historical manifest
selection, and Tsuki integration.

> **Development status**
>
> Ronin hooks private 32-bit Steam implementation details. Steam updates can
> disable optional features until their patterns and object assumptions are
> revalidated. This repository is under active development and is not yet a
> general-purpose portable release.

## Choose the right branch

| Branch | Purpose |
| --- | --- |
| `ronin/main` | Ronin product branch. Build, package, test, and release from here. |
| `main` | Clean mirror of `AceSLS/SLSsteam/main`. Do not add Ronin commits here. |

A Ronin patch reaches `main` only after AceSLS accepts equivalent behavior
upstream. Upstream updates are fast-forwarded into `main`, then the ordered
Ronin patch series is rebased onto that new base.

## What Ronin adds

- Discovery of Lua-managed games from `stplug-in/*.lua` and
  `luaappids.yaml`, including runtime changes.
- App-info reconstruction and provisioning for added games.
- Native CM/PICS product-data retrieval with bounded fallbacks.
- Depot-key, manifest download, staging, persistence, and synthesis support.
- Historical per-depot manifest selection through the module-owned Manifest
  Pins view and browser-assisted history collector.
- Added-game achievements, player statistics, compatibility-tool selection,
  and parental-policy integration.
- Managed-DLC quarantine for repeatedly undecryptable Lua-supplied depot keys.
- A dynamic CEF-port contract consumed by the generic Tsuki host.
- A complete Ronin module package built from the same revision as the native
  payload.

The authoritative downstream inventory, acceptance requirements, and
retirement conditions live in [docs/PATCHES.md](docs/PATCHES.md).

## Repository layout

```text
src/       AceSLS engine plus the ordered Ronin patches
tools/     build helpers, collectors, and regression tests
module/    complete self-contained Ronin module package
docs/      architecture, patch ledger, and upstream workflow
```

`module/` is the canonical package source. Tsuki may contain a deployed
compatibility copy, but SLSsteam behavior and package metadata must be changed
in this repository.

## Build and validate

Enter the repository's declared Nix development environment and build the
native payload plus package-owned helpers:

```sh
nix develop
make audit-libs
```

Build the complete staged module:

```sh
make ronin-module
```

The current Steam client pattern checks are:

```sh
make test-manifestpin-patterns
make test-depotquarantine-patterns
```

The package should also be validated with Tsuki's Ronin module SDK before
deployment. See [docs/RONIN.md](docs/RONIN.md) for the complete architecture
and Steam-update acceptance procedure.

## Maintenance

- [Downstream patch ledger](docs/PATCHES.md)
- [Upstream synchronization workflow](docs/UPSTREAMING.md)
- [Ronin architecture and package notes](docs/RONIN.md)
- [Packaged source provenance](module/SOURCE)

Do not merge `ronin/main` into `main`. To synchronize a new Ace release,
fast-forward `main`, rebase the downstream series in ledger order, run the
affected tests, rebuild `module/`, and record the new provenance.

## Upstream and credits

Ronin exists because of the work in
[AceSLS/SLSsteam](https://github.com/AceSLS/SLSsteam). Its original project
credits, license, and upstream documentation remain authoritative for the
engine inherited here.

Selected Lua-ecosystem behavior was adapted from
[swwayps/slsteam-moon](https://github.com/swwayps/slsteam-moon). Moon is a
behavioral reference, not Ronin's source base; its Lumen-specific launcher,
desktop guardian, and private notification transport are deliberately not
part of Ronin.

See [docs/LICENSE](docs/LICENSE) and the source-file SPDX declarations for
licensing details.
