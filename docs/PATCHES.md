# Ronin downstream patch ledger

This ledger describes runtime behavior carried by `ronin/main` beyond the
recorded `AceSLS/SLSsteam` base. A downstream runtime change is not complete
until this file names its requirement, owning tests, upstream overlap, and
retirement condition.

Build, documentation, test-only, and packaging changes that do not alter
SLSsteam runtime behavior do not require separate ledger entries.

## Base

- Upstream: `AceSLS/SLSsteam`
- Upstream branch: `main`
- Current recorded base: `9927612`
- Downstream branch: `ronin/main`

The exact base for any revision is:

```sh
git merge-base upstream/main ronin/main
```

Release provenance must record that base, the Ronin revision, and hashes of
every packaged payload.

## RONIN-CONFIG-1: resilient configuration and added-app discovery

**Requirement**

Discover Lua-managed applications from `stplug-in/*.lua` and
`luaappids.yaml`, combine them with supported compatibility sources, tolerate
malformed or out-of-range configuration values, and react to atomic file
replacement and runtime additions/removals.

**Why upstream does not satisfy it**

AceSLS owns the general configuration engine and `AdditionalApps`, but does
not treat the LuaTools file layout as an authoritative application source.

**Implementation**

- `9cb19f7` — pure discovery and normalization primitives
- `12f29a2`, `40ae5f9` — non-throwing scalar handling
- `8542c12` — malformed configuration recovery
- `9376973` — directory/inode-safe watching
- `3a9cb47` — integration with upstream configuration

**Acceptance**

- `tools/test_config_discovery.cpp`
- `tools/test_config_overflow.cpp`
- `tools/test_confload.cpp`
- `tools/test_confnormalize.cpp`
- Live addition and removal of a Lua-managed app without restarting Steam

**Upstream overlap**

Retain AceSLS configuration parsing, parent traversal, and generic hot-reload
fixes. Adapt this patch when those internals change; do not replace them
wholesale with an older Moon copy.

**Retirement**

Retire only if AceSLS gains an extensible source adapter that can express the
same LuaTools discovery and runtime reconciliation semantics.

## RONIN-CONTENT-1: Lua-added application content pipeline

**Requirement**

Make Lua-added applications behave like provisioned Steam applications:
reconstruct app info, retrieve PICS/product data and depot keys, stage and
persist manifests, tolerate provider outages, and select compatibility tools.

**Why upstream does not satisfy it**

AceSLS provides the ownership and hook engine but does not provision the
external Lua application/depot topology consumed by this ecosystem.

**Implementation**

- `76d9d88` — initial retained Moon feature transplant
- `src/feats/appinfo_*`
- `src/feats/cmclient.*`, `pics.*`, and supporting CM/cache code
- `src/feats/depotkey.*` and manifest storage/synthesis code
- `src/feats/compattool.hpp`

**Acceptance**

- App-info reconstruction and round-trip tests
- PICS, CM wire, cache, retry, provisioning, depot-key, and compatibility
  tests
- Live install of a Lua-added application from an empty local app-info cache
- Provider-offline failure remains localized and recovers after cooldown

**Upstream overlap**

AceSLS remains authoritative for SDK objects, protobufs, VFT discovery,
ownership behavior, and hook lifecycle. Ronin adapters must migrate onto those
interfaces instead of freezing their older definitions.

**Retirement**

Individual adapters may retire when AceSLS exposes an equivalent generic
extension point. The full entry retires only if upstream can provision the
external app/depot topology end to end.

## RONIN-PINS-1: historical manifest selection

**Requirement**

Resolve a locked application to explicit per-depot historical manifests,
preserve the installed/public side of Steam's comparison, stage validated
manifests, download the real content delta, reconcile after install, and
reverse cleanly to public content.

**Why upstream does not satisfy it**

AceSLS has `ManifestIds`, but Ronin requires a lock-aware per-application
model, archived manifest storage, reconstruction, validation, and UI/control
integration.

**Implementation**

- Initial manifest infrastructure in `76d9d88`
- Current worktree changes in `manifestbind`, `manifeststore`,
  `reconcilepin`, manifest decryption, and Ronin control
- The finalized revision must replace this worktree note when committed

**Acceptance**

- Manifest synthesis, decryption, storage, selection, pin, and pattern tests
- Live public-to-historical validation performs a nonzero content update
- Restart retains the historical manifest without an update loop
- Clearing the pin validates back to public content
- Missing patterns, sizes, keys, or manifests fail closed

**Upstream overlap**

Follow AceSLS changes to `BuildDepotDependency`, planner structures, manifest
configuration, and VFT discovery. `ManifestIds` is not a replacement for this
workflow but may supply reusable upstream primitives.

**Retirement**

Retire only if AceSLS implements equivalent installed-versus-target semantics
and the complete historical manifest lifecycle.

## RONIN-USER-1: added-app user features

**Requirement**

Provide achievements, player statistics, parental-policy integration, and
other user-facing behavior for Lua-added applications.

**Why upstream does not satisfy it**

AceSLS implements the general features, but external applications require
additional identity, ownership, schema, and lifecycle adaptation.

**Implementation**

- Initial adapters in `76d9d88`
- `src/feats/achievements.*`, `playerstats.hpp`, and `parental.*`

**Acceptance**

- Achievement and player-stat regression tests
- Parental settings lifecycle tests
- Representative live added-game checks

**Upstream overlap**

Prefer AceSLS's current protobuf achievement path, SDK structures, and hook
discovery. Keep only the external-app adaptation.

**Retirement**

Retire adapters individually when upstream supports externally provisioned
applications through the same interfaces.

## RONIN-HOST-1: generic host contracts

**Requirement**

Publish the dynamically selected CEF debugging port and preserve the launch
environment needed by the injected payload without importing Steam launcher
or desktop-management behavior.

**Why upstream does not satisfy it**

The port contract is consumed by a separate module host. AceSLS is otherwise
correctly unaware of Tsuki and Ronin.

**Implementation**

- CEF contract and launch-boundary work in `76d9d88`
- `5f31529` — preserve inherited `LD_LIBRARY_PATH` entries

**Acceptance**

- CEF contract tests
- Launch environment test
- Live attachment on a non-default CEF port

**Upstream overlap**

None at the product level. Continue to use AceSLS's launcher-independent
engine and avoid importing Moon wrapper/desktop code.

**Retirement**

Retire the CEF contract only if the standardized Ronin host interface provides
an equivalent declaration and runtime channel.

