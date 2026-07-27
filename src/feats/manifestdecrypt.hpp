// SPDX-License-Identifier: AGPL-3.0-only
//
// Normalize a raw Steam CDN depot manifest for depotcache. CDN manifests may
// carry base64/AES-encrypted filenames; Steam's own CDN client decrypts and
// reserializes them with the depot key before treating them as a local cache
// manifest. Publishing the raw CDN blob makes the install planner unable to
// map active files to the historical target.

#pragma once

#include <string>

namespace ManifestDecrypt
{
	// `key` is the raw 32-byte depot key. Returns a byte-identical copy when
	// filenames are already clear. On encrypted input, rewrites filenames,
	// filename hashes, metadata.filenames_encrypted and crc_clear. Any malformed
	// protobuf, bad key/padding, or unsupported wire type fails closed.
	bool normalizeForDepotcache(const std::string& input,
	                            const std::string& key,
	                            std::string& output,
	                            std::string& error);
}
