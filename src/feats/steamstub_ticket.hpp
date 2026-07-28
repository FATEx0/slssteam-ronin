#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace SteamStubTicket
{
	constexpr size_t kSignatureSize = 128;

	struct ForgedTicket
	{
		std::vector<uint8_t> bytes;
		uint32_t reportedSize = 0;
		uint32_t appIdOffset = 0;
		uint32_t steamIdOffset = 8;
		uint32_t signatureOffset = 0;
		uint32_t signatureSize = kSignatureSize;
	};

	inline bool forge(
	    const std::string& source,
	    uint32_t appId,
	    size_t outputCapacity,
	    ForgedTicket& output)
	{
		output = {};
		if (source.size() <= kSignatureSize)
			return false;
		if (source.size() > UINT32_MAX)
			return false;
		if (source.size() + sizeof(appId) > outputCapacity)
			return false;

		const size_t signatureOffset = source.size() - kSignatureSize;
		output.bytes.reserve(source.size() + sizeof(appId));
		output.bytes.insert(
		    output.bytes.end(), source.begin(), source.begin() + signatureOffset);

		const auto* appIdBytes =
		    reinterpret_cast<const uint8_t*>(&appId);
		output.bytes.insert(
		    output.bytes.end(), appIdBytes, appIdBytes + sizeof(appId));
		output.bytes.insert(
		    output.bytes.end(), source.begin() + signatureOffset, source.end());

		// SteamDRMP trusts the reported size and offsets rather than the
		// physical buffer length. Keeping the original size while inserting
		// the requested AppID before the signature is the off-by-four behavior
		// used by OpenSteamTool's SteamStub-only ticket path.
		output.reportedSize = static_cast<uint32_t>(source.size());
		output.appIdOffset =
		    static_cast<uint32_t>(signatureOffset);
		output.signatureOffset =
		    output.appIdOffset + sizeof(appId);
		return true;
	}
}
