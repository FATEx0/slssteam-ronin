#include "fakeappid.hpp"

#include "../config.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CSteamMatchmakingServers.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/IClientUtils.hpp"


AppId_t FakeAppIds::lastAppLaunched;

std::unordered_map<uint32_t, AppId_t> FakeAppIds::fakeAppIdMap = std::unordered_map<AppId_t, AppId_t>();
std::unordered_map<uint32_t, AppId_t> FakeAppIds::fakeAppIdMapServer = std::unordered_map<uint32_t, AppId_t>();
std::unordered_map<uint64_t, AppId_t> FakeAppIds::fakeAppIdMapPings = std::unordered_map<uint64_t, AppId_t>();

AppId_t FakeAppIds::getFakeAppId(const AppId_t appId)
{
	const auto fakeAppIds = g_config.fakeAppIds.get();

	if (fakeAppIds.contains(appId))
	{
		return fakeAppIds.at(appId);
	}
	else if (fakeAppIds.contains(0) && !g_pSteamEngine->getUser(0)->isSubscribed(appId))
	{
		return fakeAppIds.at(0);
	}

	return 0;
}

AppId_t FakeAppIds::getRealAppIdForCurrentPipe(const bool fallback)
{
	const auto utils = g_pSteamEngine->getUtils();
	if (!utils)
	{
		return 0;
	}

	const HSteamPipe hPipe = utils->getCurrentSteamPipe();
	if (fakeAppIdMap.contains(hPipe))
	{
		return fakeAppIdMap.at(hPipe);
	}

	if (fallback)
	{
		return utils->getAppId();
	}

	return 0;
}

bool FakeAppIds::shouldUseRealAppIdForInterface(const EIPCInterface type)
{
	switch(type)
	{
		//case k_EIPCInterfaceClientUser:
		//case k_EIPCInterfaceClientGameServerInternal:
		//case k_EIPCInterfaceClientFriends:
		case k_EIPCInterfaceClientUtils:
		case k_EIPCInterfaceClientBilling:
		//case k_EIPCInterfaceClientMatchmaking:
		case k_EIPCInterfaceClientApps:
		case k_EIPCInterfaceClientUserStats:
		//case k_EIPCInterfaceClientNetworking:
		case k_EIPCInterfaceClientRemoteStorage:
		case k_EIPCInterfaceClientDepotBuilder:
		case k_EIPCInterfaceClientAppManager:
		case k_EIPCInterfaceClientConfigStore:
		//case k_EIPCInterfaceClientGameCoordinator:
		//case k_EIPCInterfaceClientGameServerStats:
		case k_EIPCInterfaceClientGameStats:
		case k_EIPCInterfaceClientHTTP:
		case k_EIPCInterfaceClientScreenshots:
		case k_EIPCInterfaceClientAudio:
		case k_EIPCInterfaceClientUnifiedMessages:
		case k_EIPCInterfaceClientStreamLauncher:
		case k_EIPCInterfaceClientParentalSettings:
		case k_EIPCInterfaceClientNetworkDeviceManager:
		case k_EIPCInterfaceClientMusic:
		case k_EIPCInterfaceClientRemoteClientManager:
		case k_EIPCInterfaceClientUGC:
		case k_EIPCInterfaceClientStreamClient:
		case k_EIPCInterfaceClientProductBuilder:
		case k_EIPCInterfaceClientShortcuts:
		case k_EIPCInterfaceClientGameNotifications:
		case k_EIPCInterfaceClientVideo:
		case k_EIPCInterfaceClientInventory:
		case k_EIPCInterfaceClientVR:
		case k_EIPCInterfaceClientControllerSerialized:
		case k_EIPCInterfaceClientAppDisableUpdate:
		case k_EIPCInterfaceClientSharedConnection:
		case k_EIPCInterfaceClientShader:
		//case k_EIPCInterfaceClientNetworkingSocketsSerialized:
		case k_EIPCInterfaceClientCompat:
		case k_EIPCInterfaceClientParties:
		//case k_EIPCInterfaceClientNetworkingUtilsSerialized:
		case k_EIPCInterfaceClientRemotePlay:
		//case k_EIPCInterfaceClientGameServerPacketHandler:
		case k_EIPCInterfaceClientSystemManager:
		case k_EIPCInterfaceClientSystemPerfManager:
		case k_EIPCInterfaceClientSystemDockManager:
		case k_EIPCInterfaceClientSystemAudioManager:
		case k_EIPCInterfaceClientSystemDisplayManager:
		case k_EIPCInterfaceClientTimeline:
			return true;

		default:
			return false;
	}
}

void FakeAppIds::launchApp(const AppId_t appId)
{
	lastAppLaunched = appId;
}

void FakeAppIds::setAppIdForCurrentPipe(AppId_t& appId)
{
	const auto utils = g_pSteamEngine->getUtils();

	fakeAppIdMap[utils->getCurrentSteamPipe()] = lastAppLaunched;
	g_pLog->debug("fakeAppIdMap[%p] = %u\n", utils->getCurrentSteamPipe(), lastAppLaunched);

	//Do not change Steam Client itself (AppId 0)
	if (!appId)
	{
		return;
	}

	const AppId_t newAppId = getFakeAppId(appId);
	if (newAppId)
	{
		g_pLog->once("Changing AppId of %u\n", appId);
		appId = newAppId;
	}
}

void FakeAppIds::runIPCFrame(const bool post)
{
	AppId_t appId = getRealAppIdForCurrentPipe(false);
	const AppId_t fakeAppId = getFakeAppId(appId);

	if (!appId || !fakeAppId || appId == fakeAppId)
	{
		return;
	}

	if (post)
	{
		appId = fakeAppId;
	}

	if (g_config.extendedLogging.get())
	{
		const auto utils = g_pSteamEngine->getUtils();
		g_pLog->debug("Setting AppId to %u in pipe %p\n", appId, utils ? utils->getCurrentSteamPipe() : 0);
	}
	g_pSteamEngine->setAppIdForCurrentPipe(appId);
}

void FakeAppIds::getServerDetails(const uint32_t handle, gameserverdetails_t& details)
{
	if (!fakeAppIdMapServer.contains(handle))
	{
		return;
	}

	const AppId_t realAppId = fakeAppIdMapServer[handle];
	fakeAppIdMapPings[*reinterpret_cast<uint64_t*>(&details.address)] = realAppId;
	details.appId = realAppId;

	g_pLog->debug("Changing appId back to %u\n", realAppId);
}

uint32_t FakeAppIds::requestInternetServerList(const AppId_t appId)
{
	const AppId_t fake = getFakeAppId(appId);
	if (!fake)
	{
		return 0;
	}

	g_pLog->debug("Replacing %u with %u\n", appId, fake);
	return fake;
}

void FakeAppIds::pingResponse(gameserverdetails_t *details)
{
	if (!details)
	{
		return;
	}

	const uint64_t ip = *reinterpret_cast<uint64_t*>(&details->address);
	if (!fakeAppIdMapPings.contains(ip))
	{
		return;
	}

	details->appId = fakeAppIdMapPings[ip];
}

void FakeAppIds::sendGamesPlayed(CNetPacket *pkt)
{
	auto msg = pkt->deserializeBody<CMsgClientGamesPlayed>();

	for(int i = 0; i < msg.games_played_size(); i++)
	{
		const auto game = msg.mutable_games_played(i);
		const uint64_t gameId = game->game_id();

		if (gameId & GAME_TYPE_SHORTCUT)
		{
			continue;
		}

		const AppId_t fakeAppId = FakeAppIds::getFakeAppId(gameId);
		if (!fakeAppId)
		{
			continue;
		}

		g_pLog->debug("Setting %llu to %u\n", gameId, fakeAppId);
		game->set_game_id(fakeAppId);
	}

	pkt->serialize(msg);
}

void FakeAppIds::sendRichPresenceUpload(CNetPacket* pkt)
{
	auto header = pkt->deserializeHeader();
	g_pLog->debug("Routing appId %u\n", header.routing_appid());

	const auto appId = getFakeAppId(header.routing_appid());

	if (!appId)
	{
		return;
	}

	//This won't fix localized rich presences, but it's better than nothing
	header.set_routing_appid(appId);

	auto msg = pkt->deserializeBody<CMsgClientRichPresenceUpload>();
	pkt->serialize(msg, &header);
}

void FakeAppIds::sendMsg(CNetPacket* pkt)
{
	switch(pkt->getProtoBufType())
	{
		case k_EMsgClientGamesPlayed:
		case k_EMsgClientGamesPlayedNoDataBlob:
		case k_EMsgClientGamesPlayedWithDataBlob:
			sendGamesPlayed(pkt);
			break;
		
		case k_EMsgClientRichPresenceUpload:
			sendRichPresenceUpload(pkt);
			break;

		default:
			break;
	}
}
