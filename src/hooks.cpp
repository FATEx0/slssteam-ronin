#include "hooks.hpp"

#include "api.hpp"
#include "config.hpp"
#include "decompiler.hpp"
#include "globals.hpp"
#include "log.hpp"
#include "memhlp.hpp"
#include "patterns.hpp"
#include "vftableinfo.hpp"

#include "sdk/CAPIJob.hpp"
#include "sdk/CClientUnifiedServiceTransport.hpp"
#include "sdk/CNetPacket.hpp"
#include "sdk/CProtoBufMsgBase.hpp"
#include "sdk/CSteamEngine.hpp"
#include "sdk/CSteamMatchmakingServers.hpp"
#include "sdk/CUser.hpp"
#include "sdk/CUtl.hpp"
#include "sdk/IClientAppManager.hpp"
#include "sdk/IClientApps.hpp"
#include "sdk/IClientUser.hpp"
#include "sdk/IClientUtils.hpp"
#include "sdk/steam.hpp"

#include "feats/achievements.hpp"
#include "feats/apps.hpp"
#include "feats/depotkey.hpp"
#include "feats/depotquarantine.hpp"
#include "feats/dlc.hpp"
#include "feats/misc.hpp"
#include "feats/manifestcode.hpp"
#include "feats/manifestbind.hpp"
#include "feats/packagepatch.hpp"
#include "feats/parental.hpp"
#include "feats/pics.hpp"
#include "feats/reconcilepin.hpp"
#include "feats/fakeappid.hpp"
#include "feats/ticket.hpp"

#include "libmem/libmem.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include <vector>


template<typename T>
Hook<T>::Hook(const char* name)
{
	this->name = std::string(name);
}

template<typename T>
DetourHook<T>::DetourHook(const char* name) : Hook<T>::Hook(name)
{
	this->size = 0;
}

//TODO: Fix this ungodly mess
template<typename T>
DetourHook<T>::DetourHook() : DetourHook<T>("")
{

}

template<typename T>
VFTHook<T>::VFTHook(const char* name) : Hook<T>::Hook(name)
{
	this->hooked = false;
}

template<typename T>
VFTHook<T>::VFTHook() : VFTHook<T>("")
{

}

template<typename T>
bool DetourHook<T>::setup(const char* name, const lm_address_t fn, T hookFn)
{
	this->name = name;
	this->originalFn.address = fn;
	this->hookFn.fn = hookFn;

	return true;
}

template<typename T>
bool DetourHook<T>::setup(const Pattern_t& pattern, T hookFn)
{
	this->name = pattern.name;
	this->originalFn.address = pattern.address;
	this->hookFn.fn = hookFn;

	return true;
}

template<typename T>
bool DetourHook<T>::setup(const VFTableInfo_t& info, T hookFn)
{
	this->name = info.getPrintName();
	this->originalFn.address = info.address;
	this->hookFn.fn = hookFn;

	return true;
}

template<typename T>
void DetourHook<T>::place()
{
	this->size = LM_HookCode(this->originalFn.address, this->hookFn.address, &this->tramp.address);
	MemHlp::fixPICThunkCall(this->name.c_str(), this->originalFn.address, this->tramp.address);

	g_pLog->debug
	(
		"Detour hooked %s (%p) with hook at %p and tramp at %p\n",
		this->name.c_str(),
		this->originalFn.address,
		this->hookFn.address,
		this->tramp.address
	);
}

template<typename T>
void DetourHook<T>::remove()
{
	if (!this->size)
	{
		return;
	}

	LM_UnhookCode(this->originalFn.address, this->tramp.address, this->size);
	this->size = 0;

	g_pLog->debug("Unhooked %s\n", this->name.c_str());
}

template<typename T>
void VFTHook<T>::place()
{
	LM_VmtHook(this->vft.get(), this->index, this->hookFn.address);
	this->hooked = true;

	g_pLog->debug
	(
		"VFT hooked %s (%p) with hook at %p\n",
		this->name.c_str(),
		this->originalFn.address,
		this->hookFn.address
	);
}

template<typename T>
void VFTHook<T>::remove()
{
	//No clue how libmem reacts when unhooking a non existent hook
	//so we do this
	if (!this->hooked)
	{
		return;
	}

	LM_VmtUnhook(this->vft.get(), this->index);
	this->hooked = false;

	g_pLog->debug("Unhooked %s!\n", this->name.c_str());
}

template<typename T>
void VFTHook<T>::setup(std::shared_ptr<lm_vmt_t> vft, const VFTableInfo_t& info, T hookFn)
{
	this->vft = vft;
	this->index = info.index;
	this->name = info.getPrintName();

	this->originalFn.address = LM_VmtGetOriginal(this->vft.get(), this->index);
	this->hookFn.fn = hookFn;
}

__attribute__((hot))
static void hkTraceIPC(const char* iface, const char* fn)
{
	Hooks::TraceIPC.tramp.fn(iface, fn);

	if (g_config.extendedLogging.get())
	{
		g_pLog->debug
		(
			"%s(%s, %s)\n",

			Hooks::TraceIPC.name.c_str(),
			iface,
			fn
		);
	}
}

static uint32_t hkAPIJob_SendAndRecv(CAPIJob* pAPIJob, CProtoBufMsgBase* send, uint32_t a2, uint32_t timeOut, CProtoBufMsgBase* recv, EMsg targetType)
{
	uint32_t ret = Achievements::sendAndRecvGetUserStats(pAPIJob, send, timeOut, recv, targetType);

	if (!ret)
	{
		ret = Hooks::CAPIJob_SendAndRecv.tramp.fn(pAPIJob, send, a2, timeOut, recv, targetType);
	}

	g_pLog->debug
	(
		"%s(%p, %s, %u, %u, %s, %u) -> %u\n",

		Hooks::CAPIJob_SendAndRecv.name.c_str(),
		pAPIJob,
		MemHlp::getTypeName(send),
		a2,
		timeOut,
		MemHlp::getTypeName(recv),
		targetType,
		ret
	);

	return ret;
}

static uint32_t hkAppDataCache_BParseResponseFromMessage(void* pAppDataCache, CProtoBufMsgBase* pMsg)
{
	const uint32_t ret = Hooks::CAppDataCache_BParseResponseFromMessage.tramp.fn(pAppDataCache, pMsg);
	g_pLog->debug
	(
		"%s(%p, %p) -> %i\n",
		Patterns::CAppDataCache::BParseResponseMessage.name.c_str(),
		pAppDataCache,
		pMsg,
		ret
	);

	Apps::parseProductInfoFromResponse(pMsg->getBody<CMsgClientPICSProductInfoResponse>());
	PICS::recvProductInfoResponse(pMsg->getBody<CMsgClientPICSProductInfoResponse>());

	return ret;
}

static uint32_t hkClientUnifiedServiceTransport_SendAndRecvMsg(CClientUnifiedServiceTransport* pUnifiedServiceTransport, const char* name, void* send, void* recv, void* arg4)
{
	uint32_t ret = Achievements::sendAndRecvGetPlayerStats
	(
		pUnifiedServiceTransport,
		name,
		reinterpret_cast<CPlayer_GetUserStats_Request*>(send),
		reinterpret_cast<CPlayer_GetUserStats_Response*>(recv)
	);

	if (ret == k_EResultNoResult)
	{
		ret = Hooks::CClientUnifiedServiceMethod_SendAndRecvMsg.tramp.fn(pUnifiedServiceTransport, name, send, recv, arg4);
	}

	Apps::sendAndRecvLastPlayedTimes(name, reinterpret_cast<CPlayer_GetLastPlayedTimes_Response*>(recv));

	g_pLog->debug
	(
		"%s(%p, %s, %s, %s, %p) -> %u\n",

		Hooks::CClientUnifiedServiceMethod_SendAndRecvMsg.name.c_str(),
		pUnifiedServiceTransport,
		name,
		MemHlp::getTypeName(send),
		MemHlp::getTypeName(recv),
		arg4,
		ret
	);

	return ret;
}

static void hkCMInterface_RecvPkt(void* pCMInterface, CNetPacket* pNetPacket)
{
	g_pLog->debug("RecvPkt %s -> %p\n", pNetPacket->getProtoBufTypeName().c_str(), pNetPacket->getType());

	if (pNetPacket->isValid() && pNetPacket->isProtoBuf())
	{
		const uint32_t type = pNetPacket->getProtoBufType();
		const auto header = pNetPacket->deserializeHeader();

		const bool disableFamilyShareLock = g_config.disableFamilyLock.get();

		if (disableFamilyShareLock && type == k_EMsgClientSharedLibraryStopPlaying)
		{
			pNetPacket->clearBody();
			g_pLog->debug("Choked %s\n", pNetPacket->getProtoBufTypeName().c_str());
		}

		if (disableFamilyShareLock && type == k_EMsgServiceMethod && header.target_job_name() == "FamilyGroupsClient.NotifyRunningApps#1")
		{
			pNetPacket->clearBody();
			g_pLog->debug("Choked %s\n", header.target_job_name().c_str());
		}

		Misc::recvMsg(pNetPacket);
		Ticket::recvMsg(pNetPacket);
		DepotKey::recvMsg(pNetPacket);
		ManifestCode::processRecv(pNetPacket);
	}

	Hooks::CCMInterface_RecvPkt.tramp.fn(pCMInterface, pNetPacket);
}

//I don't like forward declerations, but with the current style & hooks layout it's a necessity
static CSteamId hkClientUser_GetSteamId(const CSteamId& steamId);

static uint32_t hkSteamEngine_RunInterface(void* pSteamEngine, CUtlBuffer* pBufIPCCmd, CUtlBuffer* pBufIPCResult)
{
	if (!g_pSteamEngine)
	{
		g_pSteamEngine = reinterpret_cast<CSteamEngine*>(pSteamEngine);
		g_pLog->debug("g_pSteamEngine at %p\n", g_pSteamEngine);
	}

	//We do not initialize with the CSteamEngine because first run CUser is null
	Hooks::placeVFTHooks();

	//While hooking this function to replace the other hooks might seem attractive
	//we do not do so. Many calls straight up bypass the IPC layer and go
	//straight for the original VFT implementations (IClientAppManager comes to mind)
	//Although it's a great spot to quickly test things

	//pBufIPCCmd
	//mem + 0 : 1 = EIPCCmd::RunInterface
	//mem + 1 : 1 = interfaceType
	//mem + 2 : 4 = *(this + 4)
	//mem + 6 : 4 = function Id
	//arguments follow
	//then fencepost?
	const EIPCInterface type = *reinterpret_cast<EIPCInterface*>(pBufIPCCmd->mem.base + 1);
	const uint32_t fnId = *reinterpret_cast<uint32_t*>(pBufIPCCmd->mem.base + 6);
	const bool switchFakeAppIds = FakeAppIds::shouldUseRealAppIdForInterface(type);

	if (switchFakeAppIds)
	{
		FakeAppIds::runIPCFrame(false);
	}

	//g_pLog->debug("In\n%s\n", MemHlp::hexdump(pBufIPCCmd->mem.base, pBufIPCCmd->offset).c_str());

	const uint32_t ret = Hooks::CSteamEngine_RunInterface.tramp.fn(pSteamEngine, pBufIPCCmd, pBufIPCResult);

	if (switchFakeAppIds)
	{
		FakeAppIds::runIPCFrame(true);
	}

	const EIPCExitCode exitCode = *reinterpret_cast<EIPCExitCode*>(pBufIPCResult->mem.base + 0);

	//IClientUser::GetSteamID has been optimized to hell and back
	//So to hook it we need a naked function hook that requires quite the
	//complex logic to get the full steamId. So I made an exception for this function,
	//since it seems to always get called from RunInterface anyway
	//53                                      push    ebx
	//8B 54 24 0C                             mov     edx, [esp+4+arg_4]
	//8B 44 24 08                             mov     eax, [esp+4+arg_0]
	//8B 9A B2 E8 FF FF                       mov     ebx, [edx-174Eh] //SteamId low
	//8B 8A AE E8 FF FF                       mov     ecx, [edx-1752h] //SteamId high
	//89 58 04                                mov     [eax+4], ebx
	//89 08                                   mov     [eax], ecx
	//                                        //Optimally inject here, grab eax, copy into g_currentSteamId
	//5B                                      pop     ebx
	//C2 04 00                                retn    4
	if (type == k_EIPCInterfaceClientUser && exitCode == EIPCExitCode::Success && fnId == 0xD6FC3200)
	{
		//Universe always set, steamId gets filled in after login
		if (!g_currentSteamId.isSet())
		{
			memcpy(&g_currentSteamId, pBufIPCResult->mem.base + 1, sizeof(CSteamId));
		}

		const CSteamId newId = hkClientUser_GetSteamId(g_currentSteamId);
		memcpy(pBufIPCResult->mem.base + 1, &newId, sizeof(newId));
	}

	//pBufIPCResult
	//mem + 0 : 1 = EIPCExitCode
	//return values follow
	//g_pLog->debug("Out\n%s\n", MemHlp::hexdump(pBufIPCResult->mem.base, pBufIPCResult->offset).c_str());

	Apps::runIPCFrame();
	SLSAPI::runIPCFrame();

	if (g_config.extendedLogging.get())
	{
		const auto utils = g_pSteamEngine->getUtils();

		g_pLog->debug
		(
			"%s -> %u with type %p, fn %p for appId %u (%u)\n",

			Hooks::CSteamEngine_RunInterface.name.c_str(),
			ret,
			type,
			fnId,
			FakeAppIds::getRealAppIdForCurrentPipe(),
			utils ? utils->getAppId() : 0
		);
	}

	return ret;
}

static AppId_t hkSteamEngine_SetAppIdForCurrentPipe(void* pSteamEngine, AppId_t appId, bool a2)
{
	FakeAppIds::setAppIdForCurrentPipe(appId);

	const AppId_t ret = Hooks::CSteamEngine_SetAppIdForCurrentPipe.tramp.fn(pSteamEngine, appId, a2);

	g_pLog->debug
	(
		"%s(%p, %u, %i) -> %i\n",

		Hooks::CSteamEngine_SetAppIdForCurrentPipe.name.c_str(),
		pSteamEngine,
		appId,
		a2,
		ret
	);

	return ret;
}

static bool hkWebSocketConnection_BBuildAndAsyncSendFrame(void* pWebSocketConnection, uint32_t type, void* pData, uint32_t dataSize)
{
	if (type == 2)
	{
		CNetPacket packet {};
		packet.body = reinterpret_cast<CNetPacketBody*>(pData);
		packet.size = dataSize;

		g_pLog->debug("SendPkt %s -> %p\n", packet.getProtoBufTypeName().c_str(), packet.getType());

		if (packet.isValid() && packet.isProtoBuf())
		{
			Apps::sendMsg(&packet);
			FakeAppIds::sendMsg(&packet);
			DepotKey::sendMsg(&packet);

			//Do not free ourself since Steam does so. We reuse our CNetPacket buffer
			pData = packet.body;
			dataSize = packet.size;
		}
	}

	return ManifestCode::hkBBuildAndAsyncSendFrame(
	    pWebSocketConnection, static_cast<EWebSocketOpCode>(type),
	    static_cast<uint8_t*>(pData), dataSize);
}

static gameserverdetails_t* hkSteamMatchmakingServers_GetServerDetails(void* pSteamMatchmakingServers, uint32_t handle, uint32_t serverIdx)
{
	gameserverdetails_t* ret = Hooks::CSteamMatchmakingServers_GetServerDetails.tramp.fn(pSteamMatchmakingServers, handle, serverIdx);

	g_pLog->debug
	(
		"%s(%p, %p, %u) -> %p\n",

		Hooks::CSteamMatchmakingServers_GetServerDetails.name.c_str(),
		pSteamMatchmakingServers,
		handle,
		serverIdx,
		ret
	);

	if(ret)
	{
		FakeAppIds::getServerDetails(handle, *ret);
	}

	return ret;
}

static uint32_t hkSteamMatchmakingServers_RequestInternetServerList(void* pSteamMatchmakingServers, AppId_t appId, uint32_t a2, uint32_t a3, uint32_t a4)
{
	const uint32_t fake = FakeAppIds::requestInternetServerList(appId);

	uint32_t handle = Hooks::CSteamMatchmakingServers_RequestInternetServerList.tramp.fn(pSteamMatchmakingServers, fake ? fake : appId, a2, a3, a4);

	g_pLog->debug
	(
		"%s(%p, %u, %p, %p, %p)->%p\n",

		Hooks::CSteamMatchmakingServers_RequestInternetServerList.name.c_str(),
		pSteamMatchmakingServers,
		appId,
		a2,
		a3,
		a4,
		handle
	);

	FakeAppIds::fakeAppIdMapServer[handle] = appId;

	return handle;
}

__attribute__((hot))
static uint32_t hkUser_CheckAppOwnership(void* pClientUser, AppId_t appId, AppOwnershipInfo_t* pOwnershipInfo)
{
	const uint32_t ret = Hooks::CUser_CheckAppOwnership.tramp.fn(pClientUser, appId, pOwnershipInfo);
	PackagePatch::tryReconcileLicenses();

	//Do not log pOwnershipInfo because it gets deleted very quickly, so it's pretty much useless in the logs
	g_pLog->once
	(
		"%s(%p, %u) -> %i\n",

		Hooks::CUser_CheckAppOwnership.name.c_str(),
		pClientUser,
		appId,
		ret
	);

	if (Apps::checkAppOwnership(appId, pOwnershipInfo) || DLC::checkAppOwnership(appId, pOwnershipInfo))
	{
		return true;
	}

	return ret;
}

static uint32_t hkUser_GetSubscribedApps(void* pClientUser, AppId_t* pAppList, uint32_t size, uint8_t a3)
{
	uint32_t count = Hooks::CUser_GetSubscribedApps.tramp.fn(pClientUser, pAppList, size, a3);

	Apps::getSubscribedApps(pAppList, size, count);

	g_pLog->debug
	(
		"%s(%p, %p, %i, %i) -> %i\n",

		Hooks::CUser_GetSubscribedApps.name.c_str(),
		pClientUser,
		pAppList,
		size,
		a3,
		count
	);

	return count;
}

static bool hkUserAppManager_BuildDepotDependency
(
	void* pClientAppManager,
	AppId_t appId,
	void* a2,
	CUtlVector<DepotInfo_t>* depots,
	CUtlVector<DepotInfo_t>* sharedDepots,
	void* a5,
	uint32_t* pBuildId,
	bool* a7
)
{
	const bool success = Hooks::CUserAppManager_BuildDepotDependency.tramp.fn(pClientAppManager, appId, a2, depots, sharedDepots, a5, pBuildId, a7);
	g_pLog->debug("%s(%p, %u) -> %i\n", Hooks::CUserAppManager_BuildDepotDependency.name.c_str(), pClientAppManager, appId, success);

	Apps::buildDepotDependency(appId, depots, sharedDepots);

	return success;
}

static bool hkClientAppManager_BCanRemotePlayTogether(void* pClientAppManager, AppId_t appId)
{
	const bool ret = Hooks::IClientAppManager_BCanRemotePlayTogether.originalFn.fn(pClientAppManager, appId);
	g_pLog->debug
	(
		"%s(%p, %u) -> %u\n",
		Hooks::IClientAppManager_BCanRemotePlayTogether.name.c_str(),
		pClientAppManager,
		appId,
		ret
	);

	return true;
}

static void* hkClientAppManager_LaunchApp(void* pClientAppManager, AppId_t* pAppId, void* a2, void* a3, void* a4)
{
	if (pAppId)
	{
		g_pLog->once
		(
			"%s(%p, %u, %p, %p, %p)\n",

			Hooks::IClientAppManager_LaunchApp.name.c_str(),
			pClientAppManager,
			*pAppId,
			a2,
			a3,
			a4
		);

		FakeAppIds::launchApp(*pAppId);
		Ticket::launchApp(*pAppId);
	}

	//Do not do anything in post! Otherwise App launching will break
	return Hooks::IClientAppManager_LaunchApp.originalFn.fn(pClientAppManager, pAppId, a2, a3, a4);
}

static bool hkClientAppManager_IsAppDlcInstalled(void* pClientAppManager, AppId_t appId, AppId_t dlcId)
{
	const bool ret = Hooks::IClientAppManager_IsAppDlcInstalled.originalFn.fn(pClientAppManager, appId, dlcId);
	g_pLog->once
	(
		"%s(%p, %u, %u) -> %i\n",

		Hooks::IClientAppManager_IsAppDlcInstalled.name.c_str(),
		pClientAppManager,
		appId,
		dlcId,
		ret
	);

	if (DLC::isAppDlcInstalled(dlcId))
	{
		return true;
	}

	return ret;
}

static bool hkClientAppManager_BIsDlcEnabled(void* pClientAppManager, AppId_t appId, AppId_t dlcId, void* a3)
{
	const bool ret = Hooks::IClientAppManager_BIsDlcEnabled.originalFn.fn(pClientAppManager, appId, dlcId, a3);
	g_pLog->once
	(
		"%s(%p, %u, %u, %p) -> %i\n",

		Hooks::IClientAppManager_BIsDlcEnabled.name.c_str(),
		pClientAppManager,
		appId,
		dlcId,
		a3,
		ret
	);

	
	if (DLC::isDlcEnabled(appId))
	{
		return true;
	}

	return ret;
}

static bool hkClientAppManager_GetUpdateInfo(void* pClientAppManager, AppId_t appId, uint32_t* a2)
{
	const bool success = Hooks::IClientAppManager_GetAppUpdateInfo.originalFn.fn(pClientAppManager, appId, a2);
	g_pLog->once("%s(%p, %u, %p) -> %i\n", Hooks::IClientAppManager_GetAppUpdateInfo.name.c_str(), pClientAppManager, appId, a2, success);

	if (Apps::shouldDisableUpdates(appId))
	{
		g_pLog->once("Disabled updates for %u\n", appId);
		return false;
	}

	return success;
}

static unsigned int hkClientApps_GetDLCCount(void* pClientApps, AppId_t appId)
{
	uint32_t count = Hooks::IClientApps_GetDLCCount.originalFn.fn(pClientApps, appId);
	g_pLog->once
	(
		"%s(%p, %u) -> %u\n",

		Hooks::IClientApps_GetDLCCount.name.c_str(),
		pClientApps,
		appId,
		count
	);
	
	const uint32_t override = DLC::getDlcCount(appId);
	if (override)
	{
		return override;
	}

	return count;
}

static bool hkClientApps_GetDLCDataByIndex(void* pClientApps, AppId_t appId, int dlcIndex, AppId_t* pDlcId, bool* pIsAvailable, char* pChDlcName, size_t dlcNameLen)
{
	//Preserve original call to populate stuff
	const bool ret = DLC::getDlcDataByIndex(appId, dlcIndex, pDlcId, pIsAvailable, pChDlcName, dlcNameLen)
		|| Hooks::IClientApps_GetDLCDataByIndex.originalFn.fn(pClientApps, appId, dlcIndex, pDlcId, pIsAvailable, pChDlcName, dlcNameLen);


	g_pLog->once
	(
		"%s(%p, %u, %i, %p, %p, %s, %i) -> %i\n",

		Hooks::IClientApps_GetDLCDataByIndex.name.c_str(),
		pClientApps,
		appId,
		dlcIndex,
		pDlcId,
		pIsAvailable,
		pChDlcName,
		dlcNameLen,
		ret
	);

	return ret;
}

static bool hkClientRemoteStorage_IsCloudEnabledForApp(void* pClientRemoteStorage, AppId_t appId)
{
	const bool enabled = Hooks::IClientRemoteStorage_IsCloudEnabledForApp.tramp.fn(pClientRemoteStorage, appId);
	g_pLog->once
	(
		"%s(%p, %u) -> %i\n",

		Hooks::IClientRemoteStorage_IsCloudEnabledForApp.name.c_str(),
		pClientRemoteStorage,
		appId,
		enabled
	);

	if (Apps::shouldDisableCloud(appId))
	{
		g_pLog->once("Disabled cloud for %u\n", appId);
		return false;
	}

	return enabled;
}

static bool hkClientUser_BLoggedOn(void* pClientUser)
{
	const bool ret = Hooks::IClientUser_BLoggedOn.originalFn.fn(pClientUser);
	//Useless logging
	//g_pLog->debug
	//(
	//	"%s(%p) -> %i\n",
	//	Hooks::IClientUser_BLoggedOn.name.c_str(),
	//	pClientUser,
	//	ret
	//);
	
	if (Misc::shouldFakeOffline())
	{
		return false;
	}

	return ret;
}

static uint32_t hkClientUser_BUpdateAppOwnershipTicket(void* pClientUser, AppId_t appId, bool staleOnly)
{
	const auto cached = Ticket::getCachedTicket(appId);
	if (g_pSteamEngine->getUser(0)->isSubscribed(appId) && !cached)
	{
		staleOnly = false;
		g_pLog->debug("Force re-requesting OwnershipInfo for %u\n", appId);
	}

	const uint32_t ret = Hooks::IClientUser_BUpdateAppOwnershipTicket.originalFn.fn(pClientUser, appId, staleOnly);

	g_pLog->debug
	(
		"%s(%p, %u, %i) -> %u\n",

		Hooks::IClientUser_BUpdateAppOwnershipTicket.name.c_str(),
		pClientUser,
		appId,
		staleOnly,
		ret
	);

	return ret;
}

static uint32_t hkClientUser_GetAppOwnershipTicketExtendedData
(
	void* pClientUser,
	AppId_t appId,
	void* pTicket,
	uint32_t ticketSize,
	uint32_t* pOffAppId,
	uint32_t* pOffSteamId,
	uint32_t* pOffSig,
	uint32_t* pSigSize
)
{
	const uint32_t size = Hooks::IClientUser_GetAppOwnershipTicketExtendedData.originalFn.fn
	(
		pClientUser,
		appId,
		pTicket,
		ticketSize,
		pOffAppId,
		pOffSteamId,
		pOffSig,
		pSigSize
   );

	g_pLog->once("%s(%u)->%u\n", Hooks::IClientUser_GetAppOwnershipTicketExtendedData.name.c_str(), appId, size);

	// Preserve every genuine response. Only managed AdditionalApps whose
	// ownership lookup failed are eligible for the SteamStub ticket path.
	if (size == 0 && pTicket && pOffAppId && pOffSteamId && pOffSig && pSigSize)
	{
		SteamStubTicket::ForgedTicket forged;
		if (Ticket::forgeSteamStubTicket(appId, ticketSize, forged))
		{
			std::memcpy(pTicket, forged.bytes.data(), forged.bytes.size());
			*pOffAppId = forged.appIdOffset;
			*pOffSteamId = forged.steamIdOffset;
			*pOffSig = forged.signatureOffset;
			*pSigSize = forged.signatureSize;
			g_pLog->infoOnce(
			    "SteamStub: supplied local ownership ticket for %u\n",
			    appId);
			return forged.reportedSize;
		}
	}

	if (size)
	{
		Ticket::getTicketOwnershipExtendedData(appId);
	}

	return size;
}

static bool hkClientUser_GetEncryptedAppTicket(void* pClientUser, void* pTicket, uint32_t ticketSize, uint32_t* pTicketSize)
{
	const bool success = Hooks::IClientUser_GetEncryptedAppTicket.originalFn.fn(pClientUser, pTicket, ticketSize, pTicketSize);

	g_pLog->debug
	(
		"%s(%p, %p, %u, %p) -> %u\n",

		Hooks::IClientUser_GetEncryptedAppTicket.name.c_str(),
		pClientUser,
		pTicket,
		ticketSize,
		pTicketSize,
		success
	);

	if (success)
	{
		Ticket::getEncryptedAppTicket(FakeAppIds::getRealAppIdForCurrentPipe());
	}

	return success;
}

static uint8_t hkClientUser_IsUserSubscribedAppInTicket(void* pClientUser, uint32_t steamId, uint32_t a2, uint32_t a3, AppId_t appId)
{
	const uint8_t ticketState = Hooks::IClientUser_IsUserSubscribedAppInTicket.originalFn.fn(pClientUser, steamId, a2, a3, appId);
	//g_pLog->once("IClientUser::IsUserSubscribedAppInTicket(%p, %u, %u, %u, %u) -> %i\n", pClientUser, steamId, a2, a3, appId, ticketState);
	//Don't log the steamId, protect users from themselves and stuff
	g_pLog->once
	(
		"%s(%p, %u, %u, %u) -> %i\n",

		Hooks::IClientUser_IsUserSubscribedAppInTicket.name.c_str(),
		pClientUser,
		a2,
		a3,
		appId,
		ticketState
	);
	
	if (DLC::userSubscribedInTicket(appId))
	{
		//Owned and subscribed hehe :)
		return 0;
	}

	return ticketState;
}

static CSteamId hkClientUser_GetSteamId(const CSteamId& steamId)
{
	const auto utils = g_pSteamEngine->getUtils();
	if (!utils)
	{
		return steamId;
	}

	//Never spoof inside the Steamclient
	const AppId_t realAppId = FakeAppIds::getRealAppIdForCurrentPipe();
	if (!realAppId)
	{
		return steamId;
	}

	const auto overrides = g_config.steamIdOverride.get();
	if (overrides.contains(realAppId))
	{
		const uint64_t& id64 = overrides.at(realAppId);
		if (id64)
		{
			return CSteamId(id64);
		}

		const auto cached = Ticket::getCachedTicket(realAppId);
		if (cached)
		{
			return cached->steamId;
		}

		g_pLog->once
		(
			"SteamIdOverride for %u is set with automatic mode, but no AppOwnershipTicket exists in cache! Falling through to normal operation\n",
			realAppId
		);
	}

	if (Ticket::oneTimeSteamIdSpoof.contains(realAppId))
	{
		const CSteamId newId = Ticket::oneTimeSteamIdSpoof.at(realAppId);
		Ticket::oneTimeSteamIdSpoof.erase(realAppId);

		return newId;
	}

	//Use pipe AppId, getCachedEncryptedTicket handles FakeAppIds internally
	const auto ticket = Ticket::getCachedEncryptedTicket(utils->getAppId());
	if (ticket)
	{
		return ticket->steamId;
	}

	return steamId;
}

static bool hkClientUser_RequiresLegacyCDKey(void* pClientUser, AppId_t appId, uint32_t* a2)
{
	const bool requiresKey = Hooks::IClientUser_RequiresLegacyCDKey.originalFn.fn(pClientUser, appId, a2);
	g_pLog->once
	(
		"%s(%p, %u, %u) -> %i\n",

		Hooks::IClientUser_RequiresLegacyCDKey.name.c_str(),
		pClientUser,
		appId,
		a2,
		requiresKey
	);

	if (Apps::shouldDisableCDKey(appId))
	{
		g_pLog->once("Disable CD Key for %u\n", appId);
		return false;
	}

	return requiresKey;
}

static AppId_t hkClientUtils_GetAppId(void* pClientUtils)
{
	AppId_t appId = Hooks::IClientUtils_GetAppId.originalFn.fn(pClientUtils);

	g_pLog->debug
	(
		"%s(%p) -> %u\n",

		Hooks::IClientUtils_GetAppId.name.c_str(),
		pClientUtils,
		appId
	);

	const AppId_t real = FakeAppIds::getRealAppIdForCurrentPipe(false);
	if(real)
	{
		g_pLog->debug("Overwriting appId with %u\n", real);
		return real;
	}

	return appId;
}

static bool hkClientUtils_GetOfflineMode(void* pClientUtils)
{
	const bool ret = Hooks::IClientUtils_GetOfflineMode.originalFn.fn(pClientUtils);

	if (Misc::shouldFakeOffline())
	{
		return true;
	}

	return ret;
}

static void hkCGameInfoDialog_ServerResponded(void* pSteamMatchingPingResponse, gameserverdetails_t* details)
{
	FakeAppIds::pingResponse(details);

	Hooks::CGameInfoDialog_ServerResponded.tramp.fn(pSteamMatchingPingResponse, details);

	g_pLog->debug
	(
		"%s(%p, %p) for %u\n",
		Hooks::CGameInfoDialog_ServerResponded.name.c_str(),
		pSteamMatchingPingResponse,
		details,
		details ? details->appId : 0
	);
}

static bool hkClientConfigStore_SetString(void* pClientConfigStore, uint32_t store, const char* key, const char* value)
{
	const bool success = Hooks::IClientConfigStore_SetString.tramp.fn(pClientConfigStore, store, key, value);

	//g_pLog->debug
	//(
	//	"%s(%p, %u, %s, %s) -> %u\n",

	//	Hooks::IClientConfigStore_SetString.name.c_str(),
	//	pClientConfigStore,
	//	store,
	//	key,
	//	value,
	//	success
	//);

	if (success)
	{
		Apps::setConfigStoreString(key, value);
	}

	return success;
}

namespace Hooks
{
	//TODO: Lazily intialize in a different way, or preload glibc
	DetourHook<TraceIPC_t> TraceIPC;

	DetourHook<CAPIJob_SendAndRecv_t> CAPIJob_SendAndRecv;

	DetourHook<CAppDataCache_BParseResponseFromMessage_t> CAppDataCache_BParseResponseFromMessage;

	DetourHook<CClientUnifiedServiceMethod_SendAndRecvMsg_t> CClientUnifiedServiceMethod_SendAndRecvMsg;

	DetourHook<CCMInterface_RecvPkt_t> CCMInterface_RecvPkt;

	DetourHook<CSteamMatchmakingServers_GetServerDetails_t> CSteamMatchmakingServers_GetServerDetails;
	DetourHook<CSteamMatchmakingServers_RequestInternetServerList_t> CSteamMatchmakingServers_RequestInternetServerList;

	DetourHook<CSteamEngine_RunInterface_t> CSteamEngine_RunInterface;
	DetourHook<CSteamEngine_SetAppIdForCurrentPipe_t> CSteamEngine_SetAppIdForCurrentPipe;

	DetourHook<CUser_CheckAppOwnership_t> CUser_CheckAppOwnership;
	DetourHook<CUser_GetSubscribedApps_t> CUser_GetSubscribedApps;

	DetourHook<CUserAppManager_BuildDepotDependency_t> CUserAppManager_BuildDepotDependency;

	DetourHook<CWebSocketConnection_BBuildAndAsyncSendFrame_t> CWebSocketConnection_BBuildAndAsyncSendFrame;

	DetourHook<IClientConfigStore_SetString_t> IClientConfigStore_SetString;

	DetourHook<IClientRemoteStorage_IsCloudEnabledForApp_t> IClientRemoteStorage_IsCloudEnabledForApp;

	VFTHook<IClientAppManager_BCanRemotePlayTogether_t> IClientAppManager_BCanRemotePlayTogether;
	VFTHook<IClientAppManager_BIsDlcEnabled_t> IClientAppManager_BIsDlcEnabled;
	VFTHook<IClientAppManager_GetAppUpdateInfo_t> IClientAppManager_GetAppUpdateInfo;
	VFTHook<IClientAppManager_LaunchApp_t> IClientAppManager_LaunchApp;
	VFTHook<IClientAppManager_IsAppDlcInstalled_t> IClientAppManager_IsAppDlcInstalled;

	VFTHook<IClientApps_GetDLCDataByIndex_t> IClientApps_GetDLCDataByIndex;
	VFTHook<IClientApps_GetDLCCount_t> IClientApps_GetDLCCount;

	VFTHook<IClientUser_BLoggedOn_t> IClientUser_BLoggedOn;
	VFTHook<IClientUser_BUpdateAppOwnershipTicket_t> IClientUser_BUpdateAppOwnershipTicket;
	VFTHook<IClientUser_GetAppOwnershipTicketExtendedData_t> IClientUser_GetAppOwnershipTicketExtendedData;
	VFTHook<IClientUser_GetEncryptedAppTicket_t> IClientUser_GetEncryptedAppTicket;
	VFTHook<IClientUser_IsUserSubscribedAppInTicket_t> IClientUser_IsUserSubscribedAppInTicket;
	VFTHook<IClientUser_RequiresLegacyCDKey_t> IClientUser_RequiresLegacyCDKey;

	VFTHook<IClientUtils_GetAppId_t> IClientUtils_GetAppId;
	VFTHook<IClientUtils_GetOfflineMode_t> IClientUtils_GetOfflineMode;


	//steamui.so
	DetourHook<CGameInfoDialog_ServerResponded_t> CGameInfoDialog_ServerResponded;
}

bool Hooks::setup()
{
	g_pLog->debug("Hooks::setup()\n");

	{
		const auto name = std::string("12CConfigStore");
		if (!Decompiler::vftables.contains(name))
		{
			g_pLog->debug("Failed to get %s VFTable!\n", name.c_str());
			return false;
		}

		auto& store = Decompiler::vftables.at(name);

		IClientConfigStore_SetString.setup
		(
			VFTIndexes::IClientConfigStoreMap::SetString.getPrintName().c_str(),
			store.functions[VFTIndexes::IClientConfigStoreMap::SetString.index],
			hkClientConfigStore_SetString
		);
	}

	{
		const auto name = std::string("18CUserRemoteStorage");
		if (!Decompiler::vftables.contains(name))
		{
			g_pLog->debug("Failed to get %s VFTable!\n", name.c_str());
			return false;
		}

		auto& storage = Decompiler::vftables.at(name);

		//We detourhook because the vftable seems to get relocated, so at this point in time
		//the pointers are all wrong and would need manual adjustment which breaks
		//current assumptions by VFTHook<T>
		IClientRemoteStorage_IsCloudEnabledForApp.setup
		(
			VFTIndexes::IClientRemoteStorage::IsCloudEnabledForApp.getPrintName().c_str(),
			storage.functions[VFTIndexes::IClientRemoteStorage::IsCloudEnabledForApp.index],
			hkClientRemoteStorage_IsCloudEnabledForApp
		);
	}

	bool succeeded =
		TraceIPC.setup(Patterns::TraceIPC, &hkTraceIPC)

		&& CAPIJob_SendAndRecv.setup(Patterns::CAPIJob::SendAndRecv, hkAPIJob_SendAndRecv)

		&& CAppDataCache_BParseResponseFromMessage.setup(Patterns::CAppDataCache::BParseResponseMessage, hkAppDataCache_BParseResponseFromMessage)

		//We detour hook this virtual function out of respect for my friend Selectively11. His amazing project
		//CloudRedirect hooks the same function using a VFT hook already
		&& CClientUnifiedServiceMethod_SendAndRecvMsg.setup(VFTIndexes::CClientUnifiedServiceTransport::SendAndRecvMsg, hkClientUnifiedServiceTransport_SendAndRecvMsg)

		//To lazy to move this for now. Doesn't really matter wheter we detour or vft hook
		&& CCMInterface_RecvPkt.setup(VFTIndexes::CCMInterface::RecvPkt, hkCMInterface_RecvPkt)

		&& CSteamMatchmakingServers_GetServerDetails.setup(VFTIndexes::CSteamMatchmakingServers::GetServerDetails, hkSteamMatchmakingServers_GetServerDetails)
		&& CSteamMatchmakingServers_RequestInternetServerList.setup(VFTIndexes::CSteamMatchmakingServers::RequestInternetServerList, hkSteamMatchmakingServers_RequestInternetServerList)

		&& CUser_CheckAppOwnership.setup(Patterns::CUser::CheckAppOwnership, hkUser_CheckAppOwnership)
		&& CUser_GetSubscribedApps.setup(Patterns::CUser::GetSubscribedApps, hkUser_GetSubscribedApps)

		&& CUserAppManager_BuildDepotDependency.setup(Patterns::CUserAppManager::BuildDepotDependency, hkUserAppManager_BuildDepotDependency)

		&& CSteamEngine_RunInterface.setup(Patterns::CSteamEngine::RunInterface, hkSteamEngine_RunInterface)
		&& CSteamEngine_SetAppIdForCurrentPipe.setup(Patterns::CSteamEngine::SetAppIdForCurrentPipe, hkSteamEngine_SetAppIdForCurrentPipe)

		&& CWebSocketConnection_BBuildAndAsyncSendFrame.setup(Patterns::CWebSocketConnection::BBuildAndAsyncSendFrame, hkWebSocketConnection_BBuildAndAsyncSendFrame)

		&& CGameInfoDialog_ServerResponded.setup(VFTIndexes::CGameInfoDialog::ServerResponded, hkCGameInfoDialog_ServerResponded);


	Hooks::place();
	PackagePatch::setup();
	ManifestBind::setup();
	DepotQuarantine::setup();
	ReconcilePin::setup();
	Parental::setup();
	//This is unnecessary but I'll keep this for now in case I wanna improve error checks
	return succeeded;
}

void Hooks::place()
{
	//Detours
	TraceIPC.place();

	CAPIJob_SendAndRecv.place();

	CAppDataCache_BParseResponseFromMessage.place();

	CClientUnifiedServiceMethod_SendAndRecvMsg.place();

	CCMInterface_RecvPkt.place();

	CSteamEngine_RunInterface.place();
	CSteamEngine_SetAppIdForCurrentPipe.place();

	CSteamMatchmakingServers_GetServerDetails.place();
	CSteamMatchmakingServers_RequestInternetServerList.place();

	CUser_CheckAppOwnership.place();
	CUser_GetSubscribedApps.place();

	CUserAppManager_BuildDepotDependency.place();

	CWebSocketConnection_BBuildAndAsyncSendFrame.place();

	CGameInfoDialog_ServerResponded.place();

	IClientConfigStore_SetString.place();

	IClientRemoteStorage_IsCloudEnabledForApp.place();
}

void Hooks::placeVFTHooks()
{
	static bool hooked = false;
	if (hooked)
	{
		return;
	}

	const auto usr = g_pSteamEngine->getUser();
	if (!usr)
	{
		return;
	}

	//I don't think the IPC layer is multithreaded but better safe than sorry
	static std::mutex mutex;
	std::lock_guard guard(mutex);

	g_pLog->debug("CUser at %p\n", usr);

	{
		const auto appManager = usr->getAppManager();
		g_pLog->debug("CUserAppManager at %p\n", appManager);

		std::shared_ptr<lm_vmt_t> vft = std::make_shared<lm_vmt_t>();
		LM_VmtNew(*reinterpret_cast<lm_address_t**>(appManager), vft.get());

		Hooks::IClientAppManager_BCanRemotePlayTogether.setup(vft, VFTIndexes::IClientAppManager::BCanRemotePlayTogether, hkClientAppManager_BCanRemotePlayTogether);
		Hooks::IClientAppManager_BIsDlcEnabled.setup(vft, VFTIndexes::IClientAppManager::BIsDlcEnabled, hkClientAppManager_BIsDlcEnabled);
		Hooks::IClientAppManager_GetAppUpdateInfo.setup(vft, VFTIndexes::IClientAppManager::GetUpdateInfo, hkClientAppManager_GetUpdateInfo);
		Hooks::IClientAppManager_LaunchApp.setup(vft, VFTIndexes::IClientAppManager::LaunchApp, hkClientAppManager_LaunchApp);
		Hooks::IClientAppManager_IsAppDlcInstalled.setup(vft, VFTIndexes::IClientAppManager::IsAppDlcInstalled, hkClientAppManager_IsAppDlcInstalled);

		Hooks::IClientAppManager_BCanRemotePlayTogether.place();
		Hooks::IClientAppManager_BIsDlcEnabled.place();
		Hooks::IClientAppManager_GetAppUpdateInfo.place();
		Hooks::IClientAppManager_LaunchApp.place();
		Hooks::IClientAppManager_IsAppDlcInstalled.place();

		g_pLog->debug("IClientAppManager->vft at %p\n", vft->vtable);
	}

	{
		const auto clientApps = usr->getClientApps();
		g_pLog->debug("CUserAppInfo at %p\n", clientApps);

		std::shared_ptr<lm_vmt_t> vft = std::make_shared<lm_vmt_t>();
		LM_VmtNew(*reinterpret_cast<lm_address_t**>(clientApps), vft.get());

		Hooks::IClientApps_GetDLCDataByIndex.setup(vft, VFTIndexes::IClientApps::GetDLCDataByIndex, hkClientApps_GetDLCDataByIndex);
		Hooks::IClientApps_GetDLCCount.setup(vft, VFTIndexes::IClientApps::GetDLCCount, hkClientApps_GetDLCCount);

		Hooks::IClientApps_GetDLCDataByIndex.place();
		Hooks::IClientApps_GetDLCCount.place();

		g_pLog->debug("IClientApps->vft at %p\n", vft->vtable);
	}

	{
		const auto clientUser = usr->getClientUser();
		g_pLog->debug("IClientUser at %p\n", clientUser);

		std::shared_ptr<lm_vmt_t> vft = std::make_shared<lm_vmt_t>();
		LM_VmtNew(*reinterpret_cast<lm_address_t**>(clientUser), vft.get());

		Hooks::IClientUser_BLoggedOn.setup(vft, VFTIndexes::IClientUser::BLoggedOn, &hkClientUser_BLoggedOn);
		Hooks::IClientUser_BUpdateAppOwnershipTicket.setup(vft, VFTIndexes::IClientUser::BUpdateAppOwnershipTicket, hkClientUser_BUpdateAppOwnershipTicket);
		Hooks::IClientUser_GetAppOwnershipTicketExtendedData.setup(vft, VFTIndexes::IClientUser::GetAppOwnershipTicketExtendedData, hkClientUser_GetAppOwnershipTicketExtendedData);
		//GetEncryptedAppTicket is just a wrapper for CUser::GetEncryptedAppTicket. But there is no need to go deeper
		//since we load the encrypted ticket in the Networking layer. We just need this function to spoof our steamId once
		Hooks::IClientUser_GetEncryptedAppTicket.setup(vft, VFTIndexes::IClientUser::GetEncryptedAppTicket, hkClientUser_GetEncryptedAppTicket);
		Hooks::IClientUser_IsUserSubscribedAppInTicket.setup(vft, VFTIndexes::IClientUser::IsUserSubscribedAppInTicket, hkClientUser_IsUserSubscribedAppInTicket);
		Hooks::IClientUser_RequiresLegacyCDKey.setup(vft, VFTIndexes::IClientUser::RequiresLegacyCDKey, hkClientUser_RequiresLegacyCDKey);

		Hooks::IClientUser_BLoggedOn.place();
		Hooks::IClientUser_BUpdateAppOwnershipTicket.place();
		Hooks::IClientUser_GetAppOwnershipTicketExtendedData.place();
		//Hooks::IClientUser_GetEncryptedAppTicket.place();
		Hooks::IClientUser_IsUserSubscribedAppInTicket.place();
		Hooks::IClientUser_RequiresLegacyCDKey.place();

		g_pLog->debug("IClientUser->vft at %p\n", vft->vtable);
	}

	{
		const auto utils = g_pSteamEngine->getUtils();
		g_pLog->debug("IClientUtils at %p\n", utils);

		std::shared_ptr<lm_vmt_t> vft = std::make_shared<lm_vmt_t>();
		LM_VmtNew(*reinterpret_cast<lm_address_t**>(utils), vft.get());

		Hooks::IClientUtils_GetAppId.setup(vft, VFTIndexes::IClientUtils::GetAppId, hkClientUtils_GetAppId);
		Hooks::IClientUtils_GetOfflineMode.setup(vft, VFTIndexes::IClientUtils::GetOfflineMode, hkClientUtils_GetOfflineMode);

		Hooks::IClientUtils_GetAppId.place();
		Hooks::IClientUtils_GetOfflineMode.place();

		g_pLog->debug("IClientUtils->vft at %p\n", vft->vtable);
	}

	hooked = true;
}

void Hooks::remove()
{
	Parental::remove();
	ReconcilePin::remove();
	DepotQuarantine::remove();
	ManifestBind::remove();
	PackagePatch::remove();

	//Detours
	TraceIPC.remove();

	CAPIJob_SendAndRecv.remove();

	CAppDataCache_BParseResponseFromMessage.remove();

	CClientUnifiedServiceMethod_SendAndRecvMsg.remove();

	CCMInterface_RecvPkt.remove();

	CSteamEngine_RunInterface.remove();
	CSteamEngine_SetAppIdForCurrentPipe.remove();

	CSteamMatchmakingServers_GetServerDetails.remove();
	CSteamMatchmakingServers_RequestInternetServerList.remove();

	CUser_CheckAppOwnership.remove();
	CUser_GetSubscribedApps.remove();

	CUserAppManager_BuildDepotDependency.remove();

	CWebSocketConnection_BBuildAndAsyncSendFrame.remove();

	CGameInfoDialog_ServerResponded.remove();

	IClientConfigStore_SetString.remove();

	IClientRemoteStorage_IsCloudEnabledForApp.remove();

	//VFT Hooks
	IClientAppManager_BCanRemotePlayTogether.remove();
	IClientAppManager_BIsDlcEnabled.remove();
	IClientAppManager_GetAppUpdateInfo.remove();
	IClientAppManager_LaunchApp.remove();
	IClientAppManager_IsAppDlcInstalled.remove();

	IClientApps_GetDLCDataByIndex.remove();
	IClientApps_GetDLCCount.remove();

	IClientUser_BLoggedOn.remove();
	IClientUser_BUpdateAppOwnershipTicket.remove();
	IClientUser_GetAppOwnershipTicketExtendedData.remove();
	//IClientUser_GetEncryptedAppTicket.remove();
	IClientUser_IsUserSubscribedAppInTicket.remove();
	IClientUser_RequiresLegacyCDKey.remove();

	IClientUtils_GetAppId.remove();
	IClientUtils_GetOfflineMode.remove();

}
