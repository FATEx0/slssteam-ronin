#include "apps.hpp"

#include "../sdk/CNetPacket.hpp"
#include "../sdk/CProtoBufMsgBase.hpp"
#include "../sdk/CSteamEngine.hpp"
#include "../sdk/CUser.hpp"
#include "../sdk/CUtl.hpp"
#include "../sdk/IClientApps.hpp"
#include "../sdk/IClientAppManager.hpp"

#include "../config.hpp"
#include "../globals.hpp"
#include "../utils.hpp"

#include "fakeappid.hpp"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>


bool Apps::applistRequested;
std::unordered_set<AppId_t> Apps::privateApps = std::unordered_set<AppId_t>();

std::mutex Apps::pendingLicenseChangesMutex;
std::unordered_set<AppId_t> Apps::pendingLicenseChanges = std::unordered_set<AppId_t>();

bool Apps::unlockApp(const AppId_t appId, AppOwnershipInfo_t* info, const CSteamId& ownerId)
{
	//Changing the purchased field is enough, but just for nicety in the Steamclient UI we change the owner too
	info->owner = ownerId.accountId();
	info->realOwner = 0;
	info->familyShared = info->owner != g_currentSteamId.accountId();

	info->licensePermanent = !info->familyShared;
	info->retailLicense = false;
	info->licenseExpired = false;
	info->licensePending = false;
	info->licenseLocked = false;

	info->releaseState = k_EAppReleaseStateReleased;
	info->ownsLicense = true;

	info->lowViolence = false;
	info->regionRestricted = false;

	info->autoGrant = false;
	info->trialTime = 0;
	info->fromFreeWeekend = false;
	info->freeLicense = info->familyShared;
	info->siteLicense = false;

	g_pLog->once("Unlocked %u\n", appId);
	return true;
}

bool Apps::unlockApp(const AppId_t appId, AppOwnershipInfo_t* info)
{
	return unlockApp(appId, info, g_currentSteamId);
}

void Apps::buildDepotDependency(const AppId_t appId, CUtlVector<DepotInfo_t>* depots, CUtlVector<DepotInfo_t>* sharedDepots)
{
	g_pLog->debug("Vec Alloc %u, Grow %u, Size %u\n", depots->memory.alloc, depots->memory.growSize, depots->size);

	const auto depotBlacklist = g_config.depotBlacklist.get();
	const auto manifestOverrides = g_config.manifestIds.get();

	for(unsigned int i = 0; i < depots->size; i++)
	{
		const auto depot = depots->at(i);

		if (depotBlacklist.contains(depot->depotId))
		{
			g_pLog->debug("Removing %u with %llu\n", depot->depotId, depot->manifestId);
			depots->swap(i, depots->size - 1);
			depots->size--;
		}

		if (manifestOverrides.contains(depot->depotId))
		{
			const uint64_t oldId = depot->manifestId;
			depot->manifestId = manifestOverrides.at(depot->depotId);
			g_pLog->debug("Overrode %u's manifest %llu with %llu\n", depot->depotId, oldId, depot->manifestId);
		}

		g_pLog->debug("Depot %u for %u -> %llu\n", depot->depotId, depot->appId, depot->manifestId);
	}

	for(unsigned int i = 0; i < sharedDepots->size; i++)
	{
		const auto depot = sharedDepots->at(i);
		g_pLog->debug("Shared Depot %u for %u -> %llu\n", depot->depotId, depot->appId, depot->manifestId);
	}
}

bool Apps::checkAppOwnership(AppId_t appId, AppOwnershipInfo_t* pInfo)
{
	//Wait Until GetSubscribedApps gets called once to let Steam request and populate legit data first.
	//Afterwards modifying should hopefully not affect false positives anymore
	if (!applistRequested || !pInfo || !g_currentSteamId.isSet())
	{
		return false;
	}

	const CSteamId denuvoOwner = g_config.getDenuvoGameOwner(appId);

	//Do not modify Denuvo enabled Games
	if (denuvoOwner.isSet() && denuvoOwner.steamId64 != g_currentSteamId.steamId64)
	{
		//Would love to log the SteamId, but for users anonymity I won't
		g_pLog->once("Skipping %u because it's a Denuvo game from someone else\n", appId);
		return false;
	}

	if (g_config.shouldExcludeAppId(appId))
	{
		return false;
	}

	if (pInfo->lowViolence)
	{
		pInfo->lowViolence = false;
		g_pLog->once("Decensoring %u\n", appId);
	}
	if (pInfo->regionRestricted)
	{
		pInfo->regionRestricted = false;
		g_pLog->once("Bypassing region restriction for %u\n", appId);
	}

	const auto times = g_config.subscriptionTimestamps.get();
	if (times.contains(appId))
	{
		pInfo->purchaseTime = times.at(appId);
	}

	if (!g_config.isAddedAppId(appId))
	{
		return false;
	}

	unlockApp(appId, pInfo);

	return true;
}

void Apps::getSubscribedApps(AppId_t* appList, const size_t size, uint32_t& count)
{
	//Valve calls this function twice, once with size of 0 then again
	if (!size || !appList)
	{
		count = count + g_config.addedAppIds.get().size();
		return;
	}

	//TODO: Maybe Add check if AppId already in list before blindly appending
	for(auto& appId : g_config.addedAppIds.get())
	{
		appList[count++] = appId;
	}

	applistRequested = true;
}

void Apps::parseProductInfoFromResponse(CMsgClientPICSProductInfoResponse* msg)
{
	std::lock_guard lock(pendingLicenseChangesMutex);

	auto set = std::unordered_set<AppId_t>();
	for(const auto& app : msg->apps())
	{
		if (!pendingLicenseChanges.contains(app.appid()))
		{
			continue;
		}

		set.emplace(app.appid());
		pendingLicenseChanges.erase(app.appid());
	}

	postAppLicensesChanged(set);
}

void Apps::postAppLicensesChanged(const std::unordered_set<AppId_t>& apps)
{
	if (!apps.size())
	{
		return;
	}

	const auto user = g_pSteamEngine->getUser(0);
	if (!user)
	{
		return;
	}

	AppLicensesChanged_t cb { };
	unsigned int totalPackets = std::floor(apps.size() / AppLicensesChanged_t::MAX_APPS_PER_CALLBACK);

	for(unsigned int i = 0; i < apps.size(); i++)
	{
		unsigned int idx = i % AppLicensesChanged_t::MAX_APPS_PER_CALLBACK;

		cb.apps[idx] = *std::next(apps.begin(), i);
		cb.count = idx + 1;
		cb.appsAdded |= 1llu << idx;
		cb.remainingPackets = totalPackets;

		g_pLog->debug("AppLicensesChanged_t.apps[%u] -> %u (i -> %i, packets left -> %i, appsAdded %llu)\n", idx, cb.apps[idx], i, totalPackets, cb.appsAdded);

		if (idx + 1 >= AppLicensesChanged_t::MAX_APPS_PER_CALLBACK)
		{
			user->postCallback(ECallbackType::AppLicensesChanged_t, &cb, sizeof(cb));
			totalPackets--;
			memset(&cb, 0, sizeof(cb));
		}
	}

	if (cb.count)
	{
		user->postCallback(ECallbackType::AppLicensesChanged_t, &cb, sizeof(cb));
	}

	std::ostringstream appsLog;
	for(const auto& app : apps)
	{
		appsLog << (appsLog.str().size() ? ", " : "") << app;
	}

	g_pLog->info("AppLicensesChanged callback invoked for %s!\n", appsLog.str().c_str());
}

void Apps::runIPCFrame()
{
	const auto usr = g_pSteamEngine->getUser();
	if (!usr)
	{
		return;
	}

	const std::lock_guard appsChanged(g_config.appsChangedMutex);
	const auto appInfo = usr->getClientApps();

	if (g_config.removedApps.size())
	{
		postAppLicensesChanged(g_config.removedApps);
		g_config.removedApps.clear();
	}

	const auto added = g_config.newApps;

	if (!added.size())
	{
		return;
	}

	const std::lock_guard pendingLicensesLock(pendingLicenseChangesMutex);

	//Max batch of 15, otherwise not all apps will get a response which means they won't get added
	constexpr unsigned int MAX_APPS_PER_REQUEST = 15;
	AppId_t apps[MAX_APPS_PER_REQUEST] { };

	unsigned int i = 0;
	for(; i < added.size(); i++)
	{
		const unsigned int idx = i % MAX_APPS_PER_REQUEST;
		const AppId_t appId = *std::next(added.begin(), i);

		apps[idx] = appId;

		g_pLog->debug("AppInfoRequest %u -> %u from (%i)\n", idx, apps[idx], i);

		if (idx + 1 >= MAX_APPS_PER_REQUEST)
		{
			appInfo->requestAppInfoUpdate(apps, MAX_APPS_PER_REQUEST);
			memset(apps, 0, sizeof(apps));
		}

		pendingLicenseChanges.emplace(appId);
	}

	const unsigned int idx = i % MAX_APPS_PER_REQUEST;
	if (apps[0])
	{
		appInfo->requestAppInfoUpdate(apps, idx);
	}

	g_config.newApps.clear();
}

bool Apps::shouldDisableCloud(const AppId_t appId)
{
	if (!g_config.disableCloud.get())
	{
		return false;
	}

	// AdditionalApps are unlocked via Apps::unlockApp, which also makes
	// isSubscribed() report true for them -- so the isSubscribed() check
	// below can never catch an added app on its own. Cloud still can't
	// actually sync: Valve's cloud backend validates ownership
	// server-side and rejects the upload with "Access Denied", surfacing
	// as a cloud error. Force-disable explicitly for AdditionalApps so
	// Steam doesn't attempt the doomed sync at all.
	// getLocalUser() can be null before the engine/user pointer resolves
	// (early bootstrap). Matching upstream slsteam-moon's own null-safety
	// default: an unresolved user is treated as "don't disable" here, not
	// as "unowned" -- isAddedAppId alone still forces disable regardless.
	CUser* user = getLocalUser();
	return g_config.isAddedAppId(appId) || (user != nullptr && !user->isSubscribed(appId));
}

bool Apps::shouldDisableCDKey(const AppId_t appId)
{
	// Same reasoning as shouldDisableCloud: an added app's isSubscribed()
	// reports true, so a launch-time legacy-key request still reaches
	// Valve's backend and gets AccessDenied -- for a game whose appinfo
	// still carries hadthirdpartycdkey this can fail the launch before
	// Proton is ever spawned. Note: unlike upstream slsteam-moon, Ronin
	// has no equivalent DLC-appid registration (isAddedAppDlcId), so this
	// only covers the base AdditionalApps entry itself, not its DLC.
	CUser* user = getLocalUser();
	return g_config.isAddedAppId(appId) || (user != nullptr && !user->isSubscribed(appId));
}

bool Apps::shouldDisableUpdates(const AppId_t appId)
{
	if (!g_config.disableUpdates.get())
	{
		return false;
	}

	//Using AdditionalApps here aswell so users can manually block updates
	CUser* user = getLocalUser();
	return g_config.isAddedAppId(appId) || (user != nullptr && !user->isSubscribed(appId));
}

void Apps::sendAndRecvLastPlayedTimes(const char* name, CPlayer_GetLastPlayedTimes_Response* recv)
{
	if (strcmp(name, "Player.ClientGetLastPlayedTimes#1") != 0)
	{
		return;
	}

	const auto apps = g_config.addedAppIds.get();
	for (int i = recv->games_size() - 1; i >= 0; i--)
	{
		auto game = recv->mutable_games(i);
		if (!apps.contains(game->appid()))
		{
			continue;
		}

		g_pLog->debug("Removed serverside PlayTime for %u\n", game->appid());
		recv->mutable_games()->DeleteSubrange(i, 1);
	}
}

void Apps::sendGamesPlayed(CNetPacket* pkt)
{
	const auto titles = g_config.gameTitles.get();
	const auto usr = g_pSteamEngine->getUser();
	const auto appInfo = usr->getClientApps();

	auto msg = pkt->deserializeBody<CMsgClientGamesPlayed>();

	for(int i = 0; i < msg.games_played_size(); i++)
	{
		auto game = msg.mutable_games_played(i);
		if (!game->game_id())
		{
			continue;
		}

		const uint64_t gameId = game->game_id();

		// Native non-Steam shortcut IDs use 0x2000000 in their low 32 bits.
		// Leave the original shortcut title and 64-bit ID untouched.
		if (gameId & GAME_TYPE_SHORTCUT)
		{
			g_pLog->debug("Preserving non-Steam shortcut %llu\n", gameId);
			continue;
		}

		if (g_config.disableFamilyLock.get())
		{
			game->set_owner_id(1);
		}

		if (titles.contains(gameId))
		{
			game->set_game_extra_info(titles.at(gameId));
		}
		//This probably belongs into FakeAppIds, but the GameTitles does not so it stays here
		else if (FakeAppIds::getFakeAppId(gameId))
		{
			char name[256] {}; //No clue how long titles can get
			int len;

			if (privateApps.contains(gameId))
			{
				strcpy(name, "Redacted");
				len = strlen(name);
			}
			else
			{
				len = appInfo->getAppData(gameId, "common/name", name, sizeof(name));
			}

			if (len > 0)
			{
				g_pLog->debug("AppName %s (%i)\n", name, len);
				game->set_game_extra_info(name);
			}
		}

		//msg->mutable_games_played(i)->ParseFromString(game.SerializeAsString());

		g_pLog->debug("Playing game %llu with flags %u & pid %u\n", gameId, game->game_flags(), game->process_id());
	}

	if (msg.games_played_size() < 1)
	{
		const auto statusApp = g_config.idleStatus.get();
		if (statusApp.appId)
		{
			auto game = msg.add_games_played();
			game->set_game_id(statusApp.appId);
			game->set_game_extra_info(statusApp.title);
			game->set_game_flags(0);

			if (g_config.disableFamilyLock.get())
			{
				game->set_owner_id(1);
			}
			//game->set_game_flags(EGAMEFLAG_MULTIPLAYER);
		}
	}

	pkt->serialize(msg);
}

void Apps::sendPICSInfoRequest(CNetPacket* pkt)
{
	const auto tokens = g_config.appTokens.get();
	auto msg = pkt->deserializeBody<CMsgClientPICSProductInfoRequest>();

	for(int i = 0; i < msg.apps_size(); i++)
	{
		auto app = msg.mutable_apps(i);
		if (tokens.contains(app->appid()))
		{
			app->set_access_token(tokens.at(app->appid()));
			g_pLog->debug("Used access token from config for %u\n", app->appid());
		}
	}

	pkt->serialize(msg);
}

void Apps::sendMsg(CNetPacket *pkt)
{
	switch(pkt->getProtoBufType())
	{
		case k_EMsgClientPICSProductInfoRequest:
			sendPICSInfoRequest(pkt);
			break;

		case k_EMsgClientGamesPlayed:
		case k_EMsgClientGamesPlayedNoDataBlob:
		case k_EMsgClientGamesPlayedWithDataBlob:
			sendGamesPlayed(pkt);
			break;

		default:
			break;
	}
}

void Apps::setConfigStoreString(const char* key, const char* value)
{
	if (!std::string(key).starts_with("WebStorage\\PrivateApps"))
	{
		return;
	}

	g_pLog->debug("%s -> %s\n", key, value);

	auto str = std::string(value);
	if (str.size() < 3) //List is empty, nope out
	{
		return;
	}

	privateApps.clear();
	str = str.substr(1, str.size() - 2); //[730,240,440,etc]
	const auto split = Utils::strsplit(const_cast<char*>(str.c_str()), ",");

	for(const auto& s : split)
	{
		if (!Utils::isNumber(s.c_str()))
		{
			g_pLog->debug("%s is not a number! Skipping\n", s.c_str());
		}

		const AppId_t appId = std::stoul(s);
		privateApps.emplace(appId);
		g_pLog->debug("Added %u to privateApps\n", appId);
	}
}
