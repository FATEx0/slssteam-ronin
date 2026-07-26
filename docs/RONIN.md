# SLSsteam Ronin

Ronin is an upstream-first SLSsteam variant for Tsuki. Its base is
`AceSLS/SLSsteam`; `slsteam-moon` is a behavioral reference, not the base
branch.

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
- [ ] Run isolated tests plus a controlled Tsuki/Steam A/B validation.
