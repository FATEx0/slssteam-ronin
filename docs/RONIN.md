# SLSsteam Ronin

Ronin is an upstream-first SLSsteam variant for Tsuki. Its base is
`AceSLS/SLSsteam`; `slsteam-moon` is a behavioral reference, not the base
branch.

## Historical manifest resolution

The Manifest Pins view can resolve a historical build to its complete
`depot_id -> manifest_gid` map after importing an observation made by the
packaged SteamDB browser helper.

The current locally discovered Steam snapshot is the trust anchor. Observed
builds must begin at that exact build. For each build, Ronin stores the current
complete map, verifies every SteamDB `old -> new` manifest transition against
that map, rolls those depots back to `old`, and continues toward older builds.
Unchanged depots carry backward automatically. A zero transition endpoint
means the depot was absent on that side of the update.

Each app history reconstructs only depots represented by that app's
transitions. The collector records the app's authoritative depot table,
including embedded DLC ownership, configuration hints, and current branch
BuildIDs/timestamps. Every transition depot must be declared as owned by that
same app. An app without a depot table is classified as non-downloadable and
does not contribute a payload history.

Depot observation is deliberately richer than the reconstruction input. Each
exported depot retains `depot_id`, `owner_appid`, `configuration`, and the
SteamDB `Size` and `DL` cells as `size`/`dl` objects containing both decimal
`bytes` and human-readable `display` values. The importer currently consumes
identity, ownership, and configuration only; it must not require or reinterpret
size metadata merely because pin resolution does not use it.

The collector walks every depot table rendered in the Depots pane. It retains
the source section heading, normalized category, descriptor, and owner rather
than dropping redistributable/shared rows. Presentation/database normalization
remains downstream work; pin materialization consumes only the identifiers,
ownership, timestamps, transitions, and manifest histories it validates.

Duplicate DLC/inner-DLC rows are canonicalized by depot and owner while their
individual source appearances remain attached. Shared and redistributable
history is collected from each referenced depot's bounded Manifests page,
rather than recursively crawling its often enormous owner application. DLC
release timestamps that fall between two base builds are declared to attach to
the preceding base build during materialization.

The collector also records the base app's SteamDB DLC relationships and
collects downloadable DLC apps independently. When resolving a base build,
Ronin uses the next newer base build as the exclusive end of that build's
compatibility interval. DLC and shared manifests published inside the interval
attach to its older endpoint; the newest base build has an open-ended interval.
The local anchor applies to key-bearing game and DLC depots. stplug-in Lua
files are the only available source of their decryption keys, but they are not
a complete Steam depot topology and normally omit shared/redistributable
depots. A shared history therefore does not have to appear in that anchor; it
must instead be declared by the root app and contain a timestamped manifest
covering every root-build interval. DLC not present in the local anchor is
ignored. Missing shared interval coverage rejects the import instead of
emitting a partial pin map.

Any mismatch, duplicate build, malformed identifier, missing key-bearing
anchor depot, incomplete shared interval, or numeric overflow rejects the
whole import before the previous cache is replaced. The cache is stored as mode
`0600` under the host-resolved `$MODULE_DATA/manifest-history/` directory.

This is deliberately browser-assisted. Ronin does not bypass SteamDB's access
controls, copy browser cookies, or pretend SteamDB exposes a supported API.
Coverage is only the set of builds and depot histories that SteamDB rendered
successfully during that user-triggered collection.

## Managed DLC quarantine

Ronin observes Steam's optional chunk-unpack callbacks for definitive
decryption failures on Lua-managed DLC depots. Three distinct failed chunks
bind a quarantine decision to the exact 32-byte depot key. The affected DLC
app/depot is then omitted from both the target install plan and package-0
reconciliation so Steam does not immediately add and retry it again.

Base and shared depots are never quarantined by this policy. Decisions persist
under SLSsteam's cache directory, but a changed key, removed Lua source, or
lost DLC classification releases them. Both callback patterns are optional:
signature drift disables new quarantine learning without changing Steam's
download path. This is a mitigation for demonstrably unusable add-on keys, not
a general response to network, disk, or provider failures.

## Repository ownership and layout

Ronin is the single source repository for both the native fork and its complete
Tsuki module package. The module manifest, assets, default configuration,
packaging logic, tests, and compiled payload release must not be maintained in
a second repository.

Keep AceSLS's source layout at the repository root so upstream commits retain
their original paths and can be merged with minimal conflicts:

```text
slssteam-ronin/
├── src/                       # AceSLS core plus Ronin feature patches
├── include/
├── lib/
├── Makefile
├── module/
│   ├── module.json            # canonical Tsuki module specification
│   ├── settings.json          # component-owned revisioned settings contract
│   ├── interface.json         # strict exports, imports, logs and evidence
│   ├── resources.json         # grants, secrets and mutation declarations
│   ├── content.json           # executable/data/credential classifications
│   ├── views.json
│   ├── schemas/
│   ├── assets/
│   └── payload/               # populated by the module packaging target
│       ├── SLSsteam.so
│       ├── library-inject.so
│       ├── sls-prelaunch
│       └── slssteam-control
├── scripts/
│   └── deploy-tsuki-module.sh
├── build/                     # ignored, intermediate compiler output
└── dist/                      # ignored, complete installable module archives
```

The native build and module packaging are one pipeline: compile the exact core
revision and stage its artifacts into `module/payload`. Deployment through
`make deploy-tsuki-module TSUKI_ROOT=/path/to/tsuki` copies the complete package
into a temporary directory under Tsuki's module root, validates it with the
separate Ronin SDK checkout, installs the directory, and retains one rollback revision
outside discovery. A release must never combine a manifest from one revision
with a binary from another.

Tsuki may retain generic loader/schema code, but SLSsteam-specific metadata and
configuration declarations currently living in Tsuki must migrate here once
external package discovery is ready. During migration, generated copies in
Tsuki must be treated as compatibility outputs, not an independently edited
source.

## Rules

- Preserve upstream's decompiler, VFT-index, SDK, and hook-resolution design.
- Do not copy fork versions of shared core files wholesale.
- Keep each retained behavior in a reviewable feature commit.
- Do not carry Steam wrapper, desktop-entry, systemd, sidecar-launch, updater,
  installer, or CEF-command-line setup code. Tsuki owns host integration.
- Keep the payload usable through the standard `SLSsteam.so` and
  `library-inject.so` artifacts. Tsuki must not need a Ronin-specific runtime
  interface.
- Every ported subsystem must bring its isolated regression tests.

## Package-owned control backend

The executable currently packaged as `payload/slssteam-control` is not a generic
Ronin host service. It is the SLSsteam-specific backend for the Manifest Pins
view: it discovers managed games, reads and atomically updates SLSsteam's
manifest-pin configuration, imports and validates observed SteamDB history,
resolves builds to depot manifests, and implements the package exports
`pins.*`.

Ronin supplies only the generic machinery around it: managed subprocess
lifecycle, the common JSON envelope over a Tsuki-owned Unix socket, import and
export routing, health observation, and package-owned view hosting. Reusable
envelope/framing client code may eventually belong in the Ronin SDK, but the
running backend and its SLSsteam domain logic remain owned by this package.

The backend must remain available for settings, diagnostics and the
package-owned view even when the restart-applied Steam hooks are disabled.
Ronin 3.0 declares it as an `installed` management component independently of
the `enabled` Steam-hook component. A connected `slssteam-control` therefore
must not make disabled Steam-hook functionality appear enabled or running.
Tsuki now reports the resident management plane separately from functional
hook state and derives each declared feature's availability from live hook
evidence.

## Ronin 3 operation and feature surfaces

The package publishes producer-owned
`manifest-pack.inspect/install/remove/status` exports and the
`manifest-pack.changed` event for consumers such as LuaTools. Install and
remove are confirmed Ronin operations: Tsuki verifies the canonical plan
digest and current user gesture, issues an operation- and app-scoped opaque
Steam-target grant, retains the terminal result, and revokes the grant when the
operation ends. The control backend atomically commits the pin set and reads it
back for verification; Tsuki then schedules Steam validation before finalizing
the operation and publishing its change event. If that concrete effect cannot
be scheduled, the backend restores its pre-operation snapshot and no change
event is published. Consumers must use this API rather than write SLSsteam's
private pins, configuration, manifest store or `stplug-in` files.

`feature.status` and `feature.changed` use the same live hook readiness record
as `health.evidence.get`; Tsuki also aggregates the declared evidence into its
generic module feature state and bounded change replay. There is no separate
package-local feature-state authority.

Live acceptance on 2026-08-14 used the deployed package and Tsuki's injected
RPC bridge, not the standalone control protocol. For app 2723430, a confirmed
operation moved depot 2723431 from GID 4105671086490885582 to
6405376336623177784. Steam downloaded 288,005,168 bytes, staged 1,123,934,467
bytes and committed the target GID. A second confirmed operation returned the
depot to GID 4105671086490885582; Steam downloaded 244,247,888 bytes, staged
1,125,660,475 bytes and completed without an update error. The original
SLSsteam configuration was byte-identical after restoration, and the app
manifest again named the original build and mounted depot GIDs.

## SteamStub handling

Ronin handles SteamStub through Steam's existing ownership-ticket IPC. It does
not unpack or rewrite game executables and does not launch Wine, Proton, or an
external DRM helper.

`IClientUser::GetAppOwnershipTicketExtendedData` is already part of SLSsteam's
Linux hook surface. A successful genuine Steam response is returned unchanged.
When that lookup fails for an explicitly managed AdditionalApp, Ronin reuses
the current user's locally cached AppID-7 ownership ticket and supplies the
requested AppID through SteamDRMP's off-by-four ticket parsing behavior. An
unmanaged application can never enter this path.

The pure ticket constructor fails closed for a missing/short source ticket or
an undersized destination buffer. Its regression test verifies the physical
buffer, reported size, offsets, AppID insertion, signature preservation, and
failure cases. Live acceptance on 2026-07-28 launched AppID 250180 (METAL SLUG
3, SteamStub Variant 2.1) through Proton from the original, byte-identical
executable. The game was playable; observed resolution behavior was
Proton-specific and outside Ronin's SteamStub contract.

## Steam-update compatibility boundary

Ronin hooks private 32-bit Steam implementation details. These are not stable
APIs. Optional patterns deliberately turn a signature miss into a localized,
logged feature degradation rather than aborting Ronin's complete load.
`Pattern_t::optional`, `Patterns::init()`, and `OptionalPatternSetup` in
`src/patterns.{hpp,cpp}` define this contract and contain the canonical
feature-to-pattern inventory.

After a Steam update, an unresolved optional pattern must be restored as
follows:

1. Use the Ronin log to identify every unresolved optional pattern and the
   affected feature. Do not assume only one signature changed.
2. Locate the same semantic function in the updated 32-bit
   `steamclient.so` from its behavior, callers, constants, and data flow.
   Searching for nearby bytes alone is not adequate.
3. Update the signature while wildcarding volatile addresses, offsets, and
   displacements. Do not wildcard stable structure merely to force a match.
4. Prove the signature matches exactly once in an executable segment and
   resolves to the intended function or instruction site.
5. Revalidate the hook's calling convention, prologue/trampoline assumptions,
   argument meanings, object offsets, container layout, and ownership rules.
   If any changed, update the adapter—not just the signature.
6. Rebuild from a clean tree, run the owning subsystem's focused regression
   tests, and perform its controlled live acceptance test against that exact
   Steam build.
7. Record the tested Steam build identity and result before declaring the
   feature supported again.

The optional inventory currently covers:

- Ownership/package refresh: `CUser::NotifyLicensesUpdated`,
  `CPackageInfoCache::LoadPackage`, and `CUtlMemory::Grow`.
- Manifest installation and reconciliation:
  `CDepotDownloadMgr::ProcessDepotManifest`, `PrepareDepotDownload`,
  `BuildDepotDependency`, and `EvaluateConfigChanges`.
- Parental override: `ParentalSettingsReceived` and
  `ParentalSignatureCheck`.

Some patterns cooperate. In particular, resolving only one part of the
manifest acquisition/planner chain is not proof that pinning works and may be
unsafe. The end-to-end manifest-pinning acceptance test is authoritative.

## Retained behavior

1. Configuration safety and added-app discovery
   - `stplug-in/*.lua` filename discovery
   - `luaappids.yaml`
   - installed compatibility-entry classification
   - malformed YAML repair and non-throwing scalar conversion
   - repeated atomic-save and source-directory watching
   - runtime added/removed-app reconciliation
   - durable first-discovery timestamps for Steam's native Date Added sorting;
     explicit `SubscriptionTimestamps` remain authoritative
2. Added-app product data
   - app-info provisioning and reconstruction
   - native CM/PICS client with bounded fallbacks
3. Depot and manifest layer
   - depot keys, manifests, staging, persistence, synthesis, and pinning
   - offline/provider circuit breaker
4. User-facing retained features
   - added-app achievements and player stats
   - compatibility-tool selection
   - parental-restriction override
   - CEF port publication contract
5. Regression inventory for all retained behavior

## Deliberately excluded

- Steam launcher wrappers and crash-loop guardian
- desktop files, autostart, and systemd units
- Tsuki/Lumen/CloudRedirect launching or branding
- release updater and combined-stack packaging
- special module control protocols

## Port status

- [x] Establish current AceSLS upstream as the base.
- [x] Import pure discovery and malformed-config decision layers with tests.
- [x] Integrate discovery with upstream `CConfig`.
- [x] Make `CFileWatcher` support files, directories, and atomic replacement.
- [x] Add runtime added-app reconciliation.
- [x] Port product-info provisioning and CM/PICS transport.
- [x] Port depot/manifest/offline layers.
- [x] Port achievements/player stats.
- [x] Port compatibility-tool behavior.
- [x] Port parental restrictions.
- [x] Port the CEF port publication contract.
- [x] Move the canonical Tsuki module manifest, settings, communication
      declarations, assets, and defaults into `module/`.
- [x] Add one build/package target that produces the native payload and
      complete self-contained Tsuki module from the same revision.
- [x] Add a package-owned Ronin control companion and hosted Manifest Pins
      page; Tsuki contributes only generic component/view hosting.
- [x] Replace the manually maintained Tsuki compatibility copy with an explicit
      validated package deployment operation and one rollback revision.
- [x] Run isolated tests plus a controlled Tsuki/Steam A/B validation.

The retained native feature transplant was completed and compiled on
2026-07-26. Its offline regression inventory passes. Controlled live acceptance
on 2026-07-27 exercised a pinned downgrade and return to the public build with
byte-count and mounted-GID evidence, persistence across restart, and restoration
of the original user state. A fresh post-upstream-integration acceptance run on
2026-08-14 validated the Ronin 3 package, reached the Steam UI, attached both
Tsuki contexts, connected the package control companion, resolved the current
Steam patterns, exercised ReconcilePin, PackagePatch and parental rewriting,
and completed a two-minute SLS-only soak without a crash or failed hook. The
canonical package lives in this repository. Tsuki's `modules/slsteam/` tree is
a deployed compatibility copy until external package discovery or installation
replaces that staging step.
