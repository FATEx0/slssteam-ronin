# Ronin SteamDB history collector

This unpacked browser extension reads build IDs and manifest transitions that
SteamDB has already rendered in the user's ordinary browser session. It does
not bypass Cloudflare, copy cookies, call a private API, or run in the
background. Collection begins only when the user presses **Export history**.

Load this directory as an unpacked extension, open
`https://steamdb.info/app/APPID/patchnotes/`, wait for **Builds** to load, and
press the extension button. Import the downloaded JSON from SLSsteam Ronin's
Manifest Pins page.

The JSON is an observation, not a trusted manifest map. `ronin-control`
combines it with the current local Steam snapshot, verifies every
`old -> new` transition while walking backwards, and rejects the entire import
if any link does not join. The cache contains only the history SteamDB made
available to the browser at collection time.

Every exported depot row also preserves SteamDB's Size and DL columns
independently from manifest reconstruction. The `size` and `dl` objects contain
the raw decimal byte count from `data-sort` and the displayed value;
unavailable values are `null` rather than inferred.

All depot tables rendered in the app's Depots pane are captured, not only the
primary table. Each row records its original section heading, a normalized
category (`content`, `dlc`, `shared`, or `redistributable` where directly
observable), owner AppID, descriptor, full configuration text, and Size/DL
values. These are raw observations retained for later database normalization.

Repeated `(depot_id, owner_appid)` rows are emitted once with every source row
retained under `appearances`; a directly identified `dlc` category wins over
the duplicate “Inner depots from DLC” presentation. For shared and
redistributable depots, the helper also visits the bounded per-depot Manifests
page and records every rendered manifest ID and seen timestamp. Patch pages
retain raw per-depot event text in addition to recognized manifest transitions.
