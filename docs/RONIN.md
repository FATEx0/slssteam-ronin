# SLSsteam Ronin

Ronin is an upstream-first SLSsteam variant for Tsuki. Its base is
`AceSLS/SLSsteam`; `slsteam-moon` is a behavioral reference, not the base
branch.

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
│   ├── settings.json          # when the split module format is finalized
│   ├── communication.json     # when the split module format is finalized
│   ├── assets/
│   │   ├── icon.*
│   │   └── banner.*
│   ├── config/
│   │   └── config.yaml
│   └── payload/               # populated by the module packaging target
│       ├── SLSsteam.so
│       └── library-inject.so
├── scripts/
│   └── package-tsuki-module.sh
├── build/                     # ignored, intermediate compiler output
└── dist/                      # ignored, complete installable module archives
```

The native build and module packaging must be one pipeline: compile and test
the exact core revision, stage its artifacts into `module/payload`, validate
the module specification, and archive that same staged tree. A release must
never combine a manifest from one revision with a binary from another.

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

## Retained behavior

1. Configuration safety and added-app discovery
   - `stplug-in/*.lua` filename discovery
   - `luaappids.yaml`
   - installed compatibility-entry classification
   - malformed YAML repair and non-throwing scalar conversion
   - repeated atomic-save and source-directory watching
   - runtime added/removed-app reconciliation
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
- desktop files, autostart, systemd units, and Steamless installation
- Tsuki/Lumen/CloudRedirect launching or branding
- release updater and combined-stack packaging
- special module control protocols

## Port status

- [x] Establish current AceSLS upstream as the base.
- [x] Import pure discovery and malformed-config decision layers with tests.
- [x] Integrate discovery with upstream `CConfig`.
- [x] Make `CFileWatcher` support files, directories, and atomic replacement.
- [ ] Add runtime added-app reconciliation.
- [ ] Port product-info provisioning and CM/PICS transport.
- [ ] Port depot/manifest/offline layers.
- [ ] Port achievements/player stats.
- [ ] Port compatibility-tool behavior.
- [ ] Port parental restrictions.
- [ ] Port the CEF port publication contract.
- [ ] Move the canonical Tsuki module manifest, settings, communication
      declarations, assets, and defaults into `module/`.
- [ ] Add one build/test/package target that produces the native payload and
      complete self-contained Tsuki module from the same revision.
- [ ] Remove the final SLSsteam-specific manifest copy from Tsuki after generic
      external package discovery can load Ronin directly.
- [ ] Run isolated tests plus a controlled Tsuki/Steam A/B validation.
