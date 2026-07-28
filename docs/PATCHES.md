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

## RONIN-CONFIG-2: stable native Date Added metadata

**Requirement**

Make Steam's native Date Added sorting meaningful for discovered applications
without changing their date whenever a source file is edited or rewritten.

**Why upstream does not satisfy it**

AceSLS can apply an explicit `SubscriptionTimestamps` map to ownership records,
but it does not assign or persist timestamps for applications discovered
through Ronin's `stplug-in` and `luaappids.yaml` adapters.

**Implementation**

- `src/feats/firstseen.hpp` owns the pure first-discovery and precedence rules.
- `sls-prelaunch` reconciles active AppIDs into
  `<config>/cache/ronin-first-seen.yaml` before Steam starts.
- Runtime source reload records newly discovered AppIDs without resetting
  existing or removed entries.
- `SubscriptionTimestamps` remains authoritative. Automatic first-seen values
  fill only active discovered AppIDs without an explicit override.
- The injected module reads the cache but does not rewrite it during its
  loader-time initial configuration pass.

**Acceptance**

- `tools/test_firstseen.cpp`
- Existing timestamps survive ordinary reloads, removal, and restoration.
- A new AppID receives the current timestamp exactly once.
- Explicit `SubscriptionTimestamps` override automatic values.
- Malformed cache data fails closed without overwriting the cache.
- Live Steam library sorting places a newly discovered game according to its
  recorded first-seen time and keeps that order after editing its source.

**Upstream overlap**

Continue using upstream's ownership-record application of
`SubscriptionTimestamps`. This patch supplies only the missing Ronin discovery
and persistence layer.

**Retirement**

Retire if the eventual Ronin-native application importer owns durable install
metadata and supplies an equivalent timestamp through a standard module
interface.

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
- `bfd5d21` — target-only planner redirection, archived manifest
  normalization/storage, historical build resolution, and Ronin control/UI
  integration
- `src/feats/reconcilepin.cpp` patches only the appinfo-derived target vector
  after its dedicated builder has populated it. The entry context vector is
  observation-only: live Linux testing showed it can alias Steam's
  active/installed state. Rewriting that vector made the active and target
  GIDs equal before planning, producing a metadata-only false success without
  installing the historical files.

**Acceptance**

- Manifest synthesis, decryption, storage, selection, pin, and pattern tests
- Live public-to-historical validation performs a nonzero content update
- Restart retains the historical manifest without an update loop
- Clearing the pin validates back to public content
- Missing patterns, sizes, keys, or manifests fail closed

Live Linux acceptance for app `2723430`, public depot
`4105671086490885582` to historical depot `4449340589420685266`, downloaded
271,086,368 bytes and committed 52 updated/3 deleted files. A clean restart
did not schedule another update. Clearing the pin downloaded 342,204,944
bytes back to the public manifest and committed 53 updated/2 deleted files.

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

## RONIN-COMPAT-1: Steam compatibility hash refresh

**Requirement**

Refresh the known `steamclient.so` hash list without inheriting Steam's loader
injection into the external TLS client, while preserving the user's normal
proxy, DNS, certificate, and home-directory environment. A failed refresh must
fall back to the cached list with an actionable diagnostic.

**Why upstream does not satisfy it**

The inherited curl launcher replaces the complete environment with a malformed
quoted `PATH`, omits a real `argv[0]`, and reports only an unexplained numeric
result. This fails on the Nix host and hides whether the live check used fresh
or cached compatibility data.

**Implementation**

- `src/curl.cpp` — valid curl invocation, bounded redirects/timeouts, standard
  executable locations, inherited user environment with loader variables
  removed.
- `src/update.cpp` — source-specific refresh, empty-response, cache-fallback,
  and parse diagnostics.

**Acceptance**

- Fetch the authoritative hash list with HOME/certificate resolution intact
  and Steam loader variables present in the parent.
- HTTP failure and empty response do not replace a usable cached list.
- A missing executable reports exit code 127 rather than appearing as an empty
  successful response.
- Live Ronin startup identifies whether fresh or cached compatibility data was
  used.

**Upstream overlap**

Prefer an equivalent upstream subprocess/environment fix if AceSLS adopts one.
Do not move TLS back into the audited Steam process merely to remove this
adapter.

**Retirement**

Retire when upstream provides an external fetch path with the same environment
isolation, fallback behavior, and diagnostics.

## RONIN-CONTENT-2: unusable managed-DLC quarantine

**Requirement**

Keep a managed DLC/add-on from repeatedly poisoning Steam's content-source
state when its Lua-supplied depot key definitively fails chunk decryption.
Only the affected DLC may be omitted; base, shared, and unrelated content must
remain eligible. Replacing or withdrawing the bad key must release the
quarantine automatically.

**Why upstream does not satisfy it**

AceSLS does not consume Lua-managed depot keys and therefore cannot distinguish
this ecosystem's bad-key failure from an ordinary transient content failure.

**Implementation**

- `fa57817` — Ronin adaptation and regression coverage
- Adapted from `swwayps/slsteam-moon` commit `1022c9f`
- `src/feats/depotquarantine.*` — optional callback observation and policy
- `src/feats/depotquarantine_store.hpp` — key-bound persistent decisions
- Target-only filtering in `manifestbind` plus package-0 reconciliation

**Acceptance**

- `tools/test_depotquarantine.cpp`
- `tools/test_depotquarantine_store.cpp`
- DLC classification regression coverage
- Both private callback patterns fail closed when unavailable
- Controlled live bad-key test confirms only the affected DLC is omitted,
  unrelated downloads remain usable, and a replacement key releases it

**Upstream overlap**

Continue using upstream's download and package engine. The callback patterns
and private object offsets are Steam-build compatibility seams and must follow
the maintenance procedure in `docs/RONIN.md`. Quarantine must remain inside
Ronin's target-only planner path; never broaden it to the active/public
comparison path used by historical manifest pins.

**Retirement**

Retire if the Lua key source gains authoritative validation before launch or
AceSLS exposes an equivalent scoped bad-key policy.

## Explicitly excluded Moon integration

Moon's desktop guardian, Debian launcher repair, systemd user-manager fallback,
and other Lumen-owned Steam supervision are not Ronin patches. Tsuki owns Steam
startup and restart through the generic launch-extension lifecycle; importing a
second supervisor would violate that boundary.

Moon's file queue under `~/.local/share/Lumen/notifications/` is also not
ported. User-facing Gamepad notifications are a useful outcome, but Ronin will
deliver them through a declared host interface rather than depending directly
on Lumen.

## RONIN-CONTENT-3: SteamStub ownership-ticket path

**Requirement**

Allow Lua-managed Windows games protected by SteamStub to launch without
rewriting their executable or requiring an external unpacker.

**Why upstream does not satisfy it**

AceSLS caches ownership tickets and hooks the Linux ticket interface, but a
failed ticket request for an AdditionalApp is returned unchanged. SteamStub
therefore rejects the launch even though SLSsteam has made the app and content
available.

**Implementation**

- `src/feats/steamstub_ticket.hpp` is a pure, bounded constructor for the
  SteamDRMP ticket layout.
- `Ticket::forgeSteamStubTicket` scopes the behavior to configured
  AdditionalApps and uses SLSsteam's existing AppID-7 cache.
- The existing `GetAppOwnershipTicketExtendedData` hook preserves every
  successful genuine response. Only a failed response with complete output
  pointers and sufficient capacity can be replaced.
- The physical ticket gains the requested four-byte AppID immediately before
  its signature while the original size and adjusted offsets are returned.

**Acceptance**

- `tools/test_steamstub_ticket.cpp`
- Ronin module schema validation and 32-bit payload build
- Controlled live launch of a SteamStub-protected managed game
- Verify the executable hash is unchanged and no unpacker artifact exists

Accepted live on 2026-07-28 with AppID 250180 (METAL SLUG 3, SteamStub Variant
2.1). The original executable SHA-256 remained
`f5f2130896e80ca5b5670dc2cbc4e7e6aa636775af3e8961b1418deeec6e46cd`,
no Steamless backup or marker was created, and the game started and was
playable through Proton.

**Upstream overlap**

Prefer an upstream equivalent if AceSLS adopts this narrowly scoped ticket
behavior. Do not move the logic into Tsuki or inject a helper into game
processes.

**Retirement**

Retire when upstream supplies equivalent scoped ownership-ticket handling.
