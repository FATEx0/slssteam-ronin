#include "../src/feats/steamstub_ticket.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>

namespace
{
	int failures = 0;

	void check(const char* name, bool result)
	{
		if (result)
			std::cout << "ok   " << name << '\n';
		else
		{
			std::cout << "FAIL " << name << '\n';
			++failures;
		}
	}
}

int main()
{
	std::string source(184, '\0');
	for (size_t index = 0; index < source.size(); ++index)
		source[index] = static_cast<char>(index);

	constexpr uint32_t appId = 250180;
	SteamStubTicket::ForgedTicket forged;
	check(
	    "valid source ticket is forged",
	    SteamStubTicket::forge(source, appId, source.size() + 4, forged));
	check(
	    "physical ticket grows by four bytes",
	    forged.bytes.size() == source.size() + 4);
	check(
	    "reported ticket size remains original size",
	    forged.reportedSize == source.size());
	check(
	    "SteamID offset remains unchanged",
	    forged.steamIdOffset == 8);
	check(
	    "AppID is inserted before the signature",
	    forged.appIdOffset == source.size() - SteamStubTicket::kSignatureSize);
	check(
	    "signature offset follows inserted AppID",
	    forged.signatureOffset == forged.appIdOffset + 4);
	check(
	    "prefix is preserved",
	    std::equal(
	        source.begin(),
	        source.begin() + forged.appIdOffset,
	        forged.bytes.begin()));
	check(
	    "AppID bytes are little-endian host bytes",
	    std::memcmp(
	        forged.bytes.data() + forged.appIdOffset,
	        &appId,
	        sizeof(appId)) == 0);
	check(
	    "signature is preserved",
	    std::memcmp(
	        source.data() + source.size() - SteamStubTicket::kSignatureSize,
	        forged.bytes.data() + forged.signatureOffset,
	        SteamStubTicket::kSignatureSize) == 0);

	SteamStubTicket::ForgedTicket rejected;
	check(
	    "short source fails closed",
	    !SteamStubTicket::forge(
	        std::string(SteamStubTicket::kSignatureSize, '\0'),
	        appId,
	        4096,
	        rejected));
	check(
	    "small destination fails closed",
	    !SteamStubTicket::forge(
	        source,
	        appId,
	        source.size() + 3,
	        rejected));
	check(
	    "failed forge leaves no output",
	    rejected.bytes.empty() && rejected.reportedSize == 0);

	if (failures)
	{
		std::cerr << failures << " SteamStub ticket test(s) failed\n";
		return 1;
	}
	std::cout << "all SteamStub ticket tests passed\n";
	return 0;
}
