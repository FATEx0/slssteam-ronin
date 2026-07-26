#pragma once

#include "memhlp.hpp"

#include "libmem/libmem.h"

#include <string>
#include <vector>


struct Pattern_t
{
public:
	const std::string name;
	const std::string pattern;
	const MemHlp::SigFollowMode followMode;
	std::vector<uint8_t> prologue;

	/*
	 * Steam-internal signatures are a compatibility boundary, not an ABI.
	 *
	 * A required pattern failing makes Patterns::init() reject the complete
	 * load. An optional pattern failing must instead leave address set to
	 * LM_ADDRESS_BAD; its owning feature must check that value and become a
	 * safe no-op. This lets an unrelated Steam update degrade one Ronin
	 * feature without taking down every SLSsteam feature.
	 *
	 * "optional" does not mean "unimportant" or "automatically compatible".
	 * After a Steam update, restore a failed feature by locating the same
	 * semantic function in the updated 32-bit steamclient.so, updating the
	 * signature, proving it has exactly one match, and revalidating the
	 * calling convention and every object/data-layout assumption made by the
	 * hook. A byte-pattern update alone is not sufficient proof.
	 *
	 * Keep the feature-to-pattern inventory beside OptionalPatternSetup in
	 * patterns.cpp and the full procedure in docs/RONIN.md synchronized.
	 */
	bool optional = false;

	lm_address_t address;
	lm_module_t* module;

	Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, lm_module_t* module = nullptr);
	Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, std::vector<uint8_t> prologue, lm_module_t* module = nullptr);
	//~CPattern();

	bool find();
};

namespace Patterns
{
	extern Pattern_t ParentalSignatureCheck;
	extern Pattern_t ParentalSettingsReceived;
	extern Pattern_t TraceIPC;

	namespace CAPIJob
	{
		extern Pattern_t SendAndRecv;
	}

	namespace CAppDataCache
	{
		extern Pattern_t BParseResponseMessage;
	}

	namespace CSteamEngine
	{
		extern Pattern_t SetAppIdForCurrentPipe;
		extern Pattern_t RunInterface;

		extern Pattern_t Offset_User;
	}

	namespace CWebSocketConnection
	{
		extern Pattern_t BBuildAndAsyncSendFrame;
	}

	namespace CUser
	{
		//TODO: Order & Convert old patterns
		extern Pattern_t CheckAppOwnership;
		extern Pattern_t GetSubscribedApps;
		extern Pattern_t PostCallback;
		extern Pattern_t UpdateAppOwnershipTicket;
		extern Pattern_t NotifyLicensesUpdated;
	}

	namespace CPackageInfoCache
	{
		extern Pattern_t LoadPackage;
	}

	namespace CUtlMemory
	{
		extern Pattern_t Grow;
	}

	namespace CDepotDownloadMgr
	{
		extern Pattern_t ProcessDepotManifest;
		extern Pattern_t PrepareDepotDownload;
		extern Pattern_t BuildDepotDependency;
		extern Pattern_t EvaluateConfigChanges;
	}

	namespace CUserAppManager
	{
		extern Pattern_t BuildDepotDependency;
	}

	namespace IClientAppManager
	{
		extern Pattern_t RunIPCFrame;
	}

	namespace IClientApps
	{
		extern Pattern_t RunIPCFrame;
	}

	namespace IClientRemoteStorage
	{
		extern Pattern_t RunIPCFrame;
	}

	namespace IClientUser
	{
		extern Pattern_t GetSteamId;
		extern Pattern_t RunIPCFrame;
	}

	namespace IClientUtils
	{
		extern Pattern_t RunIPCFrame;
		extern Pattern_t Offset_GetPipeIndex;
	}


	//steamui.so
	namespace ISteamMatchmakingPingResponse
	{
		extern Pattern_t ServerResponded;
	}

	extern std::vector<Pattern_t*> patterns;
	bool init();
}
