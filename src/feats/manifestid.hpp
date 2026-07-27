// SPDX-License-Identifier: AGPL-3.0-only
//
// Legacy Lua setManifestid catalog.
//
// Background: a Lua script in `<Steam>/config/stplug-in/` may carry
//
//     setManifestid(<depotId>, "<gid>")
//
// alongside `addappid(...)`.
//
// This feature ingests `setManifestid` calls into a depot-id keyed
// compatibility catalog. It does NOT rewrite PICS buffers and does NOT choose
// the install plan's manifest GID. That old behavior failed Steam's product-
// info integrity check and was deliberately removed; see pics.cpp.
//
// Today the catalog is consulted only when deciding whether a manifest-code
// request belongs to SLSsteam's managed scope. Actual Ronin build locking is
// driven independently by config.yaml's structured `ManifestPins` block and
// the ManifestBind/ReconcilePin download-planner hooks.
//
// Catalog layout (mirrors DepotKey):
//   <SLSsteam config dir>/cache/manifestid_<depotId>.yaml
//     ---
//     depotId: <uint32>
//     gid: "<numeric-string>"
//
// Application points:
//   - Importer runs at startup alongside DepotKey::importLuaScripts.
//   - ManifestCode treats catalogued depots as managed request-code/fetch
//     scope. No product-info or install-plan mutation occurs here.

#pragma once

#include <cstdint>
#include <string>

namespace ManifestId
{
	// Disk paths.
	std::string getCatalogDir();
	std::string getCatalogPath(uint32_t depotId);

	// Returns the catalogued legacy GID for `depotId`, or empty if none.
	// Presence currently affects request scope only; this does not select an
	// install manifest.
	std::string getPinnedGid(uint32_t depotId);

	// Persist a (depotId, gid) pair to the catalog.  Idempotent.
	bool savePin(uint32_t depotId, const std::string& gid);

	// Importer: scans `<Steam>/config/stplug-in/*.lua`, extracts every
	// `setManifestid(<depotId>, "<gid>")` and ingests into the catalog.
	void importLuaScripts();
}
