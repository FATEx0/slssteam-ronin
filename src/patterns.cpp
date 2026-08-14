#include "patterns.hpp"

#include "globals.hpp"
#include "log.hpp"
#include "memhlp.hpp"

#include "libmem/libmem.h"


Pattern_t::Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, lm_module_t* module)
	:
	Pattern_t(name, pattern, followMode, std::vector<int16_t>(), module)
{
}

Pattern_t::Pattern_t(const char* name, const char* pattern, MemHlp::SigFollowMode followMode, std::vector<int16_t> prologue, lm_module_t* module)
	:
	name(name),
	pattern(pattern),
	followMode(followMode),
	prologue(prologue),
	module(module)
{
	Patterns::patterns.emplace_back(this);
}

bool Pattern_t::find()
{
	address = MemHlp::searchSignature(name.c_str(), pattern.c_str(), module ? *module : g_modSteamClient , followMode, &prologue[0], prologue.size());
	return address != LM_ADDRESS_BAD;
}

bool Patterns::init()
{
	bool found = true;

	for(auto& pattern : patterns)
	{
		if (!pattern->find())
		{
			if (!pattern->optional)
				found = false;
			else
				g_pLog->warn(
				    "Optional Steam pattern %s did not resolve; "
				    "its dependent Ronin feature is disabled for this session\n",
				    pattern->name.c_str());
		}
	}

	return found;
}

using SigFollowMode = MemHlp::SigFollowMode;

namespace Patterns
{
	Pattern_t ParentalSignatureCheck
	{
		"ParentalSignatureCheck",
		"84 C0 75 27 8B 85 ? ? ? ? 8D 9D ? ? ? ? 83 EC 04 FF B0 82 01 00 00 8D 86 ? ? ? ? 50 53 E8 ? ? ? ?",
		SigFollowMode::None
	};
	Pattern_t ParentalSettingsReceived
	{
		"ParentalSettingsReceived",
		"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 00 02 00 00 8B 45 08 8B 55 1C 8B 7D 0C",
		SigFollowMode::None
	};

	Pattern_t TraceIPC
	{
		"TraceIPC",
		"0F 45 F8 85 ED",
		SigFollowMode::PrologueUpwards,
		std::vector<int16_t> { 0x53, 0x56, 0x57, 0x55 }
	};

	namespace CAPIJob
	{
		Pattern_t SendAndRecv
		{
			"CAPIJob::SendAndRecv",
			"8B 7C 24 ? FF 76 ? FF 76",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x53, 0x56, 0x57, 0x55 }
		};
	}

	namespace CAppDataCache
	{
		Pattern_t BParseResponseMessage
		{
			"CAppDataCache::BParseResponseMessage",
			"8B 77 ? 39 46",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x53, 0x56, 0x57, 0xE5, 0x89, 0x55, -1, -1, -1, -1, 05, -1, -1, -1, -1, 0xE8 }
		};
	}

	namespace CWebSocketConnection
	{
		Pattern_t BBuildAndAsyncSendFrame
		{
			"CWebSocketConnection::BBuildAndAsyncSendFrame",
			"89 C6 85 D2 78",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0xE8, 0x57, 0xE5, 0x89, 0x55 }
		};
	}

	namespace CSteamEngine
	{
		Pattern_t SetAppIdForCurrentPipe
		{
			"CSteamEngine::SetAppIdForCurrentPipe",
			"E8 ? ? ? ? 83 C4 ? 8B 45 ? 85 C0 75 ? 31 FF",
			SigFollowMode::Relative
		};
		Pattern_t RunInterface
		{
			"CSteamEngine::RunInterface",
			"8B 5A ? 29 C3",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x56, 0x57, 0xE5, 0x89, 0x55 }
		};
		Pattern_t Offset_ClientUtils
		{
			"CSteamEngine::m_ClientUtils",
			"89 86 ? ? ? ? 8D 86 ? ? ? ? 89 44 24 ? 50 E8 ? ? ? ? 83 C4",
			SigFollowMode::None
		};
		Pattern_t Offset_User
		{
			"CSteamEngine::m_pUser",
			"8B 80 ? ? ? ? FF 75 ? ? ? ? 56 FF 75",
			SigFollowMode::None
		};
	}

	namespace CUser
	{
		Pattern_t CheckAppOwnership
		{
			"CUser::CheckAppOwnership",
			"0F 94 C2 08 51",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x53, 0x56, 0x57, 0xE5, 0x89, 0x55, -1, -1, -1, -1, 0x5, -1, -1, -1, -1, 0xE8 }
		};
		Pattern_t GetSubscribedApps
		{
			"CUser::GetSubscribedApps",
			"E8 ? ? ? ? 89 C6 83 C4 ? 85 C0 0F 84 ? ? ? ? 8B 9D ? ? ? ? 39 D8",
			SigFollowMode::Relative
		};
		Pattern_t PostCallback
		{
			"CUser::PostCallback",
			"E8 ? ? ? ? 8B 75 ? 89 D8",
			SigFollowMode::Relative
		};
		Pattern_t UpdateAppOwnershipTicket
		{
			"CUser::UpdateAppOwnershipTicket",
			"52 57 89 DF FF 75",
			SigFollowMode::PrologueUpwards,
			std::vector<int16_t> { 0x53, 0x56, 0x57, 0xE5, 0x89, 0x55, -1, -1, -1, -1, 0x5, -1, -1, -1, -1, 0xE8 }
		};
		Pattern_t m_OffsetClientUser
		{
			"CUser::m_ClientUser",
			"2D ? ? ? ? C7 44 24 ? ? ? ? ? 81 E1",
			SigFollowMode::None
		};
		Pattern_t m_OffsetUserAppInfo
		{
			"CUser::m_UserAppInfo",
			"8D 90 ? ? ? ? 8B 80 ? ? ? ? 6A ? 8D 4C 24",
			SigFollowMode::None
		};
		Pattern_t m_OffsetUserAppManager
		{
			"CUser::m_UserAppmanager",
			"8D 90 ? ? ? ? 8B 80 ? ? ? ? 68 ? ? ? ? 56",
			SigFollowMode::None
		};
		Pattern_t NotifyLicensesUpdated
		{
			"CUser::NotifyLicensesUpdated",
			"55 89 E5 57 56 53 E8 ? ? ? ? 81 C3 ? ? ? ? 81 EC ? ? ? ? 8B 45 08 8B B8 ? ? 00 00 89 9D ? ? FF FF 85 FF",
			SigFollowMode::None
		};
	}

	namespace CPackageInfoCache
	{
		Pattern_t LoadPackage
		{
			"CPackageInfoCache::LoadPackage",
			"E8 ? ? ? ? 83 C4 10 84 C0 0F 84 ? ? ? ? 8B 95 ? ? FF FF 8B 7A 18 83 FF FF",
			SigFollowMode::Relative
		};
	}

	namespace CUtlMemory
	{
		Pattern_t Grow
		{
			"CUtlMemory::Grow",
			"E8 ? ? ? ? 8B 85 ? ? FF FF 83 C4 10 8B 40 44 89 85 ? ? FF FF 83 C0 01 E9",
			SigFollowMode::Relative
		};
	}

	namespace CDepotDownloadMgr
	{
		Pattern_t ProcessDepotManifest
		{
			"CDepotDownloadMgr::ProcessDepotManifest",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 83 EC 4C 8B 55 1C 89 45 C0 8B 45 18 89 55 CC 89 45 C8",
			SigFollowMode::None
		};
		Pattern_t PrepareDepotDownload
		{
			"CDepotDownloadMgr::PrepareDepotDownload",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 83 EC 60 8B 7D 08 8B 45 18 8B 55 1C FF 75 20 89 45 98 52 50 FF 75 14 89 55 9C FF 75 10 FF 75 0C 57 E8 ? ? ? ? 8B 47 4C 83 C4 20 83 F8 FF",
			SigFollowMode::None
		};
		Pattern_t BuildDepotDependency
		{
			"CDepotDownloadMgr::BuildDepotDependency",
			"E8 ? ? ? ? 05 ? ? ? ? 55 89 E5 57 56 53 81 EC 8C 04 00 00 8B 55 10 8B 7D 0C 89 85 A0 FB FF FF 8B 45 08",
			SigFollowMode::None
		};
		/*
		 * The install planner builds two sibling vectors with the shared
		 * BuildDepotDependency function. Live Linux tracing on Steam build
		 * 1784778118 proved that the first call (flag 0, ctx+0xb5c) owns the
		 * acquisition target: its GID is the one printed by "Downloading ...
		 * for depot". The second call (flag 1, ctx+0xb4c) owns the
		 * comparison/active-side pass: changing only that vector made Steam
		 * validate public files against the historical manifest while still
		 * downloading the public GID. Manifest pins must therefore alter only
		 * the first call. Globally detouring the shared builder rewrites both
		 * and makes Steam falsely report "active == target" without
		 * downloading historical content.
		 *
		 * This signature starts at the target call itself and includes the
		 * immediate post-call virtual-dispatch guard.  ManifestBind verifies
		 * that its rel32 destination is BuildDepotDependency before changing
		 * the instruction, so a Steam update fails closed rather than
		 * redirecting an unrelated call.
		 */
		Pattern_t BuildDepotTargetCall
		{
			"CDepotDownloadMgr::BuildDepotDependency target-plan call",
			"E8 ? ? ? ? 8B 46 08 8D 9F ? ? ? ? 83 C4 10 89 5D ? "
			"8B 10 8B 52 4C 39 DA 0F 85",
			SigFollowMode::None
		};
		// Structured chunk-completion callbacks. Steam reaches these before it
		// formats the content-log line, while the unpack result is still
		// available. The cdecl and regparm(3) compiler variants are optional
		// and independently guarded.
		Pattern_t OnChunkUnpackedStack
		{
			"CDepotDownloadMgr::OnChunkUnpacked[cdecl]",
			"55 89 E5 57 56 E8 ? ? ? ? 81 C6 ? ? ? ? 53 81 EC 7C 04 "
			"00 00 8B 45 0C 8B 7D 08 89 85 90 FB FF FF 8B 45 10 89 "
			"85 8C FB FF FF",
			SigFollowMode::None
		};
		Pattern_t OnChunkUnpackedReg
		{
			"CDepotDownloadMgr::OnChunkUnpacked[regparm3]",
			"55 89 E5 57 E8 ? ? ? ? 81 C7 ? ? ? ? 56 89 C6 53 81 EC "
			"7C 04 00 00 8B 45 0C 89 95 90 FB FF FF 89 8D 8C FB FF "
			"FF 89 85 94 FB FF FF",
			SigFollowMode::None
		};
		Pattern_t EvaluateConfigChanges
		{
			"CDepotDownloadMgr::EvaluateConfigChanges",
			"55 89 E5 57 56 53 81 EC DC 00 00 00 89 85 50 FF FF FF 8B 45 10 89 85 40 FF FF FF 8B 45 08 8B 40 04",
			SigFollowMode::None
		};
	}

	namespace CUserAppManager
	{
		Pattern_t BuildDepotDependency
		{
			"CUserAppManager::BuildDepotDependency",
			"E8 ? ? ? ? 83 C4 ? 84 C0 74 ? 8B 45 ? 85 C0 89 45",
			SigFollowMode::Relative
		};
	}

	namespace IClientUtils
	{
		Pattern_t Offset_GetPipeIndex
		{
			"IClientUtils::m_PipeIndex",
			"8B 91 ? ? ? ? 83 F8 FF 74 ? 8B 89 ? ? ? ? EB ? ? ? ? 8B 00 83 F8 FF 74 ? 8D 04 ? 8D 04 ? 3B 50",
			SigFollowMode::None,
		};
	}

	std::vector<Pattern_t*> patterns;

	struct OptionalPatternSetup
	{
		OptionalPatternSetup()
		{
			/*
			 * Steam-update maintenance inventory
			 * ----------------------------------
			 * Ownership/package-cache refresh:
			 *   CUser::NotifyLicensesUpdated
			 *     broadcasts the rebuilt license state after package-0
			 *     injection. Missing: warm-cache behavior remains, but a
			 *     cold cache may not learn about injected ownership live.
			 *   CPackageInfoCache::LoadPackage + CUtlMemory::Grow
			 *     inject AdditionalApps into package 0 and grow its vector.
			 *     Either missing: PackagePatch::setup() stays a no-op.
			 *
			 * Manifest pin/install pipeline:
			 *   ProcessDepotManifest + PrepareDepotDownload
			 *     keep the selected gid consistent between acquisition and
			 *     the later per-download lookup. They are a cooperating pair.
			 *   BuildDepotDependency
			 *     is the shared active/target vector builder.
			 *   BuildDepotTargetCall
			 *     identifies only the target-vector invocation. ManifestBind
			 *     redirects this call after verifying its destination is
			 *     BuildDepotDependency; the active-vector invocation must
			 *     remain untouched.
			 *   EvaluateConfigChanges
			 *     prevents the installed pinned gid from being immediately
			 *     classified as an update against the public gid.
			 *     Missing patterns disable only their guarded hook, but the
			 *     complete pinning acceptance test must pass before claiming
			 *     manifest pinning works on a new Steam build.
			 *   OnChunkUnpackedStack + OnChunkUnpackedReg
			 *     observe definitive managed-DLC decryption failures. Either
			 *     ABI variant may resolve; neither resolving disables only
			 *     quarantine learning.
			 *
			 * Parental override:
			 *   ParentalSettingsReceived + ParentalSignatureCheck
			 *     rewrite settings and bypass their corresponding signature
			 *     branch. Either missing disables the override.
			 *
			 * For every update: do not merely loosen bytes until a match is
			 * found. Recover the function by behavior/callers, wildcard only
			 * volatile operands, require one executable-segment match, inspect
			 * its prologue/calling convention/layout, then run the feature's
			 * focused tests and controlled live acceptance test. See
			 * docs/RONIN.md, "Steam-update compatibility boundary".
			 */
			ParentalSignatureCheck.optional = true;
			ParentalSettingsReceived.optional = true;
			CUser::NotifyLicensesUpdated.optional = true;
			CPackageInfoCache::LoadPackage.optional = true;
			CUtlMemory::Grow.optional = true;
			CDepotDownloadMgr::ProcessDepotManifest.optional = true;
			CDepotDownloadMgr::PrepareDepotDownload.optional = true;
			CDepotDownloadMgr::BuildDepotDependency.optional = true;
			CDepotDownloadMgr::BuildDepotTargetCall.optional = true;
			CDepotDownloadMgr::EvaluateConfigChanges.optional = true;
			CDepotDownloadMgr::OnChunkUnpackedStack.optional = true;
			CDepotDownloadMgr::OnChunkUnpackedReg.optional = true;
		}
	} optionalPatternSetup;
}
