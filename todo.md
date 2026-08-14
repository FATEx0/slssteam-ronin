# Ronin 3.0 migration follow-up

Validated against the uncommitted `ronin/main` working tree and the local
`ronin-module-sdk` Ronin 3.0 contract on 2026-07-29. The package validates as
`slsteam@1.0.0-beta.1`.

## Open host-gated work

- [ ] **Publish the producer-owned `manifest-pack.*` interface.**
  `manifest-pack.inspect/install/remove/status` and `manifest-pack.changed`
  are required before LuaTools can consume SLSsteam without touching its
  private files. This is intentionally deferred until Tsuki implements the
  Ronin 3 transaction/grant boundary needed for concrete confirmation,
  staging, verification and rollback. The gate is recorded in
  `docs/RONIN.md`; direct LuaTools writes are not an interim API.

- [ ] **Standardize feature transition delivery in Tsuki.**
  The package currently exposes positive state through
  `health.evidence.get`. Tsuki now validates the companion response schema,
  byte bound, freshness and `/proc` process-instance identity and folds the
  result into module readiness/health. The remaining gap is the canonical
  `feature.status` query and `feature.changed` notification surface. Do not
  add an independent module event whose state can disagree with that host
  aggregation. Once the canonical query/event contract lands, declare and
  test it here.

## Resolved during validation

- [x] **Enforce bounded integer and enum settings.**
  `settings.set` rejects `LogLevel` outside 0-6, `MaxSchemaTries` outside
  0-1000, negative/overflowing `FakeWalletBalance`, and invalid uint64 values.
  The original `LogLevel: 7` reproduction now returns `err` with
  `"setting integer is outside its declared range"`.

- [x] **Run the control integration test through Make.**
  `make test-slssteam-control` builds `bin/slssteam-control` and runs
  `tools/test_slssteam_control.py` through `uv`, covering framed transport,
  revision conflicts, range rejection, settings preservation, history import
  and positive locked readiness evidence.

- [x] **Describe the legacy API transport truthfully.**
  The `API` setting is now labelled “Legacy command file” and explicitly names
  `/tmp/SLSsteam.API`; it no longer claims to control a socket.

## Verified migration properties

- [x] `slssteam-control` is `activation: "installed"` while `steam-hooks` is
  `activation: "enabled"`.
- [x] `slssteam-control` is the revisioned sole settings writer and
  `steam-hooks` is an explicit read-only consumer.
- [x] SteamDB history import declares bounded, user-mediated provenance.
- [x] Hook readiness uses bounded companion-export evidence rather than
  mapped-library state alone.
- [x] Broad legacy filesystem/network authority is visible through migration
  profiles with removal milestones.
- [x] Logs use `$MODULE_LOG`, and the `slssteam-control` rename is consistent
  across source, package metadata, documentation and provenance.
- [x] Tsuki admits the package as Ronin 3.0, keeps the installed control
  component resident while hooks are disabled, routes revisioned settings
  through `settings.get`/`settings.set`, supplies fixed runtime bindings, and
  validates positive hook evidence.
